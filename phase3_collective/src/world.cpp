#include "collective.hpp"
#include <thread>
#include <unistd.h>
#include <atomic>

int world_init(World *w, int rank, int size,
               const char **host_list, int base_port,
               bool use_rdma) {
    if (w == nullptr) {
        LOG_ERR("world_init failed: w is nullptr");
        return -1;
    }
    w->rank = rank;
    w->size = size; /* size: total number of processes in the group */

    /* left: we are server (left neighbor connects to us)
     * right: we are client (we connect to right neighbor) */
    w->left = std::unique_ptr<Transport> (
        use_rdma ? create_rdma_transport() : create_tcp_transport()
    );
    w->right = std::unique_ptr<Transport> (
        use_rdma ? create_rdma_transport() : create_tcp_transport()
    );
    if (w->left == nullptr || w->right == nullptr) {
        LOG_ERR("world_init failed: w->left or w->right is nullptr");
        return -1;
    }

    /* listen/accept and connect must run in parallel — sequential would deadlock */
    std::atomic<int> listen_ret{0};
    std::thread t([&]() {
        if (w->left->listen(base_port+rank) != 0) {
            LOG_ERR("world_init failed: listen failed");
            listen_ret = -1;
            return;
        }
        if (w->left->accept() != 0) {
            LOG_ERR("world_init failed: accept failed");
            listen_ret = -1;
        }
    });

    /* retry connect — peer may not have reached listen() yet.
     * 60s window to accommodate manual launch across two terminals. */
    int rank_right = (rank + 1) % size;
    int connect_ret = -1;
    for (int i = 0; i < 600; i++) {
        if (w->right->connect(host_list[rank_right], base_port+rank_right) == 0) {
            connect_ret = 0;
            break;
        }
        usleep(100000);  /* 100ms between retries */
    }

    if (connect_ret != 0)
        LOG_ERR("world_init failed: connect failed after retries");

    t.join();

    if (listen_ret != 0 || connect_ret != 0)
        return -1;

    return 0;
}

int world_barrier(World *w, BufferHandle *r_h, BufferHandle *l_h) {
    if (w == nullptr || w->size < 2)
        return 0;

    /* One byte each way. Receives go up before the send on the path where
     * that is possible, for the same reason ring_allreduce does it. */
    const size_t n = 1;
    const uint64_t id = ~0ull;

    if (!w->left->recv_blocks()) {
        if (w->left->recv_async(l_h, n, id, 0) != 0 ||
            w->right->send_async(r_h, n, id, 0) != 0) {
            LOG_ERR("world_barrier failed: post failed");
            return -1;
        }
    } else if (w->rank % 2 == 0) {
        if (w->right->send_async(r_h, n, id, 0) != 0 ||
            w->left->recv_async(l_h, n, id, 0) != 0) {
            LOG_ERR("world_barrier failed: post failed");
            return -1;
        }
    } else {
        if (w->left->recv_async(l_h, n, id, 0) != 0 ||
            w->right->send_async(r_h, n, id, 0) != 0) {
            LOG_ERR("world_barrier failed: post failed");
            return -1;
        }
    }
    if (w->left->poll(nullptr) != 0 || w->right->poll(nullptr) != 0) {
        LOG_ERR("world_barrier failed: poll failed");
        return -1;
    }
    return 0;
}
