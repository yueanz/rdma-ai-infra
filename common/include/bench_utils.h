#pragma once
#include <stdint.h>
#include <stdio.h>
#include "timing.h"

#define kWarmup 20

/* Print latency results. samples must be sorted ascending. */
static inline void print_latency(const char *label, uint64_t *samples, int n) {
    printf("\n--- %s ---\n", label);
    printf("  %-10s %-10s %-10s %-10s\n", "min(us)", "median(us)", "p99(us)", "max(us)");
    printf("  %-10.2f %-10.2f %-10.2f %-10.2f\n",
        ns_to_us(samples[0]),
        ns_to_us(samples[n / 2]),
        ns_to_us(samples[(int)(n * 0.99)]),
        ns_to_us(samples[n - 1]));
    printf("---------------------------\n");
}

/* Print bandwidth results. */
static inline void print_bandwidth(const char *label,
                                   uint64_t total_bytes, uint64_t elapsed_ns) {
    double elapsed_ms = elapsed_ns / 1e6;
    double gbps       = (double)total_bytes * 8 / (elapsed_ns / 1e9) / 1e9;
    double GBps       = (double)total_bytes / (elapsed_ns / 1e9) / (1024.0 * 1024 * 1024);
    printf("\n--- %s ---\n", label);
    printf("  transferred : %lu bytes (%.2f MB)\n",
           total_bytes, total_bytes / (1024.0 * 1024));
    printf("  elapsed     : %.2f ms\n", elapsed_ms);
    printf("  throughput  : %.2f GB/s  /  %.2f Gbps\n", GBps, gbps);
    printf("---------------------------\n");
}
