/* Signatures from TiG Notary (COMMITS.md, "Signing"). After a commit, the agent asks the notary to sign the commit's
   hash, checks the tag it returns, stores it as refs/tags/tig/sig/<time> and lets the next push send it. Like device.c
   and sync.c it runs ssh-keygen, curl and git, so libtig needs none of it. */
#include "agent.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef TIG_AGENT_VERSION
#define TIG_AGENT_VERSION "0.0.0"
#endif

#define AGENT_NOTARY_TIMEOUT 30
#define AGENT_KEYS_TIMEOUT 20
#define AGENT_GIT_TIMEOUT 30
#define AGENT_REPLY_MAX (16 * 1024)
#define AGENT_KEYS_MAX (64 * 1024)
#define AGENT_SIGNATURE_BEGIN "-----BEGIN SSH SIGNATURE-----"
#define AGENT_SIGNATURE_END "-----END SSH SIGNATURE-----\n"
#define AGENT_TAG_PREFIX "tig/sig/"
#define AGENT_TAG_TIME_LENGTH 16 /* 20261020T140000Z */
#define AGENT_USER_AGENT "tig-agent/" TIG_AGENT_VERSION
#define AGENT_PUBLISHED_KEYS "/.well-known/tig-public-key"

/* a URL that goes to curl as an argument, never through a shell, with no credentials in it */
int agent_notary_url_ok(const char *url) {
  const char *host;
  int plain;
  size_t i;

  if (strncmp(url, "https://", 8) == 0) {
    host = url + 8;
    plain = 0;
  } else if (strncmp(url, "http://", 7) == 0) {
    host = url + 7;
    plain = 1;
  } else {
    return 0;
  }
  for (i = 0; url[i] != '\0'; ++i) {
    if (!isgraph((unsigned char)url[i]) || url[i] == '@' || url[i] == '\'' || url[i] == '"' || url[i] == '\\') {
      return 0;
    }
  }
  if (host[0] == '\0' || host[0] == '/' || host[0] == ':') {
    return 0;
  }
  if (plain) {
    /* a notary's answer is signed, but plain HTTP would still show who is asking; keys must never come this way */
    static const char *const local[] = {"localhost", "127.0.0.1", "[::1]"};
    size_t k;

    for (k = 0; k < sizeof(local) / sizeof(local[0]); ++k) {
      size_t length = strlen(local[k]);

      if (strncmp(host, local[k], length) == 0 && strchr(":/", host[length]) != 0) {
        return 1;
      }
    }
    return 0;
  }
  return 1;
}

int agent_hostname_ok(const char *text) {
  size_t length = strlen(text);
  size_t i;

  if (length == 0 || length > 253 || !isalnum((unsigned char)text[0]) || !isalnum((unsigned char)text[length - 1])) {
    return 0;
  }
  for (i = 0; i < length; ++i) {
    if (!isalnum((unsigned char)text[i]) && text[i] != '-' && text[i] != '.') {
      return 0;
    }
    if (text[i] == '.' && text[i + 1] == '.') {
      return 0;
    }
  }
  return 1;
}

int64_t agent_http_date(const char *text) {
  static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char month_name[4];
  int day;
  int year;
  int hour;
  int minute;
  int second;
  struct tm parts;
  time_t seconds;
  int month;
  int consumed = 0;

  /* sscanf can't say that a literal at the end didn't match, so the zone is checked by hand */
  if (sscanf(text, " %*3s, %d %3s %d %d:%d:%d %n", &day, month_name, &year, &hour, &minute, &second, &consumed) != 6 ||
      strcmp(text + consumed, "GMT") != 0) {
    return -1;
  }
  for (month = 0; month < 12 && strcmp(month_name, months[month]) != 0; ++month) {
  }
  if (month == 12 || day < 1 || day > 31 || year < 1970 || year > 9999 || hour > 23 || hour < 0 || minute < 0 ||
      minute > 59 || second < 0 || second > 60) {
    return -1;
  }

  memset(&parts, 0, sizeof(parts));
  parts.tm_mday = day;
  parts.tm_mon = month;
  parts.tm_year = year - 1900;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_sec = second;
  seconds = timegm(&parts);
  return seconds == (time_t)-1 ? -1 : (int64_t)seconds;
}

/* 20261020T140000Z, the form of a tag's name after tig/sig/ */
static int agent_tag_time(int64_t seconds, char *out, size_t out_size) {
  time_t time_value = (time_t)seconds;
  struct tm utc;

  return gmtime_r(&time_value, &utc) != 0 && strftime(out, out_size, "%Y%m%dT%H%M%SZ", &utc) == AGENT_TAG_TIME_LENGTH ? 0
                                                                                                                    : -1;
}

