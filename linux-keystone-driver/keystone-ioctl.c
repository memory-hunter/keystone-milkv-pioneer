//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "keystone-sbi.h"
#include "keystone.h"
#include "keystone_user.h"
#include <asm/sbi.h>
#include <linux/string.h>
#include <linux/compiler.h>
#include <linux/build_bug.h>
#include <linux/irqflags.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/jiffies.h>


#define SBI_CALL(___ext, ___which, ___arg0, ___arg1, ___arg2)    \
  ({                                                             \
    register uintptr_t a0 __asm__("a0") = (uintptr_t)(___arg0);  \
    register uintptr_t a1 __asm__("a1") = (uintptr_t)(___arg1);  \
    register uintptr_t a2 __asm__("a2") = (uintptr_t)(___arg2);  \
    register uintptr_t a6 __asm__("a6") = (uintptr_t)(___which); \
    register uintptr_t a7 __asm__("a7") = (uintptr_t)(___ext);   \
    __asm__ volatile("ecall"                                     \
                     : "+r"(a0)                                  \
                     : "r"(a1), "r"(a2), "r"(a6), "r"(a7)        \
                     : "memory");                                \
    a0;                                                          \
  })

  #define SBI_CALL_1(___ext, ___which, ___arg0) SBI_CALL(___ext, ___which, ___arg0, 0, 0)

struct edge_return
{
  unsigned long call_status;
  size_t call_ret_offset;
  size_t call_ret_size;
};
struct edge_call
{
  unsigned long call_id;
  size_t call_arg_offset;
  size_t call_arg_size;
  struct edge_return return_data;
};

#include <linux/uaccess.h>
#include <linux/types.h>

#define C920_L1D_WAYS 2U
#define C920_L1D_SETS 512U
#define C920_L1D_LINE_SIZE 64U

#define C920_L1D_W 1
#define C920_L1D_S 9
#define C920_L1D_L 6

#define C920_L1D_WAY_SHIFT (32 - C920_L1D_W)
#define C920_L1D_SET_SHIFT C920_L1D_L
#define C920_L1D_SET_MASK ((1UL << C920_L1D_S) - 1)

#define TH_L1D_CALL asm volatile(".long 0x10000b" ::: "memory")
#define TH_L1D_IALL asm volatile(".long 0x20000b" ::: "memory")
#define TH_L1D_CIALL asm volatile(".long 0x30000b" ::: "memory")
#define TH_L2_CALL asm volatile(".long 0x150000b" ::: "memory")
#define TH_L2_IALL asm volatile(".long 0x160000b" ::: "memory")
#define TH_L2_CIALL asm volatile(".long 0x170000b" ::: "memory")
#define TH_SYNC_S asm volatile(".long 0x190000b" ::: "memory")

#define TH_L1D_ISW_A0 asm volatile(".long 0x225000b" ::"r"(rs1) : "memory")
#define TH_L1D_IVA_A0 asm volatile(".long 0x265000b" ::"r"(rs1) : "memory")
#define TH_L1D_CIVA_A0 asm volatile(".long 0x275000b" ::"r"(rs1) : "memory")
#define TH_L1D_IPA_A0 asm volatile(".long 0x2a5000b" ::"r"(rs1) : "memory")
#define TH_L1D_CIPA_A0 asm volatile(".long 0x2b5000b" ::"r"(rs1) : "memory")

#define TH_FLUSH_ALL \
  TH_L1D_CIALL;      \
  TH_SYNC_S;         \
  TH_L2_CIALL;       \
  TH_SYNC_S;

static inline void th_l1d_ipa(unsigned long pa)
{
  register unsigned long rs1 asm("a0") = pa;
  TH_SYNC_S;
  TH_L1D_IPA_A0;
  TH_SYNC_S;
}

static inline void th_l1d_cipa(unsigned long pa)
{
  register unsigned long rs1 asm("a0") = pa;
  TH_SYNC_S;
  TH_L1D_CIPA_A0;
  TH_SYNC_S;
}

#include "sm_err.h"
#include <linux/moduleparam.h>

/* --- targeted-strike state -------------------------------------------- */
/* which enclave to strike, and the physical address of its target line.
 * set these however you determine the victim (e.g. from CREATE_ENCLAVE,
 * or hardcoded during experimentation). eid < 0 means "disabled". */
