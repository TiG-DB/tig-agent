/* The device key, which signs every commit (COMMITS.md, "Device keys"). It is separate from the deploy key:
   the deploy key is registered with a Git host and may be replaced, while the device key is the device's
   permanent, pseudonymous identity in its repository. Signing runs ssh-keygen, so libtig needs no crypto. */
#include "agent.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int agent_create_device_key(const char *path) {
  char output[1024];
  /* the comment is never published, but keep it free of anything identifying anyway */
  const char *argv[] = {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "tig-device", "-f", path, 0};

  if (agent_run(argv, 0, output, sizeof(output), 30) != 0) {
    agent_log_error("ssh-keygen failed: %s", output);
    return -1;
  }
  agent_log_info("created device key %s.pub", path);
  return 0;
}

long agent_read_file(const char *path, char *buffer, size_t size) {
  FILE *file = fopen(path, "rb");
  size_t length;

  if (file == 0) {
    return -1;
  }
  length = fread(buffer, 1, size - 1, file);
  buffer[length] = '\0';
  fclose(file);
  return (long)length;
}

/* the second whitespace-separated field of text, e.g. the fingerprint in `ssh-keygen -l` output */
static int agent_second_field(const char *text, char *out, size_t out_size) {
  const char *start = strchr(text, ' ');
  size_t length;

  if (start == 0) {
    return -1;
  }
  ++start;
  length = strcspn(start, " \t\r\n");
  if (length == 0 || length >= out_size) {
    return -1;
  }
  memcpy(out, start, length);
  out[length] = '\0';
  return 0;
}

static tig_status agent_sign(void *context, const char *content, size_t content_length, char *signature,
                             size_t signature_size) {
  const agent_device *device = (const agent_device *)context;
  char payload_path[AGENT_PATH_MAX];
  char signature_path[AGENT_PATH_MAX + 8];
  char output[1024];
  const char *argv[] = {"ssh-keygen", "-q", "-Y", "sign", "-n", "git", "-f", device->key_path, payload_path, 0};
  tig_status result = TIG_OK;
  FILE *payload;
  int descriptor;

  if (snprintf(payload_path, sizeof(payload_path), "%s/.sign-XXXXXX", device->work_dir) >= (int)sizeof(payload_path)) {
    return TIG_ERROR_BUFFER_TOO_SMALL;
  }
  descriptor = mkstemp(payload_path);
  if (descriptor < 0) {
    agent_log_error("%s: %s", payload_path, strerror(errno));
    return TIG_ERROR_REJECTED;
  }
  snprintf(signature_path, sizeof(signature_path), "%s.sig", payload_path);
  unlink(signature_path);

  payload = fdopen(descriptor, "wb");
  if (payload == 0 || fwrite(content, 1, content_length, payload) != content_length) {
    agent_log_error("%s: %s", payload_path, strerror(errno));
    result = TIG_ERROR_REJECTED;
  }
  if (payload != 0 ? fclose(payload) != 0 : close(descriptor) != 0) {
    result = TIG_ERROR_REJECTED;
  }

  if (result == TIG_OK && agent_run(argv, 0, output, sizeof(output), 30) != 0) {
    agent_log_error("ssh-keygen -Y sign failed: %s", output);
    result = TIG_ERROR_REJECTED;
  }
  if (result == TIG_OK && agent_read_file(signature_path, signature, signature_size) <= 0) {
    agent_log_error("%s: no signature written", signature_path);
    result = TIG_ERROR_REJECTED;
  }

  unlink(signature_path);
  unlink(payload_path);
  return result;
}

int agent_device_load(const agent_config *config, agent_device *device) {
  char public_path[AGENT_PATH_MAX];
  char public_line[1024];
  char output[1024];
  const char *argv[] = {"ssh-keygen", "-l", "-E", "sha256", "-f", public_path, 0};
  size_t type_length;
  size_t key_length;
  char *key;

  memset(device, 0, sizeof(*device));
  if (agent_path(device->key_path, sizeof(device->key_path), config, "keys/device_ed25519") != 0 ||
      agent_path(public_path, sizeof(public_path), config, "keys/device_ed25519.pub") != 0 ||
      agent_path(device->work_dir, sizeof(device->work_dir), config, "keys") != 0) {
    return -1;
  }

  /* "ssh-ed25519 AAAA... comment": the allowed signers line takes the type and key, without the comment */
  if (agent_read_file(public_path, public_line, sizeof(public_line)) <= 0) {
    agent_log_error("cannot read the device key %s (run tig-agent init first)", public_path);
    return -1;
  }
  type_length = strcspn(public_line, " ");
  key = public_line + type_length + (public_line[type_length] == ' ');
  key_length = strcspn(key, " \t\r\n");
  if (strncmp(public_line, "ssh-ed25519 ", 12) != 0 || key_length == 0) {
    agent_log_error("%s: not an Ed25519 public key", public_path);
    return -1;
  }
  key[key_length] = '\0';

  if (agent_run(argv, 0, output, sizeof(output), 30) != 0 ||
      agent_second_field(output, device->fingerprint, sizeof(device->fingerprint)) != 0 ||
      strncmp(device->fingerprint, "SHA256:", 7) != 0) {
    agent_log_error("cannot fingerprint %s: %s", public_path, output);
    return -1;
  }

  if (snprintf(device->public_key, sizeof(device->public_key), "ssh-ed25519 %s", key) >= (int)sizeof(device->public_key) ||
      snprintf(device->allowed_signer, sizeof(device->allowed_signer), "%s namespaces=\"git\" %s", device->fingerprint,
               device->public_key) >= (int)sizeof(device->allowed_signer)) {
    agent_log_error("%s: key too long", public_path);
    return -1;
  }

  device->device.allowed_signer = device->allowed_signer;
  device->device.sign = agent_sign;
  device->device.sign_context = device;
  return 0;
}
