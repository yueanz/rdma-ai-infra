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

    /* retry connect — peer may not have reached listen() yet */
    int rank_right = (rank + 1) % size;
    int connect_ret = -1;
    for (int i = 0; i < 100; i++) {
        if (w->right->connect(host_list[rank_right], base_port+rank_right) == 0) {
            connect_ret = 0;
            break;
        }
        usleep(10000);  /* 10ms between retries */
    }

    if (connect_ret != 0)
        LOG_ERR("world_init failed: connect failed after retries");

    t.join();

    if (listen_ret != 0 || connect_ret != 0)
        return -1;

    return 0;
}