static long strike_target_eid = -1;
static unsigned long strike_target_pa = 0;
static uint64_t strike_delta_ticks = 1; /* timebase ticks, not core cycles */

/* live-settable without reloading the module:
 *   echo <ticks> > /sys/module/keystone_driver/parameters/strike_delta_ticks
 *   echo <eid>   > /sys/module/keystone_driver/parameters/strike_target_eid
 *   echo <pa>    > /sys/module/keystone_driver/parameters/strike_target_pa
 * all three are read fresh from the global on each call, so a write
 * takes effect on the very next ioctl/loop iteration. eid = -1 disables
 * striking. strike_target_pa accepts hex (0x...) or decimal. */
module_param(strike_delta_ticks, ullong, 0644);
module_param(strike_target_pa, ulong, 0644);
/* strike_target_eid itself is registered further down, after
 * step_tokens exists -- writing a new target also resets the token
 * counter, see strike_target_eid_set(). */

/* --- interactive single-step gate --------------------------------------
 * When step_mode is on, every SBI call keystone_strike_drive() makes for
 * the target enclave blocks until an external controller releases one
 * token -- instead of a separate process racing a duplicate ioctl call
 * against the enclave's own natural run/resume calls, we gate the call
 * the enclave's own host loader was always going to make anyway. Only
 * one thing ever drives the SBI calls, so there is no race.
 *
 * release one step from userspace:
 *   echo 1 > /sys/module/keystone_driver/parameters/step_go
 * watch results:
 *   dmesg -w | grep keystone-step
 */
static int step_mode = 1; /* pause-by-default: on until told to turn off */
module_param(step_mode, int, 0644);

static atomic_t step_tokens = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(step_waitq);

/* Retargeting always clears any leftover tokens from a previous session
 * -- without this, tokens banked while nothing was consuming them (e.g.
 * against the wrong eid, or before the target launched) pile up and get
 * silently burned the moment a real match finally hits the gate, well
 * before your next deliberate release. */
static int strike_target_eid_set(const char *val, const struct kernel_param *kp)
{
  int rc = param_ops_long.set(val, kp);
  if (rc)
    return rc;
  atomic_set(&step_tokens, 0);
  return 0;
}
static int strike_target_eid_get(char *buf, const struct kernel_param *kp)
{
  return param_ops_long.get(buf, kp);
}
static const struct kernel_param_ops strike_target_eid_ops = {
  .set = strike_target_eid_set,
  .get = strike_target_eid_get,
};
module_param_cb(strike_target_eid, &strike_target_eid_ops, &strike_target_eid, 0644);

/* Signaled once per log_step() call (defined further down) -- makes each
 * *_go sysfs write synchronous: the write() doesn't return to userspace
 * until the step it requested has actually happened and
 * last_pc/last_error are fresh. This is what lets keystone-step
 * distinguish "a real step happened" from "nothing's listening yet" --
 * without it, a write just banks a token with no way to know whether
 * anything consumed it. */
static DECLARE_COMPLETION(step_done);

/* shared by step_go/ipa/cipa_go: release one token and block the
 * writer until the corresponding log_step() call completes. Timed out
 * (not infinite) so writing against a target that isn't actually paused
 * waiting (wrong eid, target not launched yet, step_mode off, ...) fails
 * with -ETIMEDOUT instead of hanging the calling process forever --
 * keystone-step turns that into "still waiting for eid=...", retrying. */
static int release_token_and_wait_step(void)
{
  long ret;

  atomic_inc(&step_tokens);
  wake_up_interruptible(&step_waitq);

  ret = wait_for_completion_interruptible_timeout(
      &step_done, msecs_to_jiffies(5000));
  if (ret == 0)
    return -ETIMEDOUT;
  if (ret < 0)
    return (int)ret; /* -ERESTARTSYS from a signal */
  return 0;
}

static int step_go_set(const char *val, const struct kernel_param *kp)
{
  return release_token_and_wait_step();
}
static int step_go_get(char *buf, const struct kernel_param *kp)
{
  return sprintf(buf, "%d\n", atomic_read(&step_tokens));
}
static const struct kernel_param_ops step_go_ops = {
  .set = step_go_set,
  .get = step_go_get,
};
module_param_cb(step_go, &step_go_ops, NULL, 0644);

