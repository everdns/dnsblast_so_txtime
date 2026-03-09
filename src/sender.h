#pragma once

#include <atomic>
#include <vector>
#include "config.h"
#include "dns_encode.h"
#include "tracker.h"
#include "stats.h"

void sender_loop(int thread_id,
                 std::vector<int>& sockets,
                 const std::vector<EncodedQuery>& queries,
                 std::vector<PerSocketTracker>& trackers,
                 ThreadStats& stats,
                 std::atomic<bool>& stop_flag,
                 uint64_t my_query_limit,
                 const Config& cfg,
                 uint64_t interval_ns,
                 uint64_t tai_start_ns);