/* the next line, without its newline; a last line with no newline is not one */
static int agent_next_line(const char **cursor, const char **line, size_t *length) {
  const char *end = strchr(*cursor, '\n');

  if (end == 0) {
    return 0;
  }
  *line = *cursor;
  *length = (size_t)(end - *cursor);
  *cursor = end + 1;
  return 1;
}

/* whether the line is exactly text, or starts with it, copying the rest into value */
static int agent_line_is(const char *line, size_t length, const char *text) {
  return strlen(text) == length && memcmp(line, text, length) == 0;
}

static int agent_line_value(const char *line, size_t length, const char *prefix, char *value, size_t value_size) {
  size_t prefix_length = strlen(prefix);

  if (length < prefix_length || memcmp(line, prefix, prefix_length) != 0 || length - prefix_length >= value_size) {
    return 0;
  }
  memcpy(value, line + prefix_length, length - prefix_length);
  value[length - prefix_length] = '\0';
  return 1;
}

int agent_notary_check_tag(const char *tag, const char *commit, const char *issuer, const char *device, char *name,
                           size_t name_size) {
  const char *cursor = tag;
  const char *line;
  size_t length;
  char expected[256];
  char value[512];
  char tagged[AGENT_TAG_TIME_LENGTH + 1];
  char *epoch;
  char *end;
  long long seconds;
  int have_version = 0;
  int have_issuer = 0;
  int have_device = 0;
  int signed_text = 0;
  size_t tag_length = strlen(tag);
  size_t end_length = strlen(AGENT_SIGNATURE_END);
  size_t i;

  if (strchr(tag, '\r') != 0) {
    return -1;
  }

  snprintf(expected, sizeof(expected), "object %s", commit);
  if (!agent_next_line(&cursor, &line, &length) || !agent_line_is(line, length, expected)) {
    return -1;
  }
  if (!agent_next_line(&cursor, &line, &length) || !agent_line_is(line, length, "type commit")) {
    return -1;
  }

  if (!agent_next_line(&cursor, &line, &length) ||
      !agent_line_value(line, length, "tag " AGENT_TAG_PREFIX, value, sizeof(value)) ||
      strlen(value) != AGENT_TAG_TIME_LENGTH) {
    return -1;
  }
  for (i = 0; i < AGENT_TAG_TIME_LENGTH; ++i) {
    if (i == 8 ? value[i] != 'T' : i == AGENT_TAG_TIME_LENGTH - 1 ? value[i] != 'Z' : !isdigit((unsigned char)value[i])) {
      return -1;
    }
  }

  /* "tagger TiG Notary <notary@example.com> 1792504800 +0000": the name is for the tagger's time, which is the notary's */
  if (!agent_next_line(&cursor, &line, &length) || !agent_line_value(line, length, "tagger ", expected, sizeof(expected)) ||
      length < 7 + 6 || strcmp(expected + strlen(expected) - 6, " +0000") != 0) {
    return -1;
  }
  expected[strlen(expected) - 6] = '\0';
  epoch = strrchr(expected, ' ');
  if (epoch == 0 || !isdigit((unsigned char)epoch[1])) {
    return -1;
  }
  seconds = strtoll(epoch + 1, &end, 10);
  if (*end != '\0' || agent_tag_time(seconds, tagged, sizeof(tagged)) != 0 || strcmp(tagged, value) != 0) {
    return -1;
  }

  if (!agent_next_line(&cursor, &line, &length) || length != 0) {
    return -1;
  }

  /* the message is signed, so only its first Issuer and Device count, as for a verifier; other lines are ignored */
  for (;;) {
    char field[256];

    if (!agent_next_line(&cursor, &line, &length)) {
      return -1;
    }
    if (agent_line_is(line, length, AGENT_SIGNATURE_BEGIN)) {
      signed_text = 1;
      break;
    }
    if (!have_version && agent_line_value(line, length, "TiG-Signature: ", field, sizeof(field))) {
      if (strcmp(field, "1") != 0) {
        return -1;
      }
      have_version = 1;
    } else if (!have_issuer && agent_line_value(line, length, "Issuer: ", field, sizeof(field))) {
      if (strcmp(field, issuer) != 0) {
        return -1;
      }
      have_issuer = 1;
    } else if (!have_device && agent_line_value(line, length, "Device: ", field, sizeof(field))) {
      if (strcmp(field, device) != 0) {
        return -1;
      }
      have_device = 1;
    }
  }
  if (!signed_text || !have_version || !have_issuer || !have_device || tag_length < end_length ||
      strcmp(tag + tag_length - end_length, AGENT_SIGNATURE_END) != 0) {
    return -1;
  }

  if (snprintf(name, name_size, AGENT_TAG_PREFIX "%s", value) >= (int)name_size) {
    return -1;
  }
  return 0;
}

