#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include "tracker.h"

// Logarithmic histogram for RTT percentile computation.
// Range 1: [0, 100us)     -> 100 buckets of 1us    (indices 0..99)
// Range 2: [100us, 10ms)  -> 990 buckets of 10us   (indices 100..1089)
// Range 3: [10ms, 1s)     -> 990 buckets of 1ms    (indices 1090..2079)
// Overflow: >=1s           -> index 2080
static constexpr int HISTOGRAM_BUCKETS = 2081;

inline uint32_t rtt_to_bucket(uint64_t rtt_us) {
    if (rtt_us < 100)       return static_cast<uint32_t>(rtt_us);
    if (rtt_us < 10000)     return 100 + static_cast<uint32_t>((rtt_us - 100) / 10);
    if (rtt_us < 1000000)   return 1090 + static_cast<uint32_t>((rtt_us - 10000) / 1000);
    return 2080;
}

// Returns the lower bound of the bucket in microseconds.
inline double bucket_to_rtt_us(uint32_t bucket) {
    if (bucket < 100)       return static_cast<double>(bucket);
    if (bucket < 1090)      return 100.0 + (bucket - 100) * 10.0;
    if (bucket < 2080)      return 10000.0 + (bucket - 1090) * 1000.0;
    return 1000000.0; // overflow bucket
}

// Per-thread stats. Cache-line aligned to prevent false sharing.
struct alignas(64) ThreadStats {
    uint64_t queries_sent = 0;
    uint64_t responses_received = 0;
    uint64_t rcode_noerror = 0;
    uint64_t rcode_nxdomain = 0;
    uint64_t rcode_servfail = 0;
    uint64_t rcode_other = 0;
    uint64_t timeouts = 0; // late responses (RTT > query_timeout)

    uint64_t rtt_min_ns = UINT64_MAX;
    uint64_t rtt_max_ns = 0;
    uint64_t rtt_sum_ns = 0;

    uint32_t rtt_histogram[HISTOGRAM_BUCKETS];

    ThreadStats() {
        std::memset(rtt_histogram, 0, sizeof(rtt_histogram));
    }

    void record_rtt(uint64_t rtt_ns) {
        rtt_sum_ns += rtt_ns;
        if (rtt_ns < rtt_min_ns) rtt_min_ns = rtt_ns;
        if (rtt_ns > rtt_max_ns) rtt_max_ns = rtt_ns;
        uint64_t rtt_us = rtt_ns / 1000;
        rtt_histogram[rtt_to_bucket(rtt_us)]++;
    }

    void record_rcode(uint8_t rcode) {
        responses_received++;
        switch (rcode) {
            case 0: rcode_noerror++;  break;
            case 2: rcode_servfail++; break;
            case 3: rcode_nxdomain++; break;
            default: rcode_other++;   break;
        }
    }
};

inline void aggregate_and_report(std::vector<ThreadStats>& per_thread,
                                 std::vector<std::vector<PerSocketTracker>>& trackers,
                                 double duration_s)
{
    // Count unreplied queries still in tracker rings
    uint64_t ring_timeouts = 0;
    for (auto& thread_trackers : trackers) {
        for (auto& tracker : thread_trackers) {
            for (int i = 0; i < 65536; i++) {
                if (tracker.ring[i].send_timestamp_ns != 0)
                    ring_timeouts++;
            }
        }
    }

    // Merge per-thread stats
    uint64_t total_sent = 0, total_recv = 0;
    uint64_t noerror = 0, nxdomain = 0, servfail = 0, other = 0;
    uint64_t late_timeouts = 0;
    uint64_t rtt_min = UINT64_MAX, rtt_max = 0, rtt_sum = 0;
    uint32_t merged_hist[HISTOGRAM_BUCKETS] = {};

    for (auto& s : per_thread) {
        total_sent    += s.queries_sent;
        total_recv    += s.responses_received;
        noerror       += s.rcode_noerror;
        nxdomain      += s.rcode_nxdomain;
        servfail      += s.rcode_servfail;
        other         += s.rcode_other;
        late_timeouts += s.timeouts;
        if (s.rtt_min_ns < rtt_min) rtt_min = s.rtt_min_ns;
        if (s.rtt_max_ns > rtt_max) rtt_max = s.rtt_max_ns;
        rtt_sum += s.rtt_sum_ns;

        for (int b = 0; b < HISTOGRAM_BUCKETS; b++)
            merged_hist[b] += s.rtt_histogram[b];
    }

    // Compute percentiles from merged histogram
    uint64_t total_samples = total_recv;
    uint64_t p50_target = total_samples * 50 / 100;
    uint64_t p90_target = total_samples * 90 / 100;
    uint64_t p99_target = total_samples * 99 / 100;

    uint64_t cumulative = 0;
    double p50 = 0, p90 = 0, p99 = 0;
    for (int b = 0; b < HISTOGRAM_BUCKETS; b++) {
        cumulative += merged_hist[b];
        double bucket_us = bucket_to_rtt_us(b);
        if (p50 == 0 && cumulative >= p50_target) p50 = bucket_us;
        if (p90 == 0 && cumulative >= p90_target) p90 = bucket_us;
        if (p99 == 0 && cumulative >= p99_target) p99 = bucket_us;
    }

    double avg_rtt_us = total_recv > 0 ? (double)rtt_sum / (double)total_recv / 1000.0 : 0;
    double achieved_qps = duration_s > 0 ? (double)total_sent / duration_s : 0;
    double answers_per_sec = duration_s > 0 ? (double)(noerror + nxdomain) / duration_s : 0;
    uint64_t total_timeouts = ring_timeouts + late_timeouts;

    std::printf("\n=== DNS Load Test Results ===\n");
    std::printf("Duration:            %.2f s\n", duration_s);
    std::printf("Queries sent:        %lu\n", total_sent);
    std::printf("Responses received:  %lu\n", total_recv);
    std::printf("Timeouts:            %lu (no reply: %lu, late: %lu)\n",
                total_timeouts, ring_timeouts, late_timeouts);
    std::printf("\nResponse codes:\n");
    std::printf("  NOERROR:           %lu\n", noerror);
    std::printf("  NXDOMAIN:          %lu\n", nxdomain);
    std::printf("  SERVFAIL:          %lu\n", servfail);
    std::printf("  Other:             %lu\n", other);
    std::printf("\nRTT statistics:\n");
    if (total_recv > 0) {
        std::printf("  Min:               %.2f us\n", (double)rtt_min / 1000.0);
        std::printf("  Max:               %.2f us\n", (double)rtt_max / 1000.0);
        std::printf("  Avg:               %.2f us\n", avg_rtt_us);
        std::printf("  P50:               %.2f us\n", p50);
        std::printf("  P90:               %.2f us\n", p90);
        std::printf("  P99:               %.2f us\n", p99);
    } else {
        std::printf("  (no responses received)\n");
    }
    std::printf("\nThroughput:\n");
    std::printf("  Achieved QPS:      %.0f\n", achieved_qps);
    std::printf("  Answers/sec:       %.0f\n", answers_per_sec);
}
