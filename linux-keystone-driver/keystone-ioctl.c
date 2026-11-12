//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "keystone-sbi.h"
#include "keystone.h"
#include "keystone_user.h"
#include "keystone_step_rec.h"
#include "sm_err.h"
#include <asm/sbi.h>
#include <linux/string.h>
#include <linux/compiler.h>
#include <linux/build_bug.h>
#include <linux/irqflags.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/moduleparam.h>

// opensbi calls
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

// THead asm stuff
#define TH_SYNC_S asm volatile(".long 0x190000b" ::: "memory")
#define TH_L1D_IPA_A0 asm volatile(".long 0x2a5000b" ::"r"(rs1) : "memory")
#define TH_L1D_CIPA_A0 asm volatile(".long 0x2b5000b" ::"r"(rs1) : "memory")

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

// constants
#define TIMER_SET_ROUNDTRIP_TICKS 120

static long strike_target_eid = -1;
static unsigned long strike_target_pa = 0;
static unsigned long strike_rip_start = 0;
static unsigned long strike_rip_end = 0;
static uint64_t strike_delta_ticks = 1;
static int log_pc = 1;

module_param(strike_target_eid, long, 0644);
module_param(strike_target_pa, ulong, 0644);
module_param(strike_rip_start, ulong, 0644);
module_param(strike_rip_end, ulong, 0644);
module_param(strike_delta_ticks, ullong, 0644);
module_param(log_pc, int, 0644);

#define STEP_LOG_SIZE (1 << 24)

static DEFINE_KFIFO(step_fifo, u8, STEP_LOG_SIZE);
static DECLARE_WAIT_QUEUE_HEAD(step_waitq);
static atomic_t step_log_busy = ATOMIC_INIT(0);
static unsigned long log_dropped = 0;
module_param(log_dropped, ulong, 0444);

static void step_log(u32 kind, u64 val, u32 err)
{
  struct step_rec rec = {.val = val, .kind = kind, .err = err};

  if (kfifo_avail(&step_fifo) < sizeof(rec))
  {
    log_dropped++;
    return;
  }
  kfifo_in(&step_fifo, (const u8 *)&rec, sizeof(rec));
  wake_up_interruptible(&step_waitq);
}

static int step_log_open(struct inode *inode, struct file *filp)
{
  if (atomic_cmpxchg(&step_log_busy, 0, 1))
    return -EBUSY;
  kfifo_reset_out(&step_fifo);
  log_dropped = 0;
  return 0;
}

static int step_log_release(struct inode *inode, struct file *filp)
{
  atomic_set(&step_log_busy, 0);
  return 0;
}

static ssize_t step_log_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
  unsigned int copied;
  int ret;

  if (kfifo_is_empty(&step_fifo))
  {
    if (filp->f_flags & O_NONBLOCK)
      return -EAGAIN;
    ret = wait_event_interruptible(step_waitq, !kfifo_is_empty(&step_fifo));
    if (ret)
      return ret;
  }

  ret = kfifo_to_user(&step_fifo, buf, count, &copied);
  return ret ? ret : copied;
}

static __poll_t step_log_poll(struct file *filp, poll_table *wait)
{
  poll_wait(filp, &step_waitq, wait);
  return kfifo_is_empty(&step_fifo) ? 0 : EPOLLIN | EPOLLRDNORM;
}

static const struct file_operations step_log_fops = {
    .owner = THIS_MODULE,
    .open = step_log_open,
    .release = step_log_release,
    .read = step_log_read,
    .poll = step_log_poll,
};

static struct miscdevice step_log_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "keystone_step",
    .fops = &step_log_fops,
};

int keystone_step_log_init(void)
{
  return misc_register(&step_log_dev);
}

void keystone_step_log_exit(void)
{
  misc_deregister(&step_log_dev);
}

static inline int strike_is_target(unsigned long ueid)
{
  return strike_target_eid == -2 || (strike_target_eid >= 0 && ueid == strike_target_eid);
}

static struct sbiret keystone_strike_drive(unsigned long eid, unsigned long ueid, struct sbiret (*entry)(unsigned long))
{
  struct sbiret ret;

  if (!strike_is_target(ueid))
  {
    return entry(eid);
  }

  if (log_pc)
    step_log(STEP_REC_START, ueid, 0);

  TH_SYNC_S;
  SBI_CALL_1(SBI_SET_TIMER, 0, get_cycles() + TIMER_SET_ROUNDTRIP_TICKS + strike_delta_ticks);
  ret = entry(eid);
  TH_SYNC_S;

  while (ret.error == SBI_ERR_SM_ENCLAVE_INTERRUPTED && strike_is_target(ueid))
  {
    unsigned long pc = (unsigned long)ret.value;

    if (log_pc)
    {
      step_log(STEP_REC_STEP, pc, 0);
    }

    if (strike_target_pa)
    {
      if (strike_rip_end && pc >= strike_rip_start && pc <= strike_rip_end)
      {
        th_l1d_ipa(strike_target_pa);
      }
    }

    TH_SYNC_S;
    SBI_CALL_1(SBI_SET_TIMER, 0, get_cycles() + TIMER_SET_ROUNDTRIP_TICKS + strike_delta_ticks);
    ret = sbi_sm_resume_enclave(eid);
    TH_SYNC_S;
  }

  if (log_pc)
  {
    step_log(STEP_REC_END, ret.value, (u32)ret.error);
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

  ret = keystone_strike_drive(enclave->eid, ueid, sbi_sm_resume_enclave);

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
