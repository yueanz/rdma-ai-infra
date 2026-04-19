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

static int handshake_meta(KVRemote &remote, Transport *ctrl, ScopedBuffer &ctrl_sb) {
    CtrlBuf &ctrl_buf = *static_cast<CtrlBuf*>(ctrl_sb.h.addr);

    ctrl_buf.msg[0] = KV_MSG_META;

    if (ctrl->send_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0) {
        LOG_ERR("handshake_meta failed: send_async failed");
        return -1;
    }

    if (ctrl->recv_async(&ctrl_sb.h, sizeof(ctrl_buf.meta), 0, 0) != 0) {
        LOG_ERR("handshake_meta failed: recv_async failed");
        return -1;
    }

    remote.num_slots = ctrl_buf.meta.num_slots;
    remote.slot_size = ctrl_buf.meta.slot_size;

    return 0;
}

static int kv_alloc(int &slot_idx, Transport *ctrl, ScopedBuffer &ctrl_sb) {
    CtrlBuf &ctrl_buf = *static_cast<CtrlBuf*>(ctrl_sb.h.addr);

    ctrl_buf.msg[0] = KV_MSG_ALLOC;
    if (ctrl->send_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0) {
        LOG_ERR("kv_alloc failed: send_async failed");
        return -1;
    }

    if (ctrl->recv_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0) {
        LOG_ERR("kv_alloc failed: recv_async failed");
        return -1;
    }

    slot_idx = ctrl_buf.msg[0];
    return 0;
}

static int kv_free(int slot_idx, Transport *ctrl, ScopedBuffer &ctrl_sb) {
    CtrlBuf &ctrl_buf = *static_cast<CtrlBuf*>(ctrl_sb.h.addr);

    ctrl_buf.msg[0] = KV_MSG_FREE;
    ctrl_buf.msg[1] = slot_idx;

    if (ctrl->send_async(&ctrl_sb.h, sizeof(ctrl_buf.msg), 0, 0) != 0) {
        LOG_ERR("kv_free failed: send_async failed");
        return -1;
    }

    if (ctrl->recv_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0) {
        LOG_ERR("kv_free failed: recv_async failed");
        return -1;
    }

    if (ctrl_buf.msg[0] != 0) {
        LOG_ERR("kv_free failed: unexpected ack %d", ctrl_buf.msg[0]);
        return -1;
    }
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

        if (ctrl->connect(cfg.server_ip.c_str(), cfg.port+1) != 0) {
            LOG_ERR("connect failed");
            return 1;
        }

        std::unique_ptr<Transport> data(create_rdma_transport());

        if (data->connect(cfg.server_ip.c_str(), cfg.port) != 0) {
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

        uint64_t start, t0;
        std::vector<uint64_t> latencies(cfg.iters);
        int total_iters = kWarmup + cfg.iters;

        for (int i = 0; i < total_iters; i++) {
            if (i == kWarmup)
                t0 = time_now_ns();
            start = time_now_ns();
            // slot offset baked into remote_addr; local offset=0 (local_buf is one slot)
            uint64_t remote_addr = remote.base_addr + slot_idx * remote.slot_size;
            if (data->write_async(&sb.h, remote_addr, remote.rkey,
                                remote.slot_size, i, 0) != 0) {
                LOG_ERR("write_async failed");
                return 1;
            }
            if (data->poll(nullptr) != 0) {
                LOG_ERR("poll failed");
                return 1;
            }
            if (i >= kWarmup)
                latencies[i - kWarmup] = time_elapsed_ns(start, time_now_ns());
        }

        uint64_t total_time = time_elapsed_ns(t0, time_now_ns());
        std::sort(latencies.begin(), latencies.end());
        print_latency("rdma write latency", latencies.data(), cfg.iters);
        print_bandwidth("rdma write throughput", (uint64_t)cfg.iters*remote.slot_size, total_time);

        if (kv_free(slot_idx, ctrl.get(), ctrl_sb) != 0) {
            LOG_ERR("kv_free failed");
        }

    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}