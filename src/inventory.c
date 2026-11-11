/* What the device has now, as tig-device/1 items. See "Linux sources" in tig-docs/vocabularies/DEVICE.md. Everything
   here is readable by an unprivileged user, except the device's serial number, which the package's install script
   copies to AGENT_PRODUCT_SERIAL. On other platforms most sources are missing, and their items are left out. */
#include "agent.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#endif

#define AGENT_TEXT_MAX 512
#define AGENT_DISKS_MAX 64

/* --- small helpers --- */

static void agent_trim(char *text) {
  size_t length = strlen(text);
  size_t start = 0;

  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
  while (isspace((unsigned char)text[start])) {
    ++start;
  }
  memmove(text, text + start, length - start + 1);
}

/* the first line of a file, trimmed; -1 if it can't be read or is empty */
static int agent_read_line(const char *path, char *out, size_t out_size) {
  FILE *file = fopen(path, "r");
  int found;

  out[0] = '\0';
  if (file == 0) {
    return -1;
  }
  found = fgets(out, (int)out_size, file) != 0;
  fclose(file);
  if (!found) {
    out[0] = '\0';
    return -1;
  }
  agent_trim(out);
  return out[0] != '\0' ? 0 : -1;
}

static int agent_read_linef(char *out, size_t out_size, const char *format, const char *a, const char *b) {
  char path[AGENT_PATH_MAX];

  snprintf(path, sizeof(path), format, a, b);
  return agent_read_line(path, out, out_size);
}

static int agent_exists(const char *path) {
  struct stat info;

  return stat(path, &info) == 0;
}

/* the last part of a symlink's target, e.g. a driver's name */
static int agent_link_name(const char *path, char *out, size_t out_size) {
  char target[AGENT_PATH_MAX];
  ssize_t length = readlink(path, target, sizeof(target) - 1);
  const char *slash;

  if (length <= 0) {
    return -1;
  }
  target[length] = '\0';
  slash = strrchr(target, '/');
  snprintf(out, out_size, "%s", slash != 0 ? slash + 1 : target);
  return 0;
}

