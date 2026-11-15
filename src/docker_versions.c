/* Versions for Compose containers' images (docker_versions = hub).

   A tag such as `latest` says nothing about the release. Docker Hub lists every tag of a repository with its digest, so
   the tags that share the digest of the image a container runs include its release ("19.5.5-ce.0"). Labels in the image
   can't be used instead: they are inherited from its base image, so GitLab would be recorded as Ubuntu's 24.04.

   This is the only thing the agent fetches from the Internet, and it tells Docker Hub which public images the machine
   runs, so it is off unless the owner turns it on. Only images from Docker Hub are looked up, and only by name. */
#include "agent.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AGENT_HUB_PAGES 5            /* of 100 tags, the most recently pushed first */
#define AGENT_HUB_PAGE_BYTES (4 << 20)
#define AGENT_HUB_LOOKUPS_PER_PASS 3 /* an inventory is read every commit, so the rest wait for the next */
#define AGENT_HUB_RETRY_SECONDS 3600 /* before asking again about an image with no version */
#define AGENT_HUB_BACKOFF_SECONDS 600 /* before trying again after Hub couldn't be reached */
#define AGENT_HUB_CACHE 256
#define AGENT_HUB_DIGESTS 8

/* --- the image reference --- */

static int agent_hub_name_ok(const char *text) {
  size_t i;

  for (i = 0; text[i] != '\0'; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (!(islower(c) || isdigit(c) || c == '.' || c == '_' || c == '-' || c == '/')) {
      return 0;
    }
  }
  return i > 0;
}

int agent_hub_reference(const char *image, char *repo, size_t repo_size, char *tag, size_t tag_size) {
  char name[256];
  char *at;
  char *colon;
  char *slash;
  char *first;

  if (strlen(image) >= sizeof(name) || strncmp(image, "sha256:", 7) == 0) {
    return -1;
  }
  strcpy(name, image);

  /* an image pinned by digest has no tag to find a release for */
  at = strchr(name, '@');
  if (at != 0) {
    return -1;
  }
  slash = strrchr(name, '/');
  colon = strrchr(name, ':');
  if (colon != 0 && (slash == 0 || colon > slash)) {
    *colon = '\0';
    snprintf(tag, tag_size, "%s", colon + 1);
  } else {
    snprintf(tag, tag_size, "%s", "latest");
  }

  /* a registry is named first if it looks like a host; Docker Hub's own names are the same as none */
  first = name;
  slash = strchr(name, '/');
  if (slash != 0) {
    *slash = '\0';
    if (strcmp(first, "docker.io") == 0 || strcmp(first, "index.docker.io") == 0 ||
        strcmp(first, "registry-1.docker.io") == 0) {
      memmove(name, slash + 1, strlen(slash + 1) + 1);
    } else if (strchr(first, '.') != 0 || strchr(first, ':') != 0 || strcmp(first, "localhost") == 0) {
      return -1;
    } else {
      *slash = '/';
    }
  }
  if (!agent_hub_name_ok(name) || tag[0] == '\0' || !agent_hub_name_ok(tag) || strchr(tag, '/') != 0) {
    return -1;
  }
  slash = strchr(name, '/');
  if (slash == 0) {
    snprintf(repo, repo_size, "library/%s", name);
  } else if (strchr(slash + 1, '/') != 0) {
    return -1;
  } else {
    snprintf(repo, repo_size, "%s", name);
  }
  return 0;
}

/* --- choosing a version --- */

