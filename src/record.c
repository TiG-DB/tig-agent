/* State and events for the tig-device/1 vocabulary (tig-docs/vocabularies/DEVICE.md): the initial state and discovery
   when the repository is new, an event for each boot, and an event whenever the device no longer matches its state. */
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef TIG_AGENT_VERSION
#define TIG_AGENT_VERSION "0.0.0"
#endif

#define AGENT_SOURCE "tig-agent " TIG_AGENT_VERSION

/* the kinds this agent records, in the order changes are listed */
static const char *const agent_kinds[] = {"firmware", "cpu", "memory", "disk", "filesystem", "network", "os", "battery",
                                          "stack", "container"};

#define AGENT_KIND_COUNT (sizeof(agent_kinds) / sizeof(agent_kinds[0]))

static int agent_is_kind(const char *id, const char *kind) {
  size_t length = strlen(kind);

  return strncmp(id, kind, length) == 0 && (id[length] == '\0' || id[length] == '/');
}

static int agent_is_recorded(const char *id) {
  size_t i;

  for (i = 0; i < AGENT_KIND_COUNT; ++i) {
    if (agent_is_kind(id, agent_kinds[i])) {
      return 1;
    }
  }
  return 0;
}

/* Compose stacks and containers are compared only when Docker could be read, and a container is never "replaced" by a
   different one: each is a service's replica, so one gone and another new are separate */
static int agent_is_docker(const char *id) {
  return agent_is_kind(id, "stack") || agent_is_kind(id, "container");
}

/* --- building events --- */

typedef struct agent_builder {
  int failed;
} agent_builder;

static void agent_check(agent_builder *builder, tig_status status) {
  if (status != TIG_OK) {
    builder->failed = 1;
  }
}

static tig_node *agent_change(agent_builder *builder, const char *operation, const char *id) {
  tig_node *change = tig_node_map();

  agent_check(builder, tig_node_set(change, operation, tig_node_string(id)));
  return change;
}

static tig_node *agent_action(agent_builder *builder, tig_node *actions, const char *type) {
  tig_node *action = tig_node_map();
  tig_node *pointer = action;

  agent_check(builder, tig_node_set(action, "type", tig_node_string(type)));
  agent_check(builder, tig_node_append(actions, action));
  return builder->failed ? 0 : pointer;
}

/* appends to the action's changes, creating them */
static void agent_add_change(agent_builder *builder, tig_node *action, tig_node *change) {
  if (action == 0) {
    tig_node_free(change);
    builder->failed = 1;
    return;
  }
  if (tig_node_get(action, "changes") == 0) {
    agent_check(builder, tig_node_set(action, "changes", tig_node_list()));
  }
  agent_check(builder, tig_node_append(tig_node_get(action, "changes"), change));
}

static tig_node *agent_new_event(agent_builder *builder, int64_t time_ms, int64_t not_before_ms, const char *type) {
  tig_node *event = tig_node_map();
  char text[32];

  agent_format_time(time_ms, text, sizeof(text));
  agent_check(builder, tig_node_set(event, "time", tig_node_string(text)));
  if (not_before_ms > 0) {
    agent_format_time(not_before_ms, text, sizeof(text));
    agent_check(builder, tig_node_set(event, "not_before", tig_node_string(text)));
  }
  if (type != 0) {
    agent_check(builder, tig_node_set(event, "type", tig_node_string(type)));
  }
  agent_check(builder, tig_node_set(event, "source", tig_node_string(AGENT_SOURCE)));
  return event;
}

/* changes as replay applies them, to keep the working copy of state in step with the actions */
static void agent_apply(agent_builder *builder, tig_node *items, const tig_node *change) {
  const tig_node *set = tig_node_get(change, "set");
  const tig_node *unset = tig_node_get(change, "unset");
  const char *id;
  size_t i;

  if ((id = tig_node_text(tig_node_get(change, "add"))) != 0) {
    agent_check(builder, tig_node_set(items, id, set != 0 ? tig_node_copy(set) : tig_node_map()));
  } else if ((id = tig_node_text(tig_node_get(change, "remove"))) != 0) {
    tig_node_unset(items, id);
  } else if ((id = tig_node_text(tig_node_get(change, "update"))) != 0) {
    tig_node *item = tig_node_get(items, id);

    for (i = 0; item != 0 && i < tig_node_count(set); ++i) {
      agent_check(builder, tig_node_set(item, tig_node_key_at(set, i), tig_node_copy(tig_node_at(set, i))));
    }
    for (i = 0; item != 0 && i < tig_node_count(unset); ++i) {
      tig_node_unset(item, tig_node_text(tig_node_at(unset, i)));
    }
  }
}

