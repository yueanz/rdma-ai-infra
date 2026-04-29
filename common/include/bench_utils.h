#pragma once
#include <stdint.h>
#include <stdio.h>
#include "timing.h"

#define kWarmup 20

/* Print latency results. samples must be sorted ascending.
 *
 * Percentile indexing: for percentile p of n samples, the 0-indexed
 * position is (int)((n - 1) * p/100). This puts p99 at samples[98]
 * for n=100 (the 99th value out of 100), not samples[99] (which would
 * always equal max). */
static inline void print_latency(const char *label, uint64_t *samples, int n) {
    if (n <= 0) {
        printf("\n--- %s --- (no samples)\n", label);
        return;
    }
    int idx_median = (n - 1) / 2;
    int idx_p99    = (int)((n - 1) * 0.99);
    printf("\n--- %s ---\n", label);
    printf("  %-10s %-10s %-10s %-10s\n", "min(us)", "median(us)", "p99(us)", "max(us)");
    printf("  %-10.2f %-10.2f %-10.2f %-10.2f\n",
        ns_to_us(samples[0]),
        ns_to_us(samples[idx_median]),
        ns_to_us(samples[idx_p99]),
        ns_to_us(samples[n - 1]));
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
    double gbps       = (double)total_bytes * 8 / (elapsed_ns / 1e9) / 1e9;
    double GiBps      = (double)total_bytes / (elapsed_ns / 1e9) / (1024.0 * 1024 * 1024);
    printf("\n--- %s ---\n", label);
    printf("  transferred : %lu bytes (%.2f MiB)\n",
           total_bytes, total_bytes / (1024.0 * 1024));
    printf("  elapsed     : %.2f ms\n", elapsed_ms);
    printf("  throughput  : %.2f GiB/s  /  %.2f Gbps\n", GiBps, gbps);
    printf("---------------------------\n");
}
