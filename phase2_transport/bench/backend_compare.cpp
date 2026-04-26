#include "transport.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include "timing.h"
#include "bench_utils.h"

struct Config
{
    std::string server_ip;
    int port = 12345;
    int iters = 1000;
    int size = 4096;
    bool is_rdma = false;
};

static void config_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  server: %s <rdma|tcp> <port> [--iters <n>] [--size <bytes>]\n", prog);
    fprintf(stderr, "  client: %s <rdma|tcp> <server_ip> <port> [--iters <n>] [--size <bytes>]\n", prog);
}

static int config_parse(int argc, char *argv[], Config *cfg) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "rdma") == 0) {
            cfg->is_rdma = true;
        } else if (strcmp(argv[i], "tcp") == 0) {
            cfg->is_rdma = false;
        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value after --port\n");
                return -1;
            }
            cfg->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--iters") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value after --iters\n");
                return -1;
            }
            cfg->iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value after --size\n");
                return -1;
            }
            cfg->size = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            cfg->server_ip = argv[i];
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

int run_server_sendrecv(Transport *t, Config &cfg) {
    int len = cfg.size;
    std::vector<char> buf(len);

    if (t == nullptr) {
        LOG_ERR("run_server_sendrecv failed: t is nullptr");
        return -1;
    }

    if (t->listen(cfg.port) != 0) {
        LOG_ERR("run_server_sendrecv failed: listen failed");
        return -1;
    }

    // listen() initializes ctx_ (PD), so reg_buf must come after it
    ScopedBuffer sb;
    if (sb.init(t, buf.data(), len) != 0) {
        LOG_ERR("run_server_sendrecv failed: sb init failed");
        return -1;
    }

    // pre-post recv before accept so QP is ready when client sends
    if (cfg.is_rdma && t->recv_async(&sb.h, len, 1, 0) != 0) {
        LOG_ERR("run_server_sendrecv failed: recv_async failed");
        return -1;
    }

    if (t->accept() != 0) {
        LOG_ERR("run_server_sendrecv failed: accept failed");
        return -1;
    }

    int total_iters = kWarmup + cfg.iters;
    for (int i = 0; i < total_iters; i++) {
        if (cfg.is_rdma) {
            if (t->poll(nullptr) != 0) {
                LOG_ERR("run_server_sendrecv failed: poll failed");
                return -1;
            }
            if (t->send_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_server_sendrecv failed: send_async failed");
                return -1;
            }
            if (t->poll(nullptr) != 0) {
                LOG_ERR("run_server_sendrecv failed: poll failed");
                return -1;
            }
            if (i + 1 < total_iters) {
                if (t->recv_async(&sb.h, len, 1, 0) != 0) {
                    LOG_ERR("run_server_sendrecv failed: recv_async failed");
                    return -1;
                }
            }
        } else {
            if (t->recv_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_server_sendrecv failed: recv_async failed");
                return -1;
            }
            if (t->send_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_server_sendrecv failed: send_async failed");
                return -1;
            }
        }
    }

    return 0;
}

int run_client_sendrecv(Transport *t, Config &cfg) {
    uint64_t iter_start;
    int len = cfg.size;
    std::vector<char> buf(len);
    std::vector<uint64_t> latencies(cfg.iters);

    if (t == nullptr) {
        LOG_ERR("run_client_sendrecv failed: t is nullptr");
        return -1;
    }

    if (t->connect(cfg.server_ip.c_str(), cfg.port) != 0) {
        LOG_ERR("run_client_sendrecv failed: connect failed");
        return -1;
    }

    ScopedBuffer sb;
    if (sb.init(t, buf.data(), len) != 0) {
        LOG_ERR("run_client_sendrecv failed: sb init failed");
        return -1;
    }

    int total_iters = kWarmup + cfg.iters;
    uint64_t bw_start = 0;
    for (int i = 0; i < total_iters; i++) {
        iter_start = time_now_ns();
        if (i == kWarmup) bw_start = iter_start;
        if (t->send_async(&sb.h, len, 1, 0) != 0) {
            LOG_ERR("run_client_sendrecv failed: send_async failed");
            return -1;
        }

        // wait for send completion
        if (t->poll(nullptr) != 0) {
            LOG_ERR("run_client_sendrecv failed: poll failed");
            return -1;
        }

        if (t->recv_async(&sb.h, len, 1, 0) != 0) {
            LOG_ERR("run_client_sendrecv failed: recv_async failed");
            return -1;
        }

        // wait for recv completion
        if (t->poll(nullptr) != 0) {
            LOG_ERR("run_client_sendrecv failed: poll failed");
            return -1;
        }
        if (i >= kWarmup)
            latencies[i - kWarmup] = time_elapsed_ns(iter_start, time_now_ns());
    }

    uint64_t total_time = time_elapsed_ns(bw_start, time_now_ns());
    std::sort(latencies.begin(), latencies.end());
    print_latency(cfg.is_rdma ? "send/recv latency RTT (rdma)" : "send/recv latency RTT (tcp)", latencies.data(), cfg.iters);
    print_bandwidth("rdma send/recv throughput", (uint64_t)cfg.size*cfg.iters, total_time);
    return 0;
}

