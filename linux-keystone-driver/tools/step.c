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
 *                 continue (strike_go)
 *   c + Enter  -- clean+invalidate strike_target_pa (write back current
 *                 value first, then invalidate), then continue
 *                 (cipa_go) -- use this to pin down which value becomes
 *                 visible instead of reverting to the stale one
 *   q + Enter  -- quit this tool (the enclave stays paused, still
 *                 blocked in the kernel, until something releases it)
 *
 * Setup (once, via sysfs -- step_mode defaults to on):
 *   echo <eid> > /sys/module/keystone_driver/parameters/strike_target_eid
 *   echo <pa>  > /sys/module/keystone_driver/parameters/strike_target_pa
 *   echo 1     > /sys/module/keystone_driver/parameters/strike_delta_ticks
 *   echo 1     > /sys/module/keystone_driver/parameters/step_mode   # if not already on
 *
 * Then launch the target enclave's own host loader/.ke normally -- it
 * will block waiting for the first token the moment it calls
 * RUN_ENCLAVE.
 *
 * Build:  <riscv64 cross-gcc> -o keystone-step step.c
 *
 * Each step's result (eid, stop reason, value) is logged by the driver
 * via pr_info -- watch it with `dmesg -w | grep keystone-step` in
 * another terminal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STEP_GO_PATH   "/sys/module/keystone_driver/parameters/step_go"
#define STRIKE_GO_PATH "/sys/module/keystone_driver/parameters/strike_go"
#define CIPA_GO_PATH   "/sys/module/keystone_driver/parameters/cipa_go"

static int
write_gate(const char *path)
{
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    fprintf(stderr, "(is the keystone-driver module loaded? are you root?)\n");
    return -1;
  }
  /* the value written doesn't matter -- the callback just counts writes */
  int rc = fputs("1\n", f) == EOF ? -1 : 0;
  fclose(f);
  return rc;
}

int
main(void)
{
  printf("keystone-step:\n"
         "  Enter      -- continue\n"
         "  i + Enter  -- invalidate, then continue\n"
         "  c + Enter  -- clean+invalidate (writeback first), then continue\n"
         "  q + Enter  -- quit (enclave stays paused)\n"
         "(watch step results with: dmesg -w | grep keystone-step)\n\n");

  char line[64];
  unsigned long step_count = 0;
  while (fgets(line, sizeof(line), stdin)) {
    if (line[0] == 'q')
      break;

    const char *path = STEP_GO_PATH;
    const char *label = "continue";
    if (line[0] == 'i') {
      path = STRIKE_GO_PATH;
      label = "invalidate + continue";
    } else if (line[0] == 'c') {
      path = CIPA_GO_PATH;
      label = "clean+invalidate + continue";
    }

    if (write_gate(path))
      break;

    step_count++;
    printf("[step %5lu] %s\n", step_count, label);
  }

  return 0;
}
