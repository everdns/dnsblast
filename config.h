#pragma once

#include <cstdint>
#include <string>

struct Config {
    std::string server_ip;
    uint16_t    server_port    = 53;
    std::string input_file;
    uint64_t    target_qps     = 100000;
    uint64_t    total_queries  = 0;       // 0 = unlimited (use runtime)
    double      runtime_sec    = 10.0;
    double      timeout_sec    = 2.0;
    int         num_threads    = 4;
    int         num_ports      = 4;       // total across all threads
    bool        ipv6           = false;
};

Config parse_args(int argc, char** argv);
void   print_usage(const char* progname);