/* adds the change to the action and applies it */
static void agent_record_change(agent_builder *builder, tig_node *items, tig_node *action, tig_node *change) {
  agent_apply(builder, items, change);
  agent_add_change(builder, action, change);
}

static tig_node *agent_add(agent_builder *builder, const char *id, const tig_node *attributes, const char *since) {
  tig_node *change = agent_change(builder, "add", id);

  if (since != 0) {
    agent_check(builder, tig_node_set(change, "since", tig_node_string(since)));
  }
  if (tig_node_count(attributes) > 0) {
    agent_check(builder, tig_node_set(change, "set", tig_node_copy(attributes)));
  }
  return change;
}

/* --- comparing the device with its state --- */

/* the action type for a change to one attribute, from the table in DEVICE.md */
static const char *agent_update_type(const char *id, const char *attribute, const tig_node *before, const tig_node *now) {
  if (agent_is_kind(id, "firmware") ||
      (agent_is_kind(id, "os") && (strcmp(attribute, "id") == 0 || strcmp(attribute, "version") == 0 ||
                                   strcmp(attribute, "name") == 0))) {
    return "upgraded";
  }
  if ((agent_is_kind(id, "memory") || agent_is_kind(id, "disk")) && strcmp(attribute, "capacity") == 0) {
    return "resized";
  }
  if (agent_is_kind(id, "filesystem") && strcmp(attribute, "size") == 0) {
    return "resized";
  }
  /* a container recreated from a new image is one upgrade, which sets its new `created` too, so state stays in step */
  if (agent_is_kind(id, "container") &&
      (strcmp(attribute, "image") == 0 || strcmp(attribute, "digest") == 0 ||
       (strcmp(attribute, "version") == 0 && (!tig_node_equal(tig_node_get(before, "image"), tig_node_get(now, "image")) ||
                                              !tig_node_equal(tig_node_get(before, "digest"), tig_node_get(now, "digest")))) ||
       (strcmp(attribute, "created") == 0 && (!tig_node_equal(tig_node_get(before, "image"), tig_node_get(now, "image")) ||
                                              !tig_node_equal(tig_node_get(before, "digest"), tig_node_get(now, "digest")))))) {
    return "upgraded";
  }
  return "reconfigured";
}

/* the kernel changes only at a boot, which the boot event records */
static int agent_is_compared(const char *id, const char *attribute) {
  return !(agent_is_kind(id, "os") && strcmp(attribute, "kernel") == 0);
}

/* a component known by its role whose identity changed is a different one in the same place */
static int agent_identity_changed(const char *id, const tig_node *before, const tig_node *now) {
  const char *attribute = agent_is_kind(id, "filesystem") ? "uuid" : agent_is_kind(id, "cpu") ? "model" : 0;
  const tig_node *old_value = attribute != 0 ? tig_node_get(before, attribute) : 0;
  const tig_node *new_value = attribute != 0 ? tig_node_get(now, attribute) : 0;

  return old_value != 0 && new_value != 0 && !tig_node_equal(old_value, new_value);
}

/* filesystems that list the old disk are updated to the new one in the same action */
static void agent_update_references(agent_builder *builder, tig_node *items, const tig_node *observed, tig_node *action,
                                    const char *old_id, const char *new_id) {
  size_t i;
  size_t j;

  for (i = 0; i < tig_node_count(items); ++i) {
    const char *id = tig_node_key_at(items, i);
    const tig_node *disks = tig_node_get(tig_node_at(items, i), "disks");
    const tig_node *seen = tig_node_get(tig_node_get(observed, id), "disks");
    tig_node *updated;
    tig_node *change;
    int refers = 0;

    for (j = 0; j < tig_node_count(disks); ++j) {
      const char *disk = tig_node_text(tig_node_at(disks, j));
      refers |= disk != 0 && strcmp(disk, old_id) == 0;
    }
    if (!agent_is_kind(id, "filesystem") || !refers) {
      continue;
    }

    if (seen != 0) {
      updated = tig_node_copy(seen);
    } else {
      updated = tig_node_list();
      for (j = 0; j < tig_node_count(disks); ++j) {
        const char *disk = tig_node_text(tig_node_at(disks, j));
        agent_check(builder, tig_node_append(updated, tig_node_string(strcmp(disk, old_id) == 0 ? new_id : disk)));
      }
    }
    change = agent_change(builder, "update", id);
    agent_check(builder, tig_node_set(change, "set", tig_node_map()));
    agent_check(builder, tig_node_set(tig_node_get(change, "set"), "disks", updated));
    agent_record_change(builder, items, action, change);
  }
}

