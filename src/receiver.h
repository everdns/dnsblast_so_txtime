#pragma once

#include <atomic>
#include <vector>
#include "config.h"
#include "tracker.h"
#include "stats.h"

void receiver_loop(int thread_id,
                   std::vector<int>& sockets,
                   std::vector<PerSocketTracker>& trackers,
                   ThreadStats& stats,
                   std::atomic<bool>& stop_flag,
                   const Config& cfg);
