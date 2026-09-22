/* Interactive single-stepper for the keystone driver's step_mode gate.
 *
 * step_mode pauses the enclave's own natural run/resume calls (issued by
 * its own host loader) at every timer-forced interruption, and blocks
 * inside the kernel until this tool releases one token. There is no
 * separate ioctl call racing the enclave's loader -- this tool only ever
 * writes tokens; the enclave's own process is what's actually blocked.
 *
 * At each pause you choose:
 *   Enter      -- just continue (step_go)
 *   i + Enter  -- invalidate strike_target_pa (drop dirty data), then
 *                 continue (ipa)
 *   c + Enter  -- clean+invalidate strike_target_pa (write back current
 *                 value first, then invalidate), then continue
 *                 (cipa_go) -- use this to pin down which value becomes
 *                 visible instead of reverting to the stale one
 *   q + Enter  -- quit: releases whatever's currently blocked and resets
 *                 strike_target_eid to -1, so nothing keeps getting
 *                 interrupted after you leave. Same cleanup runs on
 *                 Ctrl-C.
 *
 * Usage:
 *   ./keystone-step --eid <eid> [--delta <ticks>] [--pa <pa>]
 *
 *   --eid <eid>     driver-side enclave id (from dmesg -- see
 *                   "created enclave, driver eid = N" in keystone-ioctl.c)
 *   --delta <ticks> timer ticks per step (optional -- leaves whatever's
 *                   currently set if omitted)
 *   --pa <pa>       physical address for the i/c gates (optional; hex
 *                   0x... or decimal)
 *
 * You can launch this *before* the target enclave exists: it sets
 * strike_target_eid first, then its own first step_go write is exactly
 * what releases that target's very first pause the moment it calls
 * RUN_ENCLAVE. Until the target shows up, that first write times out
 * (5s, from the kernel side) and this tool just retries it, printing a
 * wait message each time -- so start this first, then launch the .ke in
 * another terminal.
 *
 * Each step's result (error/value) is read back directly from sysfs
 * (last_error/last_pc) right after the write that caused it -- no dmesg
 * scraping needed, though the driver can still mirror it to dmesg too
 * if you turn that on:
 *   echo 1 > /sys/module/keystone_driver/parameters/step_log_dmesg
 *
 * Build:  <riscv64 cross-gcc> -o keystone-step step.c
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the driver's idr allocates real ueids starting at this offset
 * (ENCLAVE_IDR_MIN in keystone-enclave.c) -- so a user-facing --eid of 0
 * is actually driver ueid ENCLAVE_IDR_BASE, and so on. */
#define ENCLAVE_IDR_BASE 0x1000

#define PARAM_DIR        "/sys/module/keystone_driver/parameters/"
#define STEP_GO_PATH     PARAM_DIR "step_go"
#define IPA_PATH         PARAM_DIR "ipa"
#define CIPA_GO_PATH     PARAM_DIR "cipa_go"
#define LAST_PC_PATH     PARAM_DIR "last_pc"
#define LAST_ERROR_PATH  PARAM_DIR "last_error"

#define SBI_ERR_SM_ENCLAVE_SUCCESS          0
#define SBI_ERR_SM_ENCLAVE_UNKNOWN_ERROR    100000
#define SBI_ERR_SM_ENCLAVE_INVALID_ID       100001
#define SBI_ERR_SM_ENCLAVE_INTERRUPTED      100002
#define SBI_ERR_SM_ENCLAVE_PMP_FAILURE      100003
#define SBI_ERR_SM_ENCLAVE_NOT_RUNNABLE     100004
#define SBI_ERR_SM_ENCLAVE_NOT_DESTROYABLE  100005
#define SBI_ERR_SM_ENCLAVE_REGION_OVERLAPS  100006
#define SBI_ERR_SM_ENCLAVE_NOT_ACCESSIBLE   100007
#define SBI_ERR_SM_ENCLAVE_ILLEGAL_ARGUMENT 100008
#define SBI_ERR_SM_ENCLAVE_NOT_RUNNING      100009
#define SBI_ERR_SM_ENCLAVE_NOT_RESUMABLE    100010
#define SBI_ERR_SM_ENCLAVE_EDGE_CALL_HOST   100011
#define SBI_ERR_SM_ENCLAVE_NOT_INITIALIZED  100012
#define SBI_ERR_SM_ENCLAVE_NO_FREE_RESOURCE 100013
#define SBI_ERR_SM_ENCLAVE_SBI_PROHIBITED   100014
#define SBI_ERR_SM_ENCLAVE_ILLEGAL_PTE      100015
#define SBI_ERR_SM_ENCLAVE_NOT_FRESH        100016