/* Everything one request leaves on disk, in the agent's notary directory, which only the agent can read */
typedef struct agent_notary_files {
  char base[AGENT_PATH_MAX]; /* the request, which ssh-keygen signs */
  char signature[AGENT_PATH_MAX + 16];
  char body[AGENT_PATH_MAX + 16];
  char headers[AGENT_PATH_MAX + 16];
  char reply[AGENT_PATH_MAX + 16];
  char tag[AGENT_PATH_MAX + 16];
  char download[AGENT_PATH_MAX + 16];
} agent_notary_files;

static int agent_files_open(agent_notary_files *files, const char *directory) {
  int descriptor;

  if (snprintf(files->base, sizeof(files->base), "%s/sign-XXXXXX", directory) >= (int)sizeof(files->base)) {
    return -1;
  }
  descriptor = mkstemp(files->base);
  if (descriptor < 0) {
    agent_log_error("%s: %s", files->base, strerror(errno));
    return -1;
  }
  close(descriptor);
  snprintf(files->signature, sizeof(files->signature), "%s.sig", files->base);
  snprintf(files->body, sizeof(files->body), "%s.body", files->base);
  snprintf(files->headers, sizeof(files->headers), "%s.headers", files->base);
  snprintf(files->reply, sizeof(files->reply), "%s.reply", files->base);
  snprintf(files->tag, sizeof(files->tag), "%s.tag", files->base);
  snprintf(files->download, sizeof(files->download), "%s.download", files->base);
  return 0;
}

static void agent_files_remove(const agent_notary_files *files) {
  unlink(files->base);
  unlink(files->signature);
  unlink(files->body);
  unlink(files->headers);
  unlink(files->reply);
  unlink(files->tag);
  unlink(files->download);
}

static int agent_write_file(const char *path, const char *first, const char *second) {
  FILE *file = fopen(path, "wb");
  size_t first_length = strlen(first);
  size_t second_length = second != 0 ? strlen(second) : 0;
  int failed;

  if (file == 0) {
    agent_log_error("%s: %s", path, strerror(errno));
    return -1;
  }
  failed = fwrite(first, 1, first_length, file) != first_length ||
           (second_length > 0 && fwrite(second, 1, second_length, file) != second_length);
  if (fclose(file) != 0 || failed) {
    agent_log_error("%s: %s", path, strerror(errno));
    return -1;
  }
  return 0;
}

static void agent_trim(char *text) {
  size_t length = strlen(text);

  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
}

/* the value of the last header with this name (which includes its colon), e.g. "date:"; empty if there is none */
static void agent_header(const char *path, const char *name, char *value, size_t value_size) {
  char line[512];
  FILE *file = fopen(path, "r");
  size_t length = strlen(name);

  value[0] = '\0';
  if (file == 0) {
    return;
  }
  while (fgets(line, sizeof(line), file) != 0) {
    if (strncasecmp(line, name, length) == 0) {
      char *text = line + length;

      while (*text == ' ' || *text == '\t') {
        ++text;
      }
      agent_trim(text);
      snprintf(value, value_size, "%s", text);
    }
  }
  fclose(file);
}

