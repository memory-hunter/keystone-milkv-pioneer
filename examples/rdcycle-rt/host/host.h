//******************************************************************************
// rdcycle-test host — minimal shared-buffer / ocall plumbing.
// Stripped from the attestation example: no verifier, no report, no nonce.
//------------------------------------------------------------------------------
#ifndef _RDCYCLE_TEST_HOST_H_
#define _RDCYCLE_TEST_HOST_H_

#include <optional>
#include <utility>

#include "edge/edge_common.h"
#include "host/keystone.h"

class SharedBuffer {
public:
  SharedBuffer(void *buffer, size_t buffer_len)
      /* For now we assume the call struct is at the front of the shared
       * buffer. This will have to change to allow nested calls. */
      : edge_call_((struct edge_call *)buffer), buffer_((uintptr_t)buffer),
        buffer_len_(buffer_len) {}

  uintptr_t ptr() { return buffer_; }
  size_t size() { return buffer_len_; }

  std::optional<char *> get_c_string_or_set_bad_offset();
  std::optional<unsigned long> get_unsigned_long_or_set_bad_offset();

  void set_ok();
  void setup_ret_or_bad_ptr(unsigned long ret_val);

private:
  uintptr_t data_ptr();
  int args_ptr(uintptr_t *ptr, size_t *size);
  int validate_ptr(uintptr_t ptr);
  int get_offset_from_ptr(uintptr_t ptr, edge_data_offset *offset);
  int get_ptr_from_offset(edge_data_offset offset, uintptr_t *ptr);
  std::optional<std::pair<uintptr_t, size_t>>
  get_call_args_ptr_or_set_bad_offset();

  void set_bad_offset();
  void set_bad_ptr();
  int setup_ret(void *ptr, size_t size);

  struct edge_call *const edge_call_;
  uintptr_t const buffer_;
  size_t const buffer_len_;
};

#endif /* _RDCYCLE_TEST_HOST_H_ */
