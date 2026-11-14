/* Docker Compose stacks (tig-docs/vocabularies/DEVICE.md, "Compose stacks"): the stacks and containers as items, and a
   telemetry stream per container.

   The agent talks to the Docker Engine API through curl, with GET requests only, so that it can be pointed at a
   read-only socket proxy and never needs the rights that the Docker socket itself gives. The API's JSON is parsed as
   YAML, which it is a subset of. */
#include "agent.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AGENT_DOCKER_MAX 256 /* containers; any more are left out */
#define AGENT_DOCKER_TIMEOUT 30
#define AGENT_DOCKER_BASE_OUTPUT (INT64_C(1) << 20)
#define AGENT_DOCKER_EACH_OUTPUT (64 * 1024)
#define AGENT_DOCKER_MINUTE_MS INT64_C(60000)

/* only containers Compose made, which it labels */
#define AGENT_DOCKER_LIST \
  "/containers/json?all=1&filters=%7B%22label%22%3A%5B%22com.docker.compose.project%22%5D%7D"

#define AGENT_LABEL_PROJECT "com.docker.compose.project"
#define AGENT_LABEL_SERVICE "com.docker.compose.service"
#define AGENT_LABEL_NUMBER "com.docker.compose.container-number"
#define AGENT_LABEL_ONEOFF "com.docker.compose.oneoff"

/* printed by curl after each response, to tell the responses apart */
#define AGENT_DOCKER_WRITE_OUT "\\n@@TIG@@\\n"
static const char agent_docker_marker[] = "\n@@TIG@@\n";

int agent_docker_enabled(const agent_config *config) {
  return config->docker_host[0] != '\0';
}

/* --- identity --- */

static void agent_docker_part(const char *text, char *out, size_t out_size) {
  size_t i;

  for (i = 0; text[i] != '\0' && i + 1 < out_size; ++i) {
    unsigned char c = (unsigned char)text[i];
    out[i] = isalnum(c) ? (char)tolower(c) : '-';
  }
  out[i] = '\0';
  if (i == 0) {
    strcpy(out, "0");
  }
}

typedef struct agent_container {
  char id[80];     /* Docker's, for requests */
  char item[256];  /* container/<project>/<service>/<n> */
  char stack[96];  /* stack/<project> */
  const tig_node *node;
} agent_container;

static const char *agent_label(const tig_node *container, const char *name) {
  return tig_node_text(tig_node_get(tig_node_get(container, "Labels"), name));
}

/* 0 if this is a container Compose runs for a service, 1 if it isn't one to record */
static int agent_container_identify(const tig_node *node, agent_container *container) {
  const char *id = tig_node_text(tig_node_get(node, "Id"));
  const char *project = agent_label(node, AGENT_LABEL_PROJECT);
  const char *service = agent_label(node, AGENT_LABEL_SERVICE);
  const char *number = agent_label(node, AGENT_LABEL_NUMBER);
  const char *oneoff = agent_label(node, AGENT_LABEL_ONEOFF);
  char project_part[64];
  char service_part[64];
  char number_part[16];
  size_t i;

  if (id == 0 || project == 0 || service == 0 || strlen(id) >= sizeof(container->id)) {
    return 1;
  }
  /* `docker compose run` containers are one-off tasks, not part of the stack */
  if (oneoff != 0 && strcmp(oneoff, "True") == 0) {
    return 1;
  }
  for (i = 0; id[i] != '\0'; ++i) {
    if (!isxdigit((unsigned char)id[i])) {
      return 1;
    }
  }

  agent_docker_part(project, project_part, sizeof(project_part));
  agent_docker_part(service, service_part, sizeof(service_part));
  agent_docker_part(number != 0 ? number : "1", number_part, sizeof(number_part));
  strcpy(container->id, id);
  snprintf(container->item, sizeof(container->item), "container/%s/%s/%s", project_part, service_part, number_part);
  snprintf(container->stack, sizeof(container->stack), "stack/%s", project_part);
  container->node = node;
  return 0;
}