/* POSTs one signed request. The HTTP status goes in *status; the body of the reply is in files->reply */
static int agent_notary_post(const agent_config *config, const agent_device *device, const char *commit,
                             int64_t seconds, const agent_notary_files *files, long *status) {
  char time_text[32];
  char request[1024];
  char signature[2048];
  char output[1024];
  char url[600];
  char body_argument[AGENT_PATH_MAX + 16];
  size_t url_length;
  const char *sign_argv[] = {"ssh-keygen", "-q", "-Y", "sign", "-n", "tig-notary", "-f", device->key_path, files->base, 0};
  const char *post_argv[] = {"curl",
                             "-sS",
                             "--proto",
                             "=http,https",
                             "--max-time",
                             "30",
                             "-A",
                             AGENT_USER_AGENT,
                             "-H",
                             "Content-Type: text/plain",
                             "-D",
                             files->headers,
                             "-o",
                             files->reply,
                             "-w",
                             "%{http_code}",
                             "--data-binary",
                             body_argument,
                             url,
                             0};
  char *end;

  agent_format_time(seconds * 1000, time_text, sizeof(time_text));
  if (snprintf(request, sizeof(request), "TiG-Sign-Request: 1\nObject: %s\nTime: %s\nKey: %s\n", commit, time_text,
               device->public_key) >= (int)sizeof(request) ||
      agent_write_file(files->base, request, 0) != 0) {
    return -1;
  }

  /* the signature covers every byte of the request, in the notary's namespace */
  unlink(files->signature);
  if (agent_run(sign_argv, 0, output, sizeof(output), 30) != 0) {
    agent_log_error("ssh-keygen -Y sign failed: %s", output);
    return -1;
  }
  if (agent_read_file(files->signature, signature, sizeof(signature)) <= 0 ||
      agent_write_file(files->body, request, signature) != 0) {
    agent_log_error("%s: no signature written", files->signature);
    return -1;
  }

  snprintf(body_argument, sizeof(body_argument), "@%s", files->body);
  url_length = strlen(config->notary);
  while (url_length > 0 && config->notary[url_length - 1] == '/') {
    --url_length;
  }
  if (snprintf(url, sizeof(url), "%.*s/v1/sign", (int)url_length, config->notary) >= (int)sizeof(url)) {
    return -1;
  }

  unlink(files->headers);
  unlink(files->reply);
  if (agent_run(post_argv, 0, output, sizeof(output), AGENT_NOTARY_TIMEOUT + 10) != 0) {
    agent_trim(output);
    agent_log_warning("cannot reach the notary at %s: %s", config->notary, output);
    return -1;
  }
  *status = strtol(output, &end, 10);
  if (end == output || *status < 100 || *status > 599) {
    agent_log_warning("the notary at %s gave no HTTP status: %s", config->notary, output);
    return -1;
  }
  return 0;
}

/* asks for a signature of the commit; the tag is left in tag. A 422 means the device's clock is out, so the request is made
   again once, with the time from the notary's Date header (COMMITS.md, "Errors"). */
static int agent_notary_request_tag(const agent_config *config, const agent_device *device, const char *commit,
                                    const agent_notary_files *files, char *tag, size_t tag_size) {
  int64_t seconds = (int64_t)time(0);
  int attempt;

  for (attempt = 0; attempt < 2; ++attempt) {
    char message[256];
    long status = 0;
    long length;

    if (agent_notary_post(config, device, commit, seconds, files, &status) != 0) {
      return -1;
    }
    length = agent_read_file(files->reply, tag, tag_size);
    if (length < 0) {
      /* curl writes no file for an empty reply */
      tag[0] = '\0';
      length = 0;
    }
    if (status == 200) {
      return length > 0 ? 0 : -1;
    }

    snprintf(message, sizeof(message), "%.*s", (int)strcspn(tag, "\r\n"), tag);
    if (status == 422 && attempt == 0) {
      char date[64];
      int64_t notary_time;

      agent_header(files->headers, "date:", date, sizeof(date));
      notary_time = agent_http_date(date);
      if (notary_time < 0) {
        agent_log_warning("the notary says the clock is out, but sent no usable Date: %s", message);
        return -1;
      }
      agent_log_warning("the clock is %lld seconds from the notary's; asking again with its time",
                        (long long)(seconds - notary_time));
      seconds = notary_time;
      continue;
    }
    if (status == 429) {
      char wait[32];

      agent_header(files->headers, "retry-after:", wait, sizeof(wait));
      agent_log_warning("the notary is rate limiting this device, so signing waits (retry after %s seconds)",
                        wait[0] != '\0' ? wait : "?");
    } else {
      agent_log_warning("the notary returned %ld: %s", status, message);
    }
    return -1;
  }
  return -1;
}

/* the published keys of the issuer, kept in the notary directory with only its own lines: the tag has to verify as the
   issuer, so a file that also lists other principals can't let their keys sign for it */
