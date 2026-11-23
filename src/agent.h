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
  char docker_host[256]; /* unix:///path or http://host:port of a Docker Engine API; empty: not recorded */
  int docker_versions;   /* look container images' versions up on Docker Hub (docker_versions = hub) */
  char notary[256];      /* TiG Notary's URL, e.g. https://notary.tigdb.org; empty: never ask for signatures */
  char issuer[256];      /* the hostname that publishes the notary's keys (COMMITS.md, "Public keys") */
  char issuer_keys[512]; /* where it publishes them, if not https://<issuer>/.well-known/tig-public-key */
  int64_t sample_interval_ms;
  int64_t commit_interval_ms;
  int64_t push_interval_ms;
} agent_config;

/* the device's serial number, which only root can read from DMI, copied there by the package's install script */
#define AGENT_PRODUCT_SERIAL "/etc/tig-agent/product_serial"

#define AGENT_BOOT_ID_MAX 64

/* all times are UNIX seconds, 0 if never */
typedef struct agent_status {
  int64_t last_sample;
  int64_t last_commit;
  int64_t last_push_attempt;
  int64_t last_push;
  char last_push_result[16]; /* ok, failed, rejected or skipped */
  int64_t last_sign;         /* when TiG Notary last signed a commit */
  char last_sign_result[16]; /* ok, current, failed or skipped */
  int64_t last_compare;      /* when the device was last compared with its state */
  char last_compare_boot[AGENT_BOOT_ID_MAX]; /* the boot that comparison was in */
  int docker_baseline;       /* Docker was read at the last comparison, even if it had no stacks */
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
  char public_key[1024];         /* ssh-ed25519 AAAA..., with no comment */
  char allowed_signer[1024];     /* the key's line in .tig/allowed_signers */
  tig_device device;             /* signs through ssh-keygen; points into this struct */
} agent_device;

/* device.c */
int agent_create_device_key(const char *path);
int agent_device_load(const agent_config *config, agent_device *device);
/* reads up to size - 1 bytes of a file into buffer and ends it with a NUL; returns the length, or -1 */
long agent_read_file(const char *path, char *buffer, size_t size);

/* repository.c: with a device, commits are signed, and unsigned history gains the key first */
int  agent_setup(const agent_config *config);
int  agent_open(const agent_config *config, agent_device *device, tig_repository **out);
int  agent_lock(const agent_config *config);
void agent_unlock(int lock);

/* telemetry.c: samples every stream this machine supports */
int agent_sample(const agent_config *config, tig_repository *repository);

/* inventory.c: the device as it is now, as tig-device/1 items (tig-docs/vocabularies/DEVICE.md) */
typedef struct agent_inventory {
  tig_node *entity; /* the entity's attributes */
  tig_node *items;  /* item ID to attributes, without the entity */
  tig_node *since;  /* item ID to when it is known to have been present from, for the items where that's known */
} agent_inventory;

typedef struct agent_boot {
  char id[AGENT_BOOT_ID_MAX]; /* empty if unknown */
  int64_t time_ms;            /* UNIX ms, 0 if unknown */
} agent_boot;

int  agent_inventory_read(agent_inventory *inventory);
void agent_inventory_free(agent_inventory *inventory);
int  agent_boot_read(agent_boot *boot);
/* UNIX ms as RFC 3339 UTC, to the second */
void agent_format_time(int64_t unix_ms, char *out, size_t out_size);

/* sha256.c: "<kind>/" and the first 8 hex digits of the identity's SHA-256 */
void agent_sha256(const void *data, size_t length, unsigned char digest[32]);
void agent_hashed_id(const char *kind, const char *identity, char *out, size_t out_size);

/* docker.c: Docker Compose stacks (DEVICE.md, "Compose stacks"), read through a read-only Docker Engine API */
int agent_docker_enabled(const agent_config *config);
/* adds stack/<project> and container/<project>/<service>/<n> items for the containers in a /containers/json
   list, with when each was created in inventory->since; containers not made by Compose are left out */
int agent_docker_items(const tig_node *containers, agent_inventory *inventory);
/* reads the containers now and adds their items; -1 if Docker can't be reached, which leaves the inventory alone */
int agent_docker_inventory(const agent_config *config, agent_inventory *inventory);
/* appends a reading to each container's stream */
int agent_docker_sample(const agent_config *config, tig_repository *repository, int64_t now_ms);
/* a container's ID with "/" and any character a stream name doesn't allow replaced, e.g. container.shop.web.1 */
void agent_docker_stream_name(const char *item_id, char *out, size_t out_size);
/* bytes in use as `docker stats` shows them: usage without the page cache; -1 if stats has no memory figures */
int64_t agent_docker_memory(const tig_node *stats);