void agent_docker_stream_name(const char *item_id, char *out, size_t out_size) {
  size_t i;

  for (i = 0; item_id[i] != '\0' && i + 1 < out_size; ++i) {
    out[i] = item_id[i] == '/' ? '.' : item_id[i];
  }
  out[i] = '\0';
}

/* --- state --- */

static void agent_since(tig_node *since, const char *id, const char *created) {
  const char *known = tig_node_text(tig_node_get(since, id));

  /* RFC 3339 times in UTC sort as text */
  if (known == 0 || strcmp(created, known) < 0) {
    tig_node_set(since, id, tig_node_string(created));
  }
}

int agent_docker_items(const tig_node *containers, agent_inventory *inventory) {
  size_t i;
  size_t added = 0;

  for (i = 0; i < tig_node_count(containers); ++i) {
    const tig_node *node = tig_node_at(containers, i);
    agent_container container;
    const char *image = tig_node_text(tig_node_get(node, "Image"));
    const char *digest = tig_node_text(tig_node_get(node, "ImageID"));
    const char *created_text = tig_node_text(tig_node_get(node, "Created"));
    tig_node *attributes;
    tig_node *stack;
    char created[32];

    if (agent_container_identify(node, &container) != 0) {
      continue;
    }
    /* an inventory that stops short would take the rest for removed, so none is better than part of one */
    if (added >= AGENT_DOCKER_MAX) {
      agent_log_error("more than %d Compose containers; Compose stacks are not recorded", AGENT_DOCKER_MAX);
      return -1;
    }

    attributes = tig_node_map();
    if (attributes == 0) {
      return -1;
    }
    if (image != 0) {
      tig_node_set(attributes, "image", tig_node_string(image));
    }
    if (digest != 0) {
      tig_node_set(attributes, "digest", tig_node_string(digest));
    }
    created[0] = '\0';
    if (created_text != 0 && atoll(created_text) > 0) {
      agent_format_time((int64_t)atoll(created_text) * 1000, created, sizeof(created));
      tig_node_set(attributes, "created", tig_node_string(created));
    }
    if (tig_node_set(inventory->items, container.item, attributes) != TIG_OK) {
      return -1;
    }

    /* one item for the stack, however many containers it has; `project` is the name as Compose reports it */
    if (tig_node_get(inventory->items, container.stack) == 0) {
      stack = tig_node_map();
      if (stack == 0 || tig_node_set(stack, "project", tig_node_string(agent_label(node, AGENT_LABEL_PROJECT))) != TIG_OK ||
          tig_node_set(inventory->items, container.stack, stack) != TIG_OK) {
        return -1;
      }
    }
    if (created[0] != '\0') {
      agent_since(inventory->since, container.item, created);
      agent_since(inventory->since, container.stack, created);
    }
    ++added;
  }
  return 0;
}

/* --- the API --- */

typedef struct agent_responses {
  char *buffer;
  char *bodies[2 * AGENT_DOCKER_MAX + 1];
  size_t count;
} agent_responses;

/* GETs every path in one curl run and splits the output into one body per path; a body is empty if its request
   failed. Returns -1 if curl couldn't run, or, with a single path, if the request failed. */
