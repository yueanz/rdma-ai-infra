#pragma once
#include <stdio.h>

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[info] " fmt "\n", ##__VA_ARGS__)

#define LOG_ERR(fmt, ...)   fprintf(stderr, "[error] [%s:%d]" fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)