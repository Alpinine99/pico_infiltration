#pragma once

#include <stdio.h>

#if NLOGGING == 1
#define INFO(...)    ((void)0)
#define ERROR(...)    ((void)0)
#define WARNING(...)  ((void)0)
#else
#define INFO(fmt, ...)    printf("INFO: " fmt "\n", ##__VA_ARGS__)
#define ERROR(fmt, ...)   fprintf(stderr, "ERROR: " fmt "\n", ##__VA_ARGS__)
#define WARNING(fmt, ...) fprintf(stderr, "WARNING: " fmt "\n", ##__VA_ARGS__)
#endif

