#pragma once

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR } log_level_t;

int  log_open(void);
void log_close(void);
void log_write(log_level_t lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#if NDEBUG == 0
#define DEBUG(fmt, ...) log_write(LOG_DEBUG, fmt __VA_OPT__(,) __VA_ARGS__)
#else
#define DEBUG(...) ((void)0)
#endif

#define INFO(fmt, ...)    log_write(LOG_INFO,    fmt __VA_OPT__(,) __VA_ARGS__)
#define WARNING(fmt, ...) log_write(LOG_WARNING, fmt __VA_OPT__(,) __VA_ARGS__)
#define ERROR(fmt, ...)   log_write(LOG_ERROR,   fmt __VA_OPT__(,) __VA_ARGS__)
#define ERROR_ERRNO(fmt, ...) \
  log_write(LOG_ERROR, fmt ": %s" __VA_OPT__(,) __VA_ARGS__, strerror(errno))

