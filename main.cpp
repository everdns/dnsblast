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
    fprintf(stderr, "Threads:       %d\n", cfg.num_threads);
    fprintf(stderr, "Ports:         %d\n", cfg.num_ports);
    fprintf(stderr, "Server:        %s:%u (%s)\n",
            cfg.server_ip.c_str(), cfg.server_port,
            cfg.ipv6 ? "IPv6" : "IPv4");
    fprintf(stderr, "\n");

    // If total_queries is 0 (unlimited), compute a practical limit from QPS × duration
    // so we have a finite per-thread query budget.
    uint64_t effective_total = cfg.total_queries;
    if (effective_total == 0) {
        // Allow up to 2x the expected queries (headroom)
        effective_total = (uint64_t)(cfg.target_qps * cfg.runtime_sec * 2.0);
        if (effective_total == 0) effective_total = UINT64_MAX;
    }

    // Allocate per-thread stats (heap-allocated for alignment)
    auto* thread_stats = new ThreadStats[(size_t)cfg.num_threads];

    // Distribute queries across threads
    uint64_t per_thread_limit = effective_total / (uint64_t)cfg.num_threads;
    uint64_t remainder = effective_total % (uint64_t)cfg.num_threads;

    // Launch worker threads
    std::vector<std::thread> threads;
    threads.reserve((size_t)cfg.num_threads);

    uint64_t start_time = now_ns();

    for (int i = 0; i < cfg.num_threads; i++) {
        WorkerCtx ctx;
        ctx.worker_id      = i;
        ctx.cfg             = &cfg;
        ctx.queries         = queries.data();
        ctx.num_queries     = queries.size();
        ctx.my_query_limit  = per_thread_limit + ((uint64_t)i < remainder ? 1 : 0);
        ctx.stats           = &thread_stats[i];
        ctx.stop_flag       = &g_stop;

        threads.emplace_back(worker_main, ctx);
    }

    // Timer: enforce total runtime
    uint64_t deadline_ns = (uint64_t)(cfg.runtime_sec * 1e9);
    while (!g_stop.load(std::memory_order_relaxed)) {
        usleep(1000); // 1ms check granularity
        if (now_ns() - start_time >= deadline_ns) {
            g_stop.store(true, std::memory_order_relaxed);
            break;
        }
    }

    // Wait for all workers to finish
    for (auto& t : threads) t.join();

    uint64_t end_time = now_ns();
    double duration_s = (double)(end_time - start_time) / 1e9;

    // Aggregate and report
    GlobalStats g = aggregate_stats(thread_stats, cfg.num_threads);
    print_results(g, duration_s);

    delete[] thread_stats;
    return 0;
}
