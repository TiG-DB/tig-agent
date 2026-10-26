#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t agent_elapsed_ms(const struct timespec *start) {
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)(now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

static void agent_run_child(const char *const *argv, const char *const *env, int output_fd) {
  int null_fd = open("/dev/null", O_RDONLY);
  size_t i;

  if (null_fd >= 0) {
    dup2(null_fd, STDIN_FILENO);
    close(null_fd);
  }
  dup2(output_fd, STDOUT_FILENO);
  dup2(output_fd, STDERR_FILENO);
  close(output_fd);

  for (i = 0; env != 0 && env[i] != 0; ++i) {
    putenv((char *)env[i]);
  }

  execvp(argv[0], (char *const *)argv);
  fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
  _exit(127);
}

int agent_run(const char *const *argv, const char *const *env, char *output, size_t output_size,
              int timeout_seconds) {
  int fds[2];
  pid_t pid;
  struct timespec start;
  size_t used = 0;
  int timed_out = 0;
  int status;

  if (pipe(fds) != 0) {
    agent_log_error("%s: %s", argv[0], strerror(errno));
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    agent_log_error("%s: %s", argv[0], strerror(errno));
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    close(fds[0]);
    agent_run_child(argv, env, fds[1]);
  }
  close(fds[1]);

  clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    struct pollfd poll_fd;
    char buffer[512];
    int64_t remaining = (int64_t)timeout_seconds * 1000 - agent_elapsed_ms(&start);
    ssize_t count;
    int ready;

    if (remaining <= 0) {
      timed_out = 1;
      break;
    }

    poll_fd.fd = fds[0];
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;
    ready = poll(&poll_fd, 1, (int)remaining);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    if (ready < 0) {
      timed_out = 1;
      break;
    }
    if (ready == 0) {
      continue;
    }

    count = read(fds[0], buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    if (output != 0 && used + 1 < output_size) {
      size_t space = output_size - 1 - used;
      size_t take = (size_t)count < space ? (size_t)count : space;

      memcpy(output + used, buffer, take);
      used += take;
    }
  }
  close(fds[0]);
  if (output != 0 && output_size > 0) {
    output[used] = '\0';
  }

  if (timed_out) {
    kill(pid, SIGKILL);
  }
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }

  if (timed_out) {
    agent_log_error("%s timed out after %d seconds", argv[0], timeout_seconds);
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
