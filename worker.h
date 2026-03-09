#pragma once

#include "config.h"
#include "dns.h"
#include "stats.h"
#include <atomic>

// Shared state between a sender-receiver pair.
// Sender creates sockets and sets ready flag; receiver waits for it.
struct WorkerShared {
    int*  fds;
    int   num_fds;
    int   epfd;

    // TxId tracker: sender writes send_time[txid], receiver reads and clears.
    // Uses relaxed atomics — no fences needed, just tear-free access.
    std::atomic<uint64_t> send_time[65536];

    const std::atomic<bool>* stop_flag;
    std::atomic<bool> ready{false};  // sender sets after socket setup
};

// Context for a sender (TX-only) thread.
struct SenderCtx {
    int                       worker_id;
    const Config*             cfg;
    const PrebuiltQuery*      queries;
    size_t                    num_queries;
    uint64_t                  my_query_limit;
    ThreadStats*              stats;    // TX stats: queries_sent, send_errors, timeouts
    WorkerShared*             shared;
};

// Context for a receiver (RX-only) thread.
struct ReceiverCtx {
    int                       worker_id;
    const Config*             cfg;
    ThreadStats*              stats;    // RX stats: responses_received, rtt_*, rcode_*
    WorkerShared*             shared;
};

void sender_main(SenderCtx ctx);
void receiver_main(ReceiverCtx ctx);