static int agent_notary_fetch_keys(const agent_config *config, const char *keys_path,
                                   const agent_notary_files *files) {
  char url[700];
  char output[1024];
  char temporary[AGENT_PATH_MAX + 8];
  const char *argv[] = {"curl", "-sS", "--fail", "--proto", "=http,https", "--max-time", "20", "-A", AGENT_USER_AGENT,
                        "-o", files->download, url, 0};
  char *text;
  char *line;
  char *next;
  size_t issuer_length = strlen(config->issuer);
  long length;
  int kept = 0;
  FILE *file;

  if (config->issuer_keys[0] != '\0') {
    snprintf(url, sizeof(url), "%s", config->issuer_keys);
  } else if (snprintf(url, sizeof(url), "https://%s" AGENT_PUBLISHED_KEYS, config->issuer) >= (int)sizeof(url)) {
    return -1;
  }
  if (agent_run(argv, 0, output, sizeof(output), AGENT_KEYS_TIMEOUT + 10) != 0) {
    agent_trim(output);
    agent_log_warning("cannot fetch the keys %s publishes from %s: %s", config->issuer, url, output);
    return -1;
  }

  text = (char *)malloc(AGENT_KEYS_MAX + 1);
  if (text == 0) {
    return -1;
  }
  length = agent_read_file(files->download, text, AGENT_KEYS_MAX + 1);
  if (length < 0 || length > AGENT_KEYS_MAX) {
    agent_log_warning("%s is missing or larger than %d bytes", url, AGENT_KEYS_MAX);
    free(text);
    return -1;
  }

  snprintf(temporary, sizeof(temporary), "%s.new", keys_path);
  file = fopen(temporary, "wb");
  if (file == 0) {
    agent_log_error("%s: %s", temporary, strerror(errno));
    free(text);
    return -1;
  }
  for (line = text; line != 0; line = next) {
    next = strchr(line, '\n');
    if (next != 0) {
      *next++ = '\0';
    }
    agent_trim(line);
    if (strncmp(line, config->issuer, issuer_length) == 0 && (line[issuer_length] == ' ' || line[issuer_length] == '\t')) {
      fprintf(file, "%s\n", line);
      ++kept;
    }
  }
  free(text);
  if (fclose(file) != 0 || kept == 0 || rename(temporary, keys_path) != 0) {
    if (kept == 0) {
      agent_log_warning("%s lists no keys for %s", url, config->issuer);
    } else {
      agent_log_error("%s: %s", keys_path, strerror(errno));
    }
    unlink(temporary);
    return -1;
  }
  return 0;
}

/* checks the stored tag object's signature with Git, against the issuer's keys: the copy kept from before, and if that
   doesn't verify, because the notary has a new key, the keys as they are published now */
static int agent_notary_verify(const agent_config *config, const char *repository, const char *keys_path,
                               const agent_notary_files *files, const char *object) {
  char setting[AGENT_PATH_MAX + 64];
  char output[1024];
  const char *argv[] = {"git", "-C", repository, "-c", setting, "verify-tag", object, 0};
  struct stat info;

  snprintf(setting, sizeof(setting), "gpg.ssh.allowedSignersFile=%s", keys_path);
  if (stat(keys_path, &info) == 0 && agent_run(argv, 0, output, sizeof(output), AGENT_GIT_TIMEOUT) == 0) {
    return 0;
  }
  if (agent_notary_fetch_keys(config, keys_path, files) != 0) {
    return -1;
  }
  if (agent_run(argv, 0, output, sizeof(output), AGENT_GIT_TIMEOUT) == 0) {
    return 0;
  }
  agent_trim(output);
  agent_log_error("the signature on the notary's tag does not verify against the keys %s publishes: %s", config->issuer,
                  output);
  return -1;
}

/* stores a tag that has been checked; 1 if a tag of that name exists already, which is never overwritten */
static int agent_notary_store(const agent_config *config, const char *repository, const char *keys_path,
                              const agent_notary_files *files, const char *tag, const char *name) {
  char reference[128];
  char object[128];
  char output[1024];
  const char *hash_argv[] = {"git", "-C", repository, "hash-object", "-t", "tag", "-w", files->tag, 0};
  const char *exists_argv[] = {"git", "-C", repository, "rev-parse", "--verify", "--quiet", reference, 0};
  const char *update_argv[] = {"git", "-C", repository, "update-ref", reference, object, "", 0};

  snprintf(reference, sizeof(reference), "refs/tags/%s", name);

  /* the tag is stored unmodified, but is only given a name once it has verified */
  if (agent_write_file(files->tag, tag, 0) != 0) {
    return -1;
  }
  if (agent_run(hash_argv, 0, object, sizeof(object), AGENT_GIT_TIMEOUT) != 0) {
    agent_trim(object);
    agent_log_error("git does not accept the notary's tag: %s", object);
    return -1;
  }
  agent_trim(object);

  if (agent_notary_verify(config, repository, keys_path, files, object) != 0) {
    return -1;
  }
  if (agent_run(exists_argv, 0, output, sizeof(output), AGENT_GIT_TIMEOUT) == 0) {
    return 1;
  }
  if (agent_run(update_argv, 0, output, sizeof(output), AGENT_GIT_TIMEOUT) != 0) {
    agent_trim(output);
    agent_log_error("cannot store %s: %s", reference, output);
    return -1;
  }
  return 0;
}

