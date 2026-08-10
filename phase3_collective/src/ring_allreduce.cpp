#include "collective.hpp"
#include <vector>

/*
 * Which chunk each step touches. Reduce-scatter runs first for N-1 steps and
 * all-gather for another N-1, so step s at or past N-1 is all-gather round
 * s-(N-1).
 */
static inline int send_chunk(int rank, int N, int s) {
    return s < N - 1 ? (rank - s + N) % N
                     : (rank - (s - (N - 1)) + 1 + N) % N;
}
static inline int recv_chunk(int rank, int N, int s) {
    return s < N - 1 ? (rank - s - 1 + N) % N
                     : (rank - (s - (N - 1)) + N) % N;
}

/*
 * Blocking-receive path (TCP).
 *
 * recv() does not return until the data is there, so both peers calling it
 * first would deadlock; even ranks send first to break the symmetry. Nothing
 * can be readied in advance here — there is no posting, only waiting — so
 * this path pays the peer's compute time on every round.
 */
static int ring_blocking(World *w, BufferHandle *r_h, BufferHandle *l_h,
                         BufferHandle *stage_h, float *buf, float *stage,
                         size_t chunk) {
    int N = w->size, rank = w->rank;
    size_t chunk_bytes = chunk * sizeof(float);

    for (int s = 0; s < 2 * (N - 1); s++) {
        bool scatter   = s < N - 1;
        size_t send_off = (size_t)send_chunk(rank, N, s) * chunk_bytes;
        BufferHandle *rh = scatter ? stage_h : l_h;
        size_t recv_off  = scatter ? 0
                                   : (size_t)recv_chunk(rank, N, s) * chunk_bytes;

        if (rank % 2 == 0) {
            if (w->right->send_async(r_h, chunk_bytes, s, send_off) != 0 ||
                w->left->recv_async(rh, chunk_bytes, s, recv_off) != 0) {
                LOG_ERR("ring_allreduce failed at step %d", s);
                return -1;
            }
        } else {
            if (w->left->recv_async(rh, chunk_bytes, s, recv_off) != 0 ||
                w->right->send_async(r_h, chunk_bytes, s, send_off) != 0) {
                LOG_ERR("ring_allreduce failed at step %d", s);
                return -1;
            }
        }
        if (w->left->poll(nullptr) != 0 || w->right->poll(nullptr) != 0) {
            LOG_ERR("ring_allreduce failed: poll at step %d", s);
            return -1;
        }
        if (scatter) {
            float *dst = buf + (size_t)recv_chunk(rank, N, s) * chunk;
            for (size_t i = 0; i < chunk; i++)
                dst[i] += stage[i];
        }
    }
    return 0;
}

/*
 * Posted-receive path (RDMA).
 *
 * The receive for the next step goes up as soon as this step's data lands —
 * before this step's sum is folded in. The peer does not wait for us: it
 * finishes its own sum and sends, and if our queue has nothing posted the
 * message is refused with an RNR NAK and the sender backs off for
 * min_rnr_timer before retrying. Since the retry only succeeds once we do
 * post, the stall lasts as long as our own compute. At 1 MB that was ~500 us
 * a round against ~160 us of actual transfer.
 *
 * Reduce-scatter alternates between the two halves of `stage` for exactly
 * this reason: the next receive is in flight while the previous one is still
 * being summed. All-gather receives land in distinct chunks of `buf` and need
 * no staging.
 */
static int ring_posted(World *w, BufferHandle *r_h, BufferHandle *l_h,
                       BufferHandle *stage_h, float *buf, float *stage,
                       size_t chunk) {
    int N = w->size, rank = w->rank;
    size_t chunk_bytes = chunk * sizeof(float);
    int steps = 2 * (N - 1);

    auto post_recv = [&](int s) -> int {
        if (s < N - 1)
            return w->left->recv_async(stage_h, chunk_bytes, s,
                                       (size_t)(s % 2) * chunk_bytes);
        return w->left->recv_async(l_h, chunk_bytes, s,
                                   (size_t)recv_chunk(rank, N, s) * chunk_bytes);
    };

    if (post_recv(0) != 0) {
        LOG_ERR("ring_allreduce failed: initial recv_async failed");
        return -1;
    }

    for (int s = 0; s < steps; s++) {
        if (w->right->send_async(r_h, chunk_bytes, s,
                                 (size_t)send_chunk(rank, N, s) * chunk_bytes) != 0) {
            LOG_ERR("ring_allreduce failed: send_async at step %d", s);
            return -1;
        }
        if (w->left->poll(nullptr) != 0) {
            LOG_ERR("ring_allreduce failed: recv poll at step %d", s);
            return -1;
        }
        /* Before the sum below, not after — that ordering is the point. */
        if (s + 1 < steps && post_recv(s + 1) != 0) {
            LOG_ERR("ring_allreduce failed: recv_async at step %d", s + 1);
            return -1;
        }
        if (w->right->poll(nullptr) != 0) {
            LOG_ERR("ring_allreduce failed: send poll at step %d", s);
            return -1;
        }
        if (s < N - 1) {
            float *dst = buf + (size_t)recv_chunk(rank, N, s) * chunk;
            const float *src = stage + (size_t)(s % 2) * chunk;
            for (size_t i = 0; i < chunk; i++)
                dst[i] += src[i];
        }
    }
    return 0;
}

int ring_allreduce(World *w, BufferHandle *r_h, BufferHandle *l_h, BufferHandle *stage_h,
                    float *buf, float *stage, size_t count) {
    if (count % w->size != 0) {
        LOG_ERR("ring_allreduce: count %zu not divisible by world size %d",
                count, w->size);
        return -1;
    }
    if (w->size < 2)
        return 0;

    size_t chunk = count / w->size;
    return w->left->recv_blocks()
        ? ring_blocking(w, r_h, l_h, stage_h, buf, stage, chunk)
        : ring_posted(w, r_h, l_h, stage_h, buf, stage, chunk);
}
