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

struct edge_return {
  unsigned long call_status;
  size_t        call_ret_offset;
  size_t        call_ret_size;
};
struct edge_call {
  unsigned long     call_id;
  size_t             call_arg_offset;
  size_t             call_arg_size;
  struct edge_return return_data;
};


#include <linux/uaccess.h>

#define TH_L1D_IALL     __asm__ volatile(".long 0x20000b"  ::: "memory")

#define TH_L1D_CIALL    __asm__ volatile(".long 0x30000b"  ::: "memory")
#define TH_L2_CIALL     __asm__ volatile(".long 0x170000b" ::: "memory")

#define TH_SYNC_S       __asm__ volatile(".long 0x190000b" ::: "memory")

#define TH_L1D_CIPA_A0     __asm__ volatile(".long 0x2b5000b" ::: "memory")
#define TH_L1D_CIVA_A0     __asm__ volatile(".long 0x275000b" ::: "memory")

#define TH_FLUSH_ALL \
    TH_SYNC_S;       \
    TH_L1D_CIALL;    \
    TH_SYNC_S;       \
    TH_L2_CIALL;     \
    TH_SYNC_S;

#define XTCMO_OCALL_FLUSH_ALL   69420UL
#define XTCMO_OCALL_L1D_IALL    6767UL

