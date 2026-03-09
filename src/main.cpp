#include "config.h"
#include "dns_encode.h"
#include "sender.h"
#include "receiver.h"
#include "stats.h"
#include "tracker.h"
#include "utils.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <netdb.h>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/net_tstamp.h>
#include <unistd.h>

static std::atomic<bool> g_stop_flag{false};

static void sigint_handler(int) {
    g_stop_flag.store(true, std::memory_order_relaxed);
}

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  -s, --server <addr>       DNS server IP address (required)\n"
        "  -p, --port <port>         DNS server port (default: 53)\n"
        "  -q, --qps <rate>          Target queries per second (default: 1000)\n"
        "  -n, --num-queries <n>     Total queries to send (0 = unlimited)\n"
        "  -t, --timeout <ms>        Query timeout in milliseconds (default: 5000)\n"
        "  -d, --duration <sec>      Total test duration in seconds (default: 10)\n"
        "  -T, --threads <n>         Number of sender threads (default: 16)\n"
        "  -P, --ports <n>           Ports per sender thread (default: 8)\n"
        "  -f, --file <path>         Query input file (required)\n"
        "  -6, --ipv6                Use IPv6\n"
        "  -h, --help                Show this help\n",
        prog);
}

Config parse_args(int argc, char** argv) {
    Config cfg;

    static struct option long_opts[] = {
        {"server",      required_argument, nullptr, 's'},
        {"port",        required_argument, nullptr, 'p'},
        {"qps",         required_argument, nullptr, 'q'},
        {"num-queries", required_argument, nullptr, 'n'},
        {"timeout",     required_argument, nullptr, 't'},
        {"duration",    required_argument, nullptr, 'd'},
        {"threads",     required_argument, nullptr, 'T'},
        {"ports",       required_argument, nullptr, 'P'},
        {"file",        required_argument, nullptr, 'f'},
        {"ipv6",        no_argument,       nullptr, '6'},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:p:q:n:t:d:T:P:f:6h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 's': cfg.server_addr_str = optarg; break;
            case 'p': cfg.server_port = static_cast<uint16_t>(std::atoi(optarg)); break;
            case 'q': cfg.target_qps = std::strtoull(optarg, nullptr, 10); break;
            case 'n': cfg.total_queries = std::strtoull(optarg, nullptr, 10); break;
            case 't': cfg.query_timeout_ms = static_cast<uint32_t>(std::atoi(optarg)); break;
            case 'd': cfg.total_runtime_s = static_cast<uint32_t>(std::atoi(optarg)); break;
            case 'T': cfg.num_threads = std::atoi(optarg); break;
            case 'P': cfg.ports_per_thread = std::atoi(optarg); break;
            case 'f': cfg.query_file = optarg; break;
            case '6': cfg.address_family = AF_INET6; break;
            case 'h': usage(argv[0]); std::exit(0);
            default:  usage(argv[0]); std::exit(1);
        }
    }

    if (cfg.server_addr_str.empty()) {
        std::fprintf(stderr, "Error: --server is required\n");
        usage(argv[0]);
        std::exit(1);
    }
    if (cfg.query_file.empty()) {
        std::fprintf(stderr, "Error: --file is required\n");
        usage(argv[0]);
        std::exit(1);
    }
    if (cfg.num_threads < 1) {
        std::fprintf(stderr, "Error: --threads must be >= 1\n");
        std::exit(1);
    }
    if (cfg.ports_per_thread < 1) {
        std::fprintf(stderr, "Error: --ports must be >= 1\n");
        std::exit(1);
    }

    // Resolve server address
    if (cfg.address_family == AF_INET) {
        auto* sa = reinterpret_cast<struct sockaddr_in*>(&cfg.server_addr);
        sa->sin_family = AF_INET;
        sa->sin_port = htons(cfg.server_port);
        if (inet_pton(AF_INET, cfg.server_addr_str.c_str(), &sa->sin_addr) != 1) {
            std::fprintf(stderr, "Error: invalid IPv4 address: %s\n",
                         cfg.server_addr_str.c_str());
            std::exit(1);
        }
        cfg.server_addr_len = sizeof(struct sockaddr_in);
    } else {
        auto* sa = reinterpret_cast<struct sockaddr_in6*>(&cfg.server_addr);
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons(cfg.server_port);
        if (inet_pton(AF_INET6, cfg.server_addr_str.c_str(), &sa->sin6_addr) != 1) {
            std::fprintf(stderr, "Error: invalid IPv6 address: %s\n",
                         cfg.server_addr_str.c_str());
            std::exit(1);
        }
        cfg.server_addr_len = sizeof(struct sockaddr_in6);
    }

    // Compute derived values
    cfg.per_thread_qps = cfg.target_qps / cfg.num_threads;
    if (cfg.per_thread_qps == 0) cfg.per_thread_qps = 1;
    cfg.interval_ns = 1'000'000'000ULL / cfg.per_thread_qps;

    return cfg;
}

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);

    std::signal(SIGINT, sigint_handler);
    std::signal(SIGTERM, sigint_handler);

    // 1. Load and pre-encode queries
    std::printf("Loading queries from %s...\n", cfg.query_file.c_str());
    auto query_specs = load_query_file(cfg.query_file);
    auto encoded = pre_encode_dns_packets(query_specs);
    std::printf("Loaded %zu queries\n", encoded.size());

    // 2. Create sockets: num_threads × ports_per_thread
    std::vector<std::vector<int>> thread_sockets(cfg.num_threads);
    for (int t = 0; t < cfg.num_threads; t++) {
        for (int p = 0; p < cfg.ports_per_thread; p++) {
            int fd = socket(cfg.address_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
            if (fd < 0) {
                std::perror("socket");
                std::exit(1);
            }

            // Bind to ephemeral port
            if (cfg.address_family == AF_INET) {
                struct sockaddr_in bind_addr{};
                bind_addr.sin_family = AF_INET;
                bind_addr.sin_addr.s_addr = INADDR_ANY;
                bind_addr.sin_port = 0;
                if (bind(fd, reinterpret_cast<struct sockaddr*>(&bind_addr),
                         sizeof(bind_addr)) < 0) {
                    std::perror("bind");
                    std::exit(1);
                }
            } else {
                struct sockaddr_in6 bind_addr{};
                bind_addr.sin6_family = AF_INET6;
                bind_addr.sin6_addr = in6addr_any;
                bind_addr.sin6_port = 0;
                if (bind(fd, reinterpret_cast<struct sockaddr*>(&bind_addr),
                         sizeof(bind_addr)) < 0) {
                    std::perror("bind");
                    std::exit(1);
                }
            }

            // Connected UDP — kernel caches route, faster sendmsg
            if (connect(fd, reinterpret_cast<struct sockaddr*>(&cfg.server_addr),
                        cfg.server_addr_len) < 0) {
                std::perror("connect");
                std::exit(1);
            }

            // Set socket priority so prio qdisc routes to band 0 → ETF child
            // priomap index 6 maps to band 0 in the default 3-band prio qdisc
            int prio = 6;
            if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio)) < 0) {
                std::perror("setsockopt SO_PRIORITY");
                std::exit(1);
            }

            // Enable SO_TXTIME
            struct sock_txtime txcfg{};
            txcfg.clockid = CLOCK_TAI;
            txcfg.flags = 0;
            if (setsockopt(fd, SOL_SOCKET, SO_TXTIME, &txcfg, sizeof(txcfg)) < 0) {
                std::perror("setsockopt SO_TXTIME");
                std::fprintf(stderr,
                    "Hint: ensure ETF qdisc is configured on the egress interface:\n"
                    "  tc qdisc add dev <iface> root handle 1: prio bands 3\n"
                    "  tc qdisc add dev <iface> parent 1:1 handle 10: etf "
                    "clockid CLOCK_TAI delta 500000 offload\n");
                std::exit(1);
            }

            // Increase socket buffer sizes
            int buf = 4 * 1024 * 1024;
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

            thread_sockets[t].push_back(fd);
        }
    }
    std::printf("Created %d sockets (%d threads x %d ports)\n",
                cfg.num_threads * cfg.ports_per_thread,
                cfg.num_threads, cfg.ports_per_thread);

    // 3. Allocate per-thread stats and trackers
    std::vector<ThreadStats> stats(cfg.num_threads);
    std::vector<std::vector<PerSocketTracker>> trackers(cfg.num_threads);
    for (int t = 0; t < cfg.num_threads; t++) {
        trackers[t].resize(cfg.ports_per_thread);
    }

    // 4. Compute per-thread query limits
    //    If total_queries == 0, use UINT64_MAX (effectively unlimited)
    uint64_t effective_total = cfg.total_queries > 0 ? cfg.total_queries : UINT64_MAX;
    std::vector<uint64_t> per_thread_limit(cfg.num_threads);
    uint64_t base_limit = effective_total / cfg.num_threads;
    uint64_t remainder = effective_total % cfg.num_threads;
    for (int t = 0; t < cfg.num_threads; t++) {
        per_thread_limit[t] = base_limit + (t < static_cast<int>(remainder) ? 1 : 0);
    }

    // 5. Spawn receiver threads first
    std::vector<std::thread> receivers;
    for (int t = 0; t < cfg.num_threads; t++) {
        receivers.emplace_back(receiver_loop, t,
                               std::ref(thread_sockets[t]),
                               std::ref(trackers[t]),
                               std::ref(stats[t]),
                               std::ref(g_stop_flag),
                               std::cref(cfg));
    }

    // 6. Compute start time — 200ms in the future (CLOCK_TAI)
    uint64_t tai_base = tai_ns() + 200'000'000ULL;

    // TAI deadline: senders will not schedule packets beyond this time
    uint64_t tai_deadline = tai_base +
        static_cast<uint64_t>(cfg.total_runtime_s) * 1'000'000'000ULL;

    // 7. Print test parameters
    std::printf("\n--- Starting DNS load test ---\n");
    std::printf("Server:              %s:%u\n", cfg.server_addr_str.c_str(), cfg.server_port);
    std::printf("Target QPS:          %lu\n", cfg.target_qps);
    std::printf("Total queries:       %s\n",
                cfg.total_queries > 0 ? std::to_string(cfg.total_queries).c_str() : "unlimited");
    std::printf("Duration:            %u s\n", cfg.total_runtime_s);
    std::printf("Timeout:             %u ms\n", cfg.query_timeout_ms);
    std::printf("Threads:             %d (x%d ports = %d sockets)\n",
                cfg.num_threads, cfg.ports_per_thread,
                cfg.num_threads * cfg.ports_per_thread);
    std::printf("Per-thread QPS:      %lu\n", cfg.per_thread_qps);
    std::printf("Interval:            %lu ns\n", cfg.interval_ns);
    std::printf("\n");

    // 8. Record wall-clock start time
    auto test_start = std::chrono::steady_clock::now();

    // 9. Spawn sender threads
    std::vector<std::thread> senders;
    for (int t = 0; t < cfg.num_threads; t++) {
        // Stagger thread start times to interleave packets across threads
        uint64_t thread_offset = t * (cfg.interval_ns / cfg.num_threads);
        senders.emplace_back(sender_loop, t,
                             std::ref(thread_sockets[t]),
                             std::cref(encoded),
                             std::ref(trackers[t]),
                             std::ref(stats[t]),
                             std::ref(g_stop_flag),
                             per_thread_limit[t],
                             std::cref(cfg),
                             cfg.interval_ns,
                             tai_base + thread_offset,
                             tai_deadline);
    }

    // 10. Join sender threads
    for (auto& s : senders) s.join();
    auto send_end = std::chrono::steady_clock::now();

    // 12. Compute remaining TX drain time
    //     Senders submit packets to the kernel instantly via SO_TXTIME, but
    //     the kernel ETF qdisc transmits them over the full scheduled window.
    //     We must wait until the last scheduled packet has actually been sent
    //     before starting the timeout drain.
    uint64_t total_sent = 0;
    for (int t = 0; t < cfg.num_threads; t++)
        total_sent += stats[t].queries_sent;

    // The last TX timestamp across all threads is approximately:
    //   tai_base + (total_sent / num_threads) * interval_ns
    // which equals tai_base + total_sent * (1e9 / target_qps) / num_threads
    // = tai_base + total_sent / target_qps seconds
    uint64_t last_txtime_ns = tai_base + (total_sent / cfg.num_threads) * cfg.interval_ns;
    uint64_t now_tai = tai_ns();
    uint64_t remaining_tx_ms = 0;
    if (last_txtime_ns > now_tai) {
        remaining_tx_ms = (last_txtime_ns - now_tai) / 1'000'000ULL + 1;
    }
    uint64_t drain_ms = remaining_tx_ms + cfg.query_timeout_ms + 100;
    std::printf("All senders finished. Waiting %lu ms for TX drain + %u ms timeout + 100 ms fudge...\n",
                remaining_tx_ms, cfg.query_timeout_ms);

    // Wait for kernel to finish transmitting + timeout for responses
    std::this_thread::sleep_for(std::chrono::milliseconds(drain_ms));
    g_stop_flag.store(true, std::memory_order_relaxed);

    // 13. Join receiver threads
    for (auto& r : receivers) r.join();

    // 14. Compute test duration (TX window, not just send-phase wall time)
    //     Use the actual scheduled TX window for accurate QPS reporting
    double duration_s = static_cast<double>((total_sent / cfg.num_threads) * cfg.interval_ns) / 1e9;
    if (duration_s < 0.001) // fallback to wall clock if too small
        duration_s = std::chrono::duration<double>(send_end - test_start).count();

    // 15. Aggregate and report
    aggregate_and_report(stats, trackers, duration_s);

    // 16. Close sockets
    for (auto& tvec : thread_sockets) {
        for (int fd : tvec) close(fd);
    }

    return 0;
}
