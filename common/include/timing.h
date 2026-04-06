#pragma once
#include <stdint.h>
#include <time.h>

#if defined(__x86_64__) || defined(__i386__)
  #define CPU_RELAX() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
  #define CPU_RELAX() __asm__ volatile("yield" ::: "memory")
#else
  #define CPU_RELAX() ((void)0)
#endif

static inline uint64_t time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline uint64_t time_elapsed_ns(uint64_t start, uint64_t end) {
    return end > start ? end - start : 0;
}

static inline double ns_to_us(uint64_t ns) {
    return (double)ns / 1000;
}