/* second gate: same token/wait mechanism as step_go, but strikes
 * strike_target_pa first -- lets userspace choose per-pause between
 * "just continue" (step_go) and "invalidate, then continue" (ipa). */
static int ipa_set(const char *val, const struct kernel_param *kp)
{
  th_l1d_ipa(strike_target_pa);
  return release_token_and_wait_step();
}
static int ipa_get(char *buf, const struct kernel_param *kp)
{
  return sprintf(buf, "%d\n", atomic_read(&step_tokens));
}
static const struct kernel_param_ops ipa_ops = {
  .set = ipa_set,
  .get = ipa_get,
};
module_param_cb(ipa, &ipa_ops, NULL, 0644);

/* third gate: same mechanism again, but uses th_l1d_cipa() (clean +
 * invalidate) instead of th_l1d_ipa() (bare invalidate) -- commits the
 * line's current value to L2/memory first, then invalidates it, instead
 * of dropping whatever's dirty. Use this to deliberately pin down which
 * value becomes visible, rather than reverting to the stale one. */
static int cipa_go_set(const char *val, const struct kernel_param *kp)
{
  th_l1d_cipa(strike_target_pa);
  return release_token_and_wait_step();
}
static int cipa_go_get(char *buf, const struct kernel_param *kp)
{
  return sprintf(buf, "%d\n", atomic_read(&step_tokens));
}
static const struct kernel_param_ops cipa_go_ops = {
  .set = cipa_go_set,
  .get = cipa_go_get,
};
module_param_cb(cipa_go, &cipa_go_ops, NULL, 0644);

static void step_wait_for_token(void)
{
  for (;;) {
    if (wait_event_interruptible(step_waitq, atomic_read(&step_tokens) > 0))
      return; /* interrupted -- don't hang a Ctrl-C'd process forever */
    if (atomic_dec_if_positive(&step_tokens) >= 0)
      return;
  }
}

/* last_seq/last_pc/last_error mirror the most recent log_step() call --
 * read-only sysfs so keystone-step can see the result of a step directly
 * instead of scraping dmesg text. */
static unsigned long step_seq = 0;
static unsigned long last_seq = 0;
static unsigned long last_pc = 0;
static unsigned long last_error = 0;
module_param(last_seq, ulong, 0444);
module_param(last_pc, ulong, 0444);
module_param(last_error, ulong, 0444);

/* OFF by default: at small strike_delta_ticks this fires on every
 * interrupt, potentially tens of thousands of times per second, and a
 * synchronous pr_info() at that rate can wedge the console/UART badly
 * enough to hang the whole machine. keystone-step doesn't need this --
 * it reads last_pc/last_error straight from sysfs. Turn it on only for
 * a low-rate/interactive run you actually want mirrored to dmesg:
 *   echo 1 > /sys/module/keystone_driver/parameters/step_log_dmesg */
static int step_log_dmesg = 0;
module_param(step_log_dmesg, int, 0644);

static inline void log_step(unsigned long eid, struct sbiret ret)
{
  last_seq = step_seq++;
  last_error = (unsigned long)ret.error;
  last_pc = (unsigned long)ret.value;

  if (step_log_dmesg)
    pr_info("keystone-step: seq=%lu eid=%lu error=%lu value=0x%lx\n",
            last_seq, eid, last_error, last_pc);

  complete(&step_done);
}

/* safety cap: max internal resumes per ioctl call, in case delta is
 * mis-tuned and every resume immediately re-triggers the timer */
#define STRIKE_MAX_ATTEMPTS 100000

/* Re-evaluated on every call rather than cached, specifically so that
 * resetting strike_target_eid from userspace (e.g. on quit) takes
 * effect immediately for an *in-flight* keystone_strike_drive() call,
 * not just future ones -- a cached is_target captured once at function
 * entry can't be un-targeted mid-loop no matter what you write to
 * sysfs afterward. */
static inline int strike_is_target(unsigned long ueid)
{
  return strike_target_eid >= 0 && ueid == strike_target_eid;
}

