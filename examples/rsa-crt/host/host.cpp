//******************************************************************************
// rsa-crt host runner.
// Single file: shared-buffer helpers + ocall dispatch + main.
// Handles only OCALL_PRINT_BUFFER (string) and OCALL_PRINT_VALUE (number).
//------------------------------------------------------------------------------
#include "host.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "edge/edge_common.h"
#include "host/keystone.h"

#define OCALL_PRINT_BUFFER 1
#define OCALL_PRINT_VALUE 2

#define LOG(...)                            \
  do                                        \
  {                                         \
    fprintf(stderr, "[host] " __VA_ARGS__); \
    fflush(stderr);                         \
  } while (0)

//------------------------------------------------------------------------------
// SharedBuffer
//------------------------------------------------------------------------------
void SharedBuffer::set_ok()
{
  edge_call_->return_data.call_status = CALL_STATUS_OK;
}
void SharedBuffer::set_bad_offset()
{
  edge_call_->return_data.call_status = CALL_STATUS_BAD_OFFSET;
}
void SharedBuffer::set_bad_ptr()
{
  edge_call_->return_data.call_status = CALL_STATUS_BAD_PTR;
}

int SharedBuffer::get_ptr_from_offset(edge_data_offset offset, uintptr_t *ptr)
{
  if (offset > UINTPTR_MAX - buffer_ || offset > buffer_len_)
    return -1;
  *ptr = buffer_ + offset;
  return 0;
}

int SharedBuffer::args_ptr(uintptr_t *ptr, size_t *size)
{
  *size = edge_call_->call_arg_size;
  return get_ptr_from_offset(edge_call_->call_arg_offset, ptr);
}

std::optional<std::pair<uintptr_t, size_t>>
SharedBuffer::get_call_args_ptr_or_set_bad_offset()
{
  uintptr_t call_args;
  size_t arg_len;
  if (args_ptr(&call_args, &arg_len) != 0)
  {
    set_bad_offset();
    return std::nullopt;
  }
  return std::pair{call_args, arg_len};
}

std::optional<char *> SharedBuffer::get_c_string_or_set_bad_offset()
{
  auto v = get_call_args_ptr_or_set_bad_offset();
  return v.has_value() ? std::optional{(char *)v.value().first} : std::nullopt;
}

std::optional<unsigned long>
SharedBuffer::get_unsigned_long_or_set_bad_offset()
{
  auto v = get_call_args_ptr_or_set_bad_offset();
  return v.has_value() ? std::optional{*(unsigned long *)v.value().first}
                       : std::nullopt;
}

uintptr_t SharedBuffer::data_ptr()
{
  return (uintptr_t)edge_call_ + sizeof(struct edge_call);
}

int SharedBuffer::validate_ptr(uintptr_t ptr)
{
  if (ptr > buffer_ + buffer_len_ || ptr < buffer_)
    return 1;
  return 0;
}

int SharedBuffer::get_offset_from_ptr(uintptr_t ptr, edge_data_offset *offset)
{
  int valid = validate_ptr(ptr);
  if (valid != 0)
    return valid;
  *offset = ptr - buffer_;
  return 0;
}

int SharedBuffer::setup_ret(void *ptr, size_t size)
{
  edge_call_->return_data.call_ret_size = size;
  return get_offset_from_ptr((uintptr_t)ptr,
                             &edge_call_->return_data.call_ret_offset);
}

void SharedBuffer::setup_ret_or_bad_ptr(unsigned long ret_val)
{
  uintptr_t data_section = data_ptr();
  memcpy((void *)data_section, &ret_val, sizeof(unsigned long));
  if (setup_ret((void *)data_section, sizeof(unsigned long)))
    set_bad_ptr();
  else
    set_ok();
}

//------------------------------------------------------------------------------
// ocall handlers
//------------------------------------------------------------------------------
static void print_buffer_wrapper(SharedBuffer &sb)
{
  auto t = sb.get_c_string_or_set_bad_offset();
  if (t.has_value())
  {
    printf("Enclave said: %s", t.value());
    fflush(stdout);
    sb.setup_ret_or_bad_ptr(strlen(t.value()));
  }
}

static void print_value_wrapper(SharedBuffer &sb)
{
  auto t = sb.get_unsigned_long_or_set_bad_offset();
  if (t.has_value())
  {
    printf("Enclave said value: %lu\n", t.value());
    fflush(stdout);
    sb.set_ok();
  }
}

static void dispatch_ocall(SharedBuffer &sb)
{
  struct edge_call *edge_call = (struct edge_call *)sb.ptr();
  switch (edge_call->call_id)
  {
  case OCALL_PRINT_BUFFER:
    print_buffer_wrapper(sb);
    break;
  case OCALL_PRINT_VALUE:
    print_value_wrapper(sb);
    break;
  default:
    break;
  }
}

//------------------------------------------------------------------------------
// main
//------------------------------------------------------------------------------
int main(int argc, char **argv)
{
  if (argc != 4)
  {
    printf("Usage: %s <eapp> <runtime> <loader>\n", argv[0]);
    return 1;
  }

  Keystone::Params params;
  params.setFreeMemSize(1024 * 1024);
  params.setUntrustedSize(1024 * 1024);

  Keystone::Enclave enclave;
  LOG("init: begin (eapp=%s rt=%s ld=%s)\n", argv[1], argv[2], argv[3]);
  enclave.init(argv[1], argv[2], argv[3], params);
  LOG("init: done\n");

  SharedBuffer shared_buffer(
      enclave.getSharedBuffer(), enclave.getSharedBufferSize());

  enclave.registerOcallDispatch([&shared_buffer](void *buffer)
                                {
    assert(buffer == (void *)shared_buffer.ptr());
    dispatch_ocall(shared_buffer); });

  uintptr_t encl_ret = 0;
  enclave.run(&encl_ret);
  LOG("run: returned, encl_ret=%lu\n", (unsigned long)encl_ret);

  return 0;
}
