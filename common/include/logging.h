#pragma once
#include <stdio.h>

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[info] " fmt "\n", ##__VA_ARGS__)

#define LOG_ERR(fmt, ...)   fprintf(stderr, "[error] [%s:%d]" fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

#ifdef DEBUG
#define LOG_DEBUG(fmt, ...) fprintf(stdout, "[debug] [%s:%d]" fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) do {} while(0)
#endif