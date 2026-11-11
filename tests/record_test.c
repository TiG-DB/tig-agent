/* Initial state, discovery, boots and changes, from made-up inventories, checked against the examples in
   tig-docs/vocabularies/DEVICE.md. */
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                                          \
  do {                                                                                            \
    if (!(condition)) {                                                                           \
      fprintf(stderr, "%s:%d: CHECK(%s) failed; last error: %s\n", __FILE__, __LINE__, #condition, \
              tig_get_last_error());                                                              \
      ++failures;                                                                                 \
    }                                                                                             \
  } while (0)

/* 2026-11-12T09:30:02Z, when the agent is installed; it booted at 2026-11-10T18:44:51Z */
#define INSTALLED INT64_C(1794475802000)
#define BOOTED INT64_C(1794336291000)
#define HOUR INT64_C(3600000)

static int failures;
static int64_t fake_now;

static int64_t fake_clock(void *context) {
  (void)context;
  return fake_now;
}

static tig_node *parse(const char *yaml) {
  tig_node *node = 0;

  if (tig_node_parse(yaml, strlen(yaml), &node) != TIG_OK) {
    fprintf(stderr, "cannot parse: %s\n%s\n", tig_get_last_error(), yaml);
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

static void test_hashed_ids(void) {
  char id[64];

  agent_hashed_id("disk", "Example NVMe 1TB\nS6EXAMPLE001", id, sizeof(id));
  CHECK(strcmp(id, "disk/7a858f1b") == 0);
  agent_hashed_id("disk", "Example NVMe 2TB\nS7EXAMPLE002", id, sizeof(id));
  CHECK(strcmp(id, "disk/eb3f1f45") == 0);
  agent_hashed_id("network", "00:00:5e:00:53:01", id, sizeof(id));
  CHECK(strcmp(id, "network/87924582") == 0);
  /* more than one block */
  agent_hashed_id("x", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", id, sizeof(id));
  CHECK(strcmp(id, "x/248d6a61") == 0);
}

static const char *const disk_1tb =
    "disk/7a858f1b: { model: Example NVMe 1TB, serial: S6EXAMPLE001, capacity: 1000204886016, transport: nvme, "
    "rotational: false }\n";
static const char *const disk_2tb =
    "disk/eb3f1f45: { model: Example NVMe 2TB, serial: S7EXAMPLE002, capacity: 2000398934016, transport: nvme, "
    "rotational: false }\n";

/* the example in DEVICE.md: the disk cloned to a larger one and the root filesystem grown */
static void test_diff(void) {
  char state[1024];
  char observed[1024];
  tig_node *before;
  tig_node *after;
  tig_node *actions;

  snprintf(state, sizeof(state),
           "%sfilesystem/root: { type: ext4, uuid: 0b3e6c1d, size: 983349346304, disks: [disk/7a858f1b] }\n"
           "entity: { model: ignored }\nboiler: { make: not ours }\n",
           disk_1tb);
  snprintf(observed, sizeof(observed),
           "%sfilesystem/root: { type: ext4, uuid: 0b3e6c1d, size: 1967925690368, disks: [disk/eb3f1f45] }\n", disk_2tb);
  before = parse(state);
  after = parse(observed);

  actions = agent_diff(before, after);
  CHECK(actions != 0);
  CHECK(renders_as(actions,
                   "- type: replaced\n"
                   "  changes:\n"
                   "    - remove: disk/7a858f1b\n"
                   "    - add: disk/eb3f1f45\n"
                   "      set:\n"
                   "        model: Example NVMe 2TB\n"
                   "        serial: S7EXAMPLE002\n"
                   "        capacity: 2000398934016\n"
                   "        transport: nvme\n"
                   "        rotational: false\n"
                   "    - update: filesystem/root\n"
                   "      set: { disks: [disk/eb3f1f45] }\n"
                   "- type: resized\n"
                   "  changes:\n"
                   "    - update: filesystem/root\n"
                   "      set: { size: 1967925690368 }\n"));
  tig_node_free(actions);

  /* nothing changed: no actions */
  actions = agent_diff(after, after);
  CHECK(actions != 0 && tig_node_count(actions) == 0);
  tig_node_free(actions);
  tig_node_free(before);
  tig_node_free(after);

  /* a new disk, one taken out, a filesystem recreated, an upgrade, a kernel (left to boots) and a lost attribute */
  before = parse("os: { id: ubuntu, version: \"24.04\", kernel: 6.8.0-48-generic }\n"
                 "filesystem/data: { type: ext4, uuid: aaaa, size: 1 }\n"
                 "network/87924582: { mac: \"00:00:5e:00:53:01\", driver: igc, medium: ethernet }\n"
                 "battery/bat0: { technology: Li-ion }\n");
  after = parse("os: { id: ubuntu, version: \"26.04\", kernel: 7.0.0-1-generic }\n"
                "filesystem/data: { type: xfs, uuid: bbbb, size: 1 }\n"
                "network/87924582: { mac: \"00:00:5e:00:53:01\", medium: ethernet }\n"
                "memory: { capacity: 8589934592 }\n");
  actions = agent_diff(before, after);
  CHECK(renders_as(actions,
                   "- type: installed\n"
                   "  changes:\n"
                   "    - add: memory\n"
                   "      set: { capacity: 8589934592 }\n"
                   "- type: replaced\n"
                   "  changes:\n"
                   "    - remove: filesystem/data\n"
                   "    - add: filesystem/data\n"
                   "      set: { type: xfs, uuid: bbbb, size: 1 }\n"
                   "- type: removed\n"
                   "  changes:\n"
                   "    - remove: battery/bat0\n"
                   "- type: upgraded\n"
                   "  changes:\n"
                   "    - update: os\n"
                   "      set: { version: \"26.04\" }\n"
                   "- type: reconfigured\n"
                   "  changes:\n"
                   "    - update: network/87924582\n"
                   "      unset: [driver]\n"));
  tig_node_free(actions);
  tig_node_free(before);
  tig_node_free(after);
}

typedef struct counts {
  int events;
  int boots;
  int discoveries;
  int changes;
  char last_change_time[32];
  char last_change_not_before[32];
  tig_node *last_boot;
} counts;

static tig_status count(void *context, const char *id, const tig_node *event) {
  counts *c = (counts *)context;
  const char *type = tig_node_text(tig_node_get(event, "type"));

  (void)id;
  ++c->events;
  if (type != 0 && strcmp(type, "boot") == 0) {
    ++c->boots;
    tig_node_free(c->last_boot);
    c->last_boot = tig_node_copy(event);
  } else if (type != 0 && strcmp(type, "discovery") == 0) {
    ++c->discoveries;
  } else {
    const char *not_before = tig_node_text(tig_node_get(event, "not_before"));
    ++c->changes;
    snprintf(c->last_change_time, sizeof(c->last_change_time), "%s", tig_node_text(tig_node_get(event, "time")));
    snprintf(c->last_change_not_before, sizeof(c->last_change_not_before), "%s", not_before != 0 ? not_before : "");
  }
  CHECK(strncmp(tig_node_text(tig_node_get(event, "source")), "tig-agent ", 10) == 0);
  return TIG_OK;
}

static counts count_events(tig_repository *repository) {
  counts c;

  memset(&c, 0, sizeof(c));
  CHECK(tig_events_each(repository, count, &c) == TIG_OK);
  return c;
}

static void observe(agent_observation *observation, const char *boot_id, int64_t boot_ms, const char *items) {
  tig_node_free(observation->inventory.items);
  observation->inventory.items = parse(items);
  snprintf(observation->boot.id, sizeof(observation->boot.id), "%s", boot_id);
  observation->boot.time_ms = boot_ms;
  observation->now_ms = fake_now;
}

static void test_record(void) {
  static const char *const first =
      "memory: { capacity: 34359738368 }\n"
      "disk/7a858f1b: { model: Example NVMe 1TB, serial: S6EXAMPLE001, capacity: 1000204886016 }\n"
      "filesystem/root: { type: ext4, uuid: 0b3e6c1d, size: 983349346304, disks: [disk/7a858f1b] }\n"
      "os: { id: ubuntu, version: \"24.04\", name: Ubuntu 24.04.1 LTS, kernel: 6.8.0-48-generic }\n";
  static const char *const second =
      "memory: { capacity: 34359738368 }\n"
      "disk/eb3f1f45: { model: Example NVMe 2TB, serial: S7EXAMPLE002, capacity: 2000398934016 }\n"
      "filesystem/root: { type: ext4, uuid: 0b3e6c1d, size: 1967925690368, disks: [disk/eb3f1f45] }\n"
      "os: { id: ubuntu, version: \"24.04\", name: Ubuntu 24.04.1 LTS, kernel: 6.8.0-51-generic }\n";
  tig_options options = {0};
  tig_repository *repository;
  agent_observation observation;
  tig_node *state;
  tig_node *initial;
  counts c;
  char template[] = "/tmp/tig-agent-record-XXXXXX";
  char path[256];
  char command[512];
  const char *dir = mkdtemp(template);

  CHECK(dir != 0);
  if (dir == 0) {
    return;
  }
  snprintf(path, sizeof(path), "%s/repository", dir);
  options.clock = fake_clock;
  CHECK(tig_init(&options) == TIG_OK);
  CHECK(tig_repository_create(path, 0, &repository) == TIG_OK);

  memset(&observation, 0, sizeof(observation));
  observation.inventory.entity = parse("{ vendor: Example Computers, serial: EXC1234567, architecture: x86_64 }");
  observation.inventory.since = parse("{ filesystem/root: \"2025-01-03T14:22:10Z\", os: \"2025-01-03T14:22:10Z\" }");

  /* installed: the initial state, a discovery, and the boot in progress, which has no changes */
  fake_now = INSTALLED;
  observe(&observation, "4d1b7a90-2c3e-4f6a-8b5d-9e0c1f2a3b4c", BOOTED, first);
  observation.first_after_boot = 1;
  CHECK(agent_record(repository, &observation) == 0);
  c = count_events(repository);
  CHECK(c.events == 2 && c.discoveries == 1 && c.boots == 1 && c.changes == 0);
  CHECK(renders_as(c.last_boot,
                   "time: \"2026-11-10T18:44:51Z\"\n"
                   "type: boot\n"
                   "source: tig-agent " TIG_AGENT_VERSION "\n"
                   "actions:\n"
                   "  - type: booted\n"
                   "    boot: 4d1b7a90-2c3e-4f6a-8b5d-9e0c1f2a3b4c\n"));
  tig_node_free(c.last_boot);

  snprintf(command, sizeof(command), "%s/state/initial.yaml", path);
  {
    FILE *file = fopen(command, "rb");
    char text[1024] = "";
    size_t length = file != 0 ? fread(text, 1, sizeof(text) - 1, file) : 0;

    text[length] = '\0';
    if (file != 0) {
      fclose(file);
    }
    initial = parse(text);
  }
  CHECK(renders_as(initial,
                   "kind: device\n"
                   "vocabulary: tig-device/1\n"
                   "items:\n"
                   "  entity: { vendor: Example Computers, serial: EXC1234567, architecture: x86_64 }\n"));
  tig_node_free(initial);

  CHECK(tig_state_replay(repository, TIG_TIME_LATEST, &state) == TIG_OK);
  CHECK(strcmp(tig_node_text(tig_node_get(tig_node_get(tig_node_get(state, "items"), "filesystem/root"), "uuid")),
               "0b3e6c1d") == 0);
  tig_node_free(state);

  /* later in the same boot, with nothing changed: nothing recorded */
  fake_now = INSTALLED + HOUR;
  observe(&observation, "4d1b7a90-2c3e-4f6a-8b5d-9e0c1f2a3b4c", BOOTED, first);
  observation.first_after_boot = 0;
  observation.previous_ms = INSTALLED;
  CHECK(agent_record(repository, &observation) == 0);
  c = count_events(repository);
  CHECK(c.events == 2);
  tig_node_free(c.last_boot);

  /* the next boot finds the disk cloned to a larger one, and a new kernel */
  fake_now = INSTALLED + 200 * HOUR;
  observe(&observation, "8c2e4f1a-6b0d-4c55-9e3a-2f1d7c9b0a64", INSTALLED + 199 * HOUR, second);
  observation.first_after_boot = 1;
  observation.previous_ms = INSTALLED + 150 * HOUR;
  CHECK(agent_record(repository, &observation) == 0);
  c = count_events(repository);
  CHECK(c.events == 4 && c.boots == 2 && c.changes == 1);
  CHECK(strcmp(c.last_change_time, "2026-11-20T16:30:02Z") == 0);       /* the boot */
  CHECK(strcmp(c.last_change_not_before, "2026-11-18T15:30:02Z") == 0); /* the previous comparison */
  CHECK(renders_as(c.last_boot,
                   "time: \"2026-11-20T16:30:02Z\"\n"
                   "type: boot\n"
                   "source: tig-agent " TIG_AGENT_VERSION "\n"
                   "actions:\n"
                   "  - type: booted\n"
                   "    boot: 8c2e4f1a-6b0d-4c55-9e3a-2f1d7c9b0a64\n"
                   "    changes:\n"
                   "      - update: os\n"
                   "        set: { kernel: 6.8.0-51-generic }\n"));
  tig_node_free(c.last_boot);

  /* state now matches the device */
  CHECK(tig_state_replay(repository, TIG_TIME_LATEST, &state) == TIG_OK);
  {
    tig_node *items = tig_node_copy(tig_node_get(state, "items"));
    tig_node *expected = parse(second);

    tig_node_unset(items, "entity");
    CHECK(tig_node_equal(items, expected));
    tig_node_free(items);
    tig_node_free(expected);
  }
  tig_node_free(state);

  /* an upgrade while it runs is dated when it is found, after the previous comparison */
  fake_now = INSTALLED + 210 * HOUR;
  observe(&observation, "8c2e4f1a-6b0d-4c55-9e3a-2f1d7c9b0a64", INSTALLED + 199 * HOUR,
          "memory: { capacity: 34359738368 }\n"
          "disk/eb3f1f45: { model: Example NVMe 2TB, serial: S7EXAMPLE002, capacity: 2000398934016 }\n"
          "filesystem/root: { type: ext4, uuid: 0b3e6c1d, size: 1967925690368, disks: [disk/eb3f1f45] }\n"
          "os: { id: ubuntu, version: \"24.04\", name: Ubuntu 24.04.2 LTS, kernel: 6.8.0-51-generic }\n");
  observation.first_after_boot = 0;
  observation.previous_ms = INSTALLED + 209 * HOUR;
  CHECK(agent_record(repository, &observation) == 0);
  c = count_events(repository);
  CHECK(c.events == 5 && c.boots == 2 && c.changes == 2);
  CHECK(strcmp(c.last_change_time, "2026-11-21T03:30:02Z") == 0);
  CHECK(strcmp(c.last_change_not_before, "2026-11-21T02:30:02Z") == 0);
  tig_node_free(c.last_boot);

  /* everything recorded commits */
  CHECK(tig_repository_commit(repository, 0) == TIG_OK);

  agent_inventory_free(&observation.inventory);
  tig_repository_close(repository);
  tig_shutdown();
  snprintf(command, sizeof(command), "rm -rf '%s'", dir);
  if (system(command) != 0) {
    fprintf(stderr, "could not remove %s\n", dir);
  }
}

int main(void) {
  test_hashed_ids();
  test_diff();
  test_record();

  if (failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
