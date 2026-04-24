#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rdma_common.h"
#include "timing.h"
#include "bench_utils.h"
#include "logging.h"

typedef struct config {
    char *server_ip;  /* NULL = server mode, non-NULL = client mode */
    int port;
    int iters;
    int size;
} config_t;

static void config_init(config_t *cfg) {
    cfg->server_ip = NULL;
    cfg->port = 12345;
    cfg->iters = 1000;
    cfg->size = 4096;
}

static int config_parse(int argc, char *argv[], config_t *cfg) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                printf("missing value after --port\n");
                return -1;
            }
            cfg->port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--iters") == 0) {
            if (i + 1 >= argc) {
                printf("missing value after --iters\n");
                return -1;
            }
            cfg->iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0) {
            if (i + 1 >= argc) {
                printf("missing value after --size\n");
                return -1;
            }
            cfg->size = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            cfg->server_ip = argv[i];
        } else {
            printf("unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

static void config_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s [--port <port>] [--iters <n>] [--size <bytes>]\n", prog);
    printf("  %s <server_ip> [--port <port>] [--iters <n>] [--size <bytes>]\n", prog);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x>y) - (x<y);
}

int main(int argc, char *argv[]) {
    int ret = 1, i;
    uint64_t start;
    config_t cfg = {0};
    rdma_ctx_t ctx = {0};
    rdma_mr_t mr = {0};
    rdma_qp_t qp = {0};
    uint64_t *latencies = NULL;

    config_init(&cfg);
    if (config_parse(argc, argv, &cfg) != 0) {
        config_usage(argv[0]);
        goto out;
    }

    if (cfg.server_ip == NULL) {
        if (rdma_cm_server(&ctx, &qp, &mr, cfg.size, cfg.port) != 0) {
            LOG_ERR("rdma_cm_server failed");
            goto out;
        }
    } else {
        if (rdma_cm_client(&ctx, &qp, &mr, cfg.size, cfg.server_ip, cfg.port) != 0) {
            LOG_ERR("rdma_cm_client failed");
            goto out;
        }
    }

    int total_iters = kWarmup + cfg.iters;
    if (cfg.server_ip == NULL) {
        // server side: post first recv before loop so it's ready before client sends
        if (rdma_post_recv(&qp, &mr, cfg.size, 1, 0) != 0) {
            LOG_ERR("rdma post recv failed");
            goto out;
        }
        for (i = 0; i < total_iters; i++) {
            if (rdma_poll_cq(&ctx, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
            if (i + 1 < total_iters) {
                if (rdma_post_recv(&qp, &mr, cfg.size, 1, 0) != 0) {
                    LOG_ERR("rdma post recv failed");
                    goto out;
                }
            }
            if (rdma_post_send(&qp, &mr, cfg.size, 1, 0) != 0) {
                LOG_ERR("rdma post send failed");
                goto out;
            }
            if (rdma_poll_cq(&ctx, NULL) != 0) {
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
            if (rdma_post_recv(&qp, &mr, cfg.size, 1, 0) != 0) {
                LOG_ERR("rdma post recv failed");
                goto out;
            }
            start = time_now_ns();
            if (rdma_post_send(&qp, &mr, cfg.size, 1, 0) != 0) {
                LOG_ERR("rdma post send failed");
                goto out;
            }
            // send completed
            if (rdma_poll_cq(&ctx, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
            // recv completed
            if (rdma_poll_cq(&ctx, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
            if (i >= kWarmup)
                latencies[i - kWarmup] = time_elapsed_ns(start, time_now_ns());
        }

        qsort(latencies, cfg.iters, sizeof(uint64_t), cmp_u64);
        print_latency("send/recv latency (RTT)", latencies, cfg.iters);
    }

    ret = 0;
out:
    free(latencies);
    rdma_qp_destroy(&qp);
    rdma_mr_dereg(&mr);
    rdma_ctx_destroy(&ctx);
    return ret;
}