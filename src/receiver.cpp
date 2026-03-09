#include "receiver.h"
#include "utils.h"

#include <cstring>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

void receiver_loop(int thread_id,
                   std::vector<int>& sockets,
                   std::vector<PerSocketTracker>& trackers,
                   ThreadStats& stats,
                   std::atomic<bool>& stop_flag,
                   const Config& cfg)
{
    pin_to_cpu(thread_id * 2 + 1);

    int epfd = epoll_create1(0);
    if (epfd < 0) return;

    for (int i = 0; i < static_cast<int>(sockets.size()); i++) {
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u32 = static_cast<uint32_t>(i);
        epoll_ctl(epfd, EPOLL_CTL_ADD, sockets[i], &ev);
    }

    static constexpr int MAX_EVENTS = 32;
    static constexpr int RECV_BATCH = 64;

    struct epoll_event events[MAX_EVENTS];
    struct mmsghdr msgs[RECV_BATCH];
    struct iovec iovecs[RECV_BATCH];
    uint8_t recv_bufs[RECV_BATCH][512];

    // Pre-init iovec/mmsghdr structures
    for (int i = 0; i < RECV_BATCH; i++) {
        iovecs[i].iov_base = recv_bufs[i];
        iovecs[i].iov_len = 512;
        std::memset(&msgs[i].msg_hdr, 0, sizeof(struct msghdr));
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    const uint64_t timeout_ns = static_cast<uint64_t>(cfg.query_timeout_ms) * 1'000'000ULL;

    while (!stop_flag.load(std::memory_order_relaxed)) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, 10 /* ms */);
        if (nfds <= 0) continue;

        uint64_t now_ns = tai_ns();

        for (int e = 0; e < nfds; e++) {
            int sock_idx = static_cast<int>(events[e].data.u32);
            int fd = sockets[sock_idx];
            auto& tracker = trackers[sock_idx];

            int received = recvmmsg(fd, msgs, RECV_BATCH, MSG_DONTWAIT, nullptr);

            for (int r = 0; r < received; r++) {
                int len = static_cast<int>(msgs[r].msg_len);
                if (len < 12) continue; // Too short for DNS header

                uint8_t* pkt = recv_bufs[r];

                // Parse DNS header
                uint16_t txid = ntohs(*reinterpret_cast<uint16_t*>(pkt));
                uint16_t flags = ntohs(*reinterpret_cast<uint16_t*>(pkt + 2));
                uint8_t rcode = flags & 0x0F;

                // RTT calculation
                uint64_t send_ts = tracker.ring[txid].send_timestamp_ns;
                if (send_ts == 0) continue; // Unknown txid or already processed

                uint64_t rtt_ns = now_ns - send_ts;
                tracker.ring[txid].send_timestamp_ns = 0; // Mark as received

                // Check timeout
                if (rtt_ns > timeout_ns) {
                    stats.timeouts++;
                    continue;
                }

                // Record RTT and response code
                stats.record_rtt(rtt_ns);
                stats.record_rcode(rcode);
            }
        }
    }

    close(epfd);
}