typedef struct agent_ids {
  const char *items[256];
  size_t count;
} agent_ids;

static void agent_ids_add(agent_ids *ids, const char *id) {
  if (ids->count < sizeof(ids->items) / sizeof(ids->items[0])) {
    ids->items[ids->count++] = id;
  }
}

/* components added, removed and replaced, one action each */
static void agent_diff_components(agent_builder *builder, tig_node *items, const tig_node *observed, tig_node *actions,
                                  const char *kind) {
  agent_ids removed = {{0}, 0};
  agent_ids added = {{0}, 0};
  char *old_copies[256];
  size_t i;

  for (i = 0; i < tig_node_count(items); ++i) {
    const char *id = tig_node_key_at(items, i);
    if (agent_is_kind(id, kind) && tig_node_get(observed, id) == 0) {
      agent_ids_add(&removed, id);
    }
  }
  for (i = 0; i < tig_node_count(observed); ++i) {
    const char *id = tig_node_key_at(observed, i);
    const tig_node *before = tig_node_get(items, id);

    if (!agent_is_kind(id, kind)) {
      continue;
    }
    if (before == 0) {
      agent_ids_add(&added, id);
    } else if (agent_identity_changed(id, before, tig_node_at(observed, i))) {
      tig_node *action = agent_action(builder, actions, "replaced");
      agent_record_change(builder, items, action, agent_change(builder, "remove", id));
      agent_record_change(builder, items, action, agent_add(builder, id, tig_node_at(observed, i), 0));
    }
  }

  /* removing items frees their IDs, which point into items, so keep copies */
  for (i = 0; i < removed.count; ++i) {
    size_t length = strlen(removed.items[i]);
    old_copies[i] = (char *)malloc(length + 1);
    if (old_copies[i] == 0) {
      builder->failed = 1;
      removed.count = i;
      break;
    }
    memcpy(old_copies[i], removed.items[i], length + 1);
    removed.items[i] = old_copies[i];
  }

  for (i = 0; i < removed.count || i < added.count; ++i) {
    tig_node *action;

    if (!agent_is_docker(kind) && i < removed.count && i < added.count) {
      action = agent_action(builder, actions, "replaced");
      agent_record_change(builder, items, action, agent_change(builder, "remove", removed.items[i]));
      agent_record_change(builder, items, action, agent_add(builder, added.items[i], tig_node_get(observed, added.items[i]), 0));
      agent_update_references(builder, items, observed, action, removed.items[i], added.items[i]);
    } else {
      if (i < removed.count) {
        action = agent_action(builder, actions, "removed");
        agent_record_change(builder, items, action, agent_change(builder, "remove", removed.items[i]));
      }
      if (i < added.count) {
        action = agent_action(builder, actions, "installed");
        agent_record_change(builder, items, action, agent_add(builder, added.items[i], tig_node_get(observed, added.items[i]), 0));
      }
    }
  }

  for (i = 0; i < removed.count; ++i) {
    free(old_copies[i]);
  }
}