/* Enters via `entry` (sbi_sm_run_enclave for the very first call,
 * sbi_sm_resume_enclave for every call after) and, if targeted, arms the
 * timer before every single SBI call -- including the first -- then loops
 * internally on every stop caused by our own armed timer, striking each
 * time, until it hits a stop reason userspace actually understands (a
 * real ocall, or exit). Control never goes back to the ioctl caller in
 * between. If step_mode is on, each SBI call blocks on a token first.
 * ueid is the driver idr key (not the SM-internal eid), needed to
 * re-check targeting live -- see strike_is_target(). */
static struct sbiret keystone_strike_drive(unsigned long eid,
                                           unsigned long ueid,
                                           struct sbiret (*entry)(unsigned long))
{
  unsigned long flags;
  unsigned int attempts = 0;
  struct sbiret ret;
  int is_target = strike_is_target(ueid);

  if (is_target && step_mode)
    step_wait_for_token();

  if (is_target)
    SBI_CALL_1(SBI_SET_TIMER, 0, get_cycles64() + strike_delta_ticks);

  local_irq_save(flags);
  ret = entry(eid);
  local_irq_restore(flags);

  if (is_target && step_mode)
    log_step(eid, ret);

  while (strike_is_target(ueid) &&
         ret.error == SBI_ERR_SM_ENCLAVE_INTERRUPTED &&
         attempts++ < STRIKE_MAX_ATTEMPTS)
  {
    /* invalidation disabled -- pure single-stepping only for now */
    /* th_l1d_ipa(strike_target_pa); */

    if (step_mode)
      step_wait_for_token();

    SBI_CALL_1(SBI_SET_TIMER, 0, get_cycles64() + strike_delta_ticks);
    local_irq_save(flags);
    ret = sbi_sm_resume_enclave(eid);
    local_irq_restore(flags);

    if (step_mode)
      log_step(eid, ret);
  }

  return ret;
}

int __keystone_destroy_enclave(unsigned int ueid);
void __enclave_info(struct enclave *enclave);
void __enclave_info(struct enclave *enclave)
{
  printk("Enclave Information:\n");
  printk("EPM:\n"
         "\tRoot page table: [%llx]\n\tPaddr: [%llx]\n\tVaddr: [%llx]\n\tSize: "
         "[%llu]\n\tIs-cma: [%s]\n\tOrder: [%llu]\n",
         enclave->epm->root_page_table, (unsigned long long)enclave->epm->pa,
         (unsigned long long)enclave->epm->ptr, enclave->epm->size,
         enclave->epm->is_cma == 0 ? "false" : "true", enclave->epm->order);

  printk("UTM:\n"
         "\tRoot page table: [%llx]\n\tPTR: [%llx]\n\tSize: [%llu]",
         enclave->utm->root_page_table, (unsigned long long)enclave->utm->ptr,
         enclave->utm->size);
}

int keystone_create_enclave(struct file *filep, unsigned long arg)
{
  /* create parameters */
  struct keystone_ioctl_create_enclave *enclp =
      (struct keystone_ioctl_create_enclave *)arg;

  struct enclave *enclave;
  enclave = create_enclave(enclp->min_pages);

  if (enclave == NULL)
  {
    return -ENOMEM;
  }

  /* Pass base page table */
  enclp->epm_paddr = enclave->epm->pa;
  enclp->epm_size = enclave->epm->size;

  /* allocate UID */
  enclp->eid = enclave_idr_alloc(enclave);
  pr_info("keystone: created enclave, driver eid = %lu (use this with keystone-step)\n",
          (unsigned long)enclp->eid);

  filep->private_data = (void *)enclp->eid;

  return 0;
}

