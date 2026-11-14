/* Compose stacks from made-up Docker Engine API replies, checked against DEVICE.md, "Compose stacks". */
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                          \
  do {                                                                                            \
    if (!(condition)) {                                                                           \
      fprintf(stderr, "%s:%d: CHECK(%s) failed; last error: %s\n", __FILE__, __LINE__, #condition, \
              tig_get_last_error());                                                              \
      ++failures;                                                                                 \
    }                                                                                             \
  } while (0)

static int failures;

static tig_node *parse(const char *text) {
  tig_node *node = 0;

  if (tig_node_parse(text, strlen(text), &node) != TIG_OK) {
    fprintf(stderr, "cannot parse: %s\n%s\n", tig_get_last_error(), text);
    ++failures;
  }
  return node;
}

static int renders_as(const tig_node *node, const char *expected) {
  char *text = 0;
  int same = tig_node_render(node, &text) == TIG_OK && strcmp(text, expected) == 0;

  if (!same) {
    fprintf(stderr, "rendered:\n%s\nexpected:\n%s\n", text != 0 ? text : "(nothing)", expected);
  }
  tig_free(text);
  return same;
}

/* as the Engine API writes it: one line of JSON, with no space after a colon */
static const char *const list_json =
    "[{\"Id\":\"3f9a0c1d2e4b\",\"Names\":[\"/shop-web-1\"],\"Image\":\"nginx:1.27\",\"ImageID\":\"sha256:aa11\","
    "\"Command\":\"nginx -g 'daemon off;'\",\"Created\":1794336291,\"Ports\":[{\"IP\":\"0.0.0.0\",\"PrivatePort\":80,"
    "\"PublicPort\":8080,\"Type\":\"tcp\"}],\"Labels\":{\"com.docker.compose.project\":\"Shop_App\","
    "\"com.docker.compose.service\":\"web\",\"com.docker.compose.container-number\":\"1\"},\"State\":\"running\","
    "\"Status\":\"Up 2 days (healthy)\"},"
    "{\"Id\":\"7b2c\",\"Image\":\"postgres:16\",\"ImageID\":\"sha256:bb22\",\"Created\":1794300000,"
    "\"Labels\":{\"com.docker.compose.project\":\"Shop_App\",\"com.docker.compose.service\":\"db\","
    "\"com.docker.compose.container-number\":\"1\"},\"State\":\"exited\"},"
    "{\"Id\":\"9d00\",\"Image\":\"alpine\",\"ImageID\":\"sha256:cc33\",\"Created\":1794340000,"
    "\"Labels\":{\"com.docker.compose.project\":\"Shop_App\",\"com.docker.compose.service\":\"web\","
    "\"com.docker.compose.container-number\":\"1\",\"com.docker.compose.oneoff\":\"True\"},\"State\":\"exited\"},"
    "{\"Id\":\"11ee\",\"Image\":\"redis\",\"ImageID\":\"sha256:dd44\",\"Created\":1794340000,\"Labels\":{},"
    "\"State\":\"running\"}]";

static void test_items(void) {
  agent_inventory inventory;
  tig_node *containers = parse(list_json);

  memset(&inventory, 0, sizeof(inventory));
  inventory.items = tig_node_map();
  inventory.since = tig_node_map();

  CHECK(agent_docker_items(containers, &inventory) == 0);
  /* the one-off and the container Compose didn't make are left out; names are made safe for item IDs */
  CHECK(renders_as(inventory.items,
                   "container/shop-app/web/1: { image: \"nginx:1.27\", digest: \"sha256:aa11\", created: \"2026-11-10T18:44:51Z\" }\n"
                   "stack/shop-app: { project: Shop_App }\n"
                   "container/shop-app/db/1: { image: \"postgres:16\", digest: \"sha256:bb22\", created: \"2026-11-10T08:40:00Z\" }\n"));
  /* a stack has been there since its oldest container was created */
  CHECK(strcmp(tig_node_text(tig_node_get(inventory.since, "stack/shop-app")), "2026-11-10T08:40:00Z") == 0);
  CHECK(strcmp(tig_node_text(tig_node_get(inventory.since, "container/shop-app/web/1")), "2026-11-10T18:44:51Z") == 0);

  tig_node_free(containers);
  tig_node_free(inventory.items);
  tig_node_free(inventory.since);
}

static void test_names_and_memory(void) {
  char name[64];
  tig_node *stats;

  agent_docker_stream_name("container/shop-app/web/1", name, sizeof(name));
  CHECK(strcmp(name, "container.shop-app.web.1") == 0);

  /* cgroup v2, then v1; the page cache is not counted */
  stats = parse("{\"memory_stats\":{\"usage\":104857600,\"stats\":{\"anon\":1,\"inactive_file\":4194304}}}");
  CHECK(agent_docker_memory(stats) == 104857600 - 4194304);
  tig_node_free(stats);
  stats = parse("{\"memory_stats\":{\"usage\":104857600,\"stats\":{\"total_inactive_file\":1048576}}}");
  CHECK(agent_docker_memory(stats) == 104857600 - 1048576);
  tig_node_free(stats);
  stats = parse("{\"memory_stats\":{\"usage\":100,\"stats\":{\"inactive_file\":500}}}");
  CHECK(agent_docker_memory(stats) == 0);
  tig_node_free(stats);
  /* a container that isn't running has none */
  stats = parse("{\"memory_stats\":{},\"cpu_stats\":{}}");
  CHECK(agent_docker_memory(stats) == -1);
  tig_node_free(stats);
}