/* attributes that changed on an item that is still there, one action per type of change */
static void agent_diff_attributes(agent_builder *builder, tig_node *items, const char *id, const tig_node *now,
                                  tig_node *actions) {
  static const char *const types[] = {"upgraded", "resized", "reconfigured"};
  const tig_node *before = tig_node_get(items, id);
  tig_node *changes[3] = {0, 0, 0};
  size_t i;
  size_t t;

  for (i = 0; i < tig_node_count(now) + tig_node_count(before); ++i) {
    int is_set = i < tig_node_count(now);
    const char *attribute = is_set ? tig_node_key_at(now, i) : tig_node_key_at(before, i - tig_node_count(now));
    const char *type = agent_update_type(id, attribute, before, now);

    /* a version is only looked up now and then, so one that isn't there now, for an image that hasn't changed, is not
       one that has gone */
    if (!is_set && agent_is_kind(id, "container") && strcmp(attribute, "version") == 0 &&
        tig_node_equal(tig_node_get(before, "image"), tig_node_get(now, "image")) &&
        tig_node_equal(tig_node_get(before, "digest"), tig_node_get(now, "digest"))) {
      continue;
    }
    if (!agent_is_compared(id, attribute) ||
        (is_set ? tig_node_equal(tig_node_get(before, attribute), tig_node_at(now, i)) : tig_node_get(now, attribute) != 0)) {
      continue;
    }
    for (t = 0; strcmp(types[t], type) != 0; ++t) {
    }
    if (changes[t] == 0) {
      changes[t] = agent_change(builder, "update", id);
    }
    if (tig_node_get(changes[t], is_set ? "set" : "unset") == 0) {
      agent_check(builder, tig_node_set(changes[t], is_set ? "set" : "unset", is_set ? tig_node_map() : tig_node_list()));
    }
    if (is_set) {
      agent_check(builder, tig_node_set(tig_node_get(changes[t], "set"), attribute, tig_node_copy(tig_node_at(now, i))));
    } else {
      agent_check(builder, tig_node_append(tig_node_get(changes[t], "unset"), tig_node_string(attribute)));
    }
  }

  for (t = 0; t < 3; ++t) {
    if (changes[t] != 0) {
      agent_record_change(builder, items, agent_action(builder, actions, types[t]), changes[t]);
    }
  }
}

tig_node *agent_diff_with(const tig_node *state_items, const tig_node *observed, int include_docker) {
  agent_builder builder = {0};
  tig_node *items = tig_node_copy(state_items);
  tig_node *actions = tig_node_list();
  size_t i;

  if (items == 0 || actions == 0) {
    tig_node_free(items);
    tig_node_free(actions);
    return 0;
  }
  for (i = 0; i < AGENT_KIND_COUNT; ++i) {
    if (include_docker || !agent_is_docker(agent_kinds[i])) {
      agent_diff_components(&builder, items, observed, actions, agent_kinds[i]);
    }
  }
  for (i = 0; i < tig_node_count(observed); ++i) {
    const char *id = tig_node_key_at(observed, i);
    if (agent_is_recorded(id) && tig_node_get(items, id) != 0 && (include_docker || !agent_is_docker(id))) {
      agent_diff_attributes(&builder, items, id, tig_node_at(observed, i), actions);
    }
  }

  tig_node_free(items);
  if (builder.failed) {
    tig_node_free(actions);
    return 0;
  }
  return actions;
}

tig_node *agent_diff(const tig_node *state_items, const tig_node *observed) {
  return agent_diff_with(state_items, observed, 1);
}

/* --- what is already recorded --- */

/* an event's time, or 0 if it isn't one this can read */
static int64_t agent_parse_time(const char *text) {
  struct tm civil;
  int fields;

  if (text == 0) {
    return 0;
  }
  memset(&civil, 0, sizeof(civil));
  fields = sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d", &civil.tm_year, &civil.tm_mon, &civil.tm_mday, &civil.tm_hour,
                  &civil.tm_min, &civil.tm_sec);
  if (fields != 6 && fields != 3) {
    return 0;
  }
  civil.tm_year -= 1900;
  civil.tm_mon -= 1;
  return (int64_t)timegm(&civil) * 1000;
}

typedef struct agent_history {
  const char *boot_id;
  int has_discovery;
  int docker_recorded; /* some event has already added, changed or removed a stack or container */
  int boot_recorded;
  int64_t latest_ms;
} agent_history;

static tig_status agent_visit(void *context, const char *id, const tig_node *event) {
  agent_history *history = (agent_history *)context;
  const char *type = tig_node_text(tig_node_get(event, "type"));
  const tig_node *actions = tig_node_get(event, "actions");
  int64_t time_ms = agent_parse_time(tig_node_text(tig_node_get(event, "time")));
  size_t i;

  (void)id;
  if (type != 0 && strcmp(type, "discovery") == 0) {
    history->has_discovery = 1;
  }
  for (i = 0; i < tig_node_count(actions); ++i) {
    const tig_node *action = tig_node_at(actions, i);
    const char *action_type = tig_node_text(tig_node_get(action, "type"));
    const char *boot = tig_node_text(tig_node_get(action, "boot"));
    const tig_node *changes = tig_node_get(action, "changes");
    size_t c;

    for (c = 0; c < tig_node_count(changes); ++c) {
      const tig_node *change = tig_node_at(changes, c);
      const char *item = tig_node_text(tig_node_get(change, "add"));

      if (item == 0) {
        item = tig_node_text(tig_node_get(change, "update"));
      }
      if (item == 0) {
        item = tig_node_text(tig_node_get(change, "remove"));
      }
      if (item != 0 && agent_is_docker(item)) {
        history->docker_recorded = 1;
      }
    }

    if (action_type != 0 && strcmp(action_type, "booted") == 0 && boot != 0 && history->boot_id != 0 &&
        strcmp(boot, history->boot_id) == 0) {
      history->boot_recorded = 1;
    }
  }
  if (time_ms > history->latest_ms) {
    history->latest_ms = time_ms;
  }
  return TIG_OK;
}

