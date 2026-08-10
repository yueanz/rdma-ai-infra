#pragma once
#include <stdio.h>

/* stderr, not stdout: several benchmarks write machine-readable rows to
 * stdout under --csv, and a progress line landing in the middle of them
 * corrupts the file. Nothing parses these, so stderr costs nothing. */
#define LOG_INFO(fmt, ...)  fprintf(stderr, "[info] " fmt "\n", ##__VA_ARGS__)

#define LOG_ERR(fmt, ...)   fprintf(stderr, "[error] [%s:%d]" fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)