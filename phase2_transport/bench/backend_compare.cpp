#include "transport.hpp"
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include "timing.h"
#include "bench_utils.h"


/* Page-aligned benchmark buffer, matching rai_mr_reg's own allocation. A heap
 * vector lands at an arbitrary offset and usually straddles a page boundary,
 * so the NIC pays two IOTLB lookups per DMA — measured as ~0.5 us on the 4 KB
 * round trip against phase 1. */
struct AlignedBuf {
    char *p = nullptr;
    int init(size_t len) {
        long ps = sysconf(_SC_PAGESIZE);
        if (ps <= 0) ps = 4096;
        if (posix_memalign((void **)&p, (size_t)ps, len) != 0)
            return -1;
        memset(p, 0, len);   /* the doorbell byte must start at zero */
        return 0;
    }
    ~AlignedBuf() { free(p); }
};

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
    fprintf(stderr, "  server: %s <rdma|tcp> [--port <p>] [--iters <n>] [--size <bytes>]\n", prog);
    fprintf(stderr, "  client: %s <rdma|tcp> <server_ip> [--port <p>] [--iters <n>] [--size <bytes>]\n", prog);
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
    AlignedBuf buf;

    if (t == nullptr) {
        LOG_ERR("run_server_sendrecv failed: t is nullptr");
        return -1;
    }

    if (t->listen(cfg.port) != 0) {
        LOG_ERR("run_server_sendrecv failed: listen failed");
        return -1;
    }

    // listen() builds qp_ (which owns the PD), so reg_buf must come after it
    ScopedBuffer sb;
    if (buf.init(len) != 0) {
        LOG_ERR("aligned buffer allocation failed");
        return -1;
    }
    if (sb.init(t, buf.p, len) != 0) {
        LOG_ERR("run_server_sendrecv failed: sb init failed");
        return -1;
    }

    /* Pre-post a batch of receives before accept: the QP must be ready when
     * the first ping lands, and keeping several posted keeps the NIC's WQE
     * fetch off the critical path (the RQ-depth result from phase 1). */
    int total_iters = kWarmup + cfg.iters;
    int prepost = 0;
    if (cfg.is_rdma) {
        prepost = total_iters < 8 ? total_iters : 8;
        for (int j = 0; j < prepost; j++) {
            if (t->recv_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_server_sendrecv failed: recv_async failed");
                return -1;
            }
        }
    }

    if (t->accept() != 0) {
        LOG_ERR("run_server_sendrecv failed: accept failed");
        return -1;
    }

    for (int i = 0; i < total_iters; i++) {
        if (cfg.is_rdma) {
            if (t->poll(nullptr) != 0) {
                LOG_ERR("run_server_sendrecv failed: poll failed");
                return -1;
            }
            /* Replace the consumed receive before the pong goes out — the
             * next ping races back the moment the client sees the pong. */
            if (i + prepost < total_iters) {
                if (t->recv_async(&sb.h, len, 1, 0) != 0) {
                    LOG_ERR("run_server_sendrecv failed: recv_async failed");
                    return -1;
                }
            }
            if (t->send_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_server_sendrecv failed: send_async failed");
                return -1;
            }
            if (t->poll(nullptr) != 0) {
                LOG_ERR("run_server_sendrecv failed: poll failed");
                return -1;
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
    AlignedBuf buf;
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
    if (buf.init(len) != 0) {
        LOG_ERR("aligned buffer allocation failed");
        return -1;
    }
    if (sb.init(t, buf.p, len) != 0) {
        LOG_ERR("run_client_sendrecv failed: sb init failed");
        return -1;
    }

    int total_iters = kWarmup + cfg.iters;
    uint64_t bw_start = 0;

    /* RDMA: keep a batch of receives posted. With a single one the NIC must
     * fetch the WQE on the critical path and the round trip doubles — the
     * RQ-depth result from phase 1. Eight is where that curve goes flat.
     * Posting before the first send also closes an RNR race: the reply races
     * back the moment the server sees a ping, and an empty receive queue then
     * is an RNR NAK on RoCE (eRDMA/iWARP silently tolerated it). */
    int prepost = 0;
    if (cfg.is_rdma) {
        prepost = total_iters < 8 ? total_iters : 8;
        for (int j = 0; j < prepost; j++) {
            if (t->recv_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_client_sendrecv failed: recv_async failed");
                return -1;
            }
        }
    }

    for (int i = 0; i < total_iters; i++) {
        iter_start = time_now_ns();
        if (i == kWarmup) bw_start = iter_start;
        if (cfg.is_rdma) {
            if (i + prepost < total_iters &&
                t->recv_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_client_sendrecv failed: recv_async failed");
                return -1;
            }
            if (t->send_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_client_sendrecv failed: send_async failed");
                return -1;
            }
            // wait for send completion, then recv completion
            if (t->poll(nullptr) != 0 || t->poll(nullptr) != 0) {
                LOG_ERR("run_client_sendrecv failed: poll failed");
                return -1;
            }
        } else {
            /* TCP recv_async is a blocking read — it must come after the
             * send or both sides sit waiting for the other's bytes. */
            if (t->send_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_client_sendrecv failed: send_async failed");
                return -1;
            }
            if (t->recv_async(&sb.h, len, 1, 0) != 0) {
                LOG_ERR("run_client_sendrecv failed: recv_async failed");
                return -1;
            }
        }
        if (i >= kWarmup)
            latencies[i - kWarmup] = time_elapsed_ns(iter_start, time_now_ns());
    }

    uint64_t total_time = time_elapsed_ns(bw_start, time_now_ns());
    std::sort(latencies.begin(), latencies.end());
    print_latency(cfg.is_rdma ? "send/recv latency RTT (rdma)" : "send/recv latency RTT (tcp)", latencies.data(), cfg.iters);
    print_bandwidth(cfg.is_rdma ? "send/recv throughput (rdma)" : "send/recv throughput (tcp)", (uint64_t)cfg.size*cfg.iters, total_time);
    return 0;
}

int run_server_write(Transport *t, Config &cfg) {
    uint32_t unused_rkey;
    uint64_t unused_remote_addr;
    int len = cfg.size;
    AlignedBuf buf;


    if (t == nullptr) {
        LOG_ERR("run_server_write failed: t is nullptr");
        return -1;
    }
    if (cfg.size < 4) {
        LOG_ERR("run_server_write failed: write test needs --size >= 4 (32-bit doorbell)");
        return -1;
    }

    if (t->listen(cfg.port) != 0) {
        LOG_ERR("run_server_write failed: listen failed");
        return -1;
    }

    // listen() builds qp_ (which owns the PD), so reg_buf must come after it
    ScopedBuffer sb;
    if (buf.init(len) != 0) {
        LOG_ERR("aligned buffer allocation failed");
        return -1;
    }
    if (sb.init(t, buf.p, len) != 0) {
        LOG_ERR("run_server_write failed: sb init failed");
        return -1;
    }

    if (t->accept() != 0) {
        LOG_ERR("run_server_write failed: accept failed");
        return -1;
    }

    if (t->exchange_buf(&sb.h, &unused_remote_addr, &unused_rkey) != 0) {
        LOG_ERR("run_server_write failed: exchange_buf failed");
        return -1;
    }

    /* The server is a passive target — its only job is to know when the test
     * is over. The last 4 bytes of the MR carry each write's iteration number;
     * wait for the final one and nothing else. Counting the intermediate
     * values is a trap: the client paces itself on NIC ACKs, not on this CPU,
     * so one interrupt parking this loop for two write intervals makes the
     * counter skip — a per-iteration wait then hangs forever. */
    volatile uint32_t *doorbell =
        (volatile uint32_t *)((uint8_t *)sb.h.addr + cfg.size - 4);
    uint32_t final_seq = (uint32_t)(kWarmup + cfg.iters);
    while (*doorbell != final_seq)
        CPU_RELAX();

    return 0;
}

int run_client_write(Transport *t, Config &cfg) {
    int len = cfg.size;
    uint32_t rkey;
    uint64_t remote_addr, iter_start;
    AlignedBuf buf;
    std::vector<uint64_t> latencies(cfg.iters);

    if (t == nullptr) {
        LOG_ERR("run_client_write failed: t is nullptr");
        return -1;
    }

    if (cfg.size < 4) {
        LOG_ERR("run_client_write failed: write test needs --size >= 4 (32-bit doorbell)");
        return -1;
    }
    if (t->connect(cfg.server_ip.c_str(), cfg.port) != 0) {
        LOG_ERR("run_client_write failed: connect failed");
        return -1;
    }

    ScopedBuffer sb;
    if (buf.init(len) != 0) {
        LOG_ERR("aligned buffer allocation failed");
        return -1;
    }
    if (sb.init(t, buf.p, len) != 0) {
        LOG_ERR("run_client_write failed: sb init failed");
        return -1;
    }

    if (t->exchange_buf(&sb.h, &remote_addr, &rkey) != 0) {
        LOG_ERR("run_client_write failed: exchange_buf failed");
        return -1;
    }

    int total_iters = kWarmup + cfg.iters;
    uint64_t bw_start = 0;
    for (int i = 0; i < total_iters; i++) {
        uint32_t seq = (uint32_t)(i + 1);   // doorbell carries the iteration number
        memcpy(buf.p + len - 4, &seq, 4);
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
    /* run_client_write only runs in is_rdma=true mode (TCP write is skipped
     * in main; see the explanation in main()'s write block). */
    print_latency("write latency (rdma)", latencies.data(), cfg.iters);
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