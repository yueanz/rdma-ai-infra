#include <stdio.h>
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
        if (bench_listen(&qp, &cfg, NULL) != 0) {
            LOG_ERR("listen failed");
            goto out;
        }
        if (rai_mr_reg(&qp, &mr, cfg.size) != 0) {
            LOG_ERR("rai_mr_reg failed");
            goto out;
        }
        /* Pre-post before accept: the client can send the instant the QP
         * reaches RTS. The first loop iteration consumes this WR. */
        if (rai_post_recv(&qp, &mr, cfg.size, 1, 0) != 0) {
            LOG_ERR("rai_post_recv (pre-post) failed");
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

    int total_iters = kWarmup + cfg.iters;
    if (cfg.server_ip == NULL) {
        for (i = 0; i < total_iters; i++) {
            if (rai_poll_cq(&qp, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
            if (i + 1 < total_iters) {
                if (rai_post_recv(&qp, &mr, cfg.size, 1, 0) != 0) {
                    LOG_ERR("rdma post recv failed");
                    goto out;
                }
            }
            if (rai_post_send(&qp, &mr, cfg.size, 1, 0) != 0) {
                LOG_ERR("rdma post send failed");
                goto out;
            }
            if (rai_poll_cq(&qp, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
        }
    } else {
        // client side
        latencies = malloc(cfg.iters * sizeof(uint64_t));
        if (latencies == NULL) {
            LOG_ERR("latencies malloc failed");
            goto out;
        }

        for (i = 0; i < total_iters; i++) {
            if (rai_post_recv(&qp, &mr, cfg.size, 1, 0) != 0) {
                LOG_ERR("rdma post recv failed");
                goto out;
            }
            iter_start = time_now_ns();
            if (rai_post_send(&qp, &mr, cfg.size, 1, 0) != 0) {
                LOG_ERR("rdma post send failed");
                goto out;
            }
            /* Send and recv share one CQ, so these two completions can
             * arrive in either order. Only their total matters here. */
            if (rai_poll_cq(&qp, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
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
        bench_report_latency("lat_send_recv", "send/recv latency (RTT)",
                             &cfg, latencies, cfg.iters);
    }

    ret = 0;
out:
    free(latencies);
    rai_mr_dereg(&mr);    /* MR depends on PD — must dereg before qp_destroy */
    rai_qp_destroy(&qp);
    return ret;
}