static const char *
error_name(long err)
{
  switch (err) {
  case SBI_ERR_SM_ENCLAVE_SUCCESS:          return "SUCCESS";
  case SBI_ERR_SM_ENCLAVE_UNKNOWN_ERROR:    return "UNKNOWN_ERROR";
  case SBI_ERR_SM_ENCLAVE_INVALID_ID:       return "INVALID_ID";
  case SBI_ERR_SM_ENCLAVE_INTERRUPTED:      return "INTERRUPTED";
  case SBI_ERR_SM_ENCLAVE_PMP_FAILURE:      return "PMP_FAILURE";
  case SBI_ERR_SM_ENCLAVE_NOT_RUNNABLE:     return "NOT_RUNNABLE";
  case SBI_ERR_SM_ENCLAVE_NOT_DESTROYABLE:  return "NOT_DESTROYABLE";
  case SBI_ERR_SM_ENCLAVE_REGION_OVERLAPS:  return "REGION_OVERLAPS";
  case SBI_ERR_SM_ENCLAVE_NOT_ACCESSIBLE:   return "NOT_ACCESSIBLE";
  case SBI_ERR_SM_ENCLAVE_ILLEGAL_ARGUMENT: return "ILLEGAL_ARGUMENT";
  case SBI_ERR_SM_ENCLAVE_NOT_RUNNING:      return "NOT_RUNNING";
  case SBI_ERR_SM_ENCLAVE_NOT_RESUMABLE:    return "NOT_RESUMABLE";
  case SBI_ERR_SM_ENCLAVE_EDGE_CALL_HOST:   return "EDGE_CALL_HOST";
  case SBI_ERR_SM_ENCLAVE_NOT_INITIALIZED:  return "NOT_INITIALIZED";
  case SBI_ERR_SM_ENCLAVE_NO_FREE_RESOURCE: return "NO_FREE_RESOURCE";
  case SBI_ERR_SM_ENCLAVE_SBI_PROHIBITED:   return "SBI_PROHIBITED";
  case SBI_ERR_SM_ENCLAVE_ILLEGAL_PTE:      return "ILLEGAL_PTE";
  case SBI_ERR_SM_ENCLAVE_NOT_FRESH:        return "NOT_FRESH";
  default:                                  return "?";
  }
}

static int
write_param(const char *path, unsigned long value)
{
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    return -1;
  }
  int rc = fprintf(f, "%lu\n", value) < 0 ? -1 : 0;
  fclose(f);
  return rc;
}

static long
read_param(const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f) {
    perror(path);
    return -1;
  }
  long v = -1;
  int n = fscanf(f, "%ld", &v);
  fclose(f);
  return n == 1 ? v : -1;
}

/* returns 0 on success, -1 on real error, 1 on timeout (retryable --
 * usually means the target hasn't reached its first RUN_ENCLAVE yet).
 * Uses a raw fd + write(2), not stdio, so errno from the failing write
 * is read directly -- no risk of fclose() or another libc call
 * clobbering it first. */
static int
write_gate(const char *path)
{
  int fd = open(path, O_WRONLY);
  if (fd < 0) {
    perror(path);
    fprintf(stderr, "(is the keystone-driver module loaded? are you root?)\n");
    return -1;
  }

  int rc = 0;
  if (write(fd, "1\n", 2) != 2)
    rc = (errno == ETIMEDOUT) ? 1 : -1;

  close(fd);
  return rc;
}

static void
usage(const char *argv0)
{
  fprintf(stderr, "usage: %s --eid <eid> [--delta <ticks>] [--pa <pa>]\n", argv0);
}

