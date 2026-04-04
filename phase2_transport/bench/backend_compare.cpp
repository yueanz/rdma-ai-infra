#include "transport.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include "timing.h"

struct Config
{
    std::string server_ip;
    int port = 12345;
    int iters = 1000;
    int size = 64;
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
    
    for (int i = 0; i < cfg.iters; i++) {
        start = time_now_ns();
        if (t->send_async(&sb.h, len, 1) != 0) {
            LOG_ERR("run_client_sendrecv failed: send_async failed");
            return -1;
        }

        if (t->recv_async(&sb.h, len, 1) != 0) {
            LOG_ERR("run_client_sendrecv failed: recv_async failed");
            return -1;
        }
    
        // wait for send completion
        if (t->poll(nullptr) != 0) {
            LOG_ERR("run_client_sendrecv failed: poll failed");
            return -1;
        }
        // wait for recv completion
        if (t->poll(nullptr) != 0) {
            LOG_ERR("run_client_sendrecv failed: poll failed");
            return -1;
        }
        latencies[i] = time_elapsed_ns(start, time_now_ns());
    }

    std::sort(latencies.begin(), latencies.end());
    printf("latency (RTT)\n");
    printf("%-10s %-10s %-10s %-10s\n", "min(us)", "median(us)", "p99(us)", "max(us)");
    printf("%-10.2f %-10.2f %-10.2f %-10.2f\n",
        ns_to_us(latencies[0]),
        ns_to_us(latencies[cfg.iters / 2]),
        ns_to_us(latencies[(int)(cfg.iters * 0.99)]),
        ns_to_us(latencies[cfg.iters - 1]));
    return 0;
}

int main(int argc, char *argv[]) {
    Config cfg;

    if (config_parse(argc, argv, &cfg) != 0) {
        config_usage(argv[0]);
        return 1;
    }

    try {
        std::unique_ptr<Transport> t(
            cfg.is_rdma ? create_rdma_transport() : create_tcp_transport()
        );

        bool is_server = cfg.server_ip.empty();
        if (is_server) 
            return run_server_sendrecv(t.get(), cfg);
        else 
            return run_client_sendrecv(t.get(), cfg);
    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}