#include <stdlib.h>
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
    int depth;
} config_t;

static void config_init(config_t *cfg) {
    cfg->server_ip = NULL;
    cfg->port = 12345;
    cfg->iters = 1000;
    cfg->size = 4096;
    cfg->depth = 16;
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
        } else if (strcmp(argv[i], "--depth") == 0) {
            if (i + 1 >= argc) {
                printf("missing value after --depth\n");
                return -1;
            }
            cfg->depth = atoi(argv[++i]);
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
    printf("  %s [--port <port>] [--iters <n>] [--size <bytes>] [--depth <n>]\n", prog);
    printf("  %s <server_ip> [--port <port>] [--iters <n>] [--size <bytes>] [--depth <n>]\n", prog);
}


int main(int argc, char *argv[]) {
    int ret = 1, i;
    uint64_t bw_start = 0, total_time;
    config_t cfg = {0};
    rai_ctx_t ctx = {0};
    rai_mr_t mr = {0};
    rai_qp_t qp = {0};
    volatile uint8_t *doorbell;
    uint32_t send_flags;

    config_init(&cfg);
    if (config_parse(argc, argv, &cfg) != 0) {
        config_usage(argv[0]);
        goto out;
    }

    if (cfg.server_ip == NULL) {
        if (rai_cm_server(&ctx, &qp, &mr, cfg.size, cfg.port) != 0) {
            LOG_ERR("rai_cm_server failed");
            goto out;
        }
    } else {
        if (rai_cm_client(&ctx, &qp, &mr, cfg.size, cfg.server_ip, cfg.port) != 0) {
            LOG_ERR("rai_cm_client failed");
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
            if ((send_flags & IBV_SEND_SIGNALED) && rai_poll_cq(&ctx, NULL) != 0) {
                LOG_ERR("rdma poll completion queue failed");
                goto out;
            }
        }
        total_time = time_elapsed_ns(bw_start, time_now_ns());
        print_bandwidth("rdma write throughput", (uint64_t)cfg.size*cfg.iters, total_time);
    }

    ret = 0;
out:
    rai_qp_destroy(&qp);
    rai_mr_dereg(&mr);
    rai_ctx_destroy(&ctx);
    return ret;
}