/* GETs one Engine API path and parses the JSON reply; NULL if it can't be read or the request failed */
tig_node *agent_docker_get(const agent_config *config, const char *path);

/* docker_versions.c: names the release a mutable tag such as `latest` is, from Docker Hub. It finds the tags there
   that have the same digest as the image the container runs, and takes the most specific version among them. */
/* the Docker Hub repository ("library/mongo") and tag of an image reference; -1 if the image isn't from Docker Hub */
int agent_hub_reference(const char *image, char *repo, size_t repo_size, char *tag, size_t tag_size);
/* 0 if the tag doesn't look like a release ("latest", "rc", "8"), else higher for a more specific one ("8.2.12") */
int agent_hub_version_score(const char *tag);
typedef struct agent_hub_match {
  char version[64]; /* the best version-like tag so far, empty if none */
  int score;
  int matched;      /* some tag has one of the digests, so the tags that share it have been reached */
} agent_hub_match;
/* adds the tags in one page of Hub's /tags reply that share one of the digests, other than the image's own tag */
void agent_hub_scan(const tig_node *reply, const char *const *digests, size_t digest_count, const char *own_tag,
                    agent_hub_match *match);
/* adds a `version` to the container items whose release can be found; items it can't are left as they are, and it
   never fails the inventory */
void agent_docker_versions(const agent_config *config, agent_inventory *inventory);

/* record.c */
typedef struct agent_observation {
  agent_inventory inventory;
  int docker_observed;       /* the inventory has the Compose stacks; without it they are left as recorded */
  int docker_baseline;       /* Docker was read at an earlier comparison, so what is new now was installed since */
  agent_boot boot;
  int64_t now_ms;
  int64_t previous_ms;       /* the previous comparison, 0 if unknown */
  int64_t latest_reading_ms; /* the latest telemetry reading, 0 if unknown */
  int first_after_boot;      /* no comparison has been made in this boot */
} agent_observation;

/* writes the initial state and discovery for a new repository, a boot event for a boot that has none, and an
   event for any difference between the device and its state */
int       agent_record(tig_repository *repository, const agent_observation *observation);
/* actions that turn state's items into the observed ones */
tig_node *agent_diff(const tig_node *state_items, const tig_node *observed);
/* as agent_diff; without include_docker, stacks and containers are not compared */
tig_node *agent_diff_with(const tig_node *state_items, const tig_node *observed, int include_docker);

/* notary.c: signature tags from TiG Notary (COMMITS.md, "Signing"). Signing is best effort: a commit that couldn't be
   signed is signed at the next sync, since a signature covers all earlier history. */
typedef enum agent_sign_result {
  AGENT_SIGN_OK,      /* the latest commit now has a new signature tag */
  AGENT_SIGN_CURRENT, /* the latest commit was already signed */
  AGENT_SIGN_SKIPPED, /* no notary is configured */
  AGENT_SIGN_FAILED
} agent_sign_result;

agent_sign_result agent_notary_sign(const agent_config *config);
/* checks a tag the notary returned, as COMMITS.md, "Response", asks the client to before storing it: that it is a tag
   of the requested commit, named for its own time, from the issuer and for this device. On success *name is the
   tag's name, e.g. tig/sig/20261020T140000Z. The signature is checked separately, by Git. */
int agent_notary_check_tag(const char *tag, const char *commit, const char *issuer, const char *device, char *name,
                           size_t name_size);
/* UNIX seconds from an HTTP Date header's value, e.g. "Tue, 20 Oct 2026 14:00:00 GMT"; -1 if it isn't one */
int64_t agent_http_date(const char *text);
/* whether a notary or keys URL is acceptable: https, or http to this machine, with nothing in it a shell or curl
   would treat specially, and no credentials */
int agent_notary_url_ok(const char *url);
/* whether text is an Internet hostname, such as an issuer */
int agent_hostname_ok(const char *text);

/* sync.c */
int               agent_configure_remote(const agent_config *config);
agent_push_result agent_push(const agent_config *config);

/* status.c */
int agent_status_load(const agent_config *config, agent_status *status);
int agent_status_save(const agent_config *config, const agent_status *status);
int agent_status_print(const agent_config *config);

/* actions.c: each takes the agent lock and records its outcome in the status file. Committing first compares the
   device with its state and records what changed. */
int agent_collect(const agent_config *config);
int agent_commit(const agent_config *config);
int agent_sync(const agent_config *config, int retry_rejected);

/* daemon.c */
int agent_daemon(const agent_config *config);

#endif
