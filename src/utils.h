#pragma once

#include <cstdint>
#include <ctime>
#include <pthread.h>
#include <sched.h>

inline void pin_to_cpu(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

inline uint64_t clock_ns(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

inline uint64_t mono_ns() {
    return clock_ns(CLOCK_MONOTONIC);
}

inline uint64_t tai_ns() {
    return clock_ns(CLOCK_TAI);
}
