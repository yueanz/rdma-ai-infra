#include "collective.hpp"
#include <vector>

int ring_allreduce(World *w, float *buf, size_t count) {
    int N = w->size;
    int rank = w->rank;

    if (count % N != 0) {
        LOG_ERR("ring_allreduce: count %zu not divisible by world size %d", count, N);
        return -1;
    }

    size_t chunk = count / N;
    size_t chunk_bytes = chunk * sizeof(float);

    // staging buffer for reduce-scatter receives
    std::vector<float> stage(chunk);

    // register buffers
    ScopedBuffer r_buf_h(w->right.get(), buf, count * sizeof(float));
    ScopedBuffer l_buf_h(w->left.get(), buf, count * sizeof(float));
    ScopedBuffer stage_h(w->left.get(), stage.data(), chunk_bytes);

    // reduce-scatter: N-1 rounds
    for (int r = 0; r < N-1; r++) {
        int send_idx = (rank - r + N) % N;
        int recv_idx = (rank - r - 1 + N) % N;

        if (w->left->recv_async(&stage_h.h, chunk_bytes, r, 0) != 0) {
            LOG_ERR("ring_allreduce failed: recv_async failed");
            return -1;
        }
        if (w->right->send_async(&r_buf_h.h, chunk_bytes, r, send_idx*chunk_bytes) != 0) {
            LOG_ERR("ring_allreduce failed: send_async failed");
            return -1;
        }
        if (w->left->poll(nullptr) != 0) {
            LOG_ERR("ring_allreduce failed: poll failed");
            return -1;
        }
        if (w->right->poll(nullptr) != 0) {
            LOG_ERR("ring_allreduce failed: poll failed");
            return -1;
        }

        // reduce
        float *dst = buf + recv_idx*chunk;
        for (size_t i = 0; i < chunk; i++) {
            dst[i] += stage[i];
        }
    }

    // all-gather; N-1 rounds
    for (int r = 0; r < N-1; r++) {
        int send_idx = (rank - r + 1 + N) % N;
        int recv_idx = (rank - r + N) % N;

        if (w->left->recv_async(&l_buf_h.h, chunk_bytes, r, recv_idx*chunk_bytes) != 0) {
            LOG_ERR("ring_allreduce failed: recv_async failed");
            return -1;
        }
        if (w->right->send_async(&r_buf_h.h, chunk_bytes, r, send_idx*chunk_bytes) != 0) {
            LOG_ERR("ring_allreduce failed: send_async failed");
            return -1;
        }
        if (w->left->poll(nullptr) != 0) {
            LOG_ERR("ring_allreduce failed: poll failed");
            return -1;
        }
        if (w->right->poll(nullptr) != 0) {
            LOG_ERR("ring_allreduce failed: poll failed");
            return -1;
        }
    }

    return 0;
}
