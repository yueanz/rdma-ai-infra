#include "transport.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include "timing.h"
#include <sched.h>

struct Config
{
    std::string server_ip;
    int port = 12345;
    int iters = 1000;
    int size = 4096;
    bool is_rdma = false;
};

struct ScopedBuffer {
    Transport *t;
    BufferHandle h;
    ScopedBuffer(Transport *t, void *buf, size_t size) : t(t), h{} {
        if (t->reg_buf(buf, size, &h) != 0)
            throw std::runtime_error("reg_buf failed");
    }
    ~ScopedBuffer() {t->dereg_buf(&h);}
    ScopedBuffer(const ScopedBuffer&) = delete;
    ScopedBuffer& operator=(const ScopedBuffer&) = delete;
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

static void print_latencies(std::vector<uint64_t>& latencies, int iters) {
    std::sort(latencies.begin(), latencies.end());
    printf("latency (RTT)\n");
    printf("%-10s %-10s %-10s %-10s\n", "min(us)", "median(us)", "p99(us)", "max(us)");
    printf("%-10.2f %-10.2f %-10.2f %-10.2f\n",
        ns_to_us(latencies[0]),
        ns_to_us(latencies[iters / 2]),
        ns_to_us(latencies[(int)(iters * 0.99)]),
        ns_to_us(latencies[iters - 1]));
}

static void print_bandwidth(uint64_t total_bytes, uint64_t elapsed_ns) {
    double elapsed_ms = elapsed_ns / 1e6;
    double gbps       = (double)total_bytes * 8 / (elapsed_ns / 1e9) / 1e9;
    double GBps       = (double)total_bytes / (elapsed_ns / 1e9) / (1024.0 * 1024 * 1024);

    printf("\n--- Bandwidth Results ---\n");
    printf("  transferred : %lu bytes (%.2f MB)\n",
           total_bytes, total_bytes / (1024.0 * 1024));
    printf("  elapsed     : %.2f ms\n", elapsed_ms);
    printf("  throughput  : %.2f GB/s  /  %.2f Gbps\n", GBps, gbps);
    printf("-------------------------\n");
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
    ScopedBuffer sb(t, buf.data(), len);

    // pre-post recv before accept so QP is ready when client sends
    if (cfg.is_rdma && t->recv_async(&sb.h, len, 1) != 0) {
        LOG_ERR("run_server_sendrecv failed: recv_async failed");
        return -1;
    }

    if (t->accept() != 0) {
        LOG_ERR("run_server_sendrecv failed: accept failed");
        return -1;
    }

    for (int i = 0; i < cfg.iters; i++) {
        if (cfg.is_rdma) {
            if (t->poll(nullptr) != 0) {
                LOG_ERR("run_server_sendrecv failed: poll failed");
                return -1;
            }
            if (t->send_async(&sb.h, len, 1) != 0) {
                LOG_ERR("run_server_sendrecv failed: send_async failed");
                return -1;
            }
            if (t->poll(nullptr) != 0) {
                LOG_ERR("run_server_sendrecv failed: poll failed");
                return -1;
            }
            if (i + 1 < cfg.iters) {
                if (t->recv_async(&sb.h, len, 1) != 0) {
                    LOG_ERR("run_server_sendrecv failed: recv_async failed");
                    return -1;
                }
            }
        } else {
            if (t->recv_async(&sb.h, len, 1) != 0) {
                LOG_ERR("run_server_sendrecv failed: recv_async failed");
                return -1;
            }
            if (t->send_async(&sb.h, len, 1) != 0) {
                LOG_ERR("run_server_sendrecv failed: send_async failed");
                return -1;
            }
        }
    }

    return 0;
}

int run_client_sendrecv(Transport *t, Config &cfg) {
    uint64_t start;
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

    ScopedBuffer sb(t, buf.data(), len);
    
    uint64_t t0 = time_now_ns();
    for (int i = 0; i < cfg.iters; i++) {
        start = time_now_ns();
        if (t->send_async(&sb.h, len, 1) != 0) {
            LOG_ERR("run_client_sendrecv failed: send_async failed");
            return -1;
        }

        // wait for send completion
        if (t->poll(nullptr) != 0) {
            LOG_ERR("run_client_sendrecv failed: poll failed");
            return -1;
        }

        if (t->recv_async(&sb.h, len, 1) != 0) {
            LOG_ERR("run_client_sendrecv failed: recv_async failed");
            return -1;
        }
    
        // wait for recv completion
        if (t->poll(nullptr) != 0) {
            LOG_ERR("run_client_sendrecv failed: poll failed");
            return -1;
        }
        latencies[i] = time_elapsed_ns(start, time_now_ns());
    }

    uint64_t total_time = time_elapsed_ns(t0, time_now_ns());
    print_latencies(latencies, cfg.iters);
    print_bandwidth((uint64_t)cfg.size*cfg.iters, total_time);
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
    ScopedBuffer sb(t, buf.data(), len);

    if (t->accept() != 0) {
        LOG_ERR("run_server_write failed: accept failed");
        return -1;
    }

    if (t->exchange_buf(&sb.h, &unused_remote_addr, &unused_rkey) != 0) {
        LOG_ERR("run_server_write failed: accept failed");
        return -1;
    }

    if (cfg.is_rdma) {
        // RDMA: doorbell polling
        volatile uint8_t *doorbell = (uint8_t *)sb.h.addr + cfg.size - 1;
        for (int i = 0; i < cfg.iters; i++) {
            while (*doorbell == 0)
                sched_yield();
            *doorbell = 0;
        }
    } else {
        // TCP: explicit recv
        for (int i = 0; i < cfg.iters; i++) {
            t->recv_async(&sb.h, len, 1);
        }
    }

    return 0;
}

int run_client_write(Transport *t, Config &cfg) {
    int len = cfg.size;
    uint32_t rkey;
    uint64_t remote_addr, start;
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

    ScopedBuffer sb(t, buf.data(), len);

    if (t->exchange_buf(&sb.h, &remote_addr, &rkey) != 0) {
        LOG_ERR("run_client_write failed: accept failed");
        return -1;
    }

    uint64_t t0 = time_now_ns();
    for (int i = 0; i < cfg.iters; i++) {
        buf[len-1] = 1;  // set doorbell in local buf
        start = time_now_ns();
        if (t->write_async(&sb.h, remote_addr, rkey,len, i) != 0) {
            LOG_ERR("run_client_write failed: write_async failed");
            return -1;
        }
        if (t->poll(nullptr) != 0) {
            LOG_ERR("run_client_write failed: poll failed");
            return -1;
        }
        latencies[i] = time_elapsed_ns(start, time_now_ns());
    }

    uint64_t total_time = time_elapsed_ns(t0, time_now_ns());
    print_latencies(latencies, cfg.iters);
    print_bandwidth((uint64_t)cfg.size*cfg.iters, total_time);
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
        // write
        {
            std::unique_ptr<Transport> t(
                cfg.is_rdma ? create_rdma_transport() : create_tcp_transport()
            );
            printf("=== write [%s] ===\n", cfg.is_rdma ? "rdma" : "tcp");
            // write — use port+2 to avoid TIME_WAIT conflict with sendrecv
            cfg.port += 2;
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
            cfg.port -= 2;  // restore
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}