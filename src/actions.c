#include "agent.h"

#include <string.h>
#include <time.h>

static const char *const agent_push_results[] = {"ok", "skipped", "failed", "rejected"};

int agent_collect(const agent_config *config) {
  tig_repository *repository;
  agent_status status;
  int lock = agent_lock(config);
  int result;

  if (lock < 0) {
    return -1;
  }
  if (agent_open(config, &repository) != 0) {
    agent_unlock(lock);
    return -1;
  }

  result = agent_sample(repository);
  tig_repository_close(repository);

  agent_status_load(config, &status);
  status.last_sample = (int64_t)time(0);
  agent_status_save(config, &status);

  agent_unlock(lock);
  return result;
}

static int agent_commit_locked(const agent_config *config) {
  tig_repository *repository;
  agent_status status;
  tig_status committed;

  if (agent_open(config, &repository) != 0) {
    return -1;
  }
  committed = tig_repository_commit(repository, "Record telemetry");
  tig_repository_close(repository);

  if (committed != TIG_OK) {
    agent_log_error("cannot commit: %s", tig_get_last_error());
    return -1;
  }

  agent_status_load(config, &status);
  status.last_commit = (int64_t)time(0);
  agent_status_save(config, &status);
  return 0;
}

int agent_commit(const agent_config *config) {
  int lock = agent_lock(config);
  int result;

  if (lock < 0) {
    return -1;
  }
  result = agent_commit_locked(config);
  agent_unlock(lock);
  return result;
}

/* commits, then pushes; a rejected push pauses pushing until retry_rejected is set, e.g. by a manual push */
int agent_sync(const agent_config *config, int retry_rejected) {
  agent_status status;
  agent_push_result pushed;
  int lock = agent_lock(config);

  if (lock < 0) {
    return AGENT_EXIT_ERROR;
  }
  if (agent_commit_locked(config) != 0) {
    agent_unlock(lock);
    return AGENT_EXIT_ERROR;
  }

  agent_status_load(config, &status);
  if (!retry_rejected && strcmp(status.last_push_result, "rejected") == 0) {
    agent_unlock(lock);
    return AGENT_EXIT_PUSH_REJECTED;
  }

  pushed = agent_push(config);
  if (pushed != AGENT_PUSH_SKIPPED) {
    status.last_push_attempt = (int64_t)time(0);
  }
  if (pushed == AGENT_PUSH_OK) {
    status.last_push = status.last_push_attempt;
  }
  strcpy(status.last_push_result, agent_push_results[pushed]);
  agent_status_save(config, &status);

  agent_unlock(lock);
  switch (pushed) {
    case AGENT_PUSH_OK:
    case AGENT_PUSH_SKIPPED:
      return AGENT_EXIT_OK;
    case AGENT_PUSH_REJECTED:
      return AGENT_EXIT_PUSH_REJECTED;
    default:
      return AGENT_EXIT_ERROR;
  }
}