int run_server_write(Transport *t, Config &cfg) {
    uint32_t unused_rkey;
    uint64_t unused_remote_addr;
    int len = cfg.size;
    std::vector<char> buf(len);


    if (t == nullptr) {
        LOG_ERR("run_server_write failed: t is nullptr");
        return -1;
    }

    if (t->listen(cfg.port) != 0) {
        LOG_ERR("run_server_write failed: listen failed");
        return -1;
    }

    // listen() initializes ctx_ (PD), so reg_buf must come after it
    ScopedBuffer sb;
    if (sb.init(t, buf.data(), len) != 0) {
        LOG_ERR("run_server_write failed: sb init failed");
        return -1;
    }

    if (t->accept() != 0) {
        LOG_ERR("run_server_write failed: accept failed");
        return -1;
    }

    if (t->exchange_buf(&sb.h, &unused_remote_addr, &unused_rkey) != 0) {
        LOG_ERR("run_server_write failed: accept failed");
        return -1;
    }

    // RDMA write: server CPU is uninvolved, just spins on doorbell byte
    // (the last byte of the MR; client sets it to 1 on each write)
    volatile uint8_t *doorbell = (uint8_t *)sb.h.addr + cfg.size - 1;
    for (int i = 0; i < kWarmup + cfg.iters; i++) {
        while (*doorbell == 0)
            CPU_RELAX();
        *doorbell = 0;
    }

    return 0;
}

int run_client_write(Transport *t, Config &cfg) {
    int len = cfg.size;
    uint32_t rkey;
    uint64_t remote_addr, iter_start;
    std::vector<char> buf(len);
    std::vector<uint64_t> latencies(cfg.iters);

    if (t == nullptr) {
        LOG_ERR("run_client_write failed: t is nullptr");
        return -1;
    }

    if (t->connect(cfg.server_ip.c_str(), cfg.port) != 0) {
        LOG_ERR("run_client_write failed: connect failed");
        return -1;
    }

    ScopedBuffer sb;
    if (sb.init(t, buf.data(), len) != 0) {
        LOG_ERR("run_client_write failed: sb init failed");
        return -1;
    }

    if (t->exchange_buf(&sb.h, &remote_addr, &rkey) != 0) {
        LOG_ERR("run_client_write failed: accept failed");
        return -1;
    }

    int total_iters = kWarmup + cfg.iters;
    uint64_t bw_start = 0;
    for (int i = 0; i < total_iters; i++) {
        buf[len-1] = 1;  // set doorbell in local buf
        iter_start = time_now_ns();
        if (i == kWarmup) bw_start = iter_start;
        if (t->write_async(&sb.h, remote_addr, rkey, len, i, 0) != 0) {
            LOG_ERR("run_client_write failed: write_async failed");
            return -1;
        }
        if (t->poll(nullptr) != 0) {
            LOG_ERR("run_client_write failed: poll failed");
            return -1;
        }
        if (i >= kWarmup)
            latencies[i - kWarmup] = time_elapsed_ns(iter_start, time_now_ns());
    }

    uint64_t total_time = time_elapsed_ns(bw_start, time_now_ns());
    std::sort(latencies.begin(), latencies.end());
    print_latency(cfg.is_rdma ? "write latency (rdma)" : "write latency (tcp)", latencies.data(), cfg.iters);
    print_bandwidth("rdma write throughput", (uint64_t)cfg.size*cfg.iters, total_time);
    return 0;
}

int main(int argc, char *argv[]) {
    Config cfg;

    if (config_parse(argc, argv, &cfg) != 0) {
        config_usage(argv[0]);
        return 1;
    }

    try {
        bool is_server = cfg.server_ip.empty();
        // sendrecv
        {
            std::unique_ptr<Transport> t(
                cfg.is_rdma ? create_rdma_transport() : create_tcp_transport()
            );
            printf("=== send/recv [%s] ===\n", cfg.is_rdma ? "rdma" : "tcp");
            if (is_server) {
                if (run_server_sendrecv(t.get(), cfg) != 0) {
                    LOG_ERR("run_server_sendrecv failed");
                    return 1;
                }
            }
            else {
                if (run_client_sendrecv(t.get(), cfg) != 0) {
                    LOG_ERR("run_client_sendrecv failed");
                    return 1;
                }
            }
        }
        // write — RDMA only (TCP has no one-sided write primitive; any TCP
        // emulation either measures local syscall time or degenerates into a
        // 2-sided send/recv with explicit ACK, so we omit it).
        if (cfg.is_rdma) {
            std::unique_ptr<Transport> t(create_rdma_transport());
            printf("=== write [rdma] ===\n");
            cfg.port += 2;  // avoid TIME_WAIT conflict with sendrecv
            if (is_server) {
                if (run_server_write(t.get(), cfg) != 0) {
                    LOG_ERR("run_server_write failed");
                    return 1;
                }
            } else {
                if (run_client_write(t.get(), cfg) != 0) {
                    LOG_ERR("run_client_write failed");
                    return 1;
                }
            }
            cfg.port -= 2;
        } else {
            printf("=== write [tcp] === SKIPPED: TCP has no one-sided write primitive\n");
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}