/* --- recording --- */

static int agent_add_event(tig_repository *repository, tig_node *event, const char *what) {
  char id[TIG_EVENT_ID_MAX + 1];
  tig_status added = tig_event_add(repository, event, id, sizeof(id));

  tig_node_free(event);
  if (added != TIG_OK) {
    agent_log_error("cannot record the %s: %s", what, tig_get_last_error());
    return -1;
  }
  agent_log_info("recorded the %s as event %s", what, id);
  return 0;
}

/* initial state holds only the entity; components go in the discovery, since they may have changed before now */
static int agent_record_discovery(tig_repository *repository, const agent_inventory *inventory, int64_t now_ms) {
  agent_builder builder = {0};
  tig_node *initial = tig_node_map();
  tig_node *items = tig_node_map();
  tig_node *event;
  tig_node *actions = tig_node_list();
  tig_node *action;
  size_t i;

  agent_check(&builder, tig_node_set(initial, "kind", tig_node_string("device")));
  agent_check(&builder, tig_node_set(initial, "vocabulary", tig_node_string("tig-device/1")));
  agent_check(&builder, tig_node_set(items, "entity", tig_node_copy(inventory->entity)));
  agent_check(&builder, tig_node_set(initial, "items", items));
  if (builder.failed || tig_state_write_initial(repository, initial) != TIG_OK) {
    agent_log_error("cannot record the initial state: %s", builder.failed ? "out of memory" : tig_get_last_error());
    tig_node_free(initial);
    tig_node_free(actions);
    return -1;
  }
  tig_node_free(initial);

  event = agent_new_event(&builder, now_ms, 0, "discovery");
  action = agent_action(&builder, actions, "discovered");
  for (i = 0; i < tig_node_count(inventory->items); ++i) {
    const char *id = tig_node_key_at(inventory->items, i);
    const char *since = tig_node_text(tig_node_get(inventory->since, id));

    agent_add_change(&builder, action, agent_add(&builder, id, tig_node_at(inventory->items, i), since != 0 ? since : "unknown"));
  }
  agent_check(&builder, tig_node_set(event, "actions", actions));
  if (builder.failed) {
    tig_node_free(event);
    agent_log_error("cannot record the discovery: out of memory");
    return -1;
  }
  return agent_add_event(repository, event, "discovery");
}

static int agent_record_boot(tig_repository *repository, const agent_boot *boot, const agent_inventory *inventory) {
  agent_builder builder = {0};
  tig_node *state = 0;
  tig_node *event;
  tig_node *actions = tig_node_list();
  tig_node *action;
  const tig_node *kernel = tig_node_get(tig_node_get(inventory->items, "os"), "kernel");
  const tig_node *recorded;

  /* compared with state as the boot found it; a boot before the discovery has no changes */
  if (tig_state_replay(repository, boot->time_ms, &state) != TIG_OK) {
    agent_log_error("cannot replay state: %s", tig_get_last_error());
    tig_node_free(actions);
    return -1;
  }
  recorded = tig_node_get(tig_node_get(state, "items"), "os");

  event = agent_new_event(&builder, boot->time_ms, 0, "boot");
  action = agent_action(&builder, actions, "booted");
  agent_check(&builder, tig_node_set(action, "boot", tig_node_string(boot->id)));
  if (recorded != 0 && kernel != 0 && !tig_node_equal(tig_node_get(recorded, "kernel"), kernel)) {
    tig_node *change = agent_change(&builder, "update", "os");

    agent_check(&builder, tig_node_set(change, "set", tig_node_map()));
    agent_check(&builder, tig_node_set(tig_node_get(change, "set"), "kernel", tig_node_copy(kernel)));
    agent_add_change(&builder, action, change);
  }
  agent_check(&builder, tig_node_set(event, "actions", actions));
  tig_node_free(state);
  if (builder.failed) {
    tig_node_free(event);
    agent_log_error("cannot record the boot: out of memory");
    return -1;
  }
  return agent_add_event(repository, event, "boot");
}