int __keystone_destroy_enclave(unsigned int ueid);
void __enclave_info(struct enclave *enclave);
void __enclave_info(struct enclave *enclave) {
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

int keystone_create_enclave(struct file *filep, unsigned long arg) {
  /* create parameters */
  struct keystone_ioctl_create_enclave *enclp =
      (struct keystone_ioctl_create_enclave *)arg;

  struct enclave *enclave;
  enclave = create_enclave(enclp->min_pages);

  if (enclave == NULL) {
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

int keystone_finalize_enclave(unsigned long arg) {
  struct sbiret ret;
  struct enclave *enclave;
  struct utm *utm;
  struct keystone_sbi_create_t create_args;

  struct keystone_ioctl_create_enclave *enclp =
      (struct keystone_ioctl_create_enclave *)arg;

  enclave = get_enclave_by_id(enclp->eid);
  if (!enclave) {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  enclave->is_init = false;

  /* SBI Call */
  create_args.epm_region.paddr = enclave->epm->pa;
  create_args.epm_region.size = enclave->epm->size;

  utm = enclave->utm;

  if (utm) {
    create_args.utm_region.paddr = __pa(utm->ptr);
    create_args.utm_region.size = utm->size;
  } else {
    create_args.utm_region.paddr = 0;
    create_args.utm_region.size = 0;
  }

  // physical addresses for runtime, user, and freemem
  create_args.runtime_paddr = enclp->runtime_paddr;
  create_args.user_paddr = enclp->user_paddr;
  create_args.free_paddr = enclp->free_paddr;
  create_args.free_requested = enclp->free_requested;

  ret = sbi_sm_create_enclave(&create_args);

  if (ret.error) {
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

int keystone_run_enclave(unsigned long data) {
  struct sbiret ret;
  unsigned long ueid;
  struct enclave *enclave;
  struct keystone_ioctl_run_enclave *arg =
      (struct keystone_ioctl_run_enclave *)data;

  ueid = arg->eid;
  enclave = get_enclave_by_id(ueid);

  __enclave_info(enclave);
  if (!enclave) {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  if (enclave->eid < 0) {
    keystone_err("real enclave does not exist\n");
    return -EINVAL;
  }

  ret = sbi_sm_run_enclave(enclave->eid);

  arg->error = ret.error;
  arg->value = ret.value;

  return 0;
}

int utm_init_ioctl(struct file *filp, unsigned long arg) {
  int ret = 0;
  struct utm *utm;
  struct enclave *enclave;
  struct keystone_ioctl_create_enclave *enclp =
      (struct keystone_ioctl_create_enclave *)arg;
  long long unsigned untrusted_size = enclp->utm_size;

  enclave = get_enclave_by_id(enclp->eid);

  if (!enclave) {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  utm = kmalloc(sizeof(struct utm), GFP_KERNEL);
  if (!utm) {
    ret = -ENOMEM;
    return ret;
  }

  ret = utm_init(utm, untrusted_size);

  /* prepare for mmap */
  enclave->utm = utm;

  enclp->utm_paddr = __pa(utm->ptr);

  return ret;
}

int keystone_destroy_enclave(struct file *filep, unsigned long arg) {
  int ret;
  struct keystone_ioctl_create_enclave *enclp =
      (struct keystone_ioctl_create_enclave *)arg;
  unsigned long ueid = enclp->eid;

  ret = __keystone_destroy_enclave(ueid);
  if (!ret) {
    filep->private_data = NULL;
  }
  return ret;
}

int __keystone_destroy_enclave(unsigned int ueid) {
  struct sbiret ret;
  struct enclave *enclave;
  enclave = get_enclave_by_id(ueid);

  if (!enclave) {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  if (enclave->eid >= 0) {
    ret = sbi_sm_destroy_enclave(enclave->eid);
    if (ret.error) {
      keystone_err(
          "fatal: cannot destroy enclave: SBI failed with error code %ld\n",
          ret.error);
      return -EINVAL;
    }
  } else {
    keystone_warn(
        "keystone_destroy_enclave: skipping (enclave does not exist)\n");
  }

  destroy_enclave(enclave);
  enclave_idr_remove(ueid);

  return 0;
}

int keystone_resume_enclave(unsigned long data) {
  struct sbiret ret;
  ret.error = 0;
  ret.value = 0;
  TH_FLUSH_ALL;
  unsigned long flags;
  local_irq_save(flags);
  // register unsigned long rs1 asm("a0") = (uintptr_t)&ret;
  // TH_L1D_CIVA_A0;
  struct keystone_ioctl_run_enclave *arg =
      (struct keystone_ioctl_run_enclave *)data;
  unsigned long ueid = arg->eid;
  struct enclave *enclave;
  enclave = get_enclave_by_id(ueid);

  if (!enclave) {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  if (enclave->eid < 0) {
    keystone_err("real enclave does not exist\n");
    return -EINVAL;
  }

  struct edge_call *ec;
  unsigned long id;

  ec = (struct edge_call *)enclave->utm->ptr;
  id = ec->call_id;

  // rs1 = (uintptr_t)&enclave;
  // TH_L1D_CIVA_A0;
  TH_FLUSH_ALL; // wbinvd
  ret = sbi_sm_resume_enclave(enclave->eid);
  if (id == XTCMO_OCALL_L1D_IALL) {
    TH_SYNC_S;
    TH_L1D_IALL; // invd l1d
    TH_SYNC_S;
  }
  
  arg->error = ret.error;
  arg->value = ret.value;
  
  local_irq_restore(flags);
  return 0;
}

long keystone_ioctl(struct file *filep, unsigned int cmd, unsigned long arg) {
  long ret;
  char data[512];

  size_t ioc_size;

  if (!arg)
    return -EINVAL;

  ioc_size = _IOC_SIZE(cmd);
  ioc_size = ioc_size > sizeof(data) ? sizeof(data) : ioc_size;

  if (copy_from_user(data, (void __user *)arg, ioc_size))
    return -EFAULT;

  switch (cmd) {
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
  /* Note that following commands could have been implemented as a part of
   * ADD_PAGE ioctl. However, there was a weird bug in compiler that generates a
   * wrong control flow that ends up with an illegal instruction if we combine
   * switch-case and if statements. We didn't identified the exact problem, so
   * we'll have these until we figure out */
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

int keystone_release(struct inode *inode, struct file *file) {
  unsigned long ueid = (unsigned long)(file->private_data);
  struct enclave *enclave;

  /* enclave has been already destroyed */
  if (!ueid) {
    return 0;
  }

  /* We need to send destroy enclave just the eid to close. */
  enclave = get_enclave_by_id(ueid);

  if (!enclave) {
    /* If eid is set to the invalid id, then we do not do anything. */
    return -EINVAL;
  }
  if (enclave->close_on_pexit) {
    return __keystone_destroy_enclave(ueid);
  }
  return 0;
}
