#pragma once

#include <cstdint>
#include <string>
#include <netinet/in.h>

struct Config {
    // Target performance
    uint64_t target_qps = 1000;
    uint64_t total_queries = 0;       // 0 = unlimited (stop by runtime only)
    uint32_t query_timeout_ms = 5000; // 5 seconds
    uint32_t total_runtime_s = 10;    // seconds

    // Network
    std::string server_addr_str;
    uint16_t server_port = 53;
    struct sockaddr_storage server_addr{};
    socklen_t server_addr_len = 0;
    int address_family = AF_INET;     // AF_INET or AF_INET6

    // Threading
    int num_threads = 16;
    int ports_per_thread = 8;

    // Input
    std::string query_file;

    // Derived (computed after parsing)
    uint64_t per_thread_qps = 0;
    uint64_t interval_ns = 0;
};

Config parse_args(int argc, char** argv);