int keystone_finalize_enclave(unsigned long arg)
{
  struct sbiret ret;
  struct enclave *enclave;
  struct utm *utm;
  struct keystone_sbi_create_t create_args;

  struct keystone_ioctl_create_enclave *enclp =
      (struct keystone_ioctl_create_enclave *)arg;

  enclave = get_enclave_by_id(enclp->eid);
  if (!enclave)
  {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  enclave->is_init = false;

  /* SBI Call */
  create_args.epm_region.paddr = enclave->epm->pa;
  create_args.epm_region.size = enclave->epm->size;

  utm = enclave->utm;

  if (utm)
  {
    create_args.utm_region.paddr = __pa(utm->ptr);
    create_args.utm_region.size = utm->size;
  }
  else
  {
    create_args.utm_region.paddr = 0;
    create_args.utm_region.size = 0;
  }

  // physical addresses for runtime, user, and freemem
  create_args.runtime_paddr = enclp->runtime_paddr;
  create_args.user_paddr = enclp->user_paddr;
  create_args.free_paddr = enclp->free_paddr;
  create_args.free_requested = enclp->free_requested;

  ret = sbi_sm_create_enclave(&create_args);

  if (ret.error)
  {
    keystone_err(
        "keystone_create_enclave: SBI call failed with error code %ld\n",
        ret.error);
    goto error_destroy_enclave;
  }

  enclave->eid = ret.value;

  return 0;

error_destroy_enclave:
  /* This can handle partial initialization failure */
  destroy_enclave(enclave);

  return -EINVAL;
}

int keystone_run_enclave(unsigned long data)
{
  struct sbiret ret;
  unsigned long ueid;
  struct enclave *enclave;
  struct keystone_ioctl_run_enclave *arg =
      (struct keystone_ioctl_run_enclave *)data;

  ueid = arg->eid;
  enclave = get_enclave_by_id(ueid);

  __enclave_info(enclave);
  if (!enclave)
  {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  if (enclave->eid < 0)
  {
    keystone_err("real enclave does not exist\n");
    return -EINVAL;
  }

  pr_info("Enclave ID: %d", enclave->eid);

  enclave->has_run = 1;
  /* strike_target_eid is the driver idr key (ueid, printed as "driver
   * eid = N") -- NOT enclave->eid, the SM's own internal enclave id, a
   * completely separate number space. keystone_strike_drive() takes
   * ueid directly now and re-checks targeting live on every iteration
   * (see strike_is_target()), rather than a boolean cached once here. */
  ret = keystone_strike_drive(enclave->eid, ueid, sbi_sm_run_enclave);

  arg->error = ret.error;
  arg->value = ret.value;

  return 0;
}

int utm_init_ioctl(struct file *filp, unsigned long arg)
{
  int ret = 0;
  struct utm *utm;
  struct enclave *enclave;
  struct keystone_ioctl_create_enclave *enclp =
      (struct keystone_ioctl_create_enclave *)arg;
  long long unsigned untrusted_size = enclp->utm_size;

  enclave = get_enclave_by_id(enclp->eid);

  if (!enclave)
  {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  utm = kmalloc(sizeof(struct utm), GFP_KERNEL);
  if (!utm)
  {
    ret = -ENOMEM;
    return ret;
  }

  ret = utm_init(utm, untrusted_size);

  /* prepare for mmap */
  enclave->utm = utm;

  enclp->utm_paddr = __pa(utm->ptr);

  return ret;
}

int keystone_destroy_enclave(struct file *filep, unsigned long arg)
{
  int ret;
  struct keystone_ioctl_create_enclave *enclp =
      (struct keystone_ioctl_create_enclave *)arg;
  unsigned long ueid = enclp->eid;

  ret = __keystone_destroy_enclave(ueid);
  if (!ret)
  {
    filep->private_data = NULL;
  }
  return ret;
}

int __keystone_destroy_enclave(unsigned int ueid)
{
  struct sbiret ret;
  struct enclave *enclave;
  enclave = get_enclave_by_id(ueid);

  if (!enclave)
  {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  if (enclave->eid >= 0)
  {
    ret = sbi_sm_destroy_enclave(enclave->eid);
    if (ret.error)
    {
      keystone_err(
          "fatal: cannot destroy enclave: SBI failed with error code %ld\n",
          ret.error);
      return -EINVAL;
    }
  }
  else
  {
    keystone_warn(
        "keystone_destroy_enclave: skipping (enclave does not exist)\n");
  }

  destroy_enclave(enclave);
  enclave_idr_remove(ueid);

  return 0;
}

int keystone_resume_enclave(unsigned long data)
{
  struct sbiret ret;
  ret.error = 0;
  ret.value = 0;

  //---------------
  struct keystone_ioctl_run_enclave *arg =
      (struct keystone_ioctl_run_enclave *)data;
  unsigned long ueid = arg->eid;
  struct enclave *enclave;
  enclave = get_enclave_by_id(ueid);

  if (!enclave)
  {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  if (enclave->eid < 0)
  {
    keystone_err("real enclave does not exist\n");
    return -EINVAL;
  }

  struct edge_call *ec;
  unsigned long id;

  ec = (struct edge_call *)enclave->utm->ptr;
  id = ec->call_id;

  ret = keystone_strike_drive(enclave->eid, ueid, sbi_sm_resume_enclave);

  arg->error = ret.error;
  arg->value = ret.value;

  return 0;
}

/* single-step: exactly one strike_delta_ticks slice, then return
 * immediately to userspace regardless of why it stopped -- no internal
 * auto-loop like keystone_strike_drive(). Call this repeatedly (one call
 * per "step" input) to watch the enclave advance in delta-tick chunks. */
int keystone_single_step_enclave(unsigned long data)
{
  struct sbiret ret;
  struct keystone_ioctl_run_enclave *arg =
      (struct keystone_ioctl_run_enclave *)data;
  unsigned long ueid = arg->eid;
  struct enclave *enclave;
  unsigned long flags;

  /* wait for an enclave with this eid to actually exist and be fully
   * created on the SM side -- lets you launch the stepper *before* the
   * target starts and catch it the moment it does, instead of racing
   * to launch it after. just a poll loop, fine for experimentation;
   * Ctrl-C (or any signal) breaks out cleanly via signal_pending(). */
  while (!(enclave = get_enclave_by_id(ueid)) || enclave->eid < 0)
  {
    if (signal_pending(current))
      return -EINTR;
    msleep(20);
  }

  SBI_CALL_1(SBI_SET_TIMER, 0, get_cycles64() + strike_delta_ticks);

  local_irq_save(flags);
  if (!enclave->has_run) {
    ret = sbi_sm_run_enclave(enclave->eid);
    enclave->has_run = 1;
  } else {
    ret = sbi_sm_resume_enclave(enclave->eid);
  }
  local_irq_restore(flags);

  /* invalidation disabled -- pure single-stepping only for now */
  /* if (ret.error == SBI_ERR_SM_ENCLAVE_INTERRUPTED)
    th_l1d_ipa(strike_target_pa); */

  arg->error = ret.error;
  arg->value = ret.value;

  return 0;
}

long keystone_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
  long ret;
  char data[512];

  size_t ioc_size;

  if (!arg)
    return -EINVAL;

  ioc_size = _IOC_SIZE(cmd);
  ioc_size = ioc_size > sizeof(data) ? sizeof(data) : ioc_size;

  if (copy_from_user(data, (void __user *)arg, ioc_size))
    return -EFAULT;

  switch (cmd)
  {
  case KEYSTONE_IOC_CREATE_ENCLAVE:
    ret = keystone_create_enclave(filep, (unsigned long)data);
    break;
  case KEYSTONE_IOC_FINALIZE_ENCLAVE:
    ret = keystone_finalize_enclave((unsigned long)data);
    break;
  case KEYSTONE_IOC_DESTROY_ENCLAVE:
    ret = keystone_destroy_enclave(filep, (unsigned long)data);
    break;
  case KEYSTONE_IOC_RUN_ENCLAVE:
    ret = keystone_run_enclave((unsigned long)data);
    break;
  case KEYSTONE_IOC_RESUME_ENCLAVE:
    ret = keystone_resume_enclave((unsigned long)data);
    break;
  case KEYSTONE_IOC_SINGLE_STEP:
    ret = keystone_single_step_enclave((unsigned long)data);
    break;
  case KEYSTONE_IOC_UTM_INIT:
    ret = utm_init_ioctl(filep, (unsigned long)data);
    break;
  default:
    return -ENOSYS;
  }

  if (copy_to_user((void __user *)arg, data, ioc_size))
    return -EFAULT;

  return ret;
}

int keystone_release(struct inode *inode, struct file *file)
{
  unsigned long ueid = (unsigned long)(file->private_data);
  struct enclave *enclave;

  /* enclave has been already destroyed */
  if (!ueid)
  {
    return 0;
  }

  /* We need to send destroy enclave just the eid to close. */
  enclave = get_enclave_by_id(ueid);

  if (!enclave)
  {
    /* If eid is set to the invalid id, then we do not do anything. */
    return -EINVAL;
  }
  if (enclave->close_on_pexit)
  {
    return __keystone_destroy_enclave(ueid);
  }
  return 0;
}
