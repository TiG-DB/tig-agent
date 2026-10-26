#include "agent.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t agent_stopping;

static void agent_on_signal(int signal_number) {
  (void)signal_number;
  agent_stopping = 1;
}

static int64_t agent_monotonic_ms(void) {
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* keeps a steady cadence, skipping runs that were missed rather than catching up */
static int64_t agent_next(int64_t previous, int64_t interval, int64_t now) {
  int64_t next = previous + interval;

  return next > now ? next : now + interval;
}

/* returns early when a signal arrives */
static void agent_sleep_until(int64_t wake_ms) {
  int64_t remaining = wake_ms - agent_monotonic_ms();
  struct timespec delay;

  if (remaining <= 0 || agent_stopping) {
    return;
  }
  delay.tv_sec = (time_t)(remaining / 1000);
  delay.tv_nsec = (long)(remaining % 1000) * 1000000;
  nanosleep(&delay, 0);
}

int agent_daemon(const agent_config *config) {
  struct sigaction action;
  int64_t now;
  int64_t next_sample;
  int64_t next_commit;
  int64_t next_push;

  memset(&action, 0, sizeof(action));
  action.sa_handler = agent_on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, 0);
  sigaction(SIGINT, &action, 0);

  if (agent_setup(config) != 0) {
    return AGENT_EXIT_ERROR;
  }

  /* push straight away, so a restart (e.g. after an update) shows at once whether pushing works */
  now = agent_monotonic_ms();
  next_sample = now;
  next_commit = now + config->commit_interval_ms;
  next_push = now;

  while (!agent_stopping) {
    now = agent_monotonic_ms();

    if (now >= next_sample) {
      agent_collect(config);
      next_sample = agent_next(next_sample, config->sample_interval_ms, now);
    }
    if (now >= next_push) {
      agent_sync(config, 0);
      next_push = agent_next(next_push, config->push_interval_ms, now);
      next_commit = now + config->commit_interval_ms;
    } else if (now >= next_commit) {
      agent_commit(config);
      next_commit = agent_next(next_commit, config->commit_interval_ms, now);
    }

    {
      int64_t wake = next_sample;

      if (next_commit < wake) {
        wake = next_commit;
      }
      if (next_push < wake) {
        wake = next_push;
      }
      agent_sleep_until(wake);
    }
  }

  agent_log_info("stopping");
  return agent_commit(config) == 0 ? AGENT_EXIT_OK : AGENT_EXIT_ERROR;
}
