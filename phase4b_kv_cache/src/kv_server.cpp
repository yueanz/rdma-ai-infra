#include <string>
#include <cstring>
#include <memory>
#include "kv_cache.hpp"
#include "transport.hpp"

struct Config
{
    int port = 12345;
    int num_slots;
    size_t slot_size;
};

static void config_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <port> <num_slots> <slot_size>\n", prog);
}

static int config_parse(int argc, char *argv[], Config *cfg) {
    if (argc < 4) {
        return -1;
    }
    
    int i = 1;
    cfg->port = atoi(argv[i++]);
    cfg->num_slots = atoi(argv[i++]);
    cfg->slot_size = atoi(argv[i++]);
    return 0;
}

static int handshake_meta(KVPool &pool, Transport *ctrl, ScopedBuffer &ctrl_sb) {
    CtrlBuf &ctrl_buf = *static_cast<CtrlBuf*>(ctrl_sb.h.addr);

    if (ctrl->recv_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0) {
        LOG_ERR("handshake_meta failed: recv_async failed");
        return -1;
    }

    if (ctrl_buf.msg[0] != KV_MSG_META) {
        LOG_ERR("handshake_meta failed: expected META, got %d", ctrl_buf.msg[0]);
        return -1;
    }

    ctrl_buf.meta.num_slots = pool.num_slots;
    ctrl_buf.meta.slot_size = pool.slot_size;

    if (ctrl->send_async(&ctrl_sb.h, sizeof(ctrl_buf.meta), 0, 0) != 0) {
        LOG_ERR("handshake_meta failed: send_async failed");
        return -1;
    }

    return 0;
}

static int serve(KVPool &pool, Transport *ctrl, ScopedBuffer &ctrl_sb) {
    CtrlBuf &ctrl_buf = *static_cast<CtrlBuf*>(ctrl_sb.h.addr);

    while (1) {
        if (ctrl->recv_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0) {
            LOG_ERR("recv_async failed");
            return -1;
        }

        int msg = ctrl_buf.msg[0];
        if (msg == KV_MSG_ALLOC) {
            int slot_idx = -1;
            if (!pool.free_list.empty()) {
                slot_idx = pool.free_list.back();
                pool.free_list.pop_back();
            }
            ctrl_buf.msg[0] = slot_idx;
            if (ctrl->send_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0) {
                LOG_ERR("send_async failed");
                return -1;
            }
        } else if (msg == KV_MSG_FREE) {
            if (ctrl->recv_async(&ctrl_sb.h, sizeof(int), 0, sizeof(int)) != 0) {
                LOG_ERR("recv_async failed");
                return -1;
            }
            int slot_idx = ctrl_buf.msg[1];
            if (slot_idx < 0 || slot_idx >= pool.num_slots) {
                LOG_ERR("slot_idx invalid");
                return -1;
            }
            pool.free_list.push_back(slot_idx);
            ctrl_buf.msg[0] = 0;    // ack
            if (ctrl->send_async(&ctrl_sb.h, sizeof(int), 0, 0) != 0) {
                LOG_ERR("send_async failed");
                return -1;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    Config cfg;

    if (config_parse(argc, argv, &cfg) != 0) {
        config_usage(argv[0]);
        return 1;
    }

    KVPool pool = {
        .slot_size = cfg.slot_size,
        .num_slots = cfg.num_slots
    };

    try {
        std::unique_ptr<Transport> ctrl(create_tcp_transport());

        if (ctrl->listen(cfg.port+1) != 0) {
            LOG_ERR("listen failed");
            return 1;
        }

        if (ctrl->accept() != 0) {
            LOG_ERR("accept failed");
            return 1;
        }

        std::unique_ptr<Transport> data(create_rdma_transport());

        if (data->listen(cfg.port) != 0) {
            LOG_ERR("listen failed");
            return 1;
        }

        if (data->accept() != 0) {
            LOG_ERR("accept failed");
            return 1;
        }

        CtrlBuf ctrl_buf{};
        ScopedBuffer ctrl_sb;
        if (ctrl_sb.init(ctrl.get(), &ctrl_buf, sizeof(ctrl_buf)) != 0) {
            LOG_ERR("ctrl_sb init failed");
            return 1;
        }

        if (handshake_meta(pool, ctrl.get(), ctrl_sb) != 0) {
            LOG_ERR("handshake_meta failed");
            return 1;
        }

        pool.mem.resize(pool.num_slots * pool.slot_size);   // may throw bad_alloc
        for (int i = 0; i < pool.num_slots; i++)
            pool.free_list.push_back(i);

        ScopedBuffer sb;
        if (sb.init(data.get(), pool.mem.data(), pool.mem.size()) != 0) {
            LOG_ERR("sb init failed");
            return 1;
        }

        if (data->exchange_buf(&sb.h, &pool.addr, &pool.rkey) != 0) {
            LOG_ERR("exchange_buf failed");
            return 1;
        }

        if (serve(pool, ctrl.get(), ctrl_sb) != 0)
            return 1;

    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    return 0;
}