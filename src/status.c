#include "agent.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int agent_status_load(const agent_config *config, agent_status *status) {
  char path[AGENT_PATH_MAX];
  char line[256];
  FILE *file;

  memset(status, 0, sizeof(*status));
  if (agent_path(path, sizeof(path), config, "agent.status") != 0) {
    return -1;
  }

  file = fopen(path, "r");
  if (file == 0) {
    return errno == ENOENT ? 0 : -1;
  }
  while (fgets(line, sizeof(line), file) != 0) {
    long long value;

    if (sscanf(line, "last_sample=%lld", &value) == 1) {
      status->last_sample = value;
    } else if (sscanf(line, "last_commit=%lld", &value) == 1) {
      status->last_commit = value;
    } else if (sscanf(line, "last_push_attempt=%lld", &value) == 1) {
      status->last_push_attempt = value;
    } else if (sscanf(line, "last_push=%lld", &value) == 1) {
      status->last_push = value;
    } else if (sscanf(line, "last_push_result=%15s", status->last_push_result) == 1) {
      /* read */
    }
  }
  fclose(file);
  return 0;
}

/* written to a temporary file and renamed, so readers never see half a file */
int agent_status_save(const agent_config *config, const agent_status *status) {
  char path[AGENT_PATH_MAX];
  char temporary[AGENT_PATH_MAX];
  FILE *file;

  if (agent_path(path, sizeof(path), config, "agent.status") != 0 ||
      agent_path(temporary, sizeof(temporary), config, "agent.status.tmp") != 0) {
    return -1;
  }

  file = fopen(temporary, "w");
  if (file == 0) {
    agent_log_error("%s: %s", temporary, strerror(errno));
    return -1;
  }
  fprintf(file, "last_sample=%lld\nlast_commit=%lld\nlast_push_attempt=%lld\nlast_push=%lld\nlast_push_result=%s\n",
          (long long)status->last_sample, (long long)status->last_commit, (long long)status->last_push_attempt,
          (long long)status->last_push, status->last_push_result);
  if (fclose(file) != 0 || rename(temporary, path) != 0) {
    agent_log_error("%s: %s", path, strerror(errno));
    return -1;
  }
  return 0;
}

static void agent_print_time(const char *label, int64_t unix_seconds) {
  char text[32] = "never";
  time_t time_value = (time_t)unix_seconds;
  struct tm utc;

  if (unix_seconds != 0 && gmtime_r(&time_value, &utc) != 0) {
    strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
  }
  printf("%-14s%s\n", label, text);
}

static void agent_print_file(const char *label, const char *path) {
  char line[1024];
  FILE *file = fopen(path, "r");

  if (file == 0 || fgets(line, sizeof(line), file) == 0) {
    strcpy(line, "(missing)\n");
  }
  if (file != 0) {
    fclose(file);
  }
  printf("%-14s%s", label, line);
}

/* exits non-zero only when the agent can't work at all, so it doubles as the updater's health check */
int agent_status_print(const agent_config *config) {
  char repository_path[AGENT_PATH_MAX];
  char key_path[AGENT_PATH_MAX];
  char head[512];
  agent_status status;
  tig_repository *repository;

  if (agent_path(repository_path, sizeof(repository_path), config, "repository") != 0 ||
      agent_path(key_path, sizeof(key_path), config, "keys/deploy_ed25519.pub") != 0) {
    return AGENT_EXIT_ERROR;
  }
  if (agent_open(config, &repository) != 0) {
    return AGENT_EXIT_ERROR;
  }
  tig_repository_close(repository);
  agent_status_load(config, &status);

  {
    const char *argv[] = {"git", "-C", repository_path, "log", "-1", "--format=%h %cI %s", 0};

    if (agent_run(argv, 0, head, sizeof(head), 30) != 0) {
      strcpy(head, "(unknown)\n");
    }
  }

  printf("%-14s%s\n", "repository", repository_path);
  printf("%-14s%s", "head", head);
  printf("%-14s%s\n", "remote", config->remote[0] != '\0' ? config->remote : "(none, kept local)");
  agent_print_time("last sample", status.last_sample);
  agent_print_time("last commit", status.last_commit);
  agent_print_time("last push", status.last_push);
  if (status.last_push_result[0] != '\0') {
    printf("%-14s%s%s\n", "push result", status.last_push_result,
           strcmp(status.last_push_result, "rejected") == 0 ? " (paused: run `tig-agent push` once resolved)" : "");
  }
  agent_print_file("deploy key", key_path);
  return AGENT_EXIT_OK;
}
