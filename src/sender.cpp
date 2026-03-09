#include "sender.h"
#include "utils.h"

#include <cerrno>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/net_tstamp.h>

void sender_loop(int thread_id,
                 std::vector<int>& sockets,
                 const std::vector<EncodedQuery>& queries,
                 std::vector<PerSocketTracker>& trackers,
                 ThreadStats& stats,
                 std::atomic<bool>& stop_flag,
                 uint64_t my_query_limit,
                 const Config& cfg,
                 uint64_t interval_ns,
                 uint64_t tai_start_ns)
{
    pin_to_cpu(thread_id * 2);

    static constexpr int BATCH_SIZE = 64;
    const int num_sockets = static_cast<int>(sockets.size());
    const int num_queries = static_cast<int>(queries.size());

    // Pre-allocate all sendmmsg structures on the stack
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovecs[BATCH_SIZE];
    alignas(8) char cmsg_buf[BATCH_SIZE][CMSG_SPACE(sizeof(uint64_t))];
    uint8_t pkt_bufs[BATCH_SIZE][512];

    // Starting query index for this thread (spread across query set)
    uint64_t query_idx = static_cast<uint64_t>(thread_id) * num_queries / cfg.num_threads;
    uint64_t pkt_seq = 0;
    uint64_t thread_sent = 0;
    int sock_rr = 0;

    while (!stop_flag.load(std::memory_order_relaxed)) {
        if (thread_sent >= my_query_limit) break;

        int batch = static_cast<int>(
            std::min(static_cast<uint64_t>(BATCH_SIZE), my_query_limit - thread_sent));

        int cur_sock = sock_rr % num_sockets;
        int fd = sockets[cur_sock];
        auto& tracker = trackers[cur_sock];

        uint64_t now_ns = mono_ns();

        for (int b = 0; b < batch; b++) {
            uint16_t txid = tracker.alloc_txid();
            const auto& eq = queries[query_idx % num_queries];

            // Copy pre-encoded packet and stamp transaction ID
            std::memcpy(pkt_bufs[b], eq.wire, eq.wire_len);
            uint16_t net_txid = htons(txid);
            std::memcpy(pkt_bufs[b], &net_txid, 2);

            // Record send timestamp for RTT tracking
            tracker.ring[txid].send_timestamp_ns = now_ns;

            // iovec
            iovecs[b].iov_base = pkt_bufs[b];
            iovecs[b].iov_len = eq.wire_len;

            // msghdr (connected socket — no destination address needed)
            std::memset(&msgs[b].msg_hdr, 0, sizeof(struct msghdr));
            msgs[b].msg_hdr.msg_iov = &iovecs[b];
            msgs[b].msg_hdr.msg_iovlen = 1;

            // SCM_TXTIME control message
            msgs[b].msg_hdr.msg_control = cmsg_buf[b];
            msgs[b].msg_hdr.msg_controllen = CMSG_SPACE(sizeof(uint64_t));

            struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msgs[b].msg_hdr);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_TXTIME;
            cmsg->cmsg_len = CMSG_LEN(sizeof(uint64_t));

            uint64_t txtime = tai_start_ns + (pkt_seq + b) * interval_ns;
            std::memcpy(CMSG_DATA(cmsg), &txtime, sizeof(uint64_t));

            query_idx++;
        }

        int sent = sendmmsg(fd, msgs, batch, 0);
        if (sent > 0) {
            stats.queries_sent += sent;
            thread_sent += sent;
            pkt_seq += sent;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Kernel qdisc buffer full — yield briefly
            sched_yield();
        }

        sock_rr++;
    }
}
