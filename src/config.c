#include "agent.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_CONFIG_MAX (64 * 1024)

void agent_config_defaults(agent_config *config) {
  memset(config, 0, sizeof(*config));
  strcpy(config->data_dir, "/var/lib/tig");
  config->sample_interval_ms = INT64_C(60) * 1000;
  config->commit_interval_ms = INT64_C(15) * 60 * 1000;
  config->push_interval_ms = INT64_C(60) * 60 * 1000;
}

/* a positive integer with a unit: "30s", "5m", "1h" or "2d" */
int agent_parse_duration(const char *text, int64_t *ms) {
  char *end;
  long long value;
  int64_t unit;

  if (!isdigit((unsigned char)text[0])) {
    return -1;
  }

  errno = 0;
  value = strtoll(text, &end, 10);
  if (errno != 0 || value <= 0) {
    return -1;
  }

  if (strcmp(end, "s") == 0) {
    unit = 1000;
  } else if (strcmp(end, "m") == 0) {
    unit = INT64_C(60) * 1000;
  } else if (strcmp(end, "h") == 0) {
    unit = INT64_C(60) * 60 * 1000;
  } else if (strcmp(end, "d") == 0) {
    unit = INT64_C(24) * 60 * 60 * 1000;
  } else {
    return -1;
  }

  if (value > INT64_MAX / unit) {
    return -1;
  }
  *ms = (int64_t)value * unit;
  return 0;
}

static char *agent_trim(char *text) {
  char *end;

  while (isspace((unsigned char)*text)) {
    ++text;
  }
  end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) {
    --end;
  }
  *end = '\0';
  return text;
}

static int agent_copy_value(char *out, size_t out_size, const char *value) {
  size_t length = strlen(value);

  if (length + 1 > out_size) {
    return -1;
  }
  memcpy(out, value, length + 1);
  return 0;
}

static int agent_config_set(agent_config *config, const char *key, const char *value) {
  if (strcmp(key, "data_dir") == 0) {
    /* the path is quoted for ssh, see sync.c */
    if (value[0] != '/' || strchr(value, '\'') != 0) {
      return -1;
    }
    return agent_copy_value(config->data_dir, sizeof(config->data_dir), value);
  }
  if (strcmp(key, "remote") == 0) {
    return agent_copy_value(config->remote, sizeof(config->remote), value);
  }
  if (strcmp(key, "docker_host") == 0) {
    size_t i;

    /* passed to curl as an argument, never through a shell; only plain text and no credentials */
    if (value[0] != '\0' && strncmp(value, "unix:///", 8) != 0 && strncmp(value, "http://", 7) != 0) {
      return -1;
    }
    for (i = 0; value[i] != '\0'; ++i) {
      if (!isgraph((unsigned char)value[i]) || value[i] == '@' || value[i] == '\'' || value[i] == '"') {
        return -1;
      }
    }
    return agent_copy_value(config->docker_host, sizeof(config->docker_host), value);
  }
  if (strcmp(key, "docker_versions") == 0) {
    if (strcmp(value, "hub") == 0) {
      config->docker_versions = 1;
      return 0;
    }
    if (value[0] == '\0' || strcmp(value, "off") == 0) {
      config->docker_versions = 0;
      return 0;
    }
    return -1;
  }
  if (strcmp(key, "sample_interval") == 0) {
    return agent_parse_duration(value, &config->sample_interval_ms);
  }
  if (strcmp(key, "commit_interval") == 0) {
    return agent_parse_duration(value, &config->commit_interval_ms);
  }
  if (strcmp(key, "push_interval") == 0) {
    return agent_parse_duration(value, &config->push_interval_ms);
  }
  return -2;
}

/* "key = value" lines; blank lines and lines starting with # are ignored */
int agent_config_parse(agent_config *config, const char *text, const char *source) {
  size_t length = strlen(text);
  char *copy = (char *)malloc(length + 1);
  char *line;
  char *next;
  int line_number = 0;
  int result = 0;

  if (copy == 0) {
    agent_log_error("%s: out of memory", source);
    return -1;
  }
  memcpy(copy, text, length + 1);

  for (line = copy; line != 0 && result == 0; line = next) {
    char *equals;
    char *key;
    char *value;
    int set;

    next = strchr(line, '\n');
    if (next != 0) {
      *next++ = '\0';
    }
    ++line_number;

    line = agent_trim(line);
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }

    equals = strchr(line, '=');
    if (equals == 0) {
      agent_log_error("%s:%d: expected key = value", source, line_number);
      result = -1;
      break;
    }
    *equals = '\0';
    key = agent_trim(line);
    value = agent_trim(equals + 1);

    set = agent_config_set(config, key, value);
    if (set == -2) {
      agent_log_error("%s:%d: unknown setting \"%s\"", source, line_number, key);
      result = -1;
    } else if (set != 0) {
      agent_log_error("%s:%d: invalid value for %s: \"%s\"", source, line_number, key, value);
      result = -1;
    }
  }

  free(copy);
  return result;
}

int agent_config_load(agent_config *config, const char *path) {
  FILE *file = fopen(path, "rb");
  char *text;
  size_t size;
  int result;

  if (file == 0) {
    agent_log_error("%s: %s", path, strerror(errno));
    return -1;
  }

  text = (char *)malloc(AGENT_CONFIG_MAX + 1);
  if (text == 0) {
    fclose(file);
    agent_log_error("%s: out of memory", path);
    return -1;
  }

  size = fread(text, 1, AGENT_CONFIG_MAX + 1, file);
  fclose(file);
  if (size > AGENT_CONFIG_MAX) {
    free(text);
    agent_log_error("%s: larger than %d bytes", path, AGENT_CONFIG_MAX);
    return -1;
  }
  text[size] = '\0';

  result = agent_config_parse(config, text, path);
  free(text);
  return result;
}

int agent_path(char *out, size_t out_size, const agent_config *config, const char *name) {
  int written = snprintf(out, out_size, "%s/%s", config->data_dir, name);

  if (written < 0 || (size_t)written >= out_size) {
    agent_log_error("path too long: %s/%s", config->data_dir, name);
    return -1;
  }
  return 0;
}
