#include <string>
#include <cstring>
#include <memory>
#include <algorithm>
#include "kv_cache.hpp"
#include "transport.hpp"
#include "bench_utils.h"
#include <unistd.h>
#include <cstdlib>

struct Config
{
    std::string server_ip;
    int port = 12345;
    int iters = 1000;
    int depth       = 1;      /* operations posted before any is polled */
    bool csv        = false;
    bool csv_header = false;
};

/* Page-aligned slot buffer, matching rai_mr_reg's own allocation; a heap
 * vector straddles a page boundary and the NIC then touches two per DMA. */
struct AlignedBuf {
    char *p = nullptr;
    int init(size_t len) {
        long ps = sysconf(_SC_PAGESIZE);
        if (ps <= 0) ps = 4096;
        void *raw = nullptr;
        if (posix_memalign(&raw, (size_t)ps, len) != 0) return -1;
        memset(raw, 0, len);
        p = (char*)raw;
        return 0;
    }
    ~AlignedBuf() { free(p); }
    AlignedBuf() = default;
    AlignedBuf(const AlignedBuf&) = delete;
    AlignedBuf& operator=(const AlignedBuf&) = delete;
};

static void config_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <server_ip> <port> [--iters <n>] [--depth <n>] [--csv]\n", prog);
    fprintf(stderr, "  %s --csv-header\n", prog);
}

static int config_parse(int argc, char *argv[], Config *cfg) {
    if (argc < 3) {
        return -1;
    }

    int i = 1;
    cfg->server_ip = argv[i++];
    cfg->port = atoi(argv[i++]);
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0) {
            if (i + 1 >= argc) {
                printf("missing value after --iters\n");
                return -1;
            }
            cfg->iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--depth") == 0) {
            if (i + 1 >= argc) {
                printf("missing value after --depth\n");
                return -1;
            }
            cfg->depth = atoi(argv[++i]);
            if (cfg->depth < 1) {
                printf("depth must be >= 1\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--csv") == 0) {
            cfg->csv = true;
        }
    }
    return 0;
}

/* recv_async + poll is the backend-agnostic pattern: TCP's poll is a no-op
 * (recv_async already blocked); RDMA's poll waits for the WR to complete. */
static int handshake_meta(KVRemote &remote, Transport *ctrl, ScopedBuffer &ctrl_sb) {
    CtrlBuf &ctrl_buf = *static_cast<CtrlBuf*>(ctrl_sb.h.addr);

    ctrl_buf.msg[0] = KV_MSG_META;

    if (ctrl->send_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0 ||
        ctrl->poll(nullptr) != 0) {
        LOG_ERR("handshake_meta failed: send META failed");
        return -1;
    }

    if (ctrl->recv_async(&ctrl_sb.h, sizeof(ctrl_buf.meta), 0, 0) != 0 ||
        ctrl->poll(nullptr) != 0) {
        LOG_ERR("handshake_meta failed: recv meta failed");
        return -1;
    }

    remote.num_slots = ctrl_buf.meta.num_slots;
    remote.slot_size = ctrl_buf.meta.slot_size;

    return 0;
}

static int kv_alloc(int &slot_idx, Transport *ctrl, ScopedBuffer &ctrl_sb) {
    CtrlBuf &ctrl_buf = *static_cast<CtrlBuf*>(ctrl_sb.h.addr);

    /* Send full ctrl_buf.msg (8 bytes) even though ALLOC has no payload —
     * keeps the wire format uniform with FREE so the server's recv_async
     * size matches under both TCP and RDMA. msg[1] is unused. */
    ctrl_buf.msg[0] = KV_MSG_ALLOC;
    ctrl_buf.msg[1] = 0;
    if (ctrl->send_async(&ctrl_sb.h, sizeof(ctrl_buf.msg), 0, 0) != 0 ||
        ctrl->poll(nullptr) != 0) {
        LOG_ERR("kv_alloc failed: send ALLOC failed");
        return -1;
    }

    if (ctrl->recv_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0 ||
        ctrl->poll(nullptr) != 0) {
        LOG_ERR("kv_alloc failed: recv slot_idx failed");
        return -1;
    }

    slot_idx = ctrl_buf.msg[0];
    return 0;
}

