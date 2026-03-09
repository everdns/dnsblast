#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>

void print_usage(const char* progname) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -s, --server <ip>        DNS server IP address (required)\n"
        "  -p, --port <port>        DNS server port (default: 53)\n"
        "  -f, --file <path>        Input query file (required)\n"
        "  -q, --qps <n>            Target queries per second (default: 100000)\n"
        "  -n, --count <n>          Total queries to send (0 = unlimited)\n"
        "  -d, --duration <sec>     Test duration in seconds (default: 10)\n"
        "  -t, --threads <n>        Number of sending threads (default: 4)\n"
        "  -P, --ports <n>          Number of sending ports (default: 4)\n"
        "  -T, --timeout <sec>      Query timeout in seconds (default: 2.0)\n"
        "  -6, --ipv6               Use IPv6\n"
        "  -h, --help               Show this help\n",
        progname);
}

Config parse_args(int argc, char** argv) {
    Config cfg;

    static struct option long_opts[] = {
        {"server",   required_argument, nullptr, 's'},
        {"port",     required_argument, nullptr, 'p'},
        {"file",     required_argument, nullptr, 'f'},
        {"qps",      required_argument, nullptr, 'q'},
        {"count",    required_argument, nullptr, 'n'},
        {"duration", required_argument, nullptr, 'd'},
        {"threads",  required_argument, nullptr, 't'},
        {"ports",    required_argument, nullptr, 'P'},
        {"timeout",  required_argument, nullptr, 'T'},
        {"ipv6",     no_argument,       nullptr, '6'},
        {"help",     no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:p:f:q:n:d:t:P:T:6h", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 's': cfg.server_ip   = optarg; break;
        case 'p': cfg.server_port = (uint16_t)atoi(optarg); break;
        case 'f': cfg.input_file  = optarg; break;
        case 'q': cfg.target_qps  = (uint64_t)strtoull(optarg, nullptr, 10); break;
        case 'n': cfg.total_queries = (uint64_t)strtoull(optarg, nullptr, 10); break;
        case 'd': cfg.runtime_sec = atof(optarg); break;
        case 't': cfg.num_threads = atoi(optarg); break;
        case 'P': cfg.num_ports   = atoi(optarg); break;
        case 'T': cfg.timeout_sec = atof(optarg); break;
        case '6': cfg.ipv6 = true; break;
        case 'h':
        default:
            print_usage(argv[0]);
            exit(opt == 'h' ? 0 : 1);
        }
    }

    if (cfg.server_ip.empty()) {
        fprintf(stderr, "Error: --server is required\n");
        print_usage(argv[0]);
        exit(1);
    }
    if (cfg.input_file.empty()) {
        fprintf(stderr, "Error: --file is required\n");
        print_usage(argv[0]);
        exit(1);
    }
    if (cfg.num_threads < 1) cfg.num_threads = 1;
    if (cfg.num_ports < cfg.num_threads) cfg.num_ports = cfg.num_threads;

    return cfg;
}