static int agent_docker_fetch(const agent_config *config, const char *const *paths, size_t count,
                              agent_responses *responses) {
  const char *host = config->docker_host;
  char base[sizeof(config->docker_host)];
  char socket_path[sizeof(config->docker_host)];
  const char **argv;
  char **urls;
  size_t argc = 0;
  size_t buffer_size = (size_t)AGENT_DOCKER_BASE_OUTPUT + count * AGENT_DOCKER_EACH_OUTPUT;
  size_t i;
  char *cursor;
  int status;
  int result = -1;

  memset(responses, 0, sizeof(*responses));
  socket_path[0] = '\0';
  if (strncmp(host, "unix://", 7) == 0) {
    snprintf(socket_path, sizeof(socket_path), "%s", host + 7);
    strcpy(base, "http://docker");
  } else {
    snprintf(base, sizeof(base), "%s", host);
    while (strlen(base) > 0 && base[strlen(base) - 1] == '/') {
      base[strlen(base) - 1] = '\0';
    }
  }

  argv = (const char **)calloc(count + 12, sizeof(*argv));
  urls = (char **)calloc(count, sizeof(*urls));
  responses->buffer = (char *)malloc(buffer_size);
  if (argv == 0 || urls == 0 || responses->buffer == 0) {
    agent_log_error("docker: out of memory");
    goto done;
  }

  argv[argc++] = "curl";
  argv[argc++] = "-s";
  argv[argc++] = "--fail";
  argv[argc++] = "--max-time";
  argv[argc++] = "20";
  argv[argc++] = "-w";
  argv[argc++] = AGENT_DOCKER_WRITE_OUT;
  if (socket_path[0] != '\0') {
    argv[argc++] = "--unix-socket";
    argv[argc++] = socket_path;
  }
  for (i = 0; i < count; ++i) {
    size_t length = strlen(base) + strlen(paths[i]) + 1;

    urls[i] = (char *)malloc(length);
    if (urls[i] == 0) {
      agent_log_error("docker: out of memory");
      goto done;
    }
    snprintf(urls[i], length, "%s%s", base, paths[i]);
    argv[argc++] = urls[i];
  }
  argv = (const char **)realloc((void *)argv, (argc + 1) * sizeof(*argv));
  argv[argc] = 0;

  status = agent_run(argv, 0, responses->buffer, buffer_size, AGENT_DOCKER_TIMEOUT);
  if (status < 0 || (count == 1 && status != 0)) {
    goto done;
  }

  cursor = responses->buffer;
  for (i = 0; i < count; ++i) {
    char *end = strstr(cursor, agent_docker_marker);

    if (end == 0) {
      goto done;
    }
    *end = '\0';
    responses->bodies[i] = cursor;
    cursor = end + strlen(agent_docker_marker);
  }
  responses->count = count;
  result = 0;

done:
  if (urls != 0) {
    for (i = 0; i < count; ++i) {
      free(urls[i]);
    }
  }
  free(urls);
  free((void *)argv);
  if (result != 0) {
    free(responses->buffer);
    responses->buffer = 0;
  }
  return result;
}

static void agent_docker_release(agent_responses *responses) {
  free(responses->buffer);
  responses->buffer = 0;
}

static tig_node *agent_docker_json(const char *text) {
  tig_node *node = 0;

  if (text == 0 || text[0] == '\0' || tig_node_parse(text, strlen(text), &node) != TIG_OK) {
    return 0;
  }
  return node;
}

/* reported once per outage, not every minute */
static int agent_docker_unreachable;

static int agent_docker_list(const agent_config *config, tig_node **containers) {
  const char *paths[1] = {AGENT_DOCKER_LIST};
  agent_responses responses;

  if (agent_docker_fetch(config, paths, 1, &responses) != 0) {
    if (!agent_docker_unreachable) {
      agent_log_warning("cannot read containers from %s; Compose stacks are not recorded until it can be", config->docker_host);
    }
    agent_docker_unreachable = 1;
    return -1;
  }
  *containers = agent_docker_json(responses.bodies[0]);
  agent_docker_release(&responses);
  if (*containers == 0 || tig_node_type_of(*containers) != TIG_NODE_LIST) {
    tig_node_free(*containers);
    *containers = 0;
    if (!agent_docker_unreachable) {
      agent_log_warning("unexpected reply from %s; Compose stacks are not recorded until it is understood", config->docker_host);
    }
    agent_docker_unreachable = 1;
    return -1;
  }
  if (agent_docker_unreachable) {
    agent_log_info("reading containers from %s again", config->docker_host);
  }
  agent_docker_unreachable = 0;
  return 0;
}