static int kv_free(int slot_idx, Transport *ctrl, ScopedBuffer &ctrl_sb) {
    CtrlBuf &ctrl_buf = *static_cast<CtrlBuf*>(ctrl_sb.h.addr);

    ctrl_buf.msg[0] = KV_MSG_FREE;
    ctrl_buf.msg[1] = slot_idx;

    if (ctrl->send_async(&ctrl_sb.h, sizeof(ctrl_buf.msg), 0, 0) != 0 ||
        ctrl->poll(nullptr) != 0) {
        LOG_ERR("kv_free failed: send FREE failed");
        return -1;
    }

    if (ctrl->recv_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0 ||
        ctrl->poll(nullptr) != 0) {
        LOG_ERR("kv_free failed: recv ack failed");
        return -1;
    }

    if (ctrl_buf.msg[0] != 0) {
        LOG_ERR("kv_free failed: unexpected ack %d", ctrl_buf.msg[0]);
        return -1;
    }
    return 0;
}

template<typename Op>
/*
 * `depth` operations are posted before any of them is polled. At 1 the timing
 * is a plain latency: post, wait, repeat. Above 1 the recorded per-operation
 * figure is the batch divided by its size, which is inverse throughput and not
 * a latency -- the operations overlap. The gbps below stays honest either way
 * because it comes from total bytes over total elapsed.
 *
 * Depth matters much more for reads than writes: the queue pair will only
 * carry max_rd_atomic reads at once, so a read issued alone waits out a full
 * request-and-return before the next starts.
 */
