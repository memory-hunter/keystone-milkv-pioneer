//******************************************************************************
// rdcycle-direct eapp  (DIRECT U-mode rdcycle)
//
// Reads the cycle counter directly from U-mode with `rdcycle`, loops, reads
// again, reports before/after/delta.
//
// Only works if the SM enables the counter gates for the enclave:
//   mcounteren.CY + scounteren.CY (read permission) and mcountinhibit.CY=0
//   (counter running) -- see sm/src/platform/generic/platform.c
//     platform_switch_to_enclave().
// Without that firmware fix a U-mode rdcycle traps illegal-instruction
// (scause 0x2) and/or reads a frozen value.
//
// See examples/rdcycle-test for the clock_gettime (S-mode) wrapper variant.
//------------------------------------------------------------------------------
#include "app/eapp_utils.h"
#include "app/syscall.h"
#include "edge/edge_common.h"

#include <stdint.h>
#include <stddef.h>

#define OCALL_PRINT_BUFFER 1
#define OCALL_PRINT_VALUE 2

#ifndef ITERS
#define ITERS 50000000UL
#endif

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

static inline uint64_t
read_cycle(void) {
  unsigned long v;
  asm volatile("rdcycle %0" : "=r"(v));
  return (uint64_t)v;
}

int
main(void) {
  PRINT_STR("rdcycle-direct: start (direct U-mode rdcycle)\n");

  uint64_t before = read_cycle();

  volatile uint64_t acc = 0;
  PRINT_STR("Running loop\n");
  for (uint64_t i = 0; i < ITERS; i++) acc += i;

  uint64_t after = read_cycle();

  PRINT_STR("Before (cycles):\n");
  ocall_print_value((unsigned long)before);
  PRINT_STR("After (cycles):\n");
  ocall_print_value((unsigned long)after);
  PRINT_STR("Delta (cycles):\n");
  ocall_print_value((unsigned long)(after - before));

  EAPP_RETURN(0);
}
