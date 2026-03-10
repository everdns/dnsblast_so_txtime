#include "sender.h"
#include "utils.h"

#include <cerrno>
#include <cstring>
#include <ctime>
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
                 uint64_t tai_start_ns,
                 uint64_t tai_deadline_ns)
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
    uint16_t batch_txids[BATCH_SIZE];

    while (!stop_flag.load(std::memory_order_relaxed)) {
        if (thread_sent >= my_query_limit) break;

        int batch = static_cast<int>(
            std::min(static_cast<uint64_t>(BATCH_SIZE), my_query_limit - thread_sent));

        int cur_sock = sock_rr % num_sockets;
        int fd = sockets[cur_sock];
        auto& tracker = trackers[cur_sock];

        // Check if the first packet in this batch would exceed the runtime deadline
        uint64_t first_txtime = tai_start_ns + pkt_seq * interval_ns;
        if (first_txtime >= tai_deadline_ns) break;

        // Pace submissions: sleep until we're within per-thread queue depth of real time
        if (cfg.per_thread_queue_depth > 0) {
            uint64_t max_lookahead_ns = cfg.per_thread_queue_depth * interval_ns;
            uint64_t earliest_submit = first_txtime > max_lookahead_ns
                ? first_txtime - max_lookahead_ns : 0;
            uint64_t now = tai_ns();
            if (now < earliest_submit) {
                struct timespec ts;
                ts.tv_sec  = static_cast<time_t>(earliest_submit / 1'000'000'000ULL);
                ts.tv_nsec = static_cast<long>(earliest_submit % 1'000'000'000ULL);
                clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &ts, nullptr);
                if (stop_flag.load(std::memory_order_relaxed)) break;
            }
        }

        // Clamp batch so no packet exceeds the deadline
        for (int b = 0; b < batch; b++) {
            uint64_t txtime = tai_start_ns + (pkt_seq + b) * interval_ns;
            if (txtime >= tai_deadline_ns) {
                batch = b;
                break;
            }
        }
        if (batch == 0) break;

        for (int b = 0; b < batch; b++) {
            uint16_t txid = tracker.alloc_txid();
            batch_txids[b] = txid;
            const auto& eq = queries[query_idx % num_queries];

            // Copy pre-encoded packet and stamp transaction ID
            std::memcpy(pkt_bufs[b], eq.wire, eq.wire_len);
            uint16_t net_txid = htons(txid);
            std::memcpy(pkt_bufs[b], &net_txid, 2);

            // Record scheduled TX time (TAI) for accurate RTT tracking
            uint64_t txtime = tai_start_ns + (pkt_seq + b) * interval_ns;
            tracker.ring[txid].send_timestamp_ns = txtime;

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

            std::memcpy(CMSG_DATA(cmsg), &txtime, sizeof(uint64_t));

            query_idx++;
        }

        int sent = sendmmsg(fd, msgs, batch, 0);
        if (sent > 0) {
            stats.queries_sent += sent;
            thread_sent += sent;
            pkt_seq += sent;
            // Clear orphaned ring entries for unsent packets in partial batch
            for (int b = sent; b < batch; b++)
                tracker.ring[batch_txids[b]].send_timestamp_ns = 0;
        } else {
            // All failed (EAGAIN / error) — clear entire batch's ring entries
            for (int b = 0; b < batch; b++)
                tracker.ring[batch_txids[b]].send_timestamp_ns = 0;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                sched_yield();
        }

        sock_rr++;
    }
}
