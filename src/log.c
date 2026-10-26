#include "agent.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* journald reads a "<N>" syslog priority from the start of each line */
static void agent_log(int priority, const char *label, const char *format, va_list args) {
  if (getenv("JOURNAL_STREAM") != 0) {
    fprintf(stderr, "<%d>", priority);
  } else if (label != 0) {
    fprintf(stderr, "%s: ", label);
  }
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
}

void agent_log_info(const char *format, ...) {
  va_list args;

  va_start(args, format);
  agent_log(6, 0, format, args);
  va_end(args);
}

void agent_log_warning(const char *format, ...) {
  va_list args;

  va_start(args, format);
  agent_log(4, "warning", format, args);
  va_end(args);
}

void agent_log_error(const char *format, ...) {
  va_list args;

  va_start(args, format);
  agent_log(3, "error", format, args);
  va_end(args);
}
