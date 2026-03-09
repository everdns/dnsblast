#pragma once

#include <cstdint>
#include <time.h>

// High-resolution monotonic clock via vDSO (no syscall on modern Linux).
inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
