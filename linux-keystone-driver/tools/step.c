#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../keystone_step_rec.h"

#define ENCLAVE_IDR_BASE 0x1000

#define PARAM_DIR "/sys/module/keystone_driver/parameters/"
#define LOG_DEV "/dev/keystone_step"

static int
write_param(const char *name, long value)
{
  char path[256];
  snprintf(path, sizeof(path), PARAM_DIR "%s", name);

  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    fprintf(stderr, "(is the keystone-driver module loaded? are you root?)\n");
    return -1;
  }
  int rc = fprintf(f, "%ld\n", value) < 0 ? -1 : 0;
  if (fclose(f))
    rc = -1;
  if (rc)
    fprintf(stderr, "%s: write failed: %s\n", path, strerror(errno));
  return rc;
}

static void
disarm(void)
{
  write_param("strike_target_eid", -1);
  write_param("log_pc", 0);
}

static int
read_param(const char *name, unsigned long *value)
{
  char path[256];
  snprintf(path, sizeof(path), PARAM_DIR "%s", name);

  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  int n = fscanf(f, "%lu", value);
  fclose(f);
  return n == 1 ? 0 : -1;
}

#define ERROR_INTERRUPTED 100002
#define ERROR_EDGE_CALL_HOST 100011

static unsigned long step_index;
static unsigned long previous_pc;
static int has_previous_pc;

static const char *
classify_step(unsigned long pc)
{
  const char *kind = "-";

  if (has_previous_pc) {
    long diff = (long)(pc - previous_pc);
    if (diff == 0)
      kind = "zero";
    else if (diff > 0 && diff <= 4)
      kind = "single";
    else
      kind = "multi";
  }
  previous_pc = pc;
  has_previous_pc = 1;
  return kind;
}

static int
print_record(const struct step_rec *rec)
{
  switch (rec->kind) {
  case STEP_REC_START:
    step_index = 0;
    has_previous_pc = 0;
    fprintf(stdout, "start ueid=0x%llx\n", (unsigned long long)rec->val);
    return 0;
  case STEP_REC_STEP:
    fprintf(stdout, "step=%lu pc=0x%016llx err=%d %s\n", step_index++,
            (unsigned long long)rec->val, ERROR_INTERRUPTED,
            classify_step((unsigned long)rec->val));
    return 0;
  case STEP_REC_END:
    fprintf(stdout, "end err=%u value=0x%llx\n", rec->err, (unsigned long long)rec->val);
    return rec->err != ERROR_INTERRUPTED && rec->err != ERROR_EDGE_CALL_HOST;
  default:
    return 0;
  }
}

static int
drain_log(int fd, int wait_ms)
{
  static struct step_rec records[4096];
  struct pollfd pfd = { .fd = fd, .events = POLLIN };
  int finished = 0;

  for (;;) {
    int pr = poll(&pfd, 1, wait_ms);
    if (pr < 0) {
      if (errno == EINTR)
        break;
      return -1;
    }
    if (pr == 0)
      break;

    ssize_t n = read(fd, records, sizeof(records));
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR)
        continue;
      return -1;
    }

    for (size_t i = 0; i < (size_t)n / sizeof(records[0]); i++)
      if (print_record(&records[i]))
        finished = 1;

    if (finished)
      break;
  }

  if (fflush(stdout))
    return -1;
  return finished;
}

static volatile sig_atomic_t stop;

static void
on_signal(int sig)
{
  (void)sig;
  stop = 1;
}

static void
usage(const char *program)
{
  fprintf(stderr,
          "usage: %s --eid <enclave id|any> [--delta <ticks>] [--no-log]\n"
          "          [--pa <physical address>] [--rip-start <addr> --rip-end <addr>]\n"
          "       %s --off\n",
          program, program);
}

int
main(int argc, char **argv)
{
  long enclave_id = -1;
  int any_enclave = 0;
  long delta_ticks = -1;
  unsigned long pa = 0;
  unsigned long rip_start = 0;
  unsigned long rip_end = 0;
  int has_pa = 0;
  int has_rip_start = 0;
  int has_rip_end = 0;
  int log_pc = 1;
  int off = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--eid") && i + 1 < argc) {
      i++;
      if (!strcmp(argv[i], "any"))
        any_enclave = 1;
      else
        enclave_id = strtol(argv[i], NULL, 0);
    }
    else if (!strcmp(argv[i], "--delta") && i + 1 < argc)
      delta_ticks = strtol(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--pa") && i + 1 < argc) {
      pa = strtoul(argv[++i], NULL, 0);
      has_pa = 1;
    } else if (!strcmp(argv[i], "--rip-start") && i + 1 < argc) {
      rip_start = strtoul(argv[++i], NULL, 0);
      has_rip_start = 1;
    } else if (!strcmp(argv[i], "--rip-end") && i + 1 < argc) {
      rip_end = strtoul(argv[++i], NULL, 0);
      has_rip_end = 1;
    } else if (!strcmp(argv[i], "--no-log"))
      log_pc = 0;
    else if (!strcmp(argv[i], "--off"))
      off = 1;
    else {
      usage(argv[0]);
      return 1;
    }
  }

  if (off) {
    disarm();
    return 0;
  }

  if ((enclave_id < 0 && !any_enclave) || has_rip_start != has_rip_end || (has_rip_start && !has_pa)) {
    usage(argv[0]);
    return 1;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  long driver_enclave_id = any_enclave ? -2 : enclave_id + ENCLAVE_IDR_BASE;

  int log_fd = -1;
  if (log_pc) {
    log_fd = open(LOG_DEV, O_RDONLY | O_NONBLOCK);
    if (log_fd < 0) {
      perror(LOG_DEV);
      return 1;
    }
  }

  if ((delta_ticks >= 0 && write_param("strike_delta_ticks", delta_ticks)) ||
      write_param("strike_target_pa", has_pa ? (long)pa : 0) ||
      write_param("strike_rip_start", (long)rip_start) ||
      write_param("strike_rip_end", (long)rip_end) ||
      write_param("log_pc", log_pc) ||
      write_param("strike_target_eid", driver_enclave_id)) {
    disarm();
    return 1;
  }

  fprintf(stderr,
          "keystone-step: armed eid=%ld (driver ueid=%ld) delta=%ld pa=0x%lx rip=[0x%lx,0x%lx] log_pc=%d\n",
          enclave_id, driver_enclave_id, delta_ticks, pa, rip_start, rip_end, log_pc);
  fprintf(stderr, "keystone-step: waiting for the enclave to run; exits when it finishes, Ctrl-C to abort\n");

  setvbuf(stdout, NULL, _IOFBF, 1 << 20);

  int rc = 0;
  while (!stop) {
    if (log_fd < 0) {
      pause();
      continue;
    }
    int r = drain_log(log_fd, 200);
    if (r < 0) {
      perror("keystone-step: log");
      rc = 1;
      break;
    }
    if (r > 0) {
      fprintf(stderr, "keystone-step: enclave finished\n");
      break;
    }
  }

  disarm();

  if (log_fd >= 0) {
    drain_log(log_fd, 0);
    unsigned long dropped = 0;
    if (!read_param("log_dropped", &dropped) && dropped)
      fprintf(stderr, "keystone-step: %lu log lines dropped (log buffer overflow)\n", dropped);
    close(log_fd);
  }
  return rc;
}
