#include "agent.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *const agent_push_results[] = {"ok", "skipped", "failed", "rejected"};
static const char *const agent_sign_results[] = {"ok", "current", "skipped", "failed"};

int agent_collect(const agent_config *config) {
  tig_repository *repository;
  agent_status status;
  int lock = agent_lock(config);
  int result;

  if (lock < 0) {
    return -1;
  }
  if (agent_open(config, 0, &repository) != 0) {
    agent_unlock(lock);
    return -1;
  }

  result = agent_sample(config, repository);
  tig_repository_close(repository);

  agent_status_load(config, &status);
  status.last_sample = (int64_t)time(0);
  agent_status_save(config, &status);

  agent_unlock(lock);
  return result;
}

/* compares the device with its state, recording what changed and any boot not yet recorded */
static int agent_observe(const agent_config *config, tig_repository *repository) {
  agent_observation observation;
  agent_status status;
  int result;

  memset(&observation, 0, sizeof(observation));
  if (tig_clock_now(repository, &observation.now_ms) != TIG_OK) {
    agent_log_error("cannot read the TiG clock: %s", tig_get_last_error());
    return -1;
  }
  if (agent_inventory_read(&observation.inventory) != 0) {
    return -1;
  }
  /* when Docker can't be reached its stacks are left as they were recorded, not taken as removed */
  observation.docker_observed = agent_docker_enabled(config) && agent_docker_inventory(config, &observation.inventory) == 0;
  /* platforms without boot IDs, e.g. macOS during development, record no boots */
  agent_boot_read(&observation.boot);

  agent_status_load(config, &status);
  observation.docker_baseline = status.docker_baseline;
  observation.previous_ms = status.last_compare * 1000;
  observation.latest_reading_ms = status.last_sample * 1000;
  observation.first_after_boot = observation.boot.id[0] != '\0' && strcmp(status.last_compare_boot, observation.boot.id) != 0;

  result = agent_record(repository, &observation);
  agent_inventory_free(&observation.inventory);
  if (result == 0) {
    status.last_compare = observation.now_ms / 1000;
    /* an empty first reading still counts, so what appears later is installed; turning Docker off forgets it */
    if (observation.docker_observed) {
      status.docker_baseline = 1;
    } else if (!agent_docker_enabled(config)) {
      status.docker_baseline = 0;
    }
    snprintf(status.last_compare_boot, sizeof(status.last_compare_boot), "%s", observation.boot.id);
    agent_status_save(config, &status);
  }
  return result;
}

static int agent_commit_locked(const agent_config *config) {
  tig_repository *repository;
  agent_device device;
  agent_status status;
  tig_status committed;

  if (agent_open(config, &device, &repository) != 0) {
    return -1;
  }
  /* telemetry is still committed if events can't be recorded; the next commit tries again */
  agent_observe(config, repository);
  committed = tig_repository_commit(repository, 0);
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

/* commits, has TiG Notary sign the commit, then pushes; a rejected push pauses pushing until retry_rejected is set,
   e.g. by a manual push. A commit the notary can't sign now is signed at the next sync, and is pushed without one. */
int agent_sync(const agent_config *config, int retry_rejected) {
  agent_status status;
  agent_push_result pushed;
  agent_sign_result signed_result;
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

  signed_result = agent_notary_sign(config);
  if (signed_result == AGENT_SIGN_OK) {
    status.last_sign = (int64_t)time(0);
  }
  strcpy(status.last_sign_result, agent_sign_results[signed_result]);

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
