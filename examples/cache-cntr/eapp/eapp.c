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

static inline void fence(void)
{
  asm volatile("fence rw, rw" ::: "memory");
}

#define INC10 asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
#define INC100 INC10 INC10 INC10 INC10 INC10 INC10 INC10 INC10 INC10 INC10
#define INC1K INC100 INC100 INC100 INC100 INC100 INC100 INC100 INC100 INC100 INC100
#define INC10K INC1K INC1K INC1K INC1K INC1K INC1K INC1K INC1K INC1K INC1K
#define INC100K INC10K INC10K INC10K INC10K INC10K INC10K INC10K INC10K INC10K
#define PADDING INC10K;\
    INC1K;\
    INC1K;\
    INC1K;\
    INC1K;\
    INC1K;\
    INC1K;\
    INC100;\
    INC100;\
    INC100;\
    INC10;\
    INC10;\
    asm volatile("nop; nop; nop;"); //0x10000 nops exactly

int main(void)
{
  volatile int dummy = 0;
  // ocall_print_value((uintptr_t)&dummy);
  fence();
  INC100K;
  INC100K;
  INC100K;
  INC100K;
  INC100K;
  INC100K;
  INC100K;
  INC100K;
  INC100K;
  INC100K;
  INC100K;
  // PADDING;
  // PADDING;
  // PADDING;
  // PADDING;
  // PADDING;
  // PADDING;
  // PADDING;
  // PADDING;
  // while (1)
  // {
  //   INC100K;
  //   INC100K;
  // }
  fence();
  // ocall_print_value(dummy);
  EAPP_RETURN(0);
}
