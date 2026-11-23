#include "agent.h"

#include <stdio.h>
#include <string.h>

#define AGENT_GIT_TIMEOUT 30
#define AGENT_PUSH_TIMEOUT 300

static void agent_trim_newlines(char *text) {
  size_t length = strlen(text);

  while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
    text[--length] = '\0';
  }
}

/* makes the repository's origin match the configured remote */
int agent_configure_remote(const agent_config *config) {
  char repository_path[AGENT_PATH_MAX];
  char current[1024];
  char output[1024];
  int has_origin;

  if (agent_path(repository_path, sizeof(repository_path), config, "repository") != 0) {
    return -1;
  }

  {
    const char *argv[] = {"git", "-C", repository_path, "remote", "get-url", "origin", 0};

    has_origin = agent_run(argv, 0, current, sizeof(current), AGENT_GIT_TIMEOUT) == 0;
    agent_trim_newlines(current);
  }

  if (config->remote[0] == '\0') {
    const char *argv[] = {"git", "-C", repository_path, "remote", "remove", "origin", 0};

    if (!has_origin) {
      return 0;
    }
    if (agent_run(argv, 0, output, sizeof(output), AGENT_GIT_TIMEOUT) != 0) {
      agent_log_error("cannot remove remote: %s", output);
      return -1;
    }
    agent_log_info("no remote configured; the repository is kept local");
    return 0;
  }

  if (has_origin && strcmp(current, config->remote) == 0) {
    return 0;
  }

  {
    const char *argv[] = {"git", "-C", repository_path, "remote", has_origin ? "set-url" : "add", "origin",
                          config->remote, 0};

    if (agent_run(argv, 0, output, sizeof(output), AGENT_GIT_TIMEOUT) != 0) {
      agent_log_error("cannot set remote to %s: %s", config->remote, output);
      return -1;
    }
  }
  agent_log_info("pushing to %s", config->remote);
  return 0;
}

/* `git push --porcelain` starts each rejected ref's line with "!" */
static int agent_push_was_rejected(const char *output) {
  return strncmp(output, "!\t", 2) == 0 || strstr(output, "\n!\t") != 0;
}

/* pushes HEAD to main, fast-forward only: TiG history is never force-pushed. The notary's signature tags go with it
   (COMMITS.md, "Signing"); they are only ever added, and a tag that is already there is left as it is. */
agent_push_result agent_push(const agent_config *config) {
  char repository_path[AGENT_PATH_MAX];
  char key_path[AGENT_PATH_MAX];
  char known_hosts_path[AGENT_PATH_MAX];
  char ssh_command[3 * AGENT_PATH_MAX];
  char output[4096];
  int written;
  int status;

  if (config->remote[0] == '\0') {
    return AGENT_PUSH_SKIPPED;
  }

  if (agent_path(repository_path, sizeof(repository_path), config, "repository") != 0 ||
      agent_path(key_path, sizeof(key_path), config, "keys/deploy_ed25519") != 0 ||
      agent_path(known_hosts_path, sizeof(known_hosts_path), config, "keys/known_hosts") != 0 ||
      agent_configure_remote(config) != 0) {
    return AGENT_PUSH_FAILED;
  }

  /* the host's key is trusted on first use and pinned in known_hosts afterwards */
  written = snprintf(ssh_command, sizeof(ssh_command),
                     "GIT_SSH_COMMAND=ssh -i '%s' -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=30 "
                     "-o StrictHostKeyChecking=accept-new -o UserKnownHostsFile='%s'",
                     key_path, known_hosts_path);
  if (written < 0 || (size_t)written >= sizeof(ssh_command)) {
    agent_log_error("ssh command too long");
    return AGENT_PUSH_FAILED;
  }

  {
    const char *env[] = {"GIT_TERMINAL_PROMPT=0", ssh_command, 0};
    const char *argv[] = {"git", "-C", repository_path, "push",
                          "--porcelain",           "origin",
                          "HEAD:refs/heads/main",  "refs/tags/tig/sig/*:refs/tags/tig/sig/*",
                          0};

    status = agent_run(argv, env, output, sizeof(output), AGENT_PUSH_TIMEOUT);
  }
  agent_trim_newlines(output);

  if (status == 0) {
    return AGENT_PUSH_OK;
  }
  if (agent_push_was_rejected(output)) {
    agent_log_error("push to %s was rejected, so pushing is paused until it is resolved "
                    "(history is never force-pushed): %s",
                    config->remote, output);
    return AGENT_PUSH_REJECTED;
  }
  agent_log_warning("push to %s failed and will be retried: %s", config->remote, output);
  return AGENT_PUSH_FAILED;
}
