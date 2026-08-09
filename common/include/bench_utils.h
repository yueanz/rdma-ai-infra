#pragma once
#include <stdint.h>
#include <stdio.h>
#include "timing.h"

#define kWarmup 20

/* qsort comparator for the latency samples print_latency expects sorted. */
static inline int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

typedef struct lat_stats {
    double min_us, median_us, p99_us, max_us;
} lat_stats_t;

/* samples must be sorted ascending.
 *
 * Percentile indexing: for percentile p of n samples, the 0-indexed
 * position is (int)((n - 1) * p/100). This puts p99 at samples[98]
 * for n=100 (the 99th value out of 100), not samples[99] (which would
 * always equal max). */
static inline lat_stats_t latency_stats(uint64_t *samples, int n) {
    lat_stats_t s = {0, 0, 0, 0};
    if (n <= 0)
        return s;
    s.min_us    = ns_to_us(samples[0]);
    s.median_us = ns_to_us(samples[(n - 1) / 2]);
    s.p99_us    = ns_to_us(samples[(int)((n - 1) * 0.99)]);
    s.max_us    = ns_to_us(samples[n - 1]);
    return s;
}

static inline double bandwidth_gbps(uint64_t total_bytes, uint64_t elapsed_ns) {
    if (elapsed_ns == 0)
        return 0;
    return (double)total_bytes * 8 / (elapsed_ns / 1e9) / 1e9;
}

/* Print latency results. samples must be sorted ascending. */
static inline void print_latency(const char *label, uint64_t *samples, int n) {
    if (n <= 0) {
        printf("\n--- %s --- (no samples)\n", label);
        return;
    }
    lat_stats_t s = latency_stats(samples, n);
    printf("\n--- %s ---\n", label);
    printf("  %-10s %-10s %-10s %-10s\n", "min(us)", "median(us)", "p99(us)", "max(us)");
    printf("  %-10.2f %-10.2f %-10.2f %-10.2f\n",
           s.min_us, s.median_us, s.p99_us, s.max_us);
    printf("---------------------------\n");
}

/* Print bandwidth results. */
static inline void print_bandwidth(const char *label,
                                   uint64_t total_bytes, uint64_t elapsed_ns) {
    if (elapsed_ns == 0) {
        printf("\n--- %s --- (no data)\n", label);
        return;
    }
    double elapsed_ms = elapsed_ns / 1e6;
    double gbps       = bandwidth_gbps(total_bytes, elapsed_ns);
    double GiBps      = (double)total_bytes / (elapsed_ns / 1e9) / (1024.0 * 1024 * 1024);
    printf("\n--- %s ---\n", label);
    printf("  transferred : %lu bytes (%.2f MiB)\n",
           total_bytes, total_bytes / (1024.0 * 1024));
    printf("  elapsed     : %.2f ms\n", elapsed_ms);
    printf("  throughput  : %.2f GiB/s  /  %.2f Gbps\n", GiBps, gbps);
    printf("---------------------------\n");
}
