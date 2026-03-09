#include "dns.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

uint16_t dns_type_from_string(const char* s) {
    if (strcasecmp(s, "A")     == 0) return 1;
    if (strcasecmp(s, "NS")    == 0) return 2;
    if (strcasecmp(s, "CNAME") == 0) return 5;
    if (strcasecmp(s, "SOA")   == 0) return 6;
    if (strcasecmp(s, "PTR")   == 0) return 12;
    if (strcasecmp(s, "MX")    == 0) return 15;
    if (strcasecmp(s, "TXT")   == 0) return 16;
    if (strcasecmp(s, "AAAA")  == 0) return 28;
    if (strcasecmp(s, "SRV")   == 0) return 33;
    if (strcasecmp(s, "HTTPS") == 0) return 65;
    if (strcasecmp(s, "ANY")   == 0) return 255;
    // Try numeric
    char* end;
    unsigned long v = strtoul(s, &end, 10);
    if (end != s && *end == '\0' && v <= 65535) return (uint16_t)v;
    return 0;
}

// Encode QNAME: "www.example.com." -> \3www\7example\3com\0
// Returns bytes written (including terminal \0 label).
static uint16_t encode_qname(uint8_t* buf, const char* fqdn) {
    uint16_t offset = 0;
    const char* p = fqdn;

    while (*p) {
        // Skip trailing dot
        if (*p == '.' && *(p + 1) == '\0') break;

        const char* dot = strchr(p, '.');
        size_t label_len;
        if (dot) {
            label_len = (size_t)(dot - p);
        } else {
            label_len = strlen(p);
        }

        if (label_len == 0 || label_len > 63) return 0; // invalid
        buf[offset++] = (uint8_t)label_len;
        memcpy(buf + offset, p, label_len);
        offset += (uint16_t)label_len;

        p += label_len;
        if (*p == '.') p++;
    }

    buf[offset++] = 0; // root label
    return offset;
}

static inline void write_u16be(uint8_t* buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
}

uint16_t encode_dns_query(uint8_t* buf, const char* fqdn, uint16_t qtype) {
    // Header: 12 bytes
    //   ID=0 (placeholder), FLAGS=0x0100 (RD=1), QDCOUNT=1, AN=0, NS=0, AR=0
    memset(buf, 0, 12);
    buf[2] = 0x01; // RD flag
    buf[5] = 0x01; // QDCOUNT = 1

    uint16_t offset = 12;

    // QNAME
    uint16_t qname_len = encode_qname(buf + offset, fqdn);
    if (qname_len == 0) return 0;
    offset += qname_len;

    // QTYPE
    write_u16be(buf + offset, qtype);
    offset += 2;

    // QCLASS = IN (1)
    write_u16be(buf + offset, 1);
    offset += 2;

    return offset;
}

std::vector<PrebuiltQuery> load_queries(const char* filename) {
    std::vector<PrebuiltQuery> queries;

    FILE* f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open query file '%s'\n", filename);
        exit(1);
    }

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        // Strip newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') continue;

        char fqdn[512], type_str[32];
        if (sscanf(line, "%511s %31s", fqdn, type_str) != 2) {
            fprintf(stderr, "Warning: skipping malformed line %d\n", lineno);
            continue;
        }

        uint16_t qtype = dns_type_from_string(type_str);
        if (qtype == 0) {
            fprintf(stderr, "Warning: unknown query type '%s' on line %d\n", type_str, lineno);
            continue;
        }

        PrebuiltQuery q;
        q.wire_len = encode_dns_query(q.wire, fqdn, qtype);
        if (q.wire_len == 0) {
            fprintf(stderr, "Warning: failed to encode query on line %d\n", lineno);
            continue;
        }

        queries.push_back(q);
    }

    fclose(f);

    if (queries.empty()) {
        fprintf(stderr, "Error: no valid queries loaded from '%s'\n", filename);
        exit(1);
    }

    fprintf(stderr, "Loaded %zu queries from '%s'\n", queries.size(), filename);
    return queries;
}