/* what changes when containers come and go, and when one is recreated */
static void test_diff(void) {
  tig_node *before = parse("stack/shop: { project: shop }\n"
                           "container/shop/web/1: { image: nginx:1.27, digest: sha256:aa11, created: \"2026-11-10T18:44:51Z\" }\n"
                           "container/shop/db/1: { image: postgres:16, digest: sha256:bb22, created: \"2026-11-10T08:40:00Z\" }\n");
  tig_node *after;
  tig_node *actions;

  /* a pulled tag and a recreated container is one upgrade, with `created` left to it; the same image recreated is a
     reconfiguration */
  after = parse("stack/shop: { project: shop }\n"
                "container/shop/web/1: { image: nginx:1.27, digest: sha256:ee55, created: \"2026-11-12T09:00:00Z\" }\n"
                "container/shop/db/1: { image: postgres:16, digest: sha256:bb22, created: \"2026-11-12T09:00:01Z\" }\n");
  actions = agent_diff(before, after);
  CHECK(renders_as(actions,
                   "- type: upgraded\n"
                   "  changes:\n"
                   "    - update: container/shop/web/1\n"
                   "      set: { digest: \"sha256:ee55\", created: \"2026-11-12T09:00:00Z\" }\n"
                   "- type: reconfigured\n"
                   "  changes:\n"
                   "    - update: container/shop/db/1\n"
                   "      set: { created: \"2026-11-12T09:00:01Z\" }\n"));
  tig_node_free(actions);
  tig_node_free(after);

  /* one service removed and another added is not a replacement */
  after = parse("stack/shop: { project: shop }\n"
                "container/shop/db/1: { image: postgres:16, digest: sha256:bb22, created: \"2026-11-10T08:40:00Z\" }\n"
                "container/shop/cache/1: { image: redis:7, digest: sha256:ff66, created: \"2026-11-12T09:00:00Z\" }\n");
  actions = agent_diff(before, after);
  CHECK(renders_as(actions,
                   "- type: removed\n"
                   "  changes:\n"
                   "    - remove: container/shop/web/1\n"
                   "- type: installed\n"
                   "  changes:\n"
                   "    - add: container/shop/cache/1\n"
                   "      set: { image: \"redis:7\", digest: \"sha256:ff66\", created: \"2026-11-12T09:00:00Z\" }\n"));
  tig_node_free(actions);
  tig_node_free(after);

  /* Docker unreachable: nothing observed, and the stacks are left as they were recorded */
  after = parse("entity: {}\n");
  actions = agent_diff_with(before, after, 0);
  CHECK(actions != 0 && tig_node_count(actions) == 0);
  tig_node_free(actions);
  /* ...but with docker read, `docker compose down` removes everything */
  actions = agent_diff_with(before, after, 1);
  CHECK(actions != 0 && tig_node_count(actions) > 0);
  tig_node_free(actions);
  tig_node_free(after);
  tig_node_free(before);
}

/* more containers than it can record: no inventory at all, rather than one that takes the rest for removed */
static void test_too_many(void) {
  enum { COUNT = 257 };
  char *json = (char *)malloc(COUNT * 300);
  size_t used = 0;
  agent_inventory inventory;
  tig_node *containers;
  int i;
  int result;

  used += (size_t)sprintf(json, "[");
  for (i = 0; i < COUNT; ++i) {
    used += (size_t)sprintf(json + used,
                            "%s{\"Id\":\"%04x\",\"Image\":\"x\",\"ImageID\":\"sha256:1\",\"Created\":1794336291,"
                            "\"Labels\":{\"com.docker.compose.project\":\"p\",\"com.docker.compose.service\":\"s%d\"},"
                            "\"State\":\"running\"}",
                            i == 0 ? "" : ",", i, i);
  }
  strcat(json, "]");
  containers = parse(json);
  memset(&inventory, 0, sizeof(inventory));
  inventory.items = tig_node_map();
  inventory.since = tig_node_map();
  result = agent_docker_items(containers, &inventory);
  CHECK(result != 0);
  tig_node_free(containers);
  tig_node_free(inventory.items);
  tig_node_free(inventory.since);
  free(json);
}

static void test_config(void) {
  agent_config config;

  agent_config_defaults(&config);
  CHECK(!agent_docker_enabled(&config));
  CHECK(agent_config_parse(&config, "docker_host = unix:///run/docker-proxy.sock\n", "test") == 0);
  CHECK(agent_docker_enabled(&config));
  CHECK(strcmp(config.docker_host, "unix:///run/docker-proxy.sock") == 0);
  CHECK(agent_config_parse(&config, "docker_host = http://127.0.0.1:2375/\n", "test") == 0);
  CHECK(agent_config_parse(&config, "docker_host =\n", "test") == 0);
  CHECK(!agent_docker_enabled(&config));

  /* only local sockets and plain http, with no credentials, quoting or spaces */
  CHECK(agent_config_parse(&config, "docker_host = /var/run/docker.sock\n", "test") != 0);
  CHECK(agent_config_parse(&config, "docker_host = https://example.com\n", "test") != 0);
  CHECK(agent_config_parse(&config, "docker_host = http://user:pass@host:2375\n", "test") != 0);
  CHECK(agent_config_parse(&config, "docker_host = unix:///a b\n", "test") != 0);
  CHECK(agent_config_parse(&config, "docker_host = unix:///a'b\n", "test") != 0);
}

int main(void) {
  test_items();
  test_names_and_memory();
  test_diff();
  test_too_many();
  test_config();
  return failures == 0 ? 0 : 1;
}
