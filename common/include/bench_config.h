#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rdma_common.h"
#include "bench_utils.h"

/* Column order for --csv. Kept next to the emitters so a new column cannot be
 * added to one without the other. */
#define BENCH_CSV_HEADER \
    "bench,conn,size,depth,iters,min_us,median_us,p99_us,max_us,gbps"

/* Shared command-line handling for the phase1 benchmarks. One struct carries
 * every option; a benchmark that has no use for a field just ignores it. */
typedef struct bench_config {
    char           *server_ip;  /* NULL = server mode, non-NULL = client mode */
    int             port;
    int             iters;
    int             size;
    int             depth;      /* WRs kept in flight: send window (bandwidth)
                                   or receive queue depth (lat_send_recv) */
    rai_conn_mode_t conn;
    int             csv;        /* emit one machine-readable row instead */
    int             csv_header; /* print the header and exit */
} bench_config_t;

static inline void bench_config_init(bench_config_t *cfg) {
    cfg->server_ip  = NULL;
    cfg->port       = 12345;
    cfg->iters      = 1000;
    cfg->size       = 4096;
    cfg->depth      = 16;
    cfg->conn       = RAI_CONN_CM;
    cfg->csv        = 0;
    cfg->csv_header = 0;
}

static inline void bench_config_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s [options]              (server)\n", prog);
    printf("  %s <server_ip> [options]  (client)\n", prog);
    printf("Options:\n");
    printf("  --port <port>     default 12345 (port+1 carries the OOB exchange)\n");
    printf("  --iters <n>       default 1000\n");
    printf("  --size <bytes>    default 4096\n");
    printf("  --depth <n>       default 16, max %d. WRs kept in flight: send window for\n"
           "                    bandwidth tests, receive queue depth for lat_send_recv\n",
           RAI_QP_MAX_WR - 1);
    printf("  --conn cm|verbs   default cm\n");
    printf("  --csv             emit one CSV row instead of a table\n");
    printf("  --csv-header      print the CSV header and exit\n");
}

static inline int bench_config_parse(int argc, char *argv[], bench_config_t *cfg) {
    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i];
        const char *val;

        if (opt[0] != '-') {
            cfg->server_ip = argv[i];
            continue;
        }
        if (strcmp(opt, "--csv") == 0) {
            cfg->csv = 1;
            continue;
        }
        if (strcmp(opt, "--csv-header") == 0) {
            cfg->csv_header = 1;
            continue;
        }
        /* Every option below takes exactly one value, so consume it up front
         * and let the dispatch be the single place each option is named. */
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
        } else if (strcmp(opt, "--conn") == 0) {
            if (strcmp(val, "cm") == 0) {
                cfg->conn = RAI_CONN_CM;
            } else if (strcmp(val, "verbs") == 0) {
                cfg->conn = RAI_CONN_VERBS;
            } else {
                printf("unknown --conn value: %s (want cm or verbs)\n", val);
                return -1;
            }
        } else {
            printf("unknown option: %s\n", opt);
            return -1;
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
    /* A work queue is a ring buffer that keeps one slot free to tell full from
     * empty, so the last of RAI_QP_MAX_WR entries can never be posted. */
    if (cfg->depth < 1 || cfg->depth >= RAI_QP_MAX_WR) {
        printf("--depth must be 1..%d\n", RAI_QP_MAX_WR - 1);
        return -1;
    }
    return 0;
}

static inline const char *bench_conn_name(const bench_config_t *cfg) {
    return cfg->conn == RAI_CONN_VERBS ? "verbs" : "cm";
}

/* Latency and bandwidth benchmarks share the CSV schema, so each fills only
 * its own columns and leaves the other's empty. `name` identifies the run in
 * CSV; `label` is the human-readable title. */
static inline void bench_report_latency(const char *name, const char *label,
                                        const bench_config_t *cfg,
                                        uint64_t *sorted, int n) {
    if (!cfg->csv) {
        print_latency(label, sorted, n);
        return;
    }
    lat_stats_t s = latency_stats(sorted, n);
    printf("%s,%s,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,\n",
           name, bench_conn_name(cfg), cfg->size, cfg->depth, cfg->iters,
           s.min_us, s.median_us, s.p99_us, s.max_us);
}

static inline void bench_report_bandwidth(const char *name, const char *label,
                                          const bench_config_t *cfg,
                                          uint64_t total_bytes, uint64_t elapsed_ns) {
    if (!cfg->csv) {
        print_bandwidth(label, total_bytes, elapsed_ns);
        return;
    }
    printf("%s,%s,%d,%d,%d,,,,,%.3f\n",
           name, bench_conn_name(cfg), cfg->size, cfg->depth, cfg->iters,
           bandwidth_gbps(total_bytes, elapsed_ns));
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
