#ifndef KEYSTONE_STEP_REC_H
#define KEYSTONE_STEP_REC_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define STEP_REC_STEP 0
#define STEP_REC_START 1
#define STEP_REC_END 2

struct step_rec {
  u64 val;
  u32 kind;
  u32 err;
};

#endif
