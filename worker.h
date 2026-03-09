#pragma once

#include "config.h"
#include "dns.h"
#include "stats.h"
#include <atomic>

// Per-worker context. Passed to the worker thread function.
struct WorkerCtx {
    int                       worker_id;
    const Config*             cfg;
    const PrebuiltQuery*      queries;       // full query array (shared, read-only)
    size_t                    num_queries;
    uint64_t                  my_query_limit; // max queries this thread should send
    ThreadStats*              stats;
    const std::atomic<bool>*  stop_flag;
};

// Worker thread entry point.
void worker_main(WorkerCtx ctx);
