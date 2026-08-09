#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rdma_common.h"
#include "timing.h"
#include "bench_utils.h"
#include "bench_config.h"
#include "logging.h"

int main(int argc, char *argv[]) {
    int ret = 1, i;
    uint64_t iter_start;
    bench_config_t cfg;
    rai_mr_t mr = {0};
    rai_qp_t qp = {0};
    uint64_t *latencies = NULL;
    volatile uint8_t *doorbell;
    int mr_listen_fd = -1;

    bench_config_init(&cfg);
    if (bench_config_parse(argc, argv, &cfg) != 0) {
        bench_config_usage(argv[0]);
        goto out;
    }

    if (cfg.server_ip == NULL) {
        if (bench_listen(&qp, &cfg, &mr_listen_fd) != 0) {
            LOG_ERR("listen failed");
            goto out;
        }
        if (rai_mr_reg(&qp, &mr, cfg.size) != 0) {
            LOG_ERR("rai_mr_reg failed");
            goto out;
        }
        /* Clear the doorbell while the QP is still in INIT. The buffer comes
         * from malloc, and a stale byte here would read as an arrival. */
        ((uint8_t *)mr.buf)[cfg.size - 1] = 0;
        if (bench_accept(&qp, &cfg) != 0) {
            LOG_ERR("accept failed");
            goto out;
        }
    } else {
        if (bench_connect(&qp, &cfg) != 0) {
            LOG_ERR("connect failed");
            goto out;
        }
        if (rai_mr_reg(&qp, &mr, cfg.size) != 0) {
            LOG_ERR("rai_mr_reg failed");
            goto out;
        }
    }

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

    /* The last byte of the payload doubles as a doorbell carrying the
     * iteration number. The receiver never writes it — it waits for its own
     * next expected value, so there is no reset to race with the sender's
     * following write. Truncating to 8 bits is safe because the sender waits
     * for each completion and so can never run 256 iterations ahead.
     *
     * Polling the *last* byte works because an RC RDMA write lands in
     * increasing address order: packets are delivered in PSN order and the
     * HCA's DMA writes are not reordered on the bus. Adaptive routing or
     * relaxed-ordering MRs weaken that, and then the doorbell has to be
     * replaced by a trailing RDMA_WRITE_WITH_IMM so the receiver gets a real
     * completion instead (this is what NCCL does). Neither applies to a
     * single-QP back-to-back link with no switch in between. */
    int total_iters = kWarmup + cfg.iters;
    doorbell = (uint8_t *)mr.buf + cfg.size - 1;
    if (cfg.server_ip == NULL) {
        // server side
        for (i = 0; i < total_iters; i++) {
            uint8_t expect = (uint8_t)(i + 1);
            while (*doorbell != expect)
                CPU_RELAX();
        }
    } else {
        // client side
        latencies = malloc(cfg.iters * sizeof(uint64_t));
        if (latencies == NULL) {
            LOG_ERR("latencies malloc failed");
            goto out;
        }

        for (i = 0; i < total_iters; i++) {
            *doorbell = (uint8_t)(i + 1);
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
            if (i >= kWarmup)
                latencies[i - kWarmup] = time_elapsed_ns(iter_start, time_now_ns());
        }
        qsort(latencies, cfg.iters, sizeof(uint64_t), cmp_u64);
        print_latency("rdma write latency (one-sided)", latencies, cfg.iters);
    }

    /* Seeing the last doorbell only means the data landed; the ACK may still
     * be on its way back. Hold the server until the client has reaped it, the
     * same way bw_rdma_write does — only one write is ever outstanding here,
     * so the margin is wide, but the race is the same shape. */
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