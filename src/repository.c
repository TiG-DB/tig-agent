#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int agent_make_directory(const char *path, mode_t mode) {
  if (mkdir(path, mode) != 0 && errno != EEXIST) {
    agent_log_error("%s: %s", path, strerror(errno));
    return -1;
  }
  return 0;
}

static int agent_exists(const char *path) {
  struct stat info;

  return stat(path, &info) == 0;
}

static int agent_create_deploy_key(const char *path) {
  char host[256];
  char comment[300];
  char output[1024];
  const char *argv[] = {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", comment, "-f", path, 0};

  if (gethostname(host, sizeof(host)) != 0) {
    strcpy(host, "unknown");
  }
  host[sizeof(host) - 1] = '\0';
  snprintf(comment, sizeof(comment), "tig-agent@%s", host);

  if (agent_run(argv, 0, output, sizeof(output), 30) != 0) {
    agent_log_error("ssh-keygen failed: %s", output);
    return -1;
  }
  agent_log_info("created deploy key %s.pub", path);
  return 0;
}

/* idempotent: creates whatever is missing */
int agent_setup(const agent_config *config) {
  char repository_path[AGENT_PATH_MAX];
  char git_path[AGENT_PATH_MAX];
  char keys_path[AGENT_PATH_MAX];
  char key_path[AGENT_PATH_MAX];
  char device_key_path[AGENT_PATH_MAX];
  agent_device device;

  if (agent_path(repository_path, sizeof(repository_path), config, "repository") != 0 ||
      agent_path(git_path, sizeof(git_path), config, "repository/.git") != 0 ||
      agent_path(keys_path, sizeof(keys_path), config, "keys") != 0 ||
      agent_path(key_path, sizeof(key_path), config, "keys/deploy_ed25519") != 0 ||
      agent_path(device_key_path, sizeof(device_key_path), config, "keys/device_ed25519") != 0) {
    return -1;
  }

  if (agent_make_directory(config->data_dir, 0750) != 0 || agent_make_directory(keys_path, 0700) != 0) {
    return -1;
  }
  if (!agent_exists(key_path) && agent_create_deploy_key(key_path) != 0) {
    return -1;
  }
  /* before the repository, so that its first commit lists the key */
  if (!agent_exists(device_key_path) && agent_create_device_key(device_key_path) != 0) {
    return -1;
  }
  if (agent_device_load(config, &device) != 0) {
    return -1;
  }

  if (!agent_exists(git_path)) {
    tig_repository *repository;

    if (tig_repository_create(repository_path, &device.device, &repository) != TIG_OK) {
      agent_log_error("cannot create %s: %s", repository_path, tig_get_last_error());
      return -1;
    }
    tig_repository_close(repository);
    agent_log_info("created TiG repository %s", repository_path);
  }

  return agent_configure_remote(config);
}

int agent_open(const agent_config *config, agent_device *device, tig_repository **out) {
  char path[AGENT_PATH_MAX];

  if (agent_path(path, sizeof(path), config, "repository") != 0) {
    return -1;
  }
  if (tig_repository_open(path, out) != TIG_OK) {
    agent_log_error("cannot open %s (run tig-agent init first): %s", path, tig_get_last_error());
    return -1;
  }
  if (device == 0) {
    return 0;
  }
  if (agent_device_load(config, device) != 0) {
    /* agent_device_load has logged why */
  } else if (tig_repository_set_device(*out, &device->device) != TIG_OK) {
    agent_log_error("cannot sign commits in %s: %s", path, tig_get_last_error());
  } else {
    return 0;
  }
  tig_repository_close(*out);
  *out = 0;
  return -1;
}

/* serialises the daemon and manual commands; returns a descriptor for agent_unlock, or -1 */
int agent_lock(const agent_config *config) {
  char path[AGENT_PATH_MAX];
  int lock;

  if (agent_path(path, sizeof(path), config, "agent.lock") != 0) {
    return -1;
  }

  lock = open(path, O_RDWR | O_CREAT, 0640);
  if (lock < 0) {
    agent_log_error("%s: %s", path, strerror(errno));
    return -1;
  }
  while (flock(lock, LOCK_EX) != 0) {
    if (errno != EINTR) {
      agent_log_error("%s: %s", path, strerror(errno));
      close(lock);
      return -1;
    }
  }
  return lock;
}

void agent_unlock(int lock) {
  if (lock >= 0) {
    close(lock);
  }
}
