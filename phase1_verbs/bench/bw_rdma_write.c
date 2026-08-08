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
    uint64_t bw_start = 0, total_time;
    bench_config_t cfg;
    rai_mr_t mr = {0};
    rai_qp_t qp = {0};
    volatile uint8_t *doorbell;
    uint32_t send_flags;
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
    *doorbell = 0;
    if (cfg.server_ip == NULL) {
        // server side: only wait for the last write's doorbell
        while (*doorbell == 0)
            CPU_RELAX();
    } else {
        // client side: only set doorbell on the last iteration
        for (i = 0; i < total_iters; i++) {
            if (i == total_iters - 1) *doorbell = 1;
            if (i == kWarmup) bw_start = time_now_ns();  // start bandwidth timer after warmup
            send_flags = ((i+1) % cfg.depth == 0 || i == total_iters-1) ? IBV_SEND_SIGNALED : 0;
            if (rai_post_write(&qp, &mr, cfg.size, send_flags,
                            qp.remote.addr, qp.remote.rkey, 1, 0) != 0) {
                LOG_ERR("rdma post write failed");
                goto out;
            }
            if ((send_flags & IBV_SEND_SIGNALED) && rai_poll_cq(&qp, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
        }
        total_time = time_elapsed_ns(bw_start, time_now_ns());
        print_bandwidth("rdma write throughput", (uint64_t)cfg.size*cfg.iters, total_time);
    }

    ret = 0;
out:
    if (mr_listen_fd >= 0)
        close(mr_listen_fd);
    rai_mr_dereg(&mr);    /* MR depends on PD — must dereg before qp_destroy */
    rai_qp_destroy(&qp);
    return ret;
}