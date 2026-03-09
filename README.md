# dnsblast — High-Performance DNS Load Testing Tool

A multithreaded DNS query generator designed to sustain millions of queries per second using `SO_TXTIME` for kernel-level packet pacing.

## Prerequisites

### System Requirements
- Linux kernel 4.19+ (for `SO_TXTIME` / ETF qdisc support)
- NIC with hardware timestamping support (for `offload` mode; software mode works without)
- g++ 9+ with C++17 support
- Root or `CAP_NET_ADMIN` for qdisc configuration

### Build Dependencies
- g++ (tested with 13.3.0)
- pthread
- Standard Linux headers (`linux/net_tstamp.h`, `sys/epoll.h`)

### Network Configuration

The ETF (Earliest TxTime First) qdisc must be configured on the egress interface before running. This is what enables `SO_TXTIME` pacing.

```bash
# Replace eth0 with your egress interface name
IFACE=eth0

# Add a prio qdisc as root, then attach ETF to band 1
sudo tc qdisc add dev $IFACE root handle 1: prio bands 3
sudo tc qdisc add dev $IFACE parent 1:1 handle 10: etf \
    clockid CLOCK_TAI delta 500000 offload

# Verify
tc -s qdisc show dev $IFACE
```

**Parameters:**
- `clockid CLOCK_TAI` — required clock source for SO_TXTIME
- `delta 500000` — 500us lead time for NIC scheduling (tune based on your hardware)
- `offload` — use NIC hardware offload if available; remove if your NIC doesn't support it

To remove the qdisc later:
```bash
sudo tc qdisc del dev $IFACE root
```

### Socket Buffer Tuning (recommended)

For high QPS, increase the kernel socket buffer limits:

```bash
sudo sysctl -w net.core.wmem_max=8388608
sudo sysctl -w net.core.rmem_max=8388608
sudo sysctl -w net.core.wmem_default=4194304
sudo sysctl -w net.core.rmem_default=4194304
```

## Building

Using make:
```bash
make
```

Using cmake:
```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Using g++ directly:
```bash
g++ -std=c++17 -O2 -Wall -Wextra -march=native -D_GNU_SOURCE \
    -I src -o dnsblast \
    src/main.cpp src/dns_encode.cpp src/sender.cpp src/receiver.cpp \
    -lpthread
```

## Usage

```
./dnsblast [options]

Required:
  -s, --server <addr>       DNS server IP address
  -f, --file <path>         Query input file

Optional:
  -p, --port <port>         DNS server port (default: 53)
  -q, --qps <rate>          Target queries per second (default: 1000)
  -n, --num-queries <n>     Total queries to send (0 = unlimited, default: 0)
  -t, --timeout <ms>        Query timeout in milliseconds (default: 5000)
  -d, --duration <sec>      Total test duration in seconds (default: 10)
  -T, --threads <n>         Number of sender threads (default: 16)
  -P, --ports <n>           Sending ports per thread (default: 8)
  -6, --ipv6                Use IPv6
  -h, --help                Show help
```

### Stop Conditions

The test stops when either:
1. `--num-queries` total queries have been sent, **or**
2. `--duration` seconds have elapsed

whichever comes first. The total query count is never exceeded.

## Query Input File Format

One query per line: `<FQDN> <TYPE>`

```
010-000-000-001.sld.lan. A
010-000-000-002.sld.lan. MX
000-000-000-002.sld.lan. AAAA
013-000-000-032.sld.lan. CNAME
013-030-000-002.sld.lan. HTTPS
```

Supported query types: `A`, `AAAA`, `MX`, `CNAME`, `NS`, `SOA`, `PTR`, `TXT`, `SRV`, `NAPTR`, `DS`, `DNSKEY`, `HTTPS`, `ANY`

Lines starting with `#` are ignored.

## Examples

Basic test at 1000 QPS for 10 seconds:
```bash
./dnsblast -s 192.168.1.1 -f queries.txt -q 1000 -d 10
```

High-rate test at 2M QPS with 16 threads, 8 ports each, send 10M queries:
```bash
./dnsblast -s 10.0.0.53 -f queries.txt \
    -q 2000000 -n 10000000 -d 30 \
    -T 16 -P 8 -t 3000
```

IPv6 target:
```bash
./dnsblast -s fd00::53 -f queries.txt -q 50000 -d 60 -6
```

## Output

After the test completes, a summary is printed:

```
=== DNS Load Test Results ===
Duration:            10.00 s
Queries sent:        10000000
Responses received:  9985432
Timeouts:            14568 (no reply: 14200, late: 368)

Response codes:
  NOERROR:           8500000
  NXDOMAIN:          1485432
  SERVFAIL:          0
  Other:             0

RTT statistics:
  Min:               42.00 us
  Max:               4521.00 us
  Avg:               185.30 us
  P50:               160.00 us
  P90:               310.00 us
  P99:               1200.00 us

Throughput:
  Achieved QPS:      1000000
  Answers/sec:       998543
```

**Definitions:**
- **Achieved QPS** = queries_sent / duration
- **Answers/sec** = (NOERROR + NXDOMAIN) / duration
- **Timeouts (no reply)** = queries that never received a response
- **Timeouts (late)** = responses received after the configured timeout

## Architecture Overview

- **Sender threads** build batches of 64 pre-encoded DNS packets and submit them via `sendmmsg()` with per-packet `SCM_TXTIME` timestamps for uniform pacing
- **Receiver threads** use `epoll` + `recvmmsg()` to batch-receive responses and compute RTT from per-socket tracking rings
- Each sender/receiver pair shares a set of connected UDP sockets but never contends (sender only sends, receiver only receives)
- Statistics are kept per-thread with no locking; merged after the test ends
- RTT percentiles are computed from a logarithmic histogram (2081 buckets, sub-microsecond to 1s range)