static int agent_hub_prerelease(const char *tag) {
  static const char *const words[] = {"rc", "alpha", "beta", "nightly", "dev", "snapshot", "preview", "canary", "edge", "next"};
  char lower[96];
  char *token;
  char *cursor;
  size_t i;
  size_t w;

  if (strlen(tag) >= sizeof(lower)) {
    return 1;
  }
  for (i = 0; tag[i] != '\0'; ++i) {
    lower[i] = (char)tolower((unsigned char)tag[i]);
  }
  lower[i] = '\0';
  cursor = lower;
  while ((token = strsep(&cursor, "-_+")) != 0) {
    for (w = 0; w < sizeof(words) / sizeof(words[0]); ++w) {
      if (strncmp(token, words[w], strlen(words[w])) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

int agent_hub_version_score(const char *tag) {
  const char *start = tag[0] == 'v' ? tag + 1 : tag;
  size_t length = strlen(tag);
  int dots = 0;
  size_t i;

  if (!isdigit((unsigned char)start[0]) || length > 63) {
    return 0;
  }
  for (i = 0; tag[i] != '\0'; ++i) {
    unsigned char c = (unsigned char)tag[i];
    if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || c == '+')) {
      return 0;
    }
    dots += c == '.';
  }
  if (dots == 0 || agent_hub_prerelease(tag)) {
    return 0;
  }
  /* more parts is more specific ("8.2.12" over "8.2"); of equal ones, the plainer ("8.2.12" over "8.2.12-noble") */
  return dots * 100 + (100 - (int)length);
}

void agent_hub_scan(const tig_node *reply, const char *const *digests, size_t digest_count, const char *own_tag,
                    agent_hub_match *match) {
  const tig_node *results = tig_node_get(reply, "results");
  size_t i;
  size_t j;
  size_t d;

  for (i = 0; i < tig_node_count(results); ++i) {
    const tig_node *entry = tig_node_at(results, i);
    const char *name = tig_node_text(tig_node_get(entry, "name"));
    const tig_node *platforms = tig_node_get(entry, "images");
    int same = 0;
    int score;

    if (name == 0) {
      continue;
    }
    /* the tag's own digest, and those of its per-platform images, which is what some Docker stores report */
    for (d = 0; d < digest_count && !same; ++d) {
      const char *tag_digest = tig_node_text(tig_node_get(entry, "digest"));

      same = tag_digest != 0 && strcmp(tag_digest, digests[d]) == 0;
      for (j = 0; j < tig_node_count(platforms) && !same; ++j) {
        const char *image_digest = tig_node_text(tig_node_get(tig_node_at(platforms, j), "digest"));
        same = image_digest != 0 && strcmp(image_digest, digests[d]) == 0;
      }
    }
    if (!same) {
      continue;
    }
    match->matched = 1;
    score = strcmp(name, own_tag) == 0 ? 0 : agent_hub_version_score(name);
    if (score > match->score) {
      match->score = score;
      snprintf(match->version, sizeof(match->version), "%s", name);
    }
  }
}

/* --- asking --- */

static int64_t agent_seconds(void) {
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec;
}

typedef struct agent_hub_known {
  char key[352];     /* image@digest */
  char version[64];  /* empty if none was found */
  int64_t asked_at;  /* seconds, monotonic */
  int permanent;     /* nothing to look up, e.g. the image isn't from Docker Hub */
} agent_hub_known;

static agent_hub_known agent_hub_cache[AGENT_HUB_CACHE];
static size_t agent_hub_cache_count;
static int64_t agent_hub_resume_at; /* when Hub may be tried again after it couldn't be reached */

static agent_hub_known *agent_hub_find(const char *key) {
  size_t i;

  for (i = 0; i < agent_hub_cache_count; ++i) {
    if (strcmp(agent_hub_cache[i].key, key) == 0) {
      return &agent_hub_cache[i];
    }
  }
  return 0;
}

static agent_hub_known *agent_hub_remember(const char *key, const char *version, int permanent) {
  agent_hub_known *entry = agent_hub_find(key);

  if (entry == 0) {
    if (agent_hub_cache_count == AGENT_HUB_CACHE) {
      return 0;
    }
    entry = &agent_hub_cache[agent_hub_cache_count++];
    snprintf(entry->key, sizeof(entry->key), "%s", key);
  }
  snprintf(entry->version, sizeof(entry->version), "%s", version);
  entry->asked_at = agent_seconds();
  entry->permanent = permanent;
  return entry;
}

static int agent_is_sha256(const char *text) {
  size_t i;

  if (strncmp(text, "sha256:", 7) != 0 || strlen(text) != 71) {
    return 0;
  }
  for (i = 7; text[i] != '\0'; ++i) {
    if (!isxdigit((unsigned char)text[i])) {
      return 0;
    }
  }
  return 1;
}

/* 1 if found, 0 if not, -1 if Docker Hub couldn't be reached */
static int agent_hub_lookup(const agent_config *config, const char *image, const char *digest, char *version,
                            size_t version_size) {
  char repo[200];
  char tag[128];
  char digest_text[AGENT_HUB_DIGESTS][80];
  const char *digests[AGENT_HUB_DIGESTS];
  size_t digest_count = 0;
  agent_hub_match match;
  char *output;
  int page;
  tig_node *inspect;
  const tig_node *repo_digests;
  size_t i;

  memset(&match, 0, sizeof(match));
  if (agent_hub_reference(image, repo, sizeof(repo), tag, sizeof(tag)) != 0 || !agent_is_sha256(digest)) {
    return 0;
  }

  /* what Docker reports as the image's ID is its manifest's digest in some Docker stores, its configuration's in
     others, which Hub doesn't know; the registry digests it was pulled by are what Hub lists then */
  snprintf(digest_text[digest_count], sizeof(digest_text[0]), "%s", digest);
  digests[digest_count] = digest_text[digest_count];
  ++digest_count;
  {
    char path[128];

    snprintf(path, sizeof(path), "/images/%s/json", digest);
    inspect = agent_docker_get(config, path);
  }
  repo_digests = tig_node_get(inspect, "RepoDigests");
  for (i = 0; i < tig_node_count(repo_digests) && digest_count < AGENT_HUB_DIGESTS; ++i) {
    const char *text = tig_node_text(tig_node_at(repo_digests, i));
    const char *at = text != 0 ? strchr(text, '@') : 0;

    if (at != 0 && agent_is_sha256(at + 1)) {
      snprintf(digest_text[digest_count], sizeof(digest_text[0]), "%s", at + 1);
      digests[digest_count] = digest_text[digest_count];
      ++digest_count;
    }
  }
  tig_node_free(inspect);

  output = (char *)malloc(AGENT_HUB_PAGE_BYTES);
  if (output == 0) {
    return -1;
  }
  for (page = 1; page <= AGENT_HUB_PAGES && !match.matched; ++page) {
    char url[400];
    char number[16];
    const char *argv[] = {"curl", "-s", "--fail", "--max-time", "20", url, 0};
    tig_node *reply = 0;
    int status;

    snprintf(number, sizeof(number), "%d", page);
    snprintf(url, sizeof(url), "https://hub.docker.com/v2/repositories/%s/tags?page_size=100&ordering=last_updated&page=%s", repo,
             number);
    status = agent_run(argv, 0, output, AGENT_HUB_PAGE_BYTES, 30);
    if (status == 22) {
      break; /* an HTTP error, such as no such repository or too many requests: no answer, but Hub is there */
    }
    if (status != 0) {
      free(output);
      return -1;
    }
    if (tig_node_parse(output, strlen(output), &reply) != TIG_OK) {
      break;
    }
    agent_hub_scan(reply, digests, digest_count, tag, &match);
    if (tig_node_count(tig_node_get(reply, "results")) < 100) {
      tig_node_free(reply);
      break; /* the last page */
    }
    tig_node_free(reply);
  }
  free(output);

  if (match.version[0] == '\0') {
    return 0;
  }
  snprintf(version, version_size, "%s", match.version);
  return 1;
}

void agent_docker_versions(const agent_config *config, agent_inventory *inventory) {
  size_t i;
  int lookups = 0;

  if (!config->docker_versions) {
    return;
  }
  for (i = 0; i < tig_node_count(inventory->items); ++i) {
    const char *id = tig_node_key_at(inventory->items, i);
    tig_node *attributes = tig_node_at(inventory->items, i);
    const char *image = tig_node_text(tig_node_get(attributes, "image"));
    const char *digest = tig_node_text(tig_node_get(attributes, "digest"));
    char key[352];
    char version[64];
    agent_hub_known *known;
    int64_t now = agent_seconds();
    int found;

    if (strncmp(id, "container/", 10) != 0 || image == 0 || digest == 0 ||
        snprintf(key, sizeof(key), "%s@%s", image, digest) >= (int)sizeof(key)) {
      continue;
    }
    known = agent_hub_find(key);
    if (known == 0 || (known->version[0] == '\0' && !known->permanent && now - known->asked_at >= AGENT_HUB_RETRY_SECONDS)) {
      if (lookups >= AGENT_HUB_LOOKUPS_PER_PASS || now < agent_hub_resume_at) {
        continue;
      }
      ++lookups;
      found = agent_hub_lookup(config, image, digest, version, sizeof(version));
      if (found < 0) {
        agent_log_warning("cannot reach Docker Hub to find image versions; trying again in %d minutes",
                          AGENT_HUB_BACKOFF_SECONDS / 60);
        agent_hub_resume_at = now + AGENT_HUB_BACKOFF_SECONDS;
        continue;
      }
      {
        char repo[200];
        char tag[128];
        int on_hub = agent_hub_reference(image, repo, sizeof(repo), tag, sizeof(tag)) == 0;

        known = agent_hub_remember(key, found == 1 ? version : "", !on_hub);
      }
    }
    if (known != 0 && known->version[0] != '\0') {
      tig_node_set(attributes, "version", tig_node_string(known->version));
    }
  }
}
