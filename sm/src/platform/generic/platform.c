/* Default platform does nothing special here */
#include "platform.h"
#include "../../enclave.h"
#include <sbi/sbi_string.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>

/* scounteren bits: CY=cycle, TM=time, IR=instret */
#define SCOUNTEREN_CY (1UL << 0)
#define SCOUNTEREN_TM (1UL << 1)
#define SCOUNTEREN_IR (1UL << 2)

/* mcountinhibit: bit N inhibits counter N; bit 0 = CY (cycle) */
#ifndef CSR_MCOUNTINHIBIT
#define CSR_MCOUNTINHIBIT 0x320
#endif
#define MCOUNTINHIBIT_CY (1UL << 0)

unsigned long platform_init_global_once() { return SBI_ERR_SM_ENCLAVE_SUCCESS; }

unsigned long platform_init_global() { return SBI_ERR_SM_ENCLAVE_SUCCESS; }

void platform_init_enclave(struct enclave *enclave) { return; }

void platform_destroy_enclave(struct enclave *enclave) { return; }

unsigned long platform_create_enclave(struct enclave *enclave) {
  return SBI_ERR_SM_ENCLAVE_SUCCESS;
}

void platform_switch_to_enclave(struct enclave *enclave) {
  /*
   * Allow the enclave's U-mode code to read cycle/time/instret directly
   * (rdcycle/rdtime/rdinstret). Without scounteren.CY, a U-mode rdcycle
   * traps as an illegal instruction inside the enclave on this core.
   *
   * Runs after the S-mode CSR swap in context_switch_to_enclave(), so it
   * overrides whatever scounteren value was swapped in for the enclave. The
   * host's scounteren is restored by the swap in context_switch_to_host().
   *
   * NOTE: this only makes the read *legal*. The counter must also be
   * *running* (mcountinhibit.CY == 0 and the C9xx PMU started) or rdcycle
   * will legally return a frozen value.
   */
  /*
   * The Linux perf / OpenSBI PMU framework context-manages the cycle counter:
   * pmu_ctr_stop_hw() both re-inhibits it (mcountinhibit.CY=1) AND revokes
   * read permission (mcounteren.CY=0) when a cycle event closes. With no perf
   * event active while the enclave owns the hart, the enclave inherits that
   * "off + unreadable" state. Force all three gates on for the enclave:
   *   mcounteren.CY  -> M-mode allows S/U to read cycle
   *   scounteren.CY  -> S-mode allows U to read cycle (direct rdcycle in eapp)
   *   mcountinhibit.CY cleared -> the counter actually advances
   * All are M-mode-only CSRs, so this can only be done here in the SM.
   */
  csr_set(CSR_MCOUNTEREN, SCOUNTEREN_CY | SCOUNTEREN_TM | SCOUNTEREN_IR);
  csr_set(CSR_SCOUNTEREN, SCOUNTEREN_CY | SCOUNTEREN_TM | SCOUNTEREN_IR);
  csr_clear(CSR_MCOUNTINHIBIT, MCOUNTINHIBIT_CY);
  return;
}

void platform_switch_from_enclave(struct enclave *enclave) { return; }

uint64_t platform_random() {
#pragma message("Platform has no entropy source, this is unsafe. TEST ONLY")
  static uint64_t w = 0, s = 0xb5ad4eceda1ce2a9;

  unsigned long cycles;
  asm volatile("rdcycle %0" : "=r"(cycles));

  // from Middle Square Weyl Sequence algorithm
  uint64_t x = cycles;
  x *= x;
  x += (w += s);
  return (x >> 32) | (x << 32);
}

// Initialization functions

/* from Sanctum BootROM */
extern byte sanctum_sm_hash[MDSIZE];
extern byte sanctum_sm_signature[SIGNATURE_SIZE];
extern byte sanctum_sm_secret_key[PRIVATE_KEY_SIZE];
extern byte sanctum_sm_public_key[PUBLIC_KEY_SIZE];
extern byte sanctum_dev_public_key[PUBLIC_KEY_SIZE];

extern byte sm_hash[MDSIZE];
extern byte sm_signature[SIGNATURE_SIZE];
extern byte sm_public_key[PUBLIC_KEY_SIZE];
extern byte sm_private_key[PRIVATE_KEY_SIZE];
extern byte dev_public_key[PUBLIC_KEY_SIZE];

void sm_copy_key(void) {
  sbi_memcpy(sm_hash, sanctum_sm_hash, MDSIZE);
  sbi_memcpy(sm_signature, sanctum_sm_signature, SIGNATURE_SIZE);
  sbi_memcpy(sm_public_key, sanctum_sm_public_key, PUBLIC_KEY_SIZE);
  sbi_memcpy(sm_private_key, sanctum_sm_secret_key, PRIVATE_KEY_SIZE);
  sbi_memcpy(dev_public_key, sanctum_dev_public_key, PUBLIC_KEY_SIZE);
}
