#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <algorithm>

// RTT histogram: 0–500ms in 1µs buckets
static constexpr int RTT_BUCKET_COUNT = 500000;

// Per-thread statistics — cache-line aligned to prevent false sharing.
struct alignas(64) ThreadStats {
    uint64_t queries_sent;
    uint64_t responses_received;
    uint64_t rcode_noerror;
    uint64_t rcode_nxdomain;
    uint64_t rcode_servfail;
    uint64_t rcode_other;
    uint64_t timeouts;
    uint64_t send_errors;

    uint64_t rtt_min_ns;
    uint64_t rtt_max_ns;
    uint64_t rtt_sum_ns;

    uint32_t rtt_histogram[RTT_BUCKET_COUNT];

    void reset() {
        memset(this, 0, sizeof(*this));
        rtt_min_ns = UINT64_MAX;
    }
};

// Aggregated global statistics.
struct GlobalStats {
    uint64_t queries_sent;
    uint64_t responses_received;
    uint64_t rcode_noerror;
    uint64_t rcode_nxdomain;
    uint64_t rcode_servfail;
    uint64_t rcode_other;
    uint64_t timeouts;
    uint64_t send_errors;

    uint64_t rtt_min_ns;
    uint64_t rtt_max_ns;
    uint64_t rtt_sum_ns;

    uint32_t rtt_histogram[RTT_BUCKET_COUNT];
};

// Aggregate per-thread stats into a single GlobalStats.
inline GlobalStats aggregate_stats(const ThreadStats* per_thread, int num_threads) {
    GlobalStats g;
    memset(&g, 0, sizeof(g));
    g.rtt_min_ns = UINT64_MAX;

    for (int t = 0; t < num_threads; t++) {
        const auto& s = per_thread[t];
        g.queries_sent       += s.queries_sent;
        g.responses_received += s.responses_received;
        g.rcode_noerror      += s.rcode_noerror;
        g.rcode_nxdomain     += s.rcode_nxdomain;
        g.rcode_servfail     += s.rcode_servfail;
        g.rcode_other        += s.rcode_other;
        g.timeouts           += s.timeouts;
        g.send_errors        += s.send_errors;
        g.rtt_min_ns = std::min(g.rtt_min_ns, s.rtt_min_ns);
        g.rtt_max_ns = std::max(g.rtt_max_ns, s.rtt_max_ns);
        g.rtt_sum_ns += s.rtt_sum_ns;
        for (int b = 0; b < RTT_BUCKET_COUNT; b++)
            g.rtt_histogram[b] += s.rtt_histogram[b];
    }
    return g;
}

// Extract a percentile from the histogram. Returns value in microseconds.
inline uint64_t percentile(const uint32_t* histogram, int num_buckets,
                           uint64_t total_count, double p) {
    if (total_count == 0) return 0;
    uint64_t target = (uint64_t)((double)total_count * p);
    uint64_t cumulative = 0;
    for (int i = 0; i < num_buckets; i++) {
        cumulative += histogram[i];
        if (cumulative >= target)
            return (uint64_t)i; // bucket index = RTT in µs
    }
    return (uint64_t)(num_buckets - 1);
}

// Print final results to stdout.
inline void print_results(const GlobalStats& g, double duration_s) {
    double avg_rtt_us = 0.0;
    if (g.responses_received > 0)
        avg_rtt_us = ((double)g.rtt_sum_ns / (double)g.responses_received) / 1000.0;

    uint64_t p50 = percentile(g.rtt_histogram, RTT_BUCKET_COUNT, g.responses_received, 0.50);
    uint64_t p90 = percentile(g.rtt_histogram, RTT_BUCKET_COUNT, g.responses_received, 0.90);
    uint64_t p99 = percentile(g.rtt_histogram, RTT_BUCKET_COUNT, g.responses_received, 0.99);

    double qps = (duration_s > 0) ? (double)g.queries_sent / duration_s : 0;
    double aps = (duration_s > 0) ? (double)(g.rcode_noerror + g.rcode_nxdomain) / duration_s : 0;

    printf("\n");
    printf("============================== Results ==============================\n");
    printf("Duration:              %.3f s\n", duration_s);
    printf("Queries sent:          %lu\n", (unsigned long)g.queries_sent);
    printf("Responses received:    %lu\n", (unsigned long)g.responses_received);
    printf("  NOERROR:             %lu\n", (unsigned long)g.rcode_noerror);
    printf("  NXDOMAIN:            %lu\n", (unsigned long)g.rcode_nxdomain);
    printf("  SERVFAIL:            %lu\n", (unsigned long)g.rcode_servfail);
    printf("  Other:               %lu\n", (unsigned long)g.rcode_other);
    printf("Timeouts:              %lu\n", (unsigned long)g.timeouts);
    printf("Send errors:           %lu\n", (unsigned long)g.send_errors);
    printf("---------------------------------------------------------------------\n");
    if (g.responses_received > 0) {
        printf("RTT min:               %lu us\n", (unsigned long)(g.rtt_min_ns / 1000));
        printf("RTT max:               %lu us\n", (unsigned long)(g.rtt_max_ns / 1000));
        printf("RTT avg:               %.1f us\n", avg_rtt_us);
        printf("RTT p50:               %lu us\n", (unsigned long)p50);
        printf("RTT p90:               %lu us\n", (unsigned long)p90);
        printf("RTT p99:               %lu us\n", (unsigned long)p99);
    } else {
        printf("RTT:                   (no responses)\n");
    }
    printf("---------------------------------------------------------------------\n");
    printf("Achieved QPS:          %.0f\n", qps);
    printf("Answers/sec:           %.0f\n", aps);
    printf("=====================================================================\n");
}