int agent_docker_inventory(const agent_config *config, agent_inventory *inventory) {
  tig_node *containers;
  int result;

  if (agent_docker_list(config, &containers) != 0) {
    return -1;
  }
  result = agent_docker_items(containers, inventory);
  tig_node_free(containers);
  return result;
}

/* --- telemetry --- */

int64_t agent_docker_memory(const tig_node *stats) {
  const tig_node *memory = tig_node_get(stats, "memory_stats");
  const char *usage = tig_node_text(tig_node_get(memory, "usage"));
  const tig_node *detail = tig_node_get(memory, "stats");
  /* cgroup v2 names it inactive_file, v1 total_inactive_file */
  const char *cache = tig_node_text(tig_node_get(detail, "inactive_file"));
  long long used;
  long long inactive = 0;

  if (usage == 0) {
    return -1;
  }
  if (cache == 0) {
    cache = tig_node_text(tig_node_get(detail, "total_inactive_file"));
  }
  used = atoll(usage);
  if (cache != 0) {
    inactive = atoll(cache);
  }
  return used > inactive ? used - inactive : 0;
}

/* the CPU time each container had used at the previous reading, to give cores in use between two */
typedef struct agent_cpu_sample {
  char id[80];
  long long usage_ns;
  int64_t at_ns;
} agent_cpu_sample;

static agent_cpu_sample agent_cpu_previous[AGENT_DOCKER_MAX];
static size_t agent_cpu_previous_count;

