#include <string>
#include <cstring>
#include <memory>
#include <algorithm>
#include "kv_cache.hpp"
#include "transport.hpp"
#include "bench_utils.h"

struct Config
{
    std::string server_ip;
    int port = 12345;
    int iters = 1000;
};

static void config_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <server_ip> <port> [--iters <n>]\n", prog);
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
static int run_bench(const char *label, Transport *data, Op op,
                     size_t slot_size, int iters) {
    std::vector<uint64_t> latencies(iters);
    int total_iters = kWarmup + iters;
    uint64_t iter_start, bw_start = 0;

    for (int i = 0; i < total_iters; i++) {
        iter_start = time_now_ns();
        if (i == kWarmup) bw_start = iter_start;
        if (op(i) != 0) {
            LOG_ERR("run_bench failed: op failed at iter %d", i);
            return -1;
        }
        if (data->poll(nullptr) != 0) {
            LOG_ERR("run_bench failed: poll failed at iter %d", i);
            return -1;
        }
        if (i >= kWarmup)
            latencies[i - kWarmup] = time_elapsed_ns(iter_start, time_now_ns());
    }

    uint64_t total_time = time_elapsed_ns(bw_start, time_now_ns());
    std::sort(latencies.begin(), latencies.end());
    print_latency(label, latencies.data(), iters);
    print_bandwidth(label, (uint64_t)iters * slot_size, total_time);
    return 0;
}
int main(int argc, char *argv[]) {
    Config cfg;

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

        std::vector<char> local_buf(remote.slot_size);
        ScopedBuffer sb;
        if (sb.init(data.get(), local_buf.data(), local_buf.size()) != 0) {
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

        // Prefill: push computed K/V tensors into remote cache slot (RDMA write, server CPU uninvolved)
        if (run_bench("rdma write", data.get(),
                [&](int i){return data->write_async(&sb.h, remote_addr, remote.rkey, remote.slot_size, i, 0);},
                remote.slot_size, cfg.iters) != 0) {
            LOG_ERR("write bench failed");
            return 1;
        }

        // Decode: fetch cached K/V tensors from remote slot per decode step (RDMA read, server CPU uninvolved)
        if (run_bench("rdma read", data.get(),
                [&](int i){return data->read_async(&sb.h, remote_addr, remote.rkey, remote.slot_size, i, 0);},
                remote.slot_size, cfg.iters) != 0) {
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