#include "logger.h"

#include <stdarg.h>
#include <time.h>

static FILE *log_fp;

static const char *const names[] = { "DEBUG", "INFO", "WARNING", "ERROR" };
#if NDEBUG == 0
static const char *const colors[] = { "\033[34;1m", "\033[1m", "\033[33;1m", "\033[31;1m" };
#endif

int log_open() {
  log_fp = fopen(LOGFILE, "a");
  if (!log_fp) {
    fprintf(stderr, "cannot open log file %s: %s\n", LOGFILE, strerror(errno));
    return -1;
  }
  setvbuf(log_fp, NULL, _IOLBF, 0);
  return 0;
}

void log_close(void) {
  if (log_fp) {
    fclose(log_fp);
    log_fp = NULL;
  }
}

void log_write(log_level_t lvl, const char *fmt, ...) {
  char ts[32], msg[512];
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  if (log_fp)
    fprintf(log_fp, "%s %-7s %s\n", ts, names[lvl], msg);

#if NDEBUG == 0
  fprintf(lvl >= LOG_WARNING ? stderr : stdout, "%s: %s%s\033[0m\n", names[lvl], colors[lvl], msg);
#endif
}

