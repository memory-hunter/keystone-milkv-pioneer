//******************************************************************************
// rdcycle-test eapp  (WRAPPER: cycle count via clock_gettime)
//
// Reads the cycle counter indirectly through clock_gettime(): the Eyrie
// runtime services it in S-mode (see runtime/call/linux_wrap.c:
// linux_clock_gettime, which does rdcycle and returns cycles as
// { tv_sec = cycles/1e9, tv_nsec = cycles%1e9 }). Recombined here.
//
// Requires the "linux_syscall" eyrie plugin (see CMakeLists.txt).
// See examples/rdcycle-direct for the U-mode direct-rdcycle variant.
//------------------------------------------------------------------------------
#include "app/eapp_utils.h"
#include "app/syscall.h"
#include "edge/edge_common.h"

#include <stdint.h>
#include <stddef.h>

#define OCALL_PRINT_BUFFER 1
#define OCALL_PRINT_VALUE 2

/* riscv64 Linux syscall number, matches runtime SYS_clock_gettime */
#define SYS_clock_gettime 113
#define CLOCK_MONOTONIC 1
#define CLOCK_FREQ 1000000000UL

#ifndef ITERS
#define ITERS 50000000UL
#endif

struct timespec_t {
  unsigned long tv_sec;
  unsigned long tv_nsec;
};

static unsigned long
ocall_print_buffer(const char* str, size_t len) {
  unsigned long ret;
  ocall(OCALL_PRINT_BUFFER, (void*)str, len, &ret, sizeof(unsigned long));
  return ret;
}

static void
ocall_print_value(unsigned long val) {
  ocall(OCALL_PRINT_VALUE, &val, sizeof(unsigned long), 0, 0);
}

#define PRINT_STR(s) ocall_print_buffer((s), sizeof(s))

/* cycle count via the runtime's S-mode rdcycle inside clock_gettime */
static uint64_t
read_cycle_via_runtime(void) {
  struct timespec_t ts = {0, 0};
  SYSCALL_2(SYS_clock_gettime, CLOCK_MONOTONIC, (uintptr_t)&ts);
  return (uint64_t)ts.tv_sec * CLOCK_FREQ + (uint64_t)ts.tv_nsec;
}

int
main(void) {
  PRINT_STR("rdcycle-test: start (via clock_gettime / S-mode rdcycle)\n");

  uint64_t before = read_cycle_via_runtime();

  volatile uint64_t acc = 0;
  PRINT_STR("Running loop\n");
  for (uint64_t i = 0; i < ITERS; i++) acc += i;

  uint64_t after = read_cycle_via_runtime();

  PRINT_STR("Before (cycles):\n");
  ocall_print_value((unsigned long)before);
  PRINT_STR("After (cycles):\n");
  ocall_print_value((unsigned long)after);
  PRINT_STR("Delta (cycles):\n");
  ocall_print_value((unsigned long)(after - before));

  EAPP_RETURN(0);
}
