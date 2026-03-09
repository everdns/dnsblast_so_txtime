#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct QuerySpec {
    std::string fqdn; // e.g. "010-000-000-001.sld.lan."
    std::string type; // e.g. "A", "AAAA", "MX", "CNAME", "HTTPS"
};

struct EncodedQuery {
    uint8_t wire[512];
    uint16_t wire_len;
};

// Parse query file: each line is "<FQDN> <TYPE>"
std::vector<QuerySpec> load_query_file(const std::string& path);

// Convert all QuerySpec entries to wire-format DNS packets.
// Transaction ID is left as 0x0000 — stamped per-send.
std::vector<EncodedQuery> pre_encode_dns_packets(const std::vector<QuerySpec>& queries);

// Map type string to DNS QTYPE number.
uint16_t dns_type_from_string(const std::string& type);