static int run_bench(const char *label, Transport *data, Op op,
                     size_t slot_size, int iters, bool csv, int depth) {
    std::vector<uint64_t> latencies(iters);
    int total_iters = kWarmup + iters;
    uint64_t iter_start, bw_start = 0;

    for (int i = 0; i < total_iters; i += depth) {
        int n = std::min(depth, total_iters - i);
        iter_start = time_now_ns();
        if (i >= kWarmup && bw_start == 0) bw_start = iter_start;
        for (int k = 0; k < n; k++) {
            if (op(i + k) != 0) {
                LOG_ERR("run_bench failed: op failed at iter %d", i + k);
                return -1;
            }
        }
        for (int k = 0; k < n; k++) {
            if (data->poll(nullptr) != 0) {
                LOG_ERR("run_bench failed: poll failed at iter %d", i + k);
                return -1;
            }
        }
        uint64_t per = time_elapsed_ns(iter_start, time_now_ns()) / n;
        for (int k = 0; k < n; k++)
            if (i + k >= kWarmup) latencies[i + k - kWarmup] = per;
    }

    uint64_t total_time = time_elapsed_ns(bw_start, time_now_ns());
    std::sort(latencies.begin(), latencies.end());
    if (csv) {
        /* Phase 1's column order (bench_config.h). */
        lat_stats_t st = latency_stats(latencies.data(), iters);
        printf("%s,rdma,%zu,%d,%d,%.2f,%.2f,%.2f,%.2f,%.3f\n",
               label, slot_size, depth, iters, st.min_us, st.median_us,
               st.p99_us, st.max_us,
               bandwidth_gbps((uint64_t)iters * slot_size, total_time));
    } else {
        print_latency(label, latencies.data(), iters);
        print_bandwidth(label, (uint64_t)iters * slot_size, total_time);
    }
    return 0;
}
int main(int argc, char *argv[]) {
    Config cfg;

    /* Before config_parse, which requires the positional args. */
    if (argc == 2 && strcmp(argv[1], "--csv-header") == 0) {
        printf("bench,conn,size,depth,iters,min_us,median_us,p99_us,max_us,gbps\n");
        return 0;
    }

    if (config_parse(argc, argv, &cfg) != 0) {
        config_usage(argv[0]);
        return 1;
    }

    KVRemote remote{};

    try {
        std::unique_ptr<Transport> ctrl(create_tcp_transport());

        /* Match kv_server's port layout: ctrl on port, data on port+2 */
        if (ctrl->connect(cfg.server_ip.c_str(), cfg.port) != 0) {
            LOG_ERR("connect failed");
            return 1;
        }

        std::unique_ptr<Transport> data(create_rdma_transport());

        if (data->connect(cfg.server_ip.c_str(), cfg.port + 2) != 0) {
            LOG_ERR("connect failed");
            return 1;
        }

        CtrlBuf ctrl_buf{};
        ScopedBuffer ctrl_sb;
        if (ctrl_sb.init(ctrl.get(), &ctrl_buf, sizeof(ctrl_buf)) != 0) {
            LOG_ERR("ctrl_sb init failed");
            return 1;
        }

        if (handshake_meta(remote, ctrl.get(), ctrl_sb) != 0) {
            LOG_ERR("handshake_meta failed");
            return 1;
        }

        AlignedBuf local_buf;
        if (local_buf.init(remote.slot_size) != 0) {
            LOG_ERR("local buffer allocation failed");
            return 1;
        }
        ScopedBuffer sb;
        if (sb.init(data.get(), local_buf.p, remote.slot_size) != 0) {
            LOG_ERR("sb init failed");
            return 1;
        }

        if (data->exchange_buf(&sb.h, &remote.base_addr, &remote.rkey) != 0) {
            LOG_ERR("exchange_buf failed");
            return 1;
        }

        int slot_idx;
        if (kv_alloc(slot_idx, ctrl.get(), ctrl_sb) != 0) {
            LOG_ERR("kv_alloc failed");
            return 1;
        }

        if (slot_idx < 0 || slot_idx >= remote.num_slots) {
            LOG_ERR("unexpected slot_idx: %d", slot_idx);
            return 1;
        }

        uint64_t remote_addr = remote.base_addr + slot_idx * remote.slot_size;

        /* Prove the data path before timing it. A one-sided write reports
         * success once the fabric accepts it, so a wrong rkey or a
         * miscomputed slot offset would land the bytes somewhere else and
         * still produce clean-looking latencies. Write a pattern, clear the
         * local buffer, read it back, compare. */
        for (size_t i = 0; i < remote.slot_size; i++)
            local_buf.p[i] = (char)(i * 31 + 7);
        if (data->write_async(&sb.h, remote_addr, remote.rkey, remote.slot_size, 0, 0) != 0 ||
            data->poll(nullptr) != 0) {
            LOG_ERR("data path check: write failed");
            return 1;
        }
        memset(local_buf.p, 0, remote.slot_size);
        if (data->read_async(&sb.h, remote_addr, remote.rkey, remote.slot_size, 0, 0) != 0 ||
            data->poll(nullptr) != 0) {
            LOG_ERR("data path check: read failed");
            return 1;
        }
        for (size_t i = 0; i < remote.slot_size; i++) {
            if (local_buf.p[i] != (char)(i * 31 + 7)) {
                LOG_ERR("data path check failed at byte %zu: got %d want %d",
                        i, local_buf.p[i], (char)(i * 31 + 7));
                return 1;
            }
        }
        if (!cfg.csv)
            LOG_INFO("data path check passed (%zu bytes round-tripped)", remote.slot_size);

        // Prefill: push computed K/V tensors into remote cache slot (RDMA write, server CPU uninvolved)
        if (run_bench("write", data.get(),
                [&](int i){return data->write_async(&sb.h, remote_addr, remote.rkey, remote.slot_size, i, 0);},
                remote.slot_size, cfg.iters, cfg.csv, cfg.depth) != 0) {
            LOG_ERR("write bench failed");
            return 1;
        }

        // Decode: fetch cached K/V tensors from remote slot per decode step (RDMA read, server CPU uninvolved)
        if (run_bench("read", data.get(),
                [&](int i){return data->read_async(&sb.h, remote_addr, remote.rkey, remote.slot_size, i, 0);},
                remote.slot_size, cfg.iters, cfg.csv, cfg.depth) != 0) {
            LOG_ERR("read bench failed");
            return 1;
        }

        if (kv_free(slot_idx, ctrl.get(), ctrl_sb) != 0) {
            LOG_ERR("kv_free failed");
            return 1;
        }

    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}