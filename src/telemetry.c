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

/* capacities are state, not telemetry, so only what is in use is sampled (DEVICE.md, "Telemetry") */
static int agent_sample_memory(agent_values values) {
  FILE *file = fopen("/proc/meminfo", "r");
  char line[256];
  unsigned long long available_kb = 0;
  int found = 0;

  if (file == 0) {
    return -1;
  }
  while (!found && fgets(line, sizeof(line), file) != 0) {
    found = sscanf(line, "MemAvailable: %llu kB", &available_kb) == 1;
  }
  fclose(file);
  if (!found) {
    return -1;
  }

  snprintf(values[0], AGENT_VALUE_MAX, "%llu", available_kb * 1024);
  return 0;
}

static int agent_sample_filesystem_root(agent_values values) {
  struct statvfs fs;
  unsigned long long block;

  if (statvfs("/", &fs) != 0) {
    return -1;
  }
  block = fs.f_frsize;

  snprintf(values[0], AGENT_VALUE_MAX, "%llu", (unsigned long long)(fs.f_blocks - fs.f_bfree) * block);
  snprintf(values[1], AGENT_VALUE_MAX, "%llu", (unsigned long long)fs.f_bavail * block);
  return 0;
}

static const tig_column agent_load_columns[] = {
    {"load1", TIG_VALUE_DECIMAL}, {"load5", TIG_VALUE_DECIMAL}, {"load15", TIG_VALUE_DECIMAL}};
static const tig_column agent_memory_columns[] = {{"available", TIG_VALUE_INTEGER}};
static const tig_column agent_filesystem_columns[] = {{"used", TIG_VALUE_INTEGER}, {"available", TIG_VALUE_INTEGER}};

/* stream names from tig-docs/vocabularies/DEVICE.md, "Telemetry" */
static const agent_stream agent_streams[] = {
    {"system.load",
     {"Load average over 1, 5 and 15 minutes", 0, agent_load_columns, 3, TIG_CHUNK_DAY, 5 * AGENT_MINUTE_MS},
     agent_sample_load},
    {"memory",
     {"Physical memory available", "bytes", agent_memory_columns, 1, TIG_CHUNK_DAY, 5 * AGENT_MINUTE_MS},
     agent_sample_memory},
    {"filesystem.root",
     {"Space on the root filesystem", "bytes", agent_filesystem_columns, 2, TIG_CHUNK_DAY, 5 * AGENT_MINUTE_MS},
     agent_sample_filesystem_root},
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
