#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rdma_common.h"
#include "timing.h"
#include "bench_utils.h"
#include "bench_config.h"
#include "logging.h"

/* Request/response over one-sided writes, so the result is directly comparable
 * to lat_send_recv's RTT — same round trip, the only difference being that the
 * responder's CPU never touches a queue pair, it just watches a byte of its own
 * memory. This is also the shape perftest's ib_write_lat uses.
 *
 * The two directions need separate memory or the reply would clobber the
 * request's doorbell, so the MR is split in half: the low half carries
 * client -> server, the high half carries server -> client. Each side polls the
 * half the other one writes. */
int main(int argc, char *argv[]) {
    int ret = 1, i;
    uint64_t iter_start;
    bench_config_t cfg;
    rai_mr_t mr = {0};
    rai_qp_t qp = {0};
    uint64_t *latencies = NULL;
    volatile uint8_t *post_buf, *poll_buf;
    size_t reply_off;
    int mr_listen_fd = -1;

    bench_config_init(&cfg);
    if (bench_config_parse(argc, argv, &cfg) != 0) {
        bench_config_usage(argv[0]);
        goto out;
    }
    reply_off = (size_t)cfg.size;

    if (cfg.server_ip == NULL) {
        if (bench_listen(&qp, &cfg, &mr_listen_fd) != 0) {
            LOG_ERR("listen failed");
            goto out;
        }
        if (rai_mr_reg(&qp, &mr, 2 * reply_off) != 0) {
            LOG_ERR("rai_mr_reg failed");
            goto out;
        }
        if (bench_accept(&qp, &cfg) != 0) {
            LOG_ERR("accept failed");
            goto out;
        }
    } else {
        if (bench_connect(&qp, &cfg) != 0) {
            LOG_ERR("connect failed");
            goto out;
        }
        if (rai_mr_reg(&qp, &mr, 2 * reply_off) != 0) {
            LOG_ERR("rai_mr_reg failed");
            goto out;
        }
    }

    /* Zero both doorbells before the peer can reach this buffer — it cannot
     * write until it learns addr/rkey from the exchange below, and the memory
     * starts out as uninitialized heap. */
    memset(mr.buf, 0, 2 * reply_off);

    /* One-sided write needs the peer's addr/rkey. Fill in our own side first —
     * the OOB helpers send qp.local and receive into qp.remote. */
    qp.local.addr = (uint64_t)(uintptr_t)mr.mr->addr;
    qp.local.rkey = mr.mr->rkey;
    if (cfg.server_ip == NULL) {
        if (rai_oob_accept(mr_listen_fd, &qp) != 0) {
            LOG_ERR("rai_oob_accept failed");
            goto out;
        }
    } else {
        if (rai_oob_connect(&qp, cfg.server_ip, cfg.port + 1) != 0) {
            LOG_ERR("rai_oob_connect failed");
            goto out;
        }
    }

    /* The last byte of each half doubles as a doorbell carrying the iteration
     * number. A receiver never writes the byte it polls — it waits for its own
     * next expected value, so there is no reset to race with the sender's
     * following write. Truncating to 8 bits is safe because neither side can
     * start an iteration before the previous round trip closed.
     *
     * Polling the *last* byte works because an RC RDMA write lands in
     * increasing address order: packets are delivered in PSN order and the
     * HCA's DMA writes are not reordered on the bus. Adaptive routing or
     * relaxed-ordering MRs weaken that, and then the doorbell has to be
     * replaced by a trailing RDMA_WRITE_WITH_IMM so the receiver gets a real
     * completion instead (this is what NCCL does). Neither applies to a
     * single-QP back-to-back link with no switch in between. */
    int total_iters = kWarmup + cfg.iters;
    if (cfg.server_ip == NULL) {
        // server side: poll the low half, reply into the high half
        poll_buf = (uint8_t *)mr.buf + reply_off - 1;
        post_buf = (uint8_t *)mr.buf + 2 * reply_off - 1;
        for (i = 0; i < total_iters; i++) {
            uint8_t seq = (uint8_t)(i + 1);
            while (*poll_buf != seq)
                CPU_RELAX();
            *post_buf = seq;
            if (rai_post_write(&qp, &mr, cfg.size, IBV_SEND_SIGNALED,
                            qp.remote.addr + reply_off, qp.remote.rkey, 1, reply_off) != 0) {
                LOG_ERR("rdma post write failed");
                goto out;
            }
            if (rai_poll_cq(&qp, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
        }
    } else {
        // client side: write into the low half, poll the high half
        post_buf = (uint8_t *)mr.buf + reply_off - 1;
        poll_buf = (uint8_t *)mr.buf + 2 * reply_off - 1;
        latencies = malloc(cfg.iters * sizeof(uint64_t));
        if (latencies == NULL) {
            LOG_ERR("latencies malloc failed");
            goto out;
        }

        for (i = 0; i < total_iters; i++) {
            uint8_t seq = (uint8_t)(i + 1);
            *post_buf = seq;
            iter_start = time_now_ns();
            if (rai_post_write(&qp, &mr, cfg.size, IBV_SEND_SIGNALED,
                            qp.remote.addr, qp.remote.rkey, 1, 0) != 0) {
                LOG_ERR("rdma post write failed");
                goto out;
            }
            if (rai_poll_cq(&qp, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
            while (*poll_buf != seq)
                CPU_RELAX();
            if (i >= kWarmup)
                latencies[i - kWarmup] = time_elapsed_ns(iter_start, time_now_ns());
        }
        qsort(latencies, cfg.iters, sizeof(uint64_t), cmp_u64);
        print_latency("rdma write RTT (one-sided ping-pong)", latencies, cfg.iters);
    }

    /* The client leaves as soon as it sees the reply's doorbell, but that only
     * means the data landed — the server is still waiting on the ACK for that
     * same write. Tearing down first would strand it, so the client blocks in
     * connect() until the server has reaped its completion and reached accept().
     * bw_rdma_write needs the same barrier for the mirror-image reason. */
    if (cfg.server_ip == NULL) {
        if (rai_oob_accept(mr_listen_fd, &qp) != 0) {
            LOG_ERR("teardown sync failed");
            goto out;
        }
    } else {
        if (rai_oob_connect(&qp, cfg.server_ip, cfg.port + 1) != 0) {
            LOG_ERR("teardown sync failed");
            goto out;
        }
    }

    ret = 0;
out:
    if (mr_listen_fd >= 0)
        close(mr_listen_fd);
    free(latencies);
    rai_mr_dereg(&mr);    /* MR depends on PD — must dereg before qp_destroy */
    rai_qp_destroy(&qp);
    return ret;
}