#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rdma_common.h"
#include "timing.h"
#include "bench_utils.h"
#include "bench_config.h"
#include "logging.h"

#define CQ_POLL_BATCH 16

int main(int argc, char *argv[]) {
    int ret = 1;
    uint64_t bw_start = 0, total_time;
    bench_config_t cfg;
    rai_mr_t mr = {0};
    rai_qp_t qp = {0};
    volatile uint8_t *doorbell;
    int mr_listen_fd = -1;

    bench_config_init(&cfg);
    if (bench_config_parse(argc, argv, &cfg) != 0) {
        bench_config_usage(argv[0]);
        goto out;
    }
    if (cfg.csv_header) {
        printf("%s\n", BENCH_CSV_HEADER);
        return 0;
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
        /* Clear the doorbell while the QP is still in INIT. Clearing it after
         * the QP goes live risks wiping the last write's flag and spinning
         * forever, and the buffer starts out as uninitialized heap anyway. */
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

    int total_iters = kWarmup + cfg.iters;
    doorbell = (uint8_t *)mr.buf + cfg.size - 1;
    if (cfg.server_ip == NULL) {
        // server side: only wait for the last write's doorbell
        while (*doorbell == 0)
            CPU_RELAX();
    } else {
        /* Keep `depth` writes outstanding, rather than posting `depth` and then
         * blocking until they drain. A send completion only arrives once the
         * peer has acknowledged, so blocking for one empties the pipeline every
         * `depth` writes and leaves the link idle for that ack — which is why
         * small depths used to fall far short of line rate. Post against a
         * window and reap without blocking, the way perftest's run_iter_bw
         * does, and depth means "how much is in flight" and nothing else. */
        struct ibv_wc wc[CQ_POLL_BATCH];
        uint64_t scnt = 0, ccnt = 0;

        *doorbell = 0;
        while (ccnt < (uint64_t)total_iters) {
            while (scnt < (uint64_t)total_iters &&
                   scnt - ccnt < (uint64_t)cfg.depth) {
                if (scnt == (uint64_t)total_iters - 1) *doorbell = 1;
                if (scnt == (uint64_t)kWarmup) bw_start = time_now_ns();
                if (rai_post_write(&qp, &mr, cfg.size, IBV_SEND_SIGNALED,
                                qp.remote.addr, qp.remote.rkey, 1, 0) != 0) {
                    LOG_ERR("rdma post write failed");
                    goto out;
                }
                scnt++;
            }
            int ne = ibv_poll_cq(qp.cq, CQ_POLL_BATCH, wc);
            if (ne < 0) {
                LOG_ERR("ibv_poll_cq failed");
                goto out;
            }
            for (int k = 0; k < ne; k++) {
                if (wc[k].status != IBV_WC_SUCCESS) {
                    LOG_ERR("write completion error: %s", ibv_wc_status_str(wc[k].status));
                    goto out;
                }
            }
            ccnt += (uint64_t)ne;
        }
        total_time = time_elapsed_ns(bw_start, time_now_ns());
        bench_report_bandwidth("bw_rdma_write", "rdma write throughput",
                               &cfg, (uint64_t)cfg.size*cfg.iters, total_time);
    }

    /* The doorbell only tells the server that the last write's *data* landed,
     * which happens before the client has reaped the matching completion. A
     * server that tears down at that point strands the client mid-ack, so hold
     * both sides until the client is actually done. */
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
    rai_mr_dereg(&mr);    /* MR depends on PD — must dereg before qp_destroy */
    rai_qp_destroy(&qp);
    return ret;
}