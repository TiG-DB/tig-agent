#ifndef TIG_AGENT_H
#define TIG_AGENT_H

#include "tig/tig.h"

#include <stddef.h>
#include <stdint.h>

#define AGENT_PATH_MAX 4096
#define AGENT_DEFAULT_CONFIG "/etc/tig-agent/agent.conf"

/* process exit codes */
enum {
  AGENT_EXIT_OK = 0,
  AGENT_EXIT_ERROR = 1,
  AGENT_EXIT_USAGE = 2,
  AGENT_EXIT_PUSH_REJECTED = 3
};

typedef struct agent_config {
  char data_dir[AGENT_PATH_MAX];
  char remote[1024]; /* empty: never push */
  int64_t sample_interval_ms;
  int64_t commit_interval_ms;
  int64_t push_interval_ms;
} agent_config;

/* all times are UNIX seconds, 0 if never */
typedef struct agent_status {
  int64_t last_sample;
  int64_t last_commit;
  int64_t last_push_attempt;
  int64_t last_push;
  char last_push_result[16]; /* ok, failed, rejected or skipped */
} agent_status;

typedef enum agent_push_result {
  AGENT_PUSH_OK,
  AGENT_PUSH_SKIPPED,
  AGENT_PUSH_FAILED,
  AGENT_PUSH_REJECTED
} agent_push_result;

/* config.c */
void agent_config_defaults(agent_config *config);
int  agent_config_load(agent_config *config, const char *path);
int  agent_config_parse(agent_config *config, const char *text, const char *source);
int  agent_parse_duration(const char *text, int64_t *ms);
int  agent_path(char *out, size_t out_size, const agent_config *config, const char *name);

/* log.c: writes a line to stderr, with a syslog priority when it goes to journald */
void agent_log_info(const char *format, ...);
void agent_log_warning(const char *format, ...);
void agent_log_error(const char *format, ...);

/* process.c: runs argv[0] from PATH with extra "NAME=value" environment entries, capturing stdout and
   stderr into output (which may be NULL); returns the exit status, or -1 if it couldn't run or timed out */
int agent_run(const char *const *argv, const char *const *env, char *output, size_t output_size,
              int timeout_seconds);

typedef struct agent_device {
  char key_path[AGENT_PATH_MAX];
  char work_dir[AGENT_PATH_MAX]; /* for the files ssh-keygen signs */
  char fingerprint[128];         /* SHA256:..., as ssh-keygen -l prints it */
  char allowed_signer[1024];     /* the key's line in .tig/allowed_signers */
  tig_device device;             /* signs through ssh-keygen; points into this struct */
} agent_device;

/* device.c */
int agent_create_device_key(const char *path);
int agent_device_load(const agent_config *config, agent_device *device);

/* repository.c: with a device, commits are signed, and unsigned history gains the key first */
int  agent_setup(const agent_config *config);
int  agent_open(const agent_config *config, agent_device *device, tig_repository **out);
int  agent_lock(const agent_config *config);
void agent_unlock(int lock);

/* telemetry.c: samples every stream this machine supports */
int agent_sample(tig_repository *repository);

/* sync.c */
int               agent_configure_remote(const agent_config *config);
agent_push_result agent_push(const agent_config *config);

/* status.c */
int agent_status_load(const agent_config *config, agent_status *status);
int agent_status_save(const agent_config *config, const agent_status *status);
int agent_status_print(const agent_config *config);

/* actions.c: each takes the agent lock and records its outcome in the status file */
int agent_collect(const agent_config *config);
int agent_commit(const agent_config *config);
int agent_sync(const agent_config *config, int retry_rejected);

/* daemon.c */
int agent_daemon(const agent_config *config);

#endif
