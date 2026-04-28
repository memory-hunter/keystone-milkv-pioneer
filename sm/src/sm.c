//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "sm.h"
#include "enclave.h"
#include "ipi.h"
#include "platform-hook.h"
#include "platform/generic/platform.h"
#include "pmp.h"
#include "sm-sbi-opensbi.h"
#include <crypto.h>
#include <sbi/riscv_barrier.h>
#include <sbi/riscv_locks.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_string.h>

static int sm_init_done = 0;
static int sm_region_id = 0, os_region_id = 0;

#ifndef TARGET_PLATFORM_HEADER
#error "SM requires a defined platform to build"
#endif

// Special target platform header, set by configure script
#include TARGET_PLATFORM_HEADER

byte sm_hash[MDSIZE] = {
    0,
};
byte sm_signature[SIGNATURE_SIZE] = {
    0,
};
byte sm_public_key[PUBLIC_KEY_SIZE] = {
    0,
};
byte sm_private_key[PRIVATE_KEY_SIZE] = {
    0,
};
byte dev_public_key[PUBLIC_KEY_SIZE] = {
    0,
};

int osm_pmp_set(uint8_t perm) {
  /* in case of OSM, PMP cfg is exactly the opposite.*/
  return pmp_set_keystone(os_region_id, perm);
}

static int smm_init(void) {
  int region = -1;
  int ret = pmp_region_init_atomic(SMM_BASE, SMM_SIZE, PMP_PRI_TOP, &region, 0);
  if (ret)
    return -1;

  return region;
}

static int osm_init(void) {
  int region = -1;
  int ret = pmp_region_init_atomic(0, -1UL, PMP_PRI_BOTTOM, &region, 1);
  if (ret)
    return -1;

  return region;
}

void sm_sign(void *signature, const void *data, size_t len) {
  sign(signature, data, len, sm_public_key, sm_private_key);
}

int sm_derive_sealing_key(unsigned char *key, const unsigned char *key_ident,
                          size_t key_ident_size,
                          const unsigned char *enclave_hash) {
  unsigned char info[MDSIZE + key_ident_size];

  sbi_memcpy(info, enclave_hash, MDSIZE);
  sbi_memcpy(info + MDSIZE, key_ident, key_ident_size);

  /*
   * The key is derived without a salt because we have no entropy source
   * available to generate the salt.
   */
  return kdf(NULL, 0, (const unsigned char *)sm_private_key, PRIVATE_KEY_SIZE,
             info, MDSIZE + key_ident_size, key, SEALING_KEY_SIZE);
}

static void sm_print_hash(void) {
  for (int i = 0; i < MDSIZE; i++) {
    sbi_printf("%02x", (char)sm_hash[i]);
  }
  sbi_printf("\n");
}

void sm_print_cert() {

  sbi_printf("Booting from Security Monitor\n");
  // sbi_printf("Size: %d\n", sm_size[0]);

  sbi_printf("============ Device PUBKEY =============\n");
  for (int i = 0; i < 8; i++) {
    sbi_printf("%x", *((int *)dev_public_key + i));
    if (i % 4 == 3)
      sbi_printf("\n");
  }
  sbi_printf("=================================\n");

  sbi_printf("============ SM PUBKEY =============\n");
  for (int i = 0; i < 8; i++) {
    sbi_printf("%x", *((int *)sm_public_key + i));
    if (i % 4 == 3)
      sbi_printf("\n");
  }
  sbi_printf("=================================\n");

  sbi_printf("=========== SM PRIVKEY =============\n");
  for (int i = 0; i < 16; i++) {
    sbi_printf("%x", *((int *)sm_private_key + i));
    if (i % 4 == 3)
      sbi_printf("\n");
  }
  sbi_printf("\n=================================\n");
  sbi_printf("=========== SM SIGNATURE ===========\n");
  for (int i = 0; i < 16; i++) {
    sbi_printf("%x", *((int *)sm_signature + i));
    if (i % 4 == 3)
      sbi_printf("\n");
  }
  sbi_printf("=================================\n");
  sbi_printf("=========== SM HASH ===========\n");
  for (int i = 0; i < 16; i++) {
    sbi_printf("%x", *((int *)sm_hash + i));
    if (i % 4 == 3)
      sbi_printf("\n");
  }
  sbi_printf("=================================\n");
}

void sm_init(bool cold_boot) {
  unsigned long hartid = csr_read(mhartid);
  // initialize SMM
  if (cold_boot) {
    /* only the cold-booting hart will execute these */
    sbi_printf("[SM] Initializing ... hart [%lx]\n", hartid);

    sbi_ecall_register_extension(&ecall_keystone_enclave);

    sm_region_id = smm_init();
    if (sm_region_id < 0) {
      sbi_printf("[SM] intolerable error - failed to initialize SM memory");
      sbi_hart_hang();
    }

    os_region_id = osm_init();
    if (os_region_id < 0) {
      sbi_printf("[SM] intolerable error - failed to initialize OS memory");
      sbi_hart_hang();
    }

    if (platform_init_global_once() != SBI_ERR_SM_ENCLAVE_SUCCESS) {
      sbi_printf("[SM] platform global init fatal error");
      sbi_hart_hang();
    }
    // Copy the keypair from the root of trust
    // NOTE: restore this for normal operation RoT
    sm_copy_key();
    //
    // sm_copy_key_moded();

    // Init the enclave metadata
    enclave_init_metadata();

    sm_init_done = 1;
    mb();
  }

  /* wait until cold-boot hart finishes */
  while (!sm_init_done) {
    mb();
  }
  sbi_printf("[SM] warm A hart %lx\n", hartid);
  /* DIAGNOSTIC: only cold hart runs PMP setup; warm harts use OpenSBI default PMP */
  if (cold_boot) {
    pmp_init();
    sbi_printf("[SM] warm B hart %lx\n", hartid);
    pmp_set_keystone(sm_region_id, PMP_NO_PERM);
    sbi_printf("[SM] warm C hart %lx\n", hartid);
    pmp_set_keystone(os_region_id, PMP_ALL_PERM);
  }
  sbi_printf("[SM] warm D hart %lx\n", hartid);

  /* Fire platform specific global init */
  if (platform_init_global() != SBI_ERR_SM_ENCLAVE_SUCCESS) {
    sbi_printf("[SM] platform global init fatal error");
    sbi_hart_hang();
  }
  sbi_printf("[SM] warm done hart %lx\n", hartid);

  sbi_printf("[SM] Keystone security monitor has been initialized!\n");

  sm_print_hash();

  // TODO: remove for debug
//  sm_print_cert();
  return;
}
