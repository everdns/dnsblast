#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// Pre-encoded DNS query ready for wire transmission.
// Transaction ID at wire[0..1] is a placeholder — overwritten per send.
struct PrebuiltQuery {
    uint8_t  wire[512];
    uint16_t wire_len;
};

// Encode a DNS query into wire format. Returns wire length, or 0 on error.
uint16_t encode_dns_query(uint8_t* buf, const char* fqdn, uint16_t qtype);

// Map a type string ("A", "AAAA", "MX", etc.) to DNS QTYPE value.
uint16_t dns_type_from_string(const char* type_str);

// Load queries from file. Each line: "<FQDN> <TYPE>"
std::vector<PrebuiltQuery> load_queries(const char* filename);

// Extract RCODE from a DNS response header.
inline uint8_t dns_rcode(const uint8_t* resp) {
    return resp[3] & 0x0F;
}

// Extract transaction ID from DNS header.
inline uint16_t dns_txid(const uint8_t* pkt) {
    return (uint16_t)(pkt[0] << 8) | pkt[1];
}
