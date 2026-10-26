#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#define AGENT_MINUTE_MS INT64_C(60000)
#define AGENT_MAX_COLUMNS 4
#define AGENT_VALUE_MAX 32

typedef char agent_values[AGENT_MAX_COLUMNS][AGENT_VALUE_MAX];

typedef struct agent_stream {
  const char *name;
  tig_stream_definition definition;
  /* fills one value per column; returns -1 if the stream isn't available on this machine */
  int (*sample)(agent_values values);
} agent_stream;

static int agent_sample_load(agent_values values) {
  double load[3];
  int i;

  if (getloadavg(load, 3) != 3) {
    return -1;
  }
  for (i = 0; i < 3; ++i) {
    snprintf(values[i], AGENT_VALUE_MAX, "%.2f", load[i]);
  }
  return 0;
}

static int agent_sample_uptime(agent_values values) {
  FILE *file = fopen("/proc/uptime", "r");
  double uptime;
  int found;

  if (file == 0) {
    return -1;
  }
  found = fscanf(file, "%lf", &uptime) == 1;
  fclose(file);
  if (!found) {
    return -1;
  }

  snprintf(values[0], AGENT_VALUE_MAX, "%.0f", uptime);
  return 0;
}

static int agent_sample_memory(agent_values values) {
  FILE *file = fopen("/proc/meminfo", "r");
  char line[256];
  unsigned long long total_kb = 0;
  unsigned long long available_kb = 0;
  int found = 0;

  if (file == 0) {
    return -1;
  }
  while (fgets(line, sizeof(line), file) != 0) {
    if (sscanf(line, "MemTotal: %llu kB", &total_kb) == 1 ||
        sscanf(line, "MemAvailable: %llu kB", &available_kb) == 1) {
      ++found;
    }
  }
  fclose(file);
  if (found != 2) {
    return -1;
  }

  snprintf(values[0], AGENT_VALUE_MAX, "%llu", total_kb * 1024);
  snprintf(values[1], AGENT_VALUE_MAX, "%llu", available_kb * 1024);
  return 0;
}

static int agent_sample_disk_root(agent_values values) {
  struct statvfs fs;
  unsigned long long block;

  if (statvfs("/", &fs) != 0) {
    return -1;
  }
  block = fs.f_frsize;

  snprintf(values[0], AGENT_VALUE_MAX, "%llu", (unsigned long long)fs.f_blocks * block);
  snprintf(values[1], AGENT_VALUE_MAX, "%llu", (unsigned long long)(fs.f_blocks - fs.f_bfree) * block);
  snprintf(values[2], AGENT_VALUE_MAX, "%llu", (unsigned long long)fs.f_bavail * block);
  return 0;
}

static const tig_column agent_load_columns[] = {
    {"load1", TIG_VALUE_DECIMAL}, {"load5", TIG_VALUE_DECIMAL}, {"load15", TIG_VALUE_DECIMAL}};
static const tig_column agent_uptime_columns[] = {{"value", TIG_VALUE_INTEGER}};
static const tig_column agent_memory_columns[] = {{"total", TIG_VALUE_INTEGER}, {"available", TIG_VALUE_INTEGER}};
static const tig_column agent_disk_columns[] = {
    {"size", TIG_VALUE_INTEGER}, {"used", TIG_VALUE_INTEGER}, {"available", TIG_VALUE_INTEGER}};

static const agent_stream agent_streams[] = {
    {"system.load",
     {"Load average over 1, 5 and 15 minutes", 0, agent_load_columns, 3, TIG_CHUNK_DAY, 5 * AGENT_MINUTE_MS},
     agent_sample_load},
    {"system.uptime",
     {"Time since boot", "seconds", agent_uptime_columns, 1, TIG_CHUNK_DAY, 5 * AGENT_MINUTE_MS},
     agent_sample_uptime},
    {"memory",
     {"Physical memory", "bytes", agent_memory_columns, 2, TIG_CHUNK_DAY, 5 * AGENT_MINUTE_MS},
     agent_sample_memory},
    {"disk.root",
     {"Space on the root filesystem", "bytes", agent_disk_columns, 3, TIG_CHUNK_DAY, 5 * AGENT_MINUTE_MS},
     agent_sample_disk_root},
};

#define AGENT_STREAM_COUNT (sizeof(agent_streams) / sizeof(agent_streams[0]))

int agent_sample(tig_repository *repository) {
  int64_t now;
  size_t i;
  int failures = 0;

  if (tig_clock_now(repository, &now) != TIG_OK) {
    agent_log_error("cannot read the TiG clock: %s", tig_get_last_error());
    return -1;
  }

  for (i = 0; i < AGENT_STREAM_COUNT; ++i) {
    const agent_stream *stream = &agent_streams[i];
    agent_values values;
    const char *pointers[AGENT_MAX_COLUMNS];
    size_t column;

    /* a stream is only defined once it has a reading, so unsupported streams leave nothing behind */
    if (stream->sample(values) != 0) {
      continue;
    }
    if (tig_telemetry_stream_define(repository, stream->name, &stream->definition) != TIG_OK) {
      agent_log_error("cannot define %s: %s", stream->name, tig_get_last_error());
      ++failures;
      continue;
    }

    for (column = 0; column < stream->definition.column_count; ++column) {
      pointers[column] = values[column];
    }
    if (tig_telemetry_append(repository, stream->name, now, pointers, stream->definition.column_count) !=
        TIG_OK) {
      agent_log_error("cannot record %s: %s", stream->name, tig_get_last_error());
      ++failures;
    }
  }

  return failures == 0 ? 0 : -1;
}
