#include "app/eapp_utils.h"
#include "app/syscall.h"
#include "edge/edge_common.h"

#include <stdint.h>
#include <stddef.h>

#define OCALL_PRINT_BUFFER 1
#define OCALL_PRINT_VALUE 2

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

static inline void fence(void)
{
  asm volatile("fence rw, rw" ::: "memory");
}

int main(void)
{
  volatile int dummy = 0;
  ocall_print_value((uintptr_t)&dummy);
  while (1)
  {
    fence();
    dummy++;
    ocall_print_value(dummy);
    fence();
  }

  EAPP_RETURN(0);
}
