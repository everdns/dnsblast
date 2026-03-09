#include "config.h"
#include "dns.h"
#include "stats.h"
#include "worker.h"
#include "clock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <signal.h>
#include <unistd.h>

static std::atomic<bool> g_stop{false};

static void signal_handler(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    // Install signal handlers for clean shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Load and pre-encode queries
    std::vector<PrebuiltQuery> queries = load_queries(cfg.input_file.c_str());

    // Print configuration
    fprintf(stderr, "Target QPS:    %lu\n", (unsigned long)cfg.target_qps);
    fprintf(stderr, "Total queries: %lu%s\n",
            (unsigned long)cfg.total_queries,
            cfg.total_queries == 0 ? " (unlimited)" : "");
    fprintf(stderr, "Duration:      %.1f s\n", cfg.runtime_sec);
    fprintf(stderr, "Timeout:       %.1f s\n", cfg.timeout_sec);
    fprintf(stderr, "Senders:       %d\n", cfg.num_threads);
    fprintf(stderr, "Receivers:     %d\n", cfg.num_threads);
    fprintf(stderr, "Ports:         %d\n", cfg.num_ports);
    fprintf(stderr, "Server:        %s:%u (%s)\n",
            cfg.server_ip.c_str(), cfg.server_port,
            cfg.ipv6 ? "IPv6" : "IPv4");
    fprintf(stderr, "\n");

    // If total_queries is 0 (unlimited), compute a practical limit from QPS × duration
    uint64_t effective_total = cfg.total_queries;
    if (effective_total == 0) {
        effective_total = (uint64_t)(cfg.target_qps * cfg.runtime_sec * 2.0);
        if (effective_total == 0) effective_total = UINT64_MAX;
    }

    int num_senders = cfg.num_threads;

    // Allocate shared state for each sender-receiver pair
    auto* shared = new WorkerShared[num_senders];
    for (int i = 0; i < num_senders; i++) {
        shared[i].stop_flag = &g_stop;
    }

    // Allocate per-thread stats: [0..num_senders-1] for TX, [num_senders..2*num_senders-1] for RX
    int total_stats = 2 * num_senders;
    auto* thread_stats = new ThreadStats[total_stats];

    // Distribute queries across senders
    uint64_t per_thread_limit = effective_total / (uint64_t)num_senders;
    uint64_t remainder = effective_total % (uint64_t)num_senders;

    // Launch threads
    std::vector<std::thread> threads;
    threads.reserve(total_stats);

    uint64_t start_time = now_ns();

    for (int i = 0; i < num_senders; i++) {
        // Launch sender
        SenderCtx sctx;
        sctx.worker_id      = i;
        sctx.cfg             = &cfg;
        sctx.queries         = queries.data();
        sctx.num_queries     = queries.size();
        sctx.my_query_limit  = per_thread_limit + ((uint64_t)i < remainder ? 1 : 0);
        sctx.stats           = &thread_stats[i];
        sctx.shared          = &shared[i];
        threads.emplace_back(sender_main, sctx);

        // Launch receiver
        ReceiverCtx rctx;
        rctx.worker_id = i;
        rctx.cfg       = &cfg;
        rctx.stats     = &thread_stats[num_senders + i];
        rctx.shared    = &shared[i];
        threads.emplace_back(receiver_main, rctx);
    }

    // Timer: enforce total runtime
    uint64_t deadline_ns = (uint64_t)(cfg.runtime_sec * 1e9);
    while (!g_stop.load(std::memory_order_relaxed)) {
        usleep(1000);
        if (now_ns() - start_time >= deadline_ns) {
            g_stop.store(true, std::memory_order_relaxed);
            break;
        }
    }

    // Wait for all threads to finish
    for (auto& t : threads) t.join();

    uint64_t end_time = now_ns();
    double duration_s = (double)(end_time - start_time) / 1e9;

    // Aggregate all stats (TX + RX)
    GlobalStats g = aggregate_stats(thread_stats, total_stats);
    print_results(g, duration_s);

    delete[] thread_stats;
    delete[] shared;
    return 0;
}
