#include "collective.hpp"
#include <vector>

int ring_allreduce(World *w, BufferHandle *r_h, BufferHandle *l_h, BufferHandle *stage_h,
                    float *buf, float *stage, size_t count) {
    int N = w->size;
    int rank = w->rank;
    size_t chunk = count / N;
    size_t chunk_bytes = chunk * sizeof(float);

    if (count % N != 0) {
        LOG_ERR("ring_allreduce: count %zu not divisible by world size %d", count, N);
        return -1;
    }

    // reduce-scatter: N-1 rounds
    for (int r = 0; r < N-1; r++) {
        int send_idx = (rank - r + N) % N;
        int recv_idx = (rank - r - 1 + N) % N;

        /* parity stagger: even ranks send first to avoid deadlock with blocking TCP recv */
        if (rank % 2 == 0) {
            if (w->right->send_async(r_h, chunk_bytes, r, send_idx*chunk_bytes) != 0) {
                LOG_ERR("ring_allreduce failed: send_async failed");
                return -1;
            }
            if (w->left->recv_async(stage_h, chunk_bytes, r, 0) != 0) {
                LOG_ERR("ring_allreduce failed: recv_async failed");
                return -1;
            }
        } else {
            if (w->left->recv_async(stage_h, chunk_bytes, r, 0) != 0) {
                LOG_ERR("ring_allreduce failed: recv_async failed");
                return -1;
            }
            if (w->right->send_async(r_h, chunk_bytes, r, send_idx*chunk_bytes) != 0) {
                LOG_ERR("ring_allreduce failed: send_async failed");
                return -1;
            }
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
        
        /* same parity stagger as reduce-scatter */
        if (rank % 2 == 0) {
            if (w->right->send_async(r_h, chunk_bytes, r, send_idx*chunk_bytes) != 0) {
                LOG_ERR("ring_allreduce failed: send_async failed");
                return -1;
            }
            if (w->left->recv_async(l_h, chunk_bytes, r, recv_idx*chunk_bytes) != 0) {
                LOG_ERR("ring_allreduce failed: recv_async failed");
                return -1;
            }
        } else {
            if (w->left->recv_async(l_h, chunk_bytes, r, recv_idx*chunk_bytes) != 0) {
                LOG_ERR("ring_allreduce failed: recv_async failed");
                return -1;
            }
            if (w->right->send_async(r_h, chunk_bytes, r, send_idx*chunk_bytes) != 0) {
                LOG_ERR("ring_allreduce failed: send_async failed");
                return -1;
            }
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
