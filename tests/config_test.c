#include "agent.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                           \
    }                                                                       \
  } while (0)

static int failures;

static void test_durations(void) {
  int64_t ms = 0;

  CHECK(agent_parse_duration("30s", &ms) == 0 && ms == 30000);
  CHECK(agent_parse_duration("5m", &ms) == 0 && ms == 300000);
  CHECK(agent_parse_duration("1h", &ms) == 0 && ms == 3600000);
  CHECK(agent_parse_duration("2d", &ms) == 0 && ms == 172800000);

  CHECK(agent_parse_duration("", &ms) != 0);
  CHECK(agent_parse_duration("5", &ms) != 0);
  CHECK(agent_parse_duration("0s", &ms) != 0);
  CHECK(agent_parse_duration("-5s", &ms) != 0);
  CHECK(agent_parse_duration(" 5s", &ms) != 0);
  CHECK(agent_parse_duration("5 s", &ms) != 0);
  CHECK(agent_parse_duration("5w", &ms) != 0);
  CHECK(agent_parse_duration("99999999999999999999s", &ms) != 0);
  CHECK(agent_parse_duration("9999999999999999d", &ms) != 0);
}

static void test_parse(void) {
  agent_config config;

  agent_config_defaults(&config);
  CHECK(strcmp(config.data_dir, "/var/lib/tig") == 0);
  CHECK(config.remote[0] == '\0');
  CHECK(config.sample_interval_ms == 60000);
  CHECK(config.commit_interval_ms == 900000);
  CHECK(config.push_interval_ms == 3600000);

  CHECK(agent_config_parse(&config,
                           "# comment\n"
                           "\n"
                           "  data_dir = /srv/tig  \n"
                           "remote=git@github.com:example/machine.git\r\n"
                           "sample_interval = 10s\n"
                           "commit_interval = 5m\n"
                           "push_interval = 30m",
                           "test") == 0);
  CHECK(strcmp(config.data_dir, "/srv/tig") == 0);
  CHECK(strcmp(config.remote, "git@github.com:example/machine.git") == 0);
  CHECK(config.sample_interval_ms == 10000);
  CHECK(config.commit_interval_ms == 300000);
  CHECK(config.push_interval_ms == 1800000);

  CHECK(agent_config_parse(&config, "remote =\n", "test") == 0);
  CHECK(config.remote[0] == '\0');

  CHECK(agent_config_parse(&config, "unknown = 1\n", "test") != 0);
  CHECK(agent_config_parse(&config, "data_dir\n", "test") != 0);
  CHECK(agent_config_parse(&config, "data_dir = relative\n", "test") != 0);
  CHECK(agent_config_parse(&config, "data_dir = /it's\n", "test") != 0);
  CHECK(agent_config_parse(&config, "push_interval = soon\n", "test") != 0);
}

static void test_notary(void) {
  agent_config config;

  agent_config_defaults(&config);
  CHECK(config.notary[0] == '\0'); /* nothing is sent to a notary unless it is asked for */
  CHECK(strcmp(config.issuer, "tigdb.org") == 0);
  CHECK(config.issuer_keys[0] == '\0');

  CHECK(agent_config_parse(&config,
                           "notary = https://notary.tigdb.org\n"
                           "issuer = tigdb.org\n"
                           "issuer_keys = http://127.0.0.1:8080/.well-known/tig-public-key\n",
                           "test") == 0);
  CHECK(strcmp(config.notary, "https://notary.tigdb.org") == 0);
  CHECK(strcmp(config.issuer_keys, "http://127.0.0.1:8080/.well-known/tig-public-key") == 0);

  CHECK(agent_config_parse(&config, "notary =\nissuer_keys =\n", "test") == 0);
  CHECK(config.notary[0] == '\0' && config.issuer_keys[0] == '\0');

  CHECK(agent_config_parse(&config, "notary = notary.tigdb.org\n", "test") != 0);
  CHECK(agent_config_parse(&config, "notary = http://notary.example.com\n", "test") != 0);
  CHECK(agent_config_parse(&config, "notary = https://user:pass@notary.example.com\n", "test") != 0);
  CHECK(agent_config_parse(&config, "issuer_keys = http://example.com/keys\n", "test") != 0);
  CHECK(agent_config_parse(&config, "issuer = tigdb.org/keys\n", "test") != 0);
  CHECK(agent_config_parse(&config, "issuer =\n", "test") != 0);
}

int main(void) {
  test_durations();
  test_parse();
  test_notary();
  return failures == 0 ? 0 : 1;
}
