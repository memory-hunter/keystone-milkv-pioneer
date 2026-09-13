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
ocall_print_buffer(const char *str, size_t len)
{
  unsigned long ret;
  ocall(OCALL_PRINT_BUFFER, (void *)str, len, &ret, sizeof(unsigned long));
  return ret;
}

static void
ocall_print_value(unsigned long val)
{
  ocall(OCALL_PRINT_VALUE, &val, sizeof(unsigned long), 0, 0);
}

#define PRINT_STR(s) ocall_print_buffer((s), sizeof(s))

static inline uint64_t
rdcycle(void)
{
  unsigned long v;
  asm volatile("rdcycle %0" : "=r"(v));
  return (uint64_t)v;
}

static inline void flush(void *addr)
{
  asm volatile(
      "xor a7, a7, a7\n"
      "add a7, a7, %0\n"
      ".long 0x278800b\n"
      :
      : "r"(addr)
      : "a7", "memory");
}

static inline void fence(void)
{
  asm volatile("fence rw, rw" ::: "memory");
}

static inline __attribute__((always_inline)) void maccess(void *addr)
{
  asm volatile("ld a7, (%0)" : : "r"(addr) : "a7", "memory");
}

#define CACHE_LINE 64
#define N_TRIALS 1000
typedef struct
{
  volatile int val;
  char _pad[CACHE_LINE - sizeof(int)];
} __attribute__((aligned(CACHE_LINE))) cache_line_int_t;

static cache_line_int_t target_line;
#define target (target_line.val)

static const uint8_t _dummy = 0;

int main(void)
{
  PRINT_STR("rdcycle-direct: start (direct U-mode rdcycle)\n");

  // uint64_t before = rdcycle();

  // volatile uint64_t acc = 0;
  // PRINT_STR("Running loop\n");
  // for (uint64_t i = 0; i < ITERS; i++)
  //   acc += i;

  // uint64_t after = rdcycle();

  // PRINT_STR("Before (cycles):\n");
  // ocall_print_value((unsigned long)before);
  // PRINT_STR("After (cycles):\n");
  // ocall_print_value((unsigned long)after);
  // PRINT_STR("Delta (cycles):\n");
  // ocall_print_value((unsigned long)(after - before));

  unsigned long hit_sum = 0, miss_sum = 0;
  unsigned long t0, t1;

  volatile int dummy = 0;

  for (int i = 0; i < N_TRIALS; i++)
  {
    fence();
    t0 = rdcycle();
    maccess((void*)&dummy);
    t1 = rdcycle();
    hit_sum += (t1 - t0);
  }

  for (int i = 0; i < N_TRIALS; i++)
  {
    fence();
    flush((void*)&dummy);
    fence();
    t0 = rdcycle();
    maccess((void*)&dummy);
    t1 = rdcycle();
    miss_sum += (t1 - t0);
  }

  unsigned long avg_hit = hit_sum / N_TRIALS;
  unsigned long avg_miss = miss_sum / N_TRIALS;

  PRINT_STR("avg cycles HIT  (in-cache)    = ");
  ocall_print_value((int)avg_hit);
  PRINT_STR("avg cycles MISS (after flush) = ");
  ocall_print_value((int)avg_miss);

  if (avg_miss > avg_hit * 2)
    PRINT_STR("RESULT: enclave pages ARE cached -- flush+reload signal visible");
  else
    PRINT_STR("RESULT: enclave pages NOT cached -- no timing difference (C=0 in PTE)");

  EAPP_RETURN(0);
}