/* Called on both 'q' and Ctrl-C. Raw open/write/close (not stdio) so
 * it's safe to call from the signal handler too. Two steps:
 *   1. release whatever's currently blocked in step_wait_for_token()
 *      (best-effort -- if nothing's waiting, this just times out after
 *      5s with no harm done)
 *   2. reset strike_target_eid to -1, which the driver now re-checks
 *      live on every loop iteration (not just once at entry), so this
 *      takes effect immediately even for that same in-flight call --
 *      nothing keeps getting interrupted after this returns.
 */
static void
release_and_reset(void)
{
  int fd = open(STEP_GO_PATH, O_WRONLY);
  if (fd >= 0) {
    write(fd, "1\n", 2);
    close(fd);
  }

  fd = open(PARAM_DIR "strike_target_eid", O_WRONLY);
  if (fd >= 0) {
    write(fd, "-1\n", 3);
    close(fd);
  }
}

static void
sigint_handler(int sig)
{
  (void)sig;
  release_and_reset();
  _exit(1);
}

int
main(int argc, char **argv)
{
  long eid = -1, delta = -1;
  unsigned long pa = 0;
  int pa_set = 0;

  /* force line buffering regardless of whether stdout is a real tty --
   * without this, output can sit in libc's buffer while we're blocked
   * in write_gate()/fgets(), making it look like nothing happened. */
  setvbuf(stdout, NULL, _IOLBF, 0);

  signal(SIGINT, sigint_handler);

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--eid") && i + 1 < argc)
      eid = strtol(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--delta") && i + 1 < argc)
      delta = strtol(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--pa") && i + 1 < argc) {
      /* strtoul, not strtol -- physical addresses on this platform can
       * have bit 63 set (e.g. 0xc0000001...), which overflows a signed
       * long and silently clamps to LONG_MAX under strtol. */
      pa = strtoul(argv[++i], NULL, 0);
      pa_set = 1;
    }
    else {
      usage(argv[0]);
      return 1;
    }
  }

  if (eid < 0) {
    usage(argv[0]);
    return 1;
  }

  eid += ENCLAVE_IDR_BASE;

  printf("keystone-step: targeting eid=%ld (driver ueid=%ld)\n", eid - ENCLAVE_IDR_BASE, eid);
  if (write_param(PARAM_DIR "strike_target_eid", (unsigned long)eid))
    return 1;
  if (delta >= 0 && write_param(PARAM_DIR "strike_delta_ticks", (unsigned long)delta))
    return 1;
  if (pa_set && write_param(PARAM_DIR "strike_target_pa", pa))
    return 1;
  if (write_param(PARAM_DIR "step_mode", 1))
    return 1;

  printf("keystone-step:\n"
         "  Enter      -- continue\n"
         "  i + Enter  -- invalidate, then continue\n"
         "  c + Enter  -- clean+invalidate, then continue\n"
         "  q + Enter  -- quit (enclave stays paused)\n\n");

  printf("keystone-step: waiting for eid=%ld to reach its first pause...\n", eid);

  /* catch the first pause automatically -- don't wait on stdin for this
   * one, since there's nothing to choose yet (no i/c/q makes sense
   * before anything has actually paused). */
  {
    int rc;
    do {
      rc = write_gate(STEP_GO_PATH);
      if (rc > 0)
        printf("keystone-step: still waiting for eid=%ld...\n", eid);
    } while (rc > 0);

    if (rc < 0)
      return 1;

    long err = read_param(LAST_ERROR_PATH);
    printf("keystone-step: parked at first step -- error=%ld (%s)\n",
           err, error_name(err));
    fflush(stdout);
  }

  char line[64];
  unsigned long step_count = 1;

  while (fgets(line, sizeof(line), stdin)) {
    if (line[0] == 'q')
      break;

    const char *path = STEP_GO_PATH;
    const char *action = "continue";
    if (line[0] == 'i') {
      path = IPA_PATH;
      action = "invalidated";
    } else if (line[0] == 'c') {
      path = CIPA_GO_PATH;
      action = "wrote back + invalidated";
    }

    int rc;
    do {
      rc = write_gate(path);
      if (rc > 0)
        printf("keystone-step: still waiting for eid=%ld...\n", eid);
    } while (rc > 0);

    if (rc < 0)
      break;

    step_count++;
    long err = read_param(LAST_ERROR_PATH);

    printf("[step %5lu] %-25s error=%ld (%s)\n",
           step_count, action, err, error_name(err));
    fflush(stdout);
  }

  return 0;
}