static int agent_equals_ignoring_case(const char *a, const char *b) {
  while (*a != '\0' && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

/* DMI fields that firmware leaves as a placeholder, which say nothing about the device */
static int agent_is_placeholder(const char *text) {
  static const char *const placeholders[] = {
      "To Be Filled By O.E.M.", "To be filled by O.E.M.", "Default string", "System Product Name",
      "System manufacturer",    "System Serial Number",    "Not Specified",  "Not Applicable",
      "None",                   "O.E.M.",                  "OEM",            "Unknown",
      "Invalid",                "N/A",                     "0123456789",     "123456789"};
  size_t i;
  int same = 1;

  if (text[0] == '\0') {
    return 1;
  }
  for (i = 0; i < sizeof(placeholders) / sizeof(placeholders[0]); ++i) {
    if (agent_equals_ignoring_case(text, placeholders[i])) {
      return 1;
    }
  }
  /* e.g. 00000000 or FFFFFFFF */
  for (i = 1; text[i] != '\0'; ++i) {
    same &= text[i] == text[0];
  }
  return same && strlen(text) > 2;
}

static int agent_set_text(tig_node *map, const char *key, const char *text) {
  if (text == 0 || text[0] == '\0') {
    return 0;
  }
  return tig_node_set(map, key, tig_node_string(text)) == TIG_OK ? 0 : -1;
}

static int agent_set_integer(tig_node *map, const char *key, long long value) {
  return tig_node_set(map, key, tig_node_integer(value)) == TIG_OK ? 0 : -1;
}

/* UNIX ms as RFC 3339, to the second */
void agent_format_time(int64_t unix_ms, char *out, size_t out_size) {
  time_t seconds = (time_t)(unix_ms / 1000);
  struct tm utc;

  if (gmtime_r(&seconds, &utc) == 0) {
    snprintf(out, out_size, "1970-01-01T00:00:00Z");
    return;
  }
  strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

/* when the root of the filesystem at path was created, as a since value; 0 if unknown */
static int agent_birth_time(const char *path, char *out, size_t out_size) {
#if defined(__linux__) && defined(STATX_BTIME)
  struct statx info;

  if (statx(AT_FDCWD, path, AT_STATX_SYNC_AS_STAT, STATX_BTIME, &info) == 0 && (info.stx_mask & STATX_BTIME) != 0 &&
      info.stx_btime.tv_sec > 0) {
    agent_format_time((int64_t)info.stx_btime.tv_sec * 1000, out, out_size);
    return 1;
  }
#else
  (void)path;
  (void)out;
  (void)out_size;
#endif
  return 0;
}

/* KEY=value lines, e.g. /etc/os-release or udev's E:KEY=value; strips quotes */
static int agent_find_value(const char *path, const char *prefix, const char *key, char *out, size_t out_size) {
  FILE *file = fopen(path, "r");
  char line[1024];
  size_t prefix_length = strlen(prefix);
  size_t key_length = strlen(key);
  int found = -1;

  out[0] = '\0';
  if (file == 0) {
    return -1;
  }
  while (found != 0 && fgets(line, sizeof(line), file) != 0) {
    char *value;
    size_t length;

    if (strncmp(line, prefix, prefix_length) != 0 || strncmp(line + prefix_length, key, key_length) != 0 ||
        line[prefix_length + key_length] != '=') {
      continue;
    }
    value = line + prefix_length + key_length + 1;
    agent_trim(value);
    length = strlen(value);
    if (length >= 2 && (value[0] == '"' || value[0] == '\'') && value[length - 1] == value[0]) {
      value[length - 1] = '\0';
      ++value;
    }
    snprintf(out, out_size, "%s", value);
    found = out[0] != '\0' ? 0 : -1;
  }
  fclose(file);
  return found;
}

static int agent_udev_value(const char *dev, const char *key, char *out, size_t out_size) {
  char path[AGENT_PATH_MAX];

  snprintf(path, sizeof(path), "/run/udev/data/b%s", dev);
  return agent_find_value(path, "E:", key, out, out_size);
}

/* --- entity --- */

static int agent_read_entity(tig_node *entity) {
  static const char *const files[] = {"sys_vendor", "product_name"};
  static const char *const keys[] = {"vendor", "model"};
  char text[AGENT_TEXT_MAX];
  char output[256];
  struct utsname name;
  size_t i;
  int result = 0;

  for (i = 0; i < 2; ++i) {
    if (agent_read_linef(text, sizeof(text), "/sys/class/dmi/id/%s%s", files[i], "") == 0 && !agent_is_placeholder(text)) {
      result |= agent_set_text(entity, keys[i], text);
    }
  }
  /* readable only by root, so the install script keeps a copy */
  if ((agent_read_line("/sys/class/dmi/id/product_serial", text, sizeof(text)) == 0 ||
       agent_read_line(AGENT_PRODUCT_SERIAL, text, sizeof(text)) == 0) &&
      !agent_is_placeholder(text)) {
    result |= agent_set_text(entity, "serial", text);
  }
  if (uname(&name) == 0) {
    result |= agent_set_text(entity, "architecture", name.machine);
  }
  /* --vm, since a container is not a virtual machine: it runs on the host's hardware */
  {
    const char *argv[] = {"systemd-detect-virt", "--vm", 0};
    if (agent_run(argv, 0, output, sizeof(output), 10) == 0) {
      agent_trim(output);
      if (output[0] != '\0' && strcmp(output, "none") != 0) {
        result |= agent_set_text(entity, "virtualisation", output);
      }
    }
  }
  return result;
}

/* --- firmware, CPUs, memory and the OS --- */

static int agent_read_firmware(tig_node *items) {
  char vendor[AGENT_TEXT_MAX];
  char version[AGENT_TEXT_MAX];
  char date[AGENT_TEXT_MAX];
  char iso[16] = "";
  tig_node *firmware;
  int month;
  int day;
  int year;
  int result = 0;

  if (agent_read_line("/sys/class/dmi/id/bios_vendor", vendor, sizeof(vendor)) != 0 || agent_is_placeholder(vendor)) {
    vendor[0] = '\0';
  }
  if (agent_read_line("/sys/class/dmi/id/bios_version", version, sizeof(version)) != 0 || agent_is_placeholder(version)) {
    version[0] = '\0';
  }
  /* MM/DD/YYYY */
  if (agent_read_line("/sys/class/dmi/id/bios_date", date, sizeof(date)) == 0 &&
      sscanf(date, "%2d/%2d/%4d", &month, &day, &year) == 3 && month >= 1 && month <= 12 && day >= 1 && day <= 31) {
    snprintf(iso, sizeof(iso), "%04d-%02d-%02d", year, month, day);
  }
  if (vendor[0] == '\0' && version[0] == '\0' && iso[0] == '\0') {
    return 0;
  }

  firmware = tig_node_map();
  result |= agent_set_text(firmware, "vendor", vendor);
  result |= agent_set_text(firmware, "version", version);
  result |= agent_set_text(firmware, "date", iso);
  return result | (tig_node_set(items, "firmware", firmware) == TIG_OK ? 0 : -1);
}

typedef struct agent_cpu {
  int socket;
  char model[AGENT_TEXT_MAX];
  int cores;      /* "cpu cores", 0 if not given */
  int siblings;   /* the socket's hardware threads, 0 if not given */
  int processors; /* logical processors seen in this socket */
} agent_cpu;

static void agent_add_processor(agent_cpu *cpus, int *count, const agent_cpu *processor) {
  int i;

  for (i = 0; i < *count; ++i) {
    if (cpus[i].socket == processor->socket) {
      ++cpus[i].processors;
      return;
    }
  }
  if (*count < 64) {
    cpus[*count] = *processor;
    cpus[*count].processors = 1;
    ++*count;
  }
}

static int agent_read_cpus(tig_node *items) {
  FILE *file = fopen("/proc/cpuinfo", "r");
  agent_cpu cpus[64];
  agent_cpu current;
  char line[1024];
  int count = 0;
  int in_processor = 0;
  int i;
  int result = 0;

  if (file == 0) {
    return 0;
  }
  memset(&current, 0, sizeof(current));
  for (;;) {
    int ended = fgets(line, sizeof(line), file) == 0;
    char *colon = ended ? 0 : strchr(line, ':');
    char *value = colon != 0 ? colon + 1 : 0;

    if (ended || line[0] == '\n') {
      if (in_processor) {
        agent_add_processor(cpus, &count, &current);
      }
      memset(&current, 0, sizeof(current));
      in_processor = 0;
      if (ended) {
        break;
      }
      continue;
    }
    if (colon == 0) {
      continue;
    }
    *colon = '\0';
    agent_trim(line);
    agent_trim(value);
    if (strcmp(line, "processor") == 0) {
      in_processor = 1;
    } else if (strcmp(line, "physical id") == 0) {
      current.socket = atoi(value);
    } else if (strcmp(line, "model name") == 0) {
      snprintf(current.model, sizeof(current.model), "%s", value);
    } else if (strcmp(line, "cpu cores") == 0) {
      current.cores = atoi(value);
    } else if (strcmp(line, "siblings") == 0) {
      current.siblings = atoi(value);
    }
  }
  fclose(file);

  for (i = 0; i < count; ++i) {
    tig_node *cpu = tig_node_map();
    char id[32];
    /* e.g. on ARM, where cpuinfo gives neither, count the processors */
    int threads = cpus[i].siblings > 0 ? cpus[i].siblings : cpus[i].processors;
    int cores = cpus[i].cores > 0 ? cpus[i].cores : threads;

    result |= agent_set_text(cpu, "model", cpus[i].model);
    result |= agent_set_integer(cpu, "cores", cores);
    result |= agent_set_integer(cpu, "threads", threads);
    snprintf(id, sizeof(id), "cpu/%d", cpus[i].socket);
    result |= tig_node_set(items, id, cpu) == TIG_OK ? 0 : -1;
  }
  return result;
}

static int agent_read_memory(tig_node *items) {
  char text[64];
  unsigned long long block = 0;
  unsigned long long capacity = 0;
  DIR *dir;
  struct dirent *entry;
  tig_node *memory;

  /* installed memory, in hot-pluggable blocks */
  if (agent_read_line("/sys/devices/system/memory/block_size_bytes", text, sizeof(text)) == 0) {
    block = strtoull(text, 0, 16);
  }
  dir = block != 0 ? opendir("/sys/devices/system/memory") : 0;
  while (dir != 0 && (entry = readdir(dir)) != 0) {
    if (strncmp(entry->d_name, "memory", 6) == 0 && isdigit((unsigned char)entry->d_name[6]) &&
        agent_read_linef(text, sizeof(text), "/sys/devices/system/memory/%s/online%s", entry->d_name, "") == 0 &&
        strcmp(text, "1") == 0) {
      capacity += block;
    }
  }
  if (dir != 0) {
    closedir(dir);
  }

  /* otherwise what the kernel sees, rounded up to a GiB */
  if (capacity == 0) {
    FILE *file = fopen("/proc/meminfo", "r");
    char line[256];
    unsigned long long kb = 0;

    while (file != 0 && fgets(line, sizeof(line), file) != 0) {
      if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) {
        break;
      }
    }
    if (file != 0) {
      fclose(file);
    }
    capacity = (kb * 1024 + (1ULL << 30) - 1) / (1ULL << 30) * (1ULL << 30);
  }
  if (capacity == 0) {
    return 0;
  }

  memory = tig_node_map();
  return agent_set_integer(memory, "capacity", (long long)capacity) |
         (tig_node_set(items, "memory", memory) == TIG_OK ? 0 : -1);
}

static int agent_read_os(tig_node *items, tig_node *since) {
  char id[AGENT_TEXT_MAX];
  char version[AGENT_TEXT_MAX];
  char name[AGENT_TEXT_MAX];
  char birth[32];
  struct utsname system;
  tig_node *os;
  int result = 0;

  agent_find_value("/etc/os-release", "", "ID", id, sizeof(id));
  agent_find_value("/etc/os-release", "", "VERSION_ID", version, sizeof(version));
  agent_find_value("/etc/os-release", "", "PRETTY_NAME", name, sizeof(name));
  if (id[0] == '\0') {
    return 0;
  }

  os = tig_node_map();
  result |= agent_set_text(os, "id", id);
  result |= agent_set_text(os, "version", version);
  result |= agent_set_text(os, "name", name);
  if (uname(&system) == 0) {
    result |= agent_set_text(os, "kernel", system.release);
  }
  result |= tig_node_set(items, "os", os) == TIG_OK ? 0 : -1;
  /* installed when its root filesystem was created */
  if (agent_birth_time("/", birth, sizeof(birth))) {
    result |= agent_set_text(since, "os", birth);
  }
  return result;
}

/* --- disks --- */

typedef struct agent_disk_name {
  char name[64]; /* e.g. nvme0n1 */
  char id[64];   /* e.g. disk/7a858f1b */
} agent_disk_name;

typedef struct agent_disks {
  agent_disk_name items[AGENT_DISKS_MAX];
  int count;
} agent_disks;

static const char *agent_disk_id(const agent_disks *disks, const char *name) {
  int i;

  for (i = 0; i < disks->count; ++i) {
    if (strcmp(disks->items[i].name, name) == 0) {
      return disks->items[i].id;
    }
  }
  return 0;
}

static const char *agent_transport(const char *name, const char *bus, const char *path) {
  if (strncmp(name, "nvme", 4) == 0) {
    return "nvme";
  }
  if (strncmp(name, "mmcblk", 6) == 0) {
    return "mmc";
  }
  if (strncmp(name, "vd", 2) == 0 || strstr(path, "virtio") != 0) {
    return "virtio";
  }
  if (strcmp(bus, "ata") == 0) {
    return "sata";
  }
  if (strcmp(bus, "scsi") == 0) {
    return "sas";
  }
  return 0;
}

static int agent_read_disk(tig_node *items, agent_disks *disks, const char *name) {
  char dev[32];
  char text[64];
  char model[AGENT_TEXT_MAX];
  char serial[AGENT_TEXT_MAX];
  char wwn[AGENT_TEXT_MAX];
  char bus[64];
  char path[AGENT_TEXT_MAX];
  char identity[AGENT_TEXT_MAX * 2 + 8];
  char id[64];
  char sys[AGENT_PATH_MAX];
  const char *transport;
  tig_node *disk;
  int result = 0;

  /* virtual devices (loop, RAM, device-mapper, RAID, zram) have no device behind them */
  snprintf(sys, sizeof(sys), "/sys/block/%s/device", name);
  if (!agent_exists(sys) || strstr(name, "boot") != 0 || strstr(name, "rpmb") != 0 ||
      agent_read_linef(dev, sizeof(dev), "/sys/block/%s/dev%s", name, "") != 0) {
    return 0;
  }
  if (agent_read_linef(text, sizeof(text), "/sys/block/%s/removable%s", name, "") == 0 && strcmp(text, "1") == 0) {
    return 0;
  }

  agent_udev_value(dev, "ID_MODEL", model, sizeof(model));
  agent_udev_value(dev, "ID_SERIAL_SHORT", serial, sizeof(serial));
  agent_udev_value(dev, "ID_WWN", wwn, sizeof(wwn));
  agent_udev_value(dev, "ID_BUS", bus, sizeof(bus));
  agent_udev_value(dev, "ID_PATH", path, sizeof(path));
  /* USB disks come and go */
  if (strcmp(bus, "usb") == 0 || strstr(path, "usb") != 0) {
    return 0;
  }

  if (model[0] != '\0' && serial[0] != '\0') {
    snprintf(identity, sizeof(identity), "%s\n%s", model, serial);
  } else if (wwn[0] != '\0') {
    snprintf(identity, sizeof(identity), "%s", wwn);
  } else if (path[0] != '\0') {
    snprintf(identity, sizeof(identity), "path:%s", path);
  } else {
    /* nothing to tell it apart by, e.g. in a container without udev */
    return 0;
  }
  agent_hashed_id("disk", identity, id, sizeof(id));

  disk = tig_node_map();
  result |= agent_set_text(disk, "model", model);
  result |= agent_set_text(disk, "serial", serial);
  result |= agent_set_text(disk, "wwn", wwn);
  if (agent_read_linef(text, sizeof(text), "/sys/block/%s/size%s", name, "") == 0) {
    result |= agent_set_integer(disk, "capacity", strtoll(text, 0, 10) * 512);
  }
  transport = agent_transport(name, bus, path);
  if (transport != 0) {
    result |= agent_set_text(disk, "transport", transport);
  }
  if (agent_read_linef(text, sizeof(text), "/sys/block/%s/queue/rotational%s", name, "") == 0) {
    result |= tig_node_set(disk, "rotational", tig_node_boolean(strcmp(text, "1") == 0)) == TIG_OK ? 0 : -1;
  }
  result |= tig_node_set(items, id, disk) == TIG_OK ? 0 : -1;

  if (disks->count < AGENT_DISKS_MAX) {
    snprintf(disks->items[disks->count].name, sizeof(disks->items[0].name), "%s", name);
    snprintf(disks->items[disks->count].id, sizeof(disks->items[0].id), "%s", id);
    ++disks->count;
  }
  return result;
}

static int agent_read_disks(tig_node *items, agent_disks *disks) {
  DIR *dir = opendir("/sys/block");
  struct dirent *entry;
  int result = 0;

  if (dir == 0) {
    return 0;
  }
  while ((entry = readdir(dir)) != 0) {
    if (entry->d_name[0] != '.') {
      result |= agent_read_disk(items, disks, entry->d_name);
    }
  }
  closedir(dir);
  return result;
}

/* --- filesystems --- */

static int agent_is_persistent(const char *type) {
  static const char *const types[] = {"ext2", "ext3", "ext4", "xfs", "btrfs", "f2fs", "zfs", "vfat",
                                      "exfat", "ntfs", "ntfs3", "jfs", "reiserfs", "bcachefs"};
  size_t i;

  for (i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
    if (strcmp(type, types[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

/* the whole disks under a block device, through partitions and device-mapper or RAID layers; -1 if any isn't one
   that is recorded */
static int agent_underlying_disks(const char *name, const agent_disks *disks, const char **out, int *count, int depth) {
  char path[AGENT_PATH_MAX];
  char resolved[PATH_MAX];
  char parent[64];
  DIR *dir;
  struct dirent *entry;
  int found = 0;
  int i;

  if (depth > 8) {
    return -1;
  }
  if (agent_disk_id(disks, name) != 0) {
    const char *id = agent_disk_id(disks, name);
    for (i = 0; i < *count; ++i) {
      if (strcmp(out[i], id) == 0) {
        return 0;
      }
    }
    if (*count < AGENT_DISKS_MAX) {
      out[(*count)++] = id;
    }
    return 0;
  }

  /* a partition's folder is inside its disk's */
  snprintf(path, sizeof(path), "/sys/class/block/%s/partition", name);
  if (agent_exists(path)) {
    const char *slash;

    snprintf(path, sizeof(path), "/sys/class/block/%s/..", name);
    if (realpath(path, resolved) == 0 || (slash = strrchr(resolved, '/')) == 0) {
      return -1;
    }
    snprintf(parent, sizeof(parent), "%s", slash + 1);
    return agent_underlying_disks(parent, disks, out, count, depth + 1);
  }

  snprintf(path, sizeof(path), "/sys/class/block/%s/slaves", name);
  dir = opendir(path);
  while (dir != 0 && (entry = readdir(dir)) != 0) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    found = 1;
    if (agent_underlying_disks(entry->d_name, disks, out, count, depth + 1) != 0) {
      closedir(dir);
      return -1;
    }
  }
  if (dir != 0) {
    closedir(dir);
  }
  return found ? 0 : -1;
}

/* the block device's name, e.g. sda2, from its major:minor */
static int agent_block_name(const char *dev, char *out, size_t out_size) {
  char path[AGENT_PATH_MAX];
  char resolved[PATH_MAX];
  const char *slash;

  snprintf(path, sizeof(path), "/sys/dev/block/%s", dev);
  if (realpath(path, resolved) == 0 || (slash = strrchr(resolved, '/')) == 0) {
    return -1;
  }
  snprintf(out, out_size, "%s", slash + 1);
  return 0;
}

/* mountinfo escapes spaces and the like as \ooo */
static void agent_unescape(char *text) {
  char *in = text;
  char *out = text;

  while (*in != '\0') {
    if (in[0] == '\\' && in[1] >= '0' && in[1] <= '7' && in[2] >= '0' && in[2] <= '7' && in[3] >= '0' && in[3] <= '7') {
      *out++ = (char)((in[1] - '0') * 64 + (in[2] - '0') * 8 + (in[3] - '0'));
      in += 4;
    } else {
      *out++ = *in++;
    }
  }
  *out = '\0';
}

/* "/" is filesystem/root; otherwise the path without its leading /, lowercased, with other characters as - */
static void agent_filesystem_id(const char *mount, char *out, size_t out_size) {
  size_t length;
  size_t i;

  if (strcmp(mount, "/") == 0) {
    snprintf(out, out_size, "filesystem/root");
    return;
  }
  snprintf(out, out_size, "filesystem/%s", mount + 1);
  length = strlen(out);
  for (i = strlen("filesystem/"); i < length; ++i) {
    char c = (char)tolower((unsigned char)out[i]);
    out[i] = (islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-' || c == '.' || c == '/') ? c : '-';
  }
}

typedef struct agent_mount {
  char dev[32];      /* the block device's major:minor */
  char subvolume[256];
  char point[1024];
  char type[32];
} agent_mount;

static int agent_compare_disk_ids(const void *left, const void *right) {
  return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static int agent_add_filesystem(tig_node *items, tig_node *since, const agent_disks *disks, const agent_mount *mount) {
  const char *underlying[AGENT_DISKS_MAX];
  char name[64];
  char uuid[128];
  char id[1100];
  char birth[32];
  struct statvfs fs;
  tig_node *filesystem;
  tig_node *list;
  int count = 0;
  int i;
  int result = 0;

  /* only filesystems on recorded disks */
  if (agent_block_name(mount->dev, name, sizeof(name)) != 0 ||
      agent_underlying_disks(name, disks, underlying, &count, 0) != 0 || count == 0) {
    return 0;
  }
  qsort(underlying, (size_t)count, sizeof(underlying[0]), agent_compare_disk_ids);

  filesystem = tig_node_map();
  list = tig_node_list();
  result |= agent_set_text(filesystem, "type", mount->type);
  /* /dev/disk/by-uuid is hidden from the service by PrivateDevices, so udev's copy comes first */
  if (agent_udev_value(mount->dev, "ID_FS_UUID", uuid, sizeof(uuid)) == 0) {
    result |= agent_set_text(filesystem, "uuid", uuid);
  } else {
    DIR *dir = opendir("/dev/disk/by-uuid");
    struct dirent *entry;
    char path[AGENT_PATH_MAX];
    char target[64];

    while (dir != 0 && (entry = readdir(dir)) != 0) {
      snprintf(path, sizeof(path), "/dev/disk/by-uuid/%s", entry->d_name);
      if (entry->d_name[0] != '.' && agent_link_name(path, target, sizeof(target)) == 0 && strcmp(target, name) == 0) {
        result |= agent_set_text(filesystem, "uuid", entry->d_name);
        break;
      }
    }
    if (dir != 0) {
      closedir(dir);
    }
  }
  if (statvfs(mount->point, &fs) == 0) {
    result |= agent_set_integer(filesystem, "size", (long long)fs.f_blocks * (long long)fs.f_frsize);
  }
  for (i = 0; i < count; ++i) {
    result |= tig_node_append(list, tig_node_string(underlying[i])) == TIG_OK ? 0 : -1;
  }
  result |= tig_node_set(filesystem, "disks", list) == TIG_OK ? 0 : -1;

  agent_filesystem_id(mount->point, id, sizeof(id));
  result |= tig_node_set(items, id, filesystem) == TIG_OK ? 0 : -1;
  if (agent_birth_time(mount->point, birth, sizeof(birth))) {
    result |= agent_set_text(since, id, birth);
  }
  return result;
}

static int agent_read_filesystems(tig_node *items, tig_node *since, const agent_disks *disks) {
  FILE *file = fopen("/proc/self/mountinfo", "r");
  agent_mount *mounts = 0;
  int count = 0;
  int capacity = 0;
  char line[4096];
  int i;
  int result = 0;

  if (file == 0) {
    return 0;
  }
  while (fgets(line, sizeof(line), file) != 0) {
    char *fields[64];
    int field_count = 0;
    char *save = 0;
    char *field;
    int separator = -1;
    agent_mount mount;
    const char *subvolume;
    int j;

    for (field = strtok_r(line, " \n", &save); field != 0 && field_count < 64; field = strtok_r(0, " \n", &save)) {
      if (strcmp(field, "-") == 0 && separator < 0) {
        separator = field_count;
      }
      fields[field_count++] = field;
    }
    /* id parent major:minor root mount-point options [optional...] - type source super-options */
    if (separator < 6 || field_count < separator + 4 || !agent_is_persistent(fields[separator + 1])) {
      continue;
    }

    memset(&mount, 0, sizeof(mount));
    snprintf(mount.dev, sizeof(mount.dev), "%s", fields[2]);
    snprintf(mount.type, sizeof(mount.type), "%s", fields[separator + 1]);
    snprintf(mount.point, sizeof(mount.point), "%s", fields[4]);
    agent_unescape(mount.point);
    /* btrfs reports an anonymous device, so find the real one from the source, e.g. /dev/sda2 */
    if (strncmp(mount.dev, "0:", 2) == 0 && strncmp(fields[separator + 2], "/dev/", 5) == 0) {
      char text[32];
      if (agent_read_linef(text, sizeof(text), "/sys/class/block/%s/dev%s", fields[separator + 2] + 5, "") == 0) {
        snprintf(mount.dev, sizeof(mount.dev), "%s", text);
      }
    }
    /* subvolumes mounted separately are separate filesystems */
    subvolume = strstr(fields[separator + 3], "subvol=");
    if (subvolume != 0) {
      snprintf(mount.subvolume, sizeof(mount.subvolume), "%.*s", (int)strcspn(subvolume, ","), subvolume);
    }

    /* the same filesystem at several points (e.g. a bind mount) is recorded at its shortest */
    for (j = 0; j < count; ++j) {
      if (strcmp(mounts[j].dev, mount.dev) == 0 && strcmp(mounts[j].subvolume, mount.subvolume) == 0) {
        break;
      }
    }
    if (j < count) {
      size_t existing = strlen(mounts[j].point);
      size_t candidate = strlen(mount.point);
      if (candidate < existing || (candidate == existing && strcmp(mount.point, mounts[j].point) < 0)) {
        mounts[j] = mount;
      }
      continue;
    }
    if (count == capacity) {
      agent_mount *grown = (agent_mount *)realloc(mounts, (size_t)(capacity == 0 ? 16 : capacity * 2) * sizeof(*mounts));
      if (grown == 0) {
        result = -1;
        break;
      }
      mounts = grown;
      capacity = capacity == 0 ? 16 : capacity * 2;
    }
    mounts[count++] = mount;
  }
  fclose(file);

  for (i = 0; i < count; ++i) {
    result |= agent_add_filesystem(items, since, disks, &mounts[i]);
  }
  free(mounts);
  return result;
}

/* --- network adapters --- */

/* the address burnt into the adapter, lowercase and colon-separated */
static int agent_permanent_mac(const char *interface, char *out, size_t out_size) {
  char text[64];

#ifdef __linux__
  {
    struct {
      struct ethtool_perm_addr header;
      unsigned char data[32];
    } request;
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&request, 0, sizeof(request));
    memset(&ifr, 0, sizeof(ifr));
    request.header.cmd = ETHTOOL_GPERMADDR;
    request.header.size = sizeof(request.data);
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", interface);
    ifr.ifr_data = (char *)&request;
    if (fd >= 0 && ioctl(fd, SIOCETHTOOL, &ifr) == 0 && request.header.size == 6) {
      const unsigned char *mac = request.header.data;
      int zero = !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);

      if (!zero) {
        close(fd);
        snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return 0;
      }
    }
    if (fd >= 0) {
      close(fd);
    }
  }
#endif

  /* otherwise the current address, if it is the permanent one */
  if (agent_read_linef(text, sizeof(text), "/sys/class/net/%s/addr_assign_type%s", interface, "") == 0 &&
      strcmp(text, "0") == 0 && agent_read_linef(out, out_size, "/sys/class/net/%s/address%s", interface, "") == 0 &&
      strcmp(out, "00:00:00:00:00:00") != 0) {
    size_t i;
    for (i = 0; out[i] != '\0'; ++i) {
      out[i] = (char)tolower((unsigned char)out[i]);
    }
    return 0;
  }
  return -1;
}

static int agent_read_network(tig_node *items, const char *interface) {
  char path[AGENT_PATH_MAX];
  char mac[64];
  char text[64];
  char driver[128];
  char subsystem[64];
  char vendor[32];
  char device[32];
  char id[64];
  tig_node *adapter;
  int result = 0;

  /* physical adapters only: virtual interfaces have no device */
  snprintf(path, sizeof(path), "/sys/class/net/%s/device", interface);
  if (!agent_exists(path) || agent_permanent_mac(interface, mac, sizeof(mac)) != 0) {
    return 0;
  }

  adapter = tig_node_map();
  result |= agent_set_text(adapter, "mac", mac);
  snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", interface);
  if (agent_exists(path) || (snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", interface), agent_exists(path))) {
    result |= agent_set_text(adapter, "medium", "wifi");
  } else if (agent_read_linef(text, sizeof(text), "/sys/class/net/%s/type%s", interface, "") == 0 && strcmp(text, "1") == 0) {
    result |= agent_set_text(adapter, "medium", "ethernet");
  }
  snprintf(path, sizeof(path), "/sys/class/net/%s/device/driver", interface);
  if (agent_link_name(path, driver, sizeof(driver)) == 0) {
    result |= agent_set_text(adapter, "driver", driver);
  }
  snprintf(path, sizeof(path), "/sys/class/net/%s/device/subsystem", interface);
  if (agent_link_name(path, subsystem, sizeof(subsystem)) == 0 && strcmp(subsystem, "pci") == 0 &&
      agent_read_linef(vendor, sizeof(vendor), "/sys/class/net/%s/device/vendor%s", interface, "") == 0 &&
      agent_read_linef(device, sizeof(device), "/sys/class/net/%s/device/device%s", interface, "") == 0 &&
      strncmp(vendor, "0x", 2) == 0 && strncmp(device, "0x", 2) == 0) {
    tig_node *hardware = tig_node_map();

    snprintf(text, sizeof(text), "%s:%s", vendor + 2, device + 2);
    result |= agent_set_text(hardware, "system", "pci");
    result |= agent_set_text(hardware, "value", text);
    result |= tig_node_set(adapter, "hardware", hardware) == TIG_OK ? 0 : -1;
  }

  agent_hashed_id("network", mac, id, sizeof(id));
  return result | (tig_node_set(items, id, adapter) == TIG_OK ? 0 : -1);
}

static int agent_read_networks(tig_node *items) {
  DIR *dir = opendir("/sys/class/net");
  struct dirent *entry;
  int result = 0;

  while (dir != 0 && (entry = readdir(dir)) != 0) {
    if (entry->d_name[0] != '.') {
      result |= agent_read_network(items, entry->d_name);
    }
  }
  if (dir != 0) {
    closedir(dir);
  }
  return result;
}

/* --- batteries --- */

/* µWh or µAh as Wh or Ah, without trailing zeros */
static tig_node *agent_micro_units(const char *text) {
  long long micro = strtoll(text, 0, 10);
  char value[64];
  size_t length;

  snprintf(value, sizeof(value), "%lld.%06lld", micro / 1000000, micro % 1000000);
  length = strlen(value);
  while (value[length - 1] == '0') {
    value[--length] = '\0';
  }
  if (value[length - 1] == '.') {
    value[--length] = '\0';
  }
  return tig_node_number(value);
}

static int agent_read_battery(tig_node *items, const char *name) {
  static const char *const files[] = {"manufacturer", "model_name", "serial_number", "technology"};
  static const char *const keys[] = {"manufacturer", "model", "serial", "technology"};
  char values[4][AGENT_TEXT_MAX];
  char text[64];
  char identity[AGENT_TEXT_MAX * 3 + 4];
  char id[300];
  tig_node *battery;
  size_t i;
  int result = 0;

  /* batteries that power the device, not a mouse's or a keyboard's */
  if (agent_read_linef(text, sizeof(text), "/sys/class/power_supply/%s/type%s", name, "") != 0 ||
      strcmp(text, "Battery") != 0 ||
      (agent_read_linef(text, sizeof(text), "/sys/class/power_supply/%s/scope%s", name, "") == 0 &&
       strcmp(text, "Device") == 0)) {
    return 0;
  }

  battery = tig_node_map();
  for (i = 0; i < 4; ++i) {
    if (agent_read_linef(values[i], sizeof(values[i]), "/sys/class/power_supply/%s/%s", name, files[i]) != 0 ||
        agent_is_placeholder(values[i])) {
      values[i][0] = '\0';
    }
    result |= agent_set_text(battery, keys[i], values[i]);
  }
  if (agent_read_linef(text, sizeof(text), "/sys/class/power_supply/%s/energy_full_design%s", name, "") == 0) {
    result |= tig_node_set(battery, "design-energy", agent_micro_units(text)) == TIG_OK ? 0 : -1;
  } else if (agent_read_linef(text, sizeof(text), "/sys/class/power_supply/%s/charge_full_design%s", name, "") == 0) {
    result |= tig_node_set(battery, "design-charge", agent_micro_units(text)) == TIG_OK ? 0 : -1;
  }

  if (values[0][0] != '\0' && values[1][0] != '\0' && values[2][0] != '\0') {
    snprintf(identity, sizeof(identity), "%s\n%s\n%s", values[0], values[1], values[2]);
    agent_hashed_id("battery", identity, id, sizeof(id));
  } else {
    /* without its own identity, a battery is known by its role, e.g. battery/bat0 */
    snprintf(id, sizeof(id), "battery/%s", name);
    for (i = 0; id[i] != '\0'; ++i) {
      char c = (char)tolower((unsigned char)id[i]);
      id[i] = (islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-' || c == '.' || c == '/') ? c : '-';
    }
  }
  return result | (tig_node_set(items, id, battery) == TIG_OK ? 0 : -1);
}

static int agent_read_batteries(tig_node *items) {
  DIR *dir = opendir("/sys/class/power_supply");
  struct dirent *entry;
  int result = 0;

  while (dir != 0 && (entry = readdir(dir)) != 0) {
    if (entry->d_name[0] != '.') {
      result |= agent_read_battery(items, entry->d_name);
    }
  }
  if (dir != 0) {
    closedir(dir);
  }
  return result;
}

/* --- public --- */

void agent_inventory_free(agent_inventory *inventory) {
  tig_node_free(inventory->entity);
  tig_node_free(inventory->items);
  tig_node_free(inventory->since);
  memset(inventory, 0, sizeof(*inventory));
}

int agent_inventory_read(agent_inventory *inventory) {
  agent_disks disks;
  int result = 0;

  memset(&disks, 0, sizeof(disks));
  inventory->entity = tig_node_map();
  inventory->items = tig_node_map();
  inventory->since = tig_node_map();
  if (inventory->entity == 0 || inventory->items == 0 || inventory->since == 0) {
    agent_inventory_free(inventory);
    return -1;
  }

  result |= agent_read_entity(inventory->entity);
  result |= agent_read_firmware(inventory->items);
  result |= agent_read_cpus(inventory->items);
  result |= agent_read_memory(inventory->items);
  result |= agent_read_disks(inventory->items, &disks);
  result |= agent_read_filesystems(inventory->items, inventory->since, &disks);
  result |= agent_read_networks(inventory->items);
  result |= agent_read_os(inventory->items, inventory->since);
  result |= agent_read_batteries(inventory->items);
  if (result != 0) {
    agent_log_error("cannot read the device's inventory: out of memory");
    agent_inventory_free(inventory);
    return -1;
  }
  return 0;
}

int agent_boot_read(agent_boot *boot) {
  FILE *file;
  char line[256];

  memset(boot, 0, sizeof(*boot));
  if (agent_read_line("/proc/sys/kernel/random/boot_id", boot->id, sizeof(boot->id)) != 0) {
    boot->id[0] = '\0';
    return -1;
  }
  file = fopen("/proc/stat", "r");
  while (file != 0 && fgets(line, sizeof(line), file) != 0) {
    long long seconds;
    if (sscanf(line, "btime %lld", &seconds) == 1) {
      boot->time_ms = (int64_t)seconds * 1000;
      break;
    }
  }
  if (file != 0) {
    fclose(file);
  }
  return boot->time_ms != 0 ? 0 : -1;
}
