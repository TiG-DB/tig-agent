#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TIG_AGENT_VERSION
#define TIG_AGENT_VERSION "0.0.0"
#endif

static void agent_usage(FILE *out) {
  fputs("usage: tig-agent [--config PATH] COMMAND\n"
        "\n"
        "Records this machine's timeline into a TiG repository.\n"
        "\n"
        "Commands:\n"
        "  init      create the data directory, TiG repository and deploy key\n"
        "  collect   sample telemetry once\n"
        "  commit    commit readings sampled since the last commit\n"
        "  push      commit, then push to the configured remote (resumes after a rejected push)\n"
        "  status    show the repository, recent activity and deploy key\n"
        "  daemon    sample, commit and push on the configured intervals\n"
        "  version   show the version\n"
        "\n"
        "The configuration is read from --config, $TIG_AGENT_CONFIG or " AGENT_DEFAULT_CONFIG ".\n",
        out);
}

static void agent_print_version(void) {
  int major;
  int minor;
  int revision;

  tig_git_engine_version(&major, &minor, &revision);
  printf("tig-agent %s (libtig %s, libgit2 %d.%d.%d)\n", TIG_AGENT_VERSION, tig_version(), major, minor, revision);
}

static int agent_command(const char *command, const agent_config *config) {
  if (strcmp(command, "init") == 0) {
    if (agent_setup(config) != 0) {
      return AGENT_EXIT_ERROR;
    }
    return agent_status_print(config);
  }
  if (strcmp(command, "collect") == 0) {
    return agent_collect(config) == 0 ? AGENT_EXIT_OK : AGENT_EXIT_ERROR;
  }
  if (strcmp(command, "commit") == 0) {
    return agent_commit(config) == 0 ? AGENT_EXIT_OK : AGENT_EXIT_ERROR;
  }
  if (strcmp(command, "push") == 0) {
    return agent_sync(config, 1);
  }
  if (strcmp(command, "status") == 0) {
    return agent_status_print(config);
  }
  if (strcmp(command, "daemon") == 0) {
    agent_log_info("tig-agent %s started, storing data in %s", TIG_AGENT_VERSION, config->data_dir);
    return agent_daemon(config);
  }

  fprintf(stderr, "tig-agent: unknown command \"%s\"\n\n", command);
  agent_usage(stderr);
  return AGENT_EXIT_USAGE;
}

int main(int argc, char **argv) {
  const char *config_path = getenv("TIG_AGENT_CONFIG");
  const char *command = 0;
  agent_config config;
  int result;
  int i;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else if (strncmp(argv[i], "--config=", 9) == 0) {
      config_path = argv[i] + 9;
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      agent_usage(stdout);
      return AGENT_EXIT_OK;
    } else if (strcmp(argv[i], "--version") == 0) {
      command = "version";
    } else if (argv[i][0] != '-' && command == 0) {
      command = argv[i];
    } else {
      agent_usage(stderr);
      return AGENT_EXIT_USAGE;
    }
  }
  if (command == 0) {
    agent_usage(stderr);
    return AGENT_EXIT_USAGE;
  }
  if (config_path == 0 || config_path[0] == '\0') {
    config_path = AGENT_DEFAULT_CONFIG;
  }

  if (tig_init(0) != TIG_OK) {
    agent_log_error("cannot start libtig: %s", tig_get_last_error());
    return AGENT_EXIT_ERROR;
  }

  if (strcmp(command, "version") == 0) {
    agent_print_version();
    result = AGENT_EXIT_OK;
  } else {
    agent_config_defaults(&config);
    result = agent_config_load(&config, config_path) == 0 ? agent_command(command, &config) : AGENT_EXIT_ERROR;
  }

  tig_shutdown();
  return result;
}