static int64_t agent_monotonic_ns(void) {
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

/* cores in use since the previous reading, or -1 if there isn't one */
static double agent_cpu_cores(const char *id, long long usage_ns, int64_t at_ns, agent_cpu_sample *next, size_t *next_count) {
  size_t i;
  double cores = -1;

  for (i = 0; i < agent_cpu_previous_count; ++i) {
    const agent_cpu_sample *before = &agent_cpu_previous[i];

    if (strcmp(before->id, id) == 0 && at_ns > before->at_ns && usage_ns >= before->usage_ns) {
      cores = (double)(usage_ns - before->usage_ns) / (double)(at_ns - before->at_ns);
    }
  }
  if (*next_count < AGENT_DOCKER_MAX) {
    snprintf(next[*next_count].id, sizeof(next[*next_count].id), "%s", id);
    next[*next_count].usage_ns = usage_ns;
    next[*next_count].at_ns = at_ns;
    ++*next_count;
  }
  return cores;
}

static const tig_column agent_container_columns[] = {{"state", TIG_VALUE_TEXT},
                                                      {"health", TIG_VALUE_TEXT},
                                                      {"restarts", TIG_VALUE_INTEGER},
                                                      {"cpu", TIG_VALUE_DECIMAL},
                                                      {"memory", TIG_VALUE_INTEGER}};

static const tig_stream_definition agent_container_stream = {
    "A Compose container's state and use of resources", 0, agent_container_columns, 5, TIG_CHUNK_DAY,
    5 * AGENT_DOCKER_MINUTE_MS};

int agent_docker_sample(const agent_config *config, tig_repository *repository, int64_t now_ms) {
  tig_node *containers;
  agent_container found[AGENT_DOCKER_MAX];
  size_t found_count = 0;
  char (*paths)[128];
  const char **path_pointers;
  size_t inspect_at[AGENT_DOCKER_MAX];
  size_t stats_at[AGENT_DOCKER_MAX];
  size_t path_count = 0;
  agent_responses responses;
  agent_cpu_sample next[AGENT_DOCKER_MAX];
  size_t next_count = 0;
  int64_t at_ns;
  size_t i;
  int failures = 0;

  if (agent_docker_list(config, &containers) != 0) {
    /* unreachable is reported once, and is not a failure of the sample */
    return 0;
  }

  for (i = 0; i < tig_node_count(containers) && found_count < AGENT_DOCKER_MAX; ++i) {
    if (agent_container_identify(tig_node_at(containers, i), &found[found_count]) == 0) {
      ++found_count;
    }
  }

  paths = (char (*)[128])malloc(sizeof(*paths) * 2 * AGENT_DOCKER_MAX);
  path_pointers = (const char **)malloc(sizeof(*path_pointers) * 2 * AGENT_DOCKER_MAX);
  if (paths == 0 || path_pointers == 0) {
    agent_log_error("docker: out of memory");
    free(paths);
    free(path_pointers);
    tig_node_free(containers);
    return -1;
  }
  for (i = 0; i < found_count; ++i) {
    const char *state = tig_node_text(tig_node_get(found[i].node, "State"));

    inspect_at[i] = path_count;
    snprintf(paths[path_count], sizeof(paths[0]), "/containers/%s/json", found[i].id);
    path_pointers[path_count] = paths[path_count];
    ++path_count;

    stats_at[i] = (size_t)-1;
    if (state != 0 && strcmp(state, "running") == 0) {
      stats_at[i] = path_count;
      snprintf(paths[path_count], sizeof(paths[0]), "/containers/%s/stats?stream=false&one-shot=true", found[i].id);
      path_pointers[path_count] = paths[path_count];
      ++path_count;
    }
  }

  if (path_count == 0 || agent_docker_fetch(config, path_pointers, path_count, &responses) != 0) {
    if (path_count != 0) {
      agent_log_warning("cannot read container details from %s", config->docker_host);
    }
    free(paths);
    free(path_pointers);
    tig_node_free(containers);
    agent_cpu_previous_count = 0;
    return 0;
  }
  at_ns = agent_monotonic_ns();

  for (i = 0; i < found_count; ++i) {
    tig_node *inspect = agent_docker_json(responses.bodies[inspect_at[i]]);
    tig_node *stats = stats_at[i] != (size_t)-1 ? agent_docker_json(responses.bodies[stats_at[i]]) : 0;
    const char *state = tig_node_text(tig_node_get(found[i].node, "State"));
    const char *health = tig_node_text(tig_node_get(tig_node_get(tig_node_get(inspect, "State"), "Health"), "Status"));
    const char *restarts = tig_node_text(tig_node_get(inspect, "RestartCount"));
    const char *usage = tig_node_text(tig_node_get(tig_node_get(tig_node_get(stats, "cpu_stats"), "cpu_usage"), "total_usage"));
    char stream[300];
    char cpu_text[32] = "";
    char memory_text[32] = "";
    const char *values[5];
    int64_t memory;

    if (usage != 0) {
      double cores = agent_cpu_cores(found[i].id, atoll(usage), at_ns, next, &next_count);

      if (cores >= 0) {
        snprintf(cpu_text, sizeof(cpu_text), "%.3f", cores);
      }
    }
    memory = stats != 0 ? agent_docker_memory(stats) : -1;
    if (memory >= 0) {
      snprintf(memory_text, sizeof(memory_text), "%lld", (long long)memory);
    }

    values[0] = state != 0 ? state : "";
    values[1] = health != 0 ? health : "";
    values[2] = restarts != 0 ? restarts : "";
    values[3] = cpu_text;
    values[4] = memory_text;

    agent_docker_stream_name(found[i].item, stream, sizeof(stream));
    /* an unreadable container is skipped rather than recorded with nothing in it */
    if (inspect != 0 && tig_telemetry_stream_define(repository, stream, &agent_container_stream) == TIG_OK) {
      if (tig_telemetry_append(repository, stream, now_ms, values, 5) != TIG_OK) {
        agent_log_error("cannot record %s: %s", stream, tig_get_last_error());
        ++failures;
      }
    } else if (inspect != 0) {
      agent_log_error("cannot define %s: %s", stream, tig_get_last_error());
      ++failures;
    }
    tig_node_free(inspect);
    tig_node_free(stats);
  }

  memcpy(agent_cpu_previous, next, sizeof(next[0]) * next_count);
  agent_cpu_previous_count = next_count;

  agent_docker_release(&responses);
  free(paths);
  free(path_pointers);
  tig_node_free(containers);
  return failures == 0 ? 0 : -1;
}