/* Compose stacks, when the owner first turns them on: a discovery of just those kinds (DEVICE.md, "Discovery"), so they
   are found as already present rather than installed */
static int agent_record_docker_discovery(tig_repository *repository, const agent_inventory *inventory, int64_t now_ms) {
  agent_builder builder = {0};
  tig_node *event = agent_new_event(&builder, now_ms, 0, "discovery");
  tig_node *actions = tig_node_list();
  tig_node *action = agent_action(&builder, actions, "discovered");
  size_t i;

  for (i = 0; i < tig_node_count(inventory->items); ++i) {
    const char *id = tig_node_key_at(inventory->items, i);
    const char *since = tig_node_text(tig_node_get(inventory->since, id));

    if (agent_is_docker(id)) {
      agent_add_change(&builder, action, agent_add(&builder, id, tig_node_at(inventory->items, i), since != 0 ? since : "unknown"));
    }
  }
  agent_check(&builder, tig_node_set(event, "actions", actions));
  if (builder.failed) {
    tig_node_free(event);
    agent_log_error("cannot record the discovery of Compose stacks: out of memory");
    return -1;
  }
  return agent_add_event(repository, event, "discovery of Compose stacks");
}

static int agent_has_docker_items(const agent_inventory *inventory) {
  size_t i;

  for (i = 0; i < tig_node_count(inventory->items); ++i) {
    if (agent_is_docker(tig_node_key_at(inventory->items, i))) {
      return 1;
    }
  }
  return 0;
}

static int agent_record_changes(tig_repository *repository, const agent_inventory *inventory, int include_docker,
                                int64_t time_ms, int64_t not_before_ms) {
  agent_builder builder = {0};
  tig_node *state;
  tig_node *actions;
  tig_node *event;

  if (tig_state_replay(repository, TIG_TIME_LATEST, &state) != TIG_OK) {
    agent_log_error("cannot replay state: %s", tig_get_last_error());
    return -1;
  }
  actions = agent_diff_with(tig_node_get(state, "items"), inventory->items, include_docker);
  tig_node_free(state);
  if (actions == 0) {
    agent_log_error("cannot compare the device with its state: out of memory");
    return -1;
  }
  if (tig_node_count(actions) == 0) {
    tig_node_free(actions);
    return 0;
  }

  event = agent_new_event(&builder, time_ms, not_before_ms > 0 && not_before_ms <= time_ms ? not_before_ms : 0, 0);
  agent_check(&builder, tig_node_set(event, "actions", actions));
  if (builder.failed) {
    tig_node_free(event);
    agent_log_error("cannot record changes: out of memory");
    return -1;
  }
  return agent_add_event(repository, event, "changes to the device");
}

int agent_record(tig_repository *repository, const agent_observation *observation) {
  agent_history history;
  int result = 0;

  memset(&history, 0, sizeof(history));
  history.boot_id = observation->boot.id[0] != '\0' ? observation->boot.id : 0;
  if (tig_events_each(repository, agent_visit, &history) != TIG_OK) {
    agent_log_error("cannot read events: %s", tig_get_last_error());
    return -1;
  }

  if (!history.has_discovery) {
    result = agent_record_discovery(repository, &observation->inventory, observation->now_ms);
  } else {
    /* a change found at the first comparison after a boot happened while the device was off, so it is dated at
       the boot, and after the previous comparison; one found later happened since the previous comparison */
    int at_boot = observation->first_after_boot && observation->boot.time_ms > 0;
    int64_t time_ms = at_boot ? observation->boot.time_ms : observation->now_ms;
    int64_t not_before_ms = observation->previous_ms;

    if (at_boot && not_before_ms == 0) {
      not_before_ms = observation->latest_reading_ms;
    }
    /* never before what is already recorded, which the change would then contradict */
    if (time_ms < history.latest_ms) {
      time_ms = observation->now_ms;
    }
    if (observation->docker_observed && !observation->docker_baseline && !history.docker_recorded &&
        agent_has_docker_items(&observation->inventory)) {
      result = agent_record_docker_discovery(repository, &observation->inventory, observation->now_ms);
    }
    if (result == 0) {
      result = agent_record_changes(repository, &observation->inventory, observation->docker_observed, time_ms, not_before_ms);
    }
  }

  if (result == 0 && history.boot_id != 0 && !history.boot_recorded && observation->boot.time_ms > 0) {
    result = agent_record_boot(repository, &observation->boot, &observation->inventory);
  }
  return result;
}
