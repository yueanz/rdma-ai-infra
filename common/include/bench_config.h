#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdma_common.h"

/* Shared command-line handling for the phase1 benchmarks. One struct carries
 * every option; a benchmark that has no use for a field just ignores it. */
typedef struct bench_config {
    char           *server_ip;  /* NULL = server mode, non-NULL = client mode */
    int             port;
    int             iters;
    int             size;
    int             depth;      /* in-flight WRs; bandwidth benchmarks only */
    rai_conn_mode_t conn;
} bench_config_t;

static inline void bench_config_init(bench_config_t *cfg) {
    cfg->server_ip = NULL;
    cfg->port      = 12345;
    cfg->iters     = 1000;
    cfg->size      = 4096;
    cfg->depth     = 16;
    cfg->conn      = RAI_CONN_CM;
}

static inline void bench_config_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s [options]              (server)\n", prog);
    printf("  %s <server_ip> [options]  (client)\n", prog);
    printf("Options:\n");
    printf("  --port <port>     default 12345 (port+1 carries the OOB exchange)\n");
    printf("  --iters <n>       default 1000\n");
    printf("  --size <bytes>    default 4096\n");
    printf("  --depth <n>       default 16, max %d (bandwidth benchmarks only)\n",
           RAI_QP_MAX_WR);
    printf("  --conn cm|verbs   default cm\n");
}

static inline int bench_config_parse(int argc, char *argv[], bench_config_t *cfg) {
    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i];
        const char *val;

        if (opt[0] != '-') {
            cfg->server_ip = argv[i];
            continue;
        }
        if (strcmp(opt, "--port")  != 0 && strcmp(opt, "--iters") != 0 &&
            strcmp(opt, "--size")  != 0 && strcmp(opt, "--depth") != 0 &&
            strcmp(opt, "--conn")  != 0) {
            printf("unknown option: %s\n", opt);
            return -1;
        }
        if (i + 1 >= argc) {
            printf("missing value after %s\n", opt);
            return -1;
        }
        val = argv[++i];

        if (strcmp(opt, "--port") == 0) {
            cfg->port = atoi(val);
        } else if (strcmp(opt, "--iters") == 0) {
            cfg->iters = atoi(val);
        } else if (strcmp(opt, "--size") == 0) {
            cfg->size = atoi(val);
        } else if (strcmp(opt, "--depth") == 0) {
            cfg->depth = atoi(val);
        } else {   /* --conn, by elimination */
            if (strcmp(val, "cm") == 0) {
                cfg->conn = RAI_CONN_CM;
            } else if (strcmp(val, "verbs") == 0) {
                cfg->conn = RAI_CONN_VERBS;
            } else {
                printf("unknown --conn value: %s (want cm or verbs)\n", val);
                return -1;
            }
        }
    }

    /* atoi returns 0 on garbage, so these also catch non-numeric input.
     * Unchecked, a negative --iters wraps when passed to qsort as size_t. */
    if (cfg->port < 1 || cfg->port > 65534) {
        printf("--port must be 1..65534 (port+1 must be valid too)\n");
        return -1;
    }
    if (cfg->iters < 1) {
        printf("--iters must be >= 1\n");
        return -1;
    }
    if (cfg->size < 1) {
        printf("--size must be >= 1\n");
        return -1;
    }
    if (cfg->depth < 1 || cfg->depth > RAI_QP_MAX_WR) {
        printf("--depth must be 1..%d\n", RAI_QP_MAX_WR);
        return -1;
    }
    return 0;
}

/* Connection setup, dispatched on cfg->conn. The two modes have identical
 * call shapes: listen leaves the QP able to accept recv WRs but not yet
 * reachable, accept makes it live, connect does both on the client side. */
static inline int bench_listen(rai_qp_t *qp, const bench_config_t *cfg, int *mr_listen_fd) {
    return cfg->conn == RAI_CONN_VERBS
        ? rai_verbs_listen_qp(qp, cfg->port, mr_listen_fd)
        : rai_cm_listen_qp(qp, cfg->port, mr_listen_fd);
}

static inline int bench_accept(rai_qp_t *qp, const bench_config_t *cfg) {
    return cfg->conn == RAI_CONN_VERBS
        ? rai_verbs_accept_qp(qp)
        : rai_cm_accept_qp(qp);
}

static inline int bench_connect(rai_qp_t *qp, const bench_config_t *cfg) {
    return cfg->conn == RAI_CONN_VERBS
        ? rai_verbs_connect_qp(qp, cfg->server_ip, cfg->port)
        : rai_cm_connect_qp(qp, cfg->server_ip, cfg->port);
}
