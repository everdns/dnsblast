#pragma once

#include <cstdint>
#include <algorithm>
#include "clock.h"

// Per-thread token bucket rate limiter.
// No locks — each worker owns its own instance.
class TokenBucket {
public:
    TokenBucket() = default;

    void init(uint64_t qps, int max_burst = 64) {
        tokens_        = 0.0;
        max_tokens_    = (double)max_burst;
        tokens_per_ns_ = (double)qps / 1e9;
        last_refill_   = now_ns();
    }

    // Try to consume up to `requested` tokens.
    // Returns the number actually granted (may be 0).
    int consume(int requested) {
        uint64_t now = now_ns();
        double elapsed = (double)(now - last_refill_);
        last_refill_ = now;

        tokens_ = std::min(max_tokens_, tokens_ + elapsed * tokens_per_ns_);

        int granted = std::min(requested, (int)tokens_);
        if (granted < 0) granted = 0;
        tokens_ -= granted;
        return granted;
    }

private:
    double   tokens_        = 0.0;
    double   max_tokens_    = 64.0;
    double   tokens_per_ns_ = 0.0;
    uint64_t last_refill_   = 0;
};
