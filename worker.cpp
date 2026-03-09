#include "worker.h"
#include "clock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <immintrin.h> // _mm_pause

// Maximum batch size for sendmmsg / recvmmsg
static constexpr int TX_BATCH = 64;
static constexpr int RX_BATCH = 64;

static void pin_thread(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

static int create_udp_socket(const struct sockaddr_storage* target, socklen_t addrlen) {
    int af = target->ss_family;
    int fd = socket(af, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    // Enlarge socket buffers
    int bufsz = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    // Bind to ephemeral port
    if (af == AF_INET) {
        struct sockaddr_in bind_addr = {};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = 0;
        bind_addr.sin_addr.s_addr = INADDR_ANY;
        bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr));
    } else {
        struct sockaddr_in6 bind_addr = {};
        bind_addr.sin6_family = AF_INET6;
        bind_addr.sin6_port = 0;
        bind_addr.sin6_addr = in6addr_any;
        bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr));
    }

    // "Connect" UDP to target — enables send() and avoids per-packet route lookup
    connect(fd, (const struct sockaddr*)target, addrlen);

    return fd;
}

static bool resolve_target(const Config& cfg, struct sockaddr_storage* addr, socklen_t* addrlen) {
    if (cfg.ipv6) {
        auto* a = (struct sockaddr_in6*)addr;
        memset(a, 0, sizeof(*a));
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(cfg.server_port);
        if (inet_pton(AF_INET6, cfg.server_ip.c_str(), &a->sin6_addr) != 1) {
            fprintf(stderr, "Error: invalid IPv6 address '%s'\n", cfg.server_ip.c_str());
            return false;
        }
        *addrlen = sizeof(struct sockaddr_in6);
    } else {
        auto* a = (struct sockaddr_in*)addr;
        memset(a, 0, sizeof(*a));
        a->sin_family = AF_INET;
        a->sin_port = htons(cfg.server_port);
        if (inet_pton(AF_INET, cfg.server_ip.c_str(), &a->sin_addr) != 1) {
            fprintf(stderr, "Error: invalid IPv4 address '%s'\n", cfg.server_ip.c_str());
            return false;
        }
        *addrlen = sizeof(struct sockaddr_in);
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  SENDER — pure TX thread
// ═══════════════════════════════════════════════════════════════════

void sender_main(SenderCtx ctx) {
    const Config& cfg = *ctx.cfg;
    ThreadStats& stats = *ctx.stats;
    WorkerShared& shared = *ctx.shared;
    stats.reset();

    // Pin sender to core (even cores)
    pin_thread(ctx.worker_id * 2);

    // Resolve target address
    struct sockaddr_storage target_addr;
    socklen_t target_addrlen;
    if (!resolve_target(cfg, &target_addr, &target_addrlen)) {
        return;
    }

    // Create sockets for this sender-receiver pair
    int ports_per_thread = std::max(1, cfg.num_ports / cfg.num_threads);
    int num_fds = ports_per_thread;
    int* fds = new int[num_fds];
    for (int i = 0; i < num_fds; i++) {
        fds[i] = create_udp_socket(&target_addr, target_addrlen);
        if (fds[i] < 0) {
            for (int j = 0; j < i; j++) close(fds[j]);
            delete[] fds;
            return;
        }
    }

    // Set up epoll for receiver
    int epfd = epoll_create1(0);
    for (int i = 0; i < num_fds; i++) {
        struct epoll_event ev = {};
        ev.events = EPOLLIN;
        ev.data.fd = fds[i];
        epoll_ctl(epfd, EPOLL_CTL_ADD, fds[i], &ev);
    }

    // Publish shared state for the receiver thread
    shared.fds = fds;
    shared.num_fds = num_fds;
    shared.epfd = epfd;
    for (int i = 0; i < 65536; i++)
        shared.send_time[i].store(0, std::memory_order_relaxed);
    shared.ready.store(true, std::memory_order_release);

    // Deadline-based pacer
    uint64_t per_thread_qps = cfg.target_qps / (uint64_t)cfg.num_threads;
    if (per_thread_qps == 0) per_thread_qps = 1;
    uint64_t pace_start = now_ns();
    uint64_t packets_paced = 0;

    // TX ID state
    uint16_t next_txid = 0;
    int socket_idx = 0;

    // Pre-allocate send buffers
    uint8_t send_bufs[TX_BATCH][512];
    uint16_t send_lens[TX_BATCH];
    struct mmsghdr tx_msgs[TX_BATCH];
    struct iovec   tx_iovs[TX_BATCH];

    uint64_t query_idx = 0;
    uint64_t my_limit = ctx.my_query_limit;

    // ──────────── TX Loop ────────────
    while (!shared.stop_flag->load(std::memory_order_relaxed)) {
        if (query_idx >= my_limit) break;

        // Wait until our next batch deadline (global start time based)
        uint64_t deadline = pace_start + packets_paced * 1000000000ULL / per_thread_qps;
        uint64_t now = now_ns();
        if (now < deadline) {
            while (now_ns() < deadline)
                _mm_pause();
        }

        int batch_size = std::min((int)(my_limit - query_idx), TX_BATCH);
        int fd = fds[socket_idx % num_fds];

        // Build entire batch
        for (int i = 0; i < batch_size; i++) {
            uint64_t ts = now_ns();
            const PrebuiltQuery& q = ctx.queries[(query_idx + (uint64_t)i) % ctx.num_queries];
            memcpy(send_bufs[i], q.wire, q.wire_len);
            send_lens[i] = q.wire_len;

            // Stamp transaction ID and record send time
            uint16_t txid = next_txid++;
            if (shared.send_time[txid].load(std::memory_order_relaxed) != 0) {
                stats.timeouts++;  // previous query with this txid never got a response
            }
            shared.send_time[txid].store(ts, std::memory_order_relaxed);

            send_bufs[i][0] = (uint8_t)(txid >> 8);
            send_bufs[i][1] = (uint8_t)(txid & 0xFF);

            tx_iovs[i].iov_base = send_bufs[i];
            tx_iovs[i].iov_len = send_lens[i];
            memset(&tx_msgs[i], 0, sizeof(tx_msgs[i]));
            tx_msgs[i].msg_hdr.msg_iov = &tx_iovs[i];
            tx_msgs[i].msg_hdr.msg_iovlen = 1;
        }

        // Batched send
        int sent = sendmmsg(fd, tx_msgs, (unsigned int)batch_size, MSG_DONTWAIT);
        if (sent > 0) {
            stats.queries_sent += (uint64_t)sent;
            query_idx += (uint64_t)sent;
            packets_paced += (uint64_t)sent;
        }
        if (sent < batch_size) {
            int failed = batch_size - (sent > 0 ? sent : 0);
            stats.send_errors += (uint64_t)failed;
            if (sent > 0) query_idx = std::min(query_idx, my_limit);
            packets_paced += (uint64_t)failed;
        }

        socket_idx++;
    }

    // Final timeout sweep — slots still occupied are unanswered queries
    for (int i = 0; i < 65536; i++) {
        if (shared.send_time[i].load(std::memory_order_relaxed) != 0) {
            stats.timeouts++;
        }
    }

    // Note: sockets and epfd are cleaned up by the receiver thread
}


// ═══════════════════════════════════════════════════════════════════
//  RECEIVER — pure RX thread
// ═══════════════════════════════════════════════════════════════════

void receiver_main(ReceiverCtx ctx) {
    const Config& cfg = *ctx.cfg;
    ThreadStats& stats = *ctx.stats;
    WorkerShared& shared = *ctx.shared;
    stats.reset();
    (void)cfg;

    // Pin receiver to core (odd cores)
    pin_thread(ctx.worker_id * 2 + 1);

    // Wait for sender to set up sockets
    while (!shared.ready.load(std::memory_order_acquire)) {
        if (shared.stop_flag->load(std::memory_order_relaxed)) return;
        _mm_pause();
    }

    int epfd = shared.epfd;

    // Pre-allocate recv buffers
    uint8_t recv_bufs[RX_BATCH][512];
    struct mmsghdr rx_msgs[RX_BATCH];
    struct iovec   rx_iovs[RX_BATCH];

    for (int i = 0; i < RX_BATCH; i++) {
        rx_iovs[i].iov_base = recv_bufs[i];
        rx_iovs[i].iov_len = 512;
        memset(&rx_msgs[i], 0, sizeof(rx_msgs[i]));
        rx_msgs[i].msg_hdr.msg_iov = &rx_iovs[i];
        rx_msgs[i].msg_hdr.msg_iovlen = 1;
    }

    // ──────────── RX Loop ────────────
    struct epoll_event events[64];

    while (!shared.stop_flag->load(std::memory_order_relaxed)) {
        int nready = epoll_wait(epfd, events, 64, 1); // 1ms timeout
        if (nready <= 0) continue;

        for (int e = 0; e < nready; e++) {
            int fd = events[e].data.fd;

            while (true) {
                for (int i = 0; i < RX_BATCH; i++) {
                    rx_iovs[i].iov_len = 512;
                }

                struct timespec no_wait = {0, 0};
                int nrecv = recvmmsg(fd, rx_msgs, RX_BATCH, MSG_DONTWAIT, &no_wait);
                if (nrecv <= 0) break;

                uint64_t rx_time = now_ns();

                for (int r = 0; r < nrecv; r++) {
                    unsigned int len = rx_msgs[r].msg_len;
                    if (len < 12) continue;

                    uint8_t* pkt = recv_bufs[r];
                    uint16_t txid = dns_txid(pkt);
                    uint8_t rcode = dns_rcode(pkt);

                    // Resolve: load and clear with relaxed atomics
                    uint64_t send_time = shared.send_time[txid].load(std::memory_order_relaxed);
                    if (send_time == 0) continue;
                    shared.send_time[txid].store(0, std::memory_order_relaxed);

                    uint64_t rtt_ns = rx_time - send_time;
                    stats.responses_received++;
                    stats.rtt_sum_ns += rtt_ns;
                    if (rtt_ns < stats.rtt_min_ns) stats.rtt_min_ns = rtt_ns;
                    if (rtt_ns > stats.rtt_max_ns) stats.rtt_max_ns = rtt_ns;

                    uint32_t rtt_us = (uint32_t)(rtt_ns / 1000);
                    if (rtt_us < (uint32_t)RTT_BUCKET_COUNT)
                        stats.rtt_histogram[rtt_us]++;

                    switch (rcode) {
                    case 0: stats.rcode_noerror++;  break;
                    case 3: stats.rcode_nxdomain++; break;
                    case 2: stats.rcode_servfail++; break;
                    default: stats.rcode_other++;   break;
                    }
                }
            }
        }
    }

    // Cleanup sockets and epoll (receiver owns cleanup since sender may exit first)
    close(epfd);
    for (int i = 0; i < shared.num_fds; i++) close(shared.fds[i]);
    delete[] shared.fds;
}
