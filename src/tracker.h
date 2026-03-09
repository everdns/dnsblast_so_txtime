#pragma once

#include <cstdint>
#include <cstring>

struct PendingQuery {
    uint64_t send_timestamp_ns; // CLOCK_MONOTONIC, 0 = slot free
};

// Per-socket ring buffer indexed by DNS transaction ID (0..65535).
// Single-writer (sender thread), single-reader (receiver thread).
struct PerSocketTracker {
    PendingQuery ring[65536];
    uint16_t next_txid;

    PerSocketTracker() : next_txid(0) {
        std::memset(ring, 0, sizeof(ring));
    }

    uint16_t alloc_txid() {
        return next_txid++;
    }
};
