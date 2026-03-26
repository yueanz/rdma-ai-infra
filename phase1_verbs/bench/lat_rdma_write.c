#include <stdlib.h>
#include "rdma_common.h"
#include "timing.h"
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
    cfg->size = 64;
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
    volatile uint8_t *doorbell;

    config_init(&cfg);
    if (config_parse(argc, argv, &cfg) != 0) {
        config_usage(argv[0]);
        goto out;
    }

    if (rdma_ctx_init(&ctx, 1, 1) != 0) {
        LOG_ERR("context init failed");
        goto out;
    }

    if (rdma_mr_reg(&ctx, &mr, cfg.size) != 0) {
        LOG_ERR("memory region reg failed");
        goto out;
    }

    if (rdma_qp_create(&ctx, &qp) != 0) {
        LOG_ERR("queue pair create failed");
        goto out;
    }

    if (rdma_qp_init(&ctx, &qp) != 0) {
        LOG_ERR("queue pair init failed");
        goto out;
    }

    qp.local.addr = (uint64_t)(uintptr_t)mr.mr->addr;
    qp.local.rkey = mr.mr->rkey;

    if (cfg.server_ip == NULL && rdma_exchange_info_server(&qp, cfg.port) != 0) {
        LOG_ERR("rdma exchange info server failed");
        goto out;
    }

    if (cfg.server_ip != NULL && rdma_exchange_info_client(&qp, cfg.server_ip, cfg.port) != 0) {
        LOG_ERR("rdma exchange info client failed");
        goto out;
    }

    if (rdma_qp_connect(&ctx, &qp) != 0) {
        LOG_ERR("rdma queue pair connect failed");
        goto out;
    }

    if (cfg.server_ip == NULL) {
        // server side
        doorbell = (uint8_t *)mr.buf + cfg.size - 1;
        *doorbell = 0;
        for (i = 0; i < cfg.iters; i++) {
            while (*doorbell == 0)
                __builtin_ia32_pause();
            *doorbell = 0;
        }
    } else {
        // client side
        latencies = malloc(cfg.iters * sizeof(uint64_t));
        if (latencies == NULL) {
            LOG_ERR("latencies malloc failed");
            goto out;
        }

        doorbell = (uint8_t *)mr.buf + cfg.size - 1;
        for (i = 0; i < cfg.iters; i++) {
            *doorbell = 1;
            start = time_now_ns();
            if (rdma_post_write(&qp, &mr, cfg.size) != 0) {
                LOG_ERR("rdma post write failed");
                goto out;
            }
            if (rdma_poll_cq(&ctx) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
            latencies[i] = time_elapsed_ns(start, time_now_ns());
        }
        qsort(latencies, cfg.iters, sizeof(uint64_t), cmp_u64);
        printf("latency (RTT)\n");
        printf("%-10s %-10s %-10s %-10s\n", "min(us)", "median(us)", "p99(us)", "max(us)");
        printf("%-10.2f %-10.2f %-10.2f %-10.2f\n",
            ns_to_us(latencies[0]),
            ns_to_us(latencies[cfg.iters / 2]),
            ns_to_us(latencies[(int)(cfg.iters * 0.99)]),
            ns_to_us(latencies[cfg.iters - 1]));
    }

    ret = 0;
out:
    free(latencies);
    rdma_qp_destroy(&qp);
    rdma_mr_dereg(&mr);
    rdma_ctx_destroy(&ctx);
    return ret;
}