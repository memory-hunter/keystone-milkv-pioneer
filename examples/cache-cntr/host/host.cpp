#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "xtcmo.h"

#include <edge_call.h>
#include <keystone.h>

#define OCALL_PRINT_STRING 1
#define OCALL_FLUSH_ALL 69420
#define OCALL_L1D_IALL 6767

static void print_string_wrapper(void *buffer)
{
    struct edge_call *ec = (struct edge_call *)buffer;
    uintptr_t args_ptr;
    size_t args_len;
    if (edge_call_args_ptr(ec, &args_ptr, &args_len) != 0)
    {
        ec->return_data.call_status = CALL_STATUS_BAD_OFFSET;
        return;
    }
    unsigned long retval = (unsigned long)printf("%s\n", (char *)args_ptr);
    fflush(stdout);
    uintptr_t ret_region = edge_call_data_ptr();
    memcpy((void *)ret_region, &retval, sizeof(retval));
    if (edge_call_setup_ret(ec, (void *)ret_region, sizeof(retval)) != 0)
        ec->return_data.call_status = CALL_STATUS_BAD_PTR;
    else
        ec->return_data.call_status = CALL_STATUS_OK;
}

static void flush_all_wrapper(void *buffer) {
    struct edge_call *ec = (struct edge_call *)buffer;
    ec->return_data.call_status = CALL_STATUS_OK;
}

static void l1d_iall_wrapper(void *buffer) {
    struct edge_call *ec = (struct edge_call *)buffer;
    ec->return_data.call_status = CALL_STATUS_OK;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "Usage: %s <eapp> <runtime> <loader>\n", argv[0]);
        return 1;
    }

    Keystone::Enclave enclave;
    Keystone::Params params;
    params.setFreeMemSize(1024 * 1024);
    params.setUntrustedSize(1024 * 1024);

    enclave.init(argv[1], argv[2], argv[3], params);

    enclave.registerOcallDispatch(incoming_call_dispatch);
    register_call(OCALL_PRINT_STRING, print_string_wrapper);
    register_call(OCALL_FLUSH_ALL, flush_all_wrapper);
    register_call(OCALL_L1D_IALL, l1d_iall_wrapper);

    edge_call_init_internals(
        (uintptr_t)enclave.getSharedBuffer(),
        enclave.getSharedBufferSize());

    enclave.run();
    return 0;
}