static int agent_is_hash(const char *text) {
  size_t length = strlen(text);
  size_t i;

  if (length != 40 && length != 64) {
    return 0;
  }
  for (i = 0; i < length; ++i) {
    if (!isdigit((unsigned char)text[i]) && !(text[i] >= 'a' && text[i] <= 'f')) {
      return 0;
    }
  }
  return 1;
}

/* the commit at HEAD, which a signature covers along with everything before it */
static int agent_head_commit(const char *repository, char *commit, size_t commit_size) {
  const char *argv[] = {"git", "-C", repository, "rev-parse", "--verify", "HEAD^{commit}", 0};

  if (agent_run(argv, 0, commit, commit_size, AGENT_GIT_TIMEOUT) != 0) {
    agent_trim(commit);
    agent_log_error("cannot find the latest commit: %s", commit);
    return -1;
  }
  agent_trim(commit);
  if (!agent_is_hash(commit)) {
    agent_log_error("cannot find the latest commit: git said \"%s\"", commit);
    return -1;
  }
  return 0;
}

static int agent_head_signed(const char *repository, const char *commit) {
  char output[256];
  const char *argv[] = {"git", "-C", repository, "for-each-ref", "--points-at", commit, "--format=%(refname)",
                        "refs/tags/tig/sig", 0};

  return agent_run(argv, 0, output, sizeof(output), AGENT_GIT_TIMEOUT) == 0 && output[0] != '\0';
}

agent_sign_result agent_notary_sign(const agent_config *config) {
  char repository[AGENT_PATH_MAX];
  char directory[AGENT_PATH_MAX];
  char keys_path[AGENT_PATH_MAX];
  char commit[128];
  char *tag;
  agent_notary_files *files;
  agent_device device;
  agent_sign_result result = AGENT_SIGN_FAILED;
  int attempt;

  if (config->notary[0] == '\0') {
    return AGENT_SIGN_SKIPPED;
  }
  if (agent_path(repository, sizeof(repository), config, "repository") != 0 ||
      agent_path(directory, sizeof(directory), config, "notary") != 0 ||
      agent_path(keys_path, sizeof(keys_path), config, "notary/tig-public-key") != 0 ||
      agent_device_load(config, &device) != 0 || agent_head_commit(repository, commit, sizeof(commit)) != 0) {
    return AGENT_SIGN_FAILED;
  }
  if (agent_head_signed(repository, commit)) {
    return AGENT_SIGN_CURRENT;
  }
  if (mkdir(directory, 0700) != 0 && errno != EEXIST) {
    agent_log_error("%s: %s", directory, strerror(errno));
    return AGENT_SIGN_FAILED;
  }

  tag = (char *)malloc(AGENT_REPLY_MAX + 1);
  files = (agent_notary_files *)malloc(sizeof(*files));
  if (tag == 0 || files == 0 || agent_files_open(files, directory) != 0) {
    free(tag);
    free(files);
    return AGENT_SIGN_FAILED;
  }

  /* two signatures in the same second would have the same name, so the second request waits a second */
  for (attempt = 0; attempt < 2; ++attempt) {
    char name[64];
    int stored;

    if (attempt > 0) {
      sleep(1);
    }
    if (agent_notary_request_tag(config, &device, commit, files, tag, AGENT_REPLY_MAX + 1) != 0) {
      break;
    }
    if (agent_notary_check_tag(tag, commit, config->issuer, device.fingerprint, name, sizeof(name)) != 0) {
      agent_log_error("the notary's tag is not a signature of %s by %s for this device, so it is not stored", commit,
                      config->issuer);
      break;
    }
    stored = agent_notary_store(config, repository, keys_path, files, tag, name);
    if (stored == 0) {
      agent_log_info("%s signed %.12s as %s", config->issuer, commit, name);
      result = AGENT_SIGN_OK;
      break;
    }
    if (stored < 0) {
      break;
    }
  }

  agent_files_remove(files);
  free(tag);
  free(files);
  return result;
}
