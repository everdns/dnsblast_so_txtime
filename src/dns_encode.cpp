#include "dns_encode.h"

#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

uint16_t dns_type_from_string(const std::string& type) {
    static const std::unordered_map<std::string, uint16_t> types = {
        {"A",     1},   {"NS",    2},   {"CNAME", 5},   {"SOA",   6},
        {"PTR",  12},   {"MX",   15},   {"TXT",  16},   {"AAAA", 28},
        {"SRV",  33},   {"NAPTR",35},   {"DS",   43},   {"DNSKEY",48},
        {"HTTPS",65},   {"ANY", 255},
    };
    auto it = types.find(type);
    if (it == types.end())
        throw std::runtime_error("Unknown DNS type: " + type);
    return it->second;
}

std::vector<QuerySpec> load_query_file(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open())
        throw std::runtime_error("Cannot open query file: " + path);

    std::vector<QuerySpec> queries;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        QuerySpec q;
        if (!(ss >> q.fqdn >> q.type)) continue;
        queries.push_back(std::move(q));
    }
    if (queries.empty())
        throw std::runtime_error("Query file is empty or has no valid entries");
    return queries;
}

// Encode FQDN to DNS wire-format QNAME.
// "example.com." -> \x07example\x03com\x00
// Returns number of bytes written.
static size_t encode_qname(const std::string& fqdn, uint8_t* out) {
    size_t pos = 0;
    size_t label_start = 0;

    for (size_t i = 0; i <= fqdn.size(); i++) {
        if (i == fqdn.size() || fqdn[i] == '.') {
            size_t label_len = i - label_start;
            if (label_len == 0) {
                // Trailing dot or empty label — write terminator if at end
                if (i == fqdn.size()) {
                    out[pos++] = 0;
                }
                label_start = i + 1;
                continue;
            }
            if (label_len > 63)
                throw std::runtime_error("DNS label exceeds 63 chars: " + fqdn);
            out[pos++] = static_cast<uint8_t>(label_len);
            std::memcpy(out + pos, fqdn.data() + label_start, label_len);
            pos += label_len;
            label_start = i + 1;
        }
    }

    // If FQDN didn't end with '.', add the root label terminator
    if (!fqdn.empty() && fqdn.back() != '.') {
        out[pos++] = 0;
    }

    return pos;
}

std::vector<EncodedQuery> pre_encode_dns_packets(const std::vector<QuerySpec>& queries) {
    std::vector<EncodedQuery> encoded;
    encoded.reserve(queries.size());

    for (const auto& q : queries) {
        EncodedQuery eq;
        std::memset(eq.wire, 0, sizeof(eq.wire));

        // DNS Header (12 bytes):
        // Bytes 0-1: Transaction ID = 0x0000 (placeholder, stamped at send time)
        // Bytes 2-3: Flags = 0x0100 (standard query, RD=1)
        // Bytes 4-5: QDCOUNT = 1
        // Bytes 6-11: ANCOUNT, NSCOUNT, ARCOUNT = 0
        uint16_t flags = htons(0x0100);
        uint16_t qdcount = htons(1);
        std::memcpy(eq.wire + 2, &flags, 2);
        std::memcpy(eq.wire + 4, &qdcount, 2);

        size_t off = 12;

        // QNAME
        off += encode_qname(q.fqdn, eq.wire + off);

        // QTYPE
        uint16_t qtype = htons(dns_type_from_string(q.type));
        std::memcpy(eq.wire + off, &qtype, 2);
        off += 2;

        // QCLASS = IN (1)
        uint16_t qclass = htons(1);
        std::memcpy(eq.wire + off, &qclass, 2);
        off += 2;

        eq.wire_len = static_cast<uint16_t>(off);
        encoded.push_back(eq);
    }

    return encoded;
}
