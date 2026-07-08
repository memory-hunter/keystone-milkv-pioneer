#include "eapp_utils.h"
#include "string.h"
#include "edge_call.h"
#include <syscall.h>

#define OCALL_PRINT_STRING 1
#define OCALL_FLUSH_ALL 69420UL
#define OCALL_L1D_IALL 6767UL

#define CACHE_LINE 64
typedef struct
{
    volatile int val;
    char _pad[CACHE_LINE - sizeof(int)];
} __attribute__((aligned(CACHE_LINE))) cache_line_int_t;

static cache_line_int_t cntr_line;
#define cntr (cntr_line.val)

static const uint8_t _dummy = 0;

static unsigned long ocall_print_string(const char *s)
{
    unsigned long retval;
    ocall(OCALL_PRINT_STRING, (void *)s, strlen(s) + 1,
          &retval, sizeof(retval));
    return retval;
}

static inline void ocall_flush_all(void)
{
    ocall(OCALL_FLUSH_ALL, (void *)&_dummy, sizeof(_dummy), 0, 0);
}

static inline void ocall_l1d_iall(void)
{
    ocall(OCALL_L1D_IALL, (void *)&_dummy, sizeof(_dummy), 0, 0);
}

static void report(const char *label, int v)
{
    char buf[64];
    int i = 0;
    while (label[i] && i < 48)
    {
        buf[i] = label[i];
        i++;
    }

    if (v == 0)
    {
        buf[i++] = '0';
    }
    else
    {
        char tmp[12];
        int n = 0;
        unsigned int u = (v < 0) ? (unsigned int)-v : (unsigned int)v;
        if (v < 0)
            buf[i++] = '-';
        while (u)
        {
            tmp[n++] = '0' + (int)(u % 10);
            u /= 10;
        }
        while (n > 0)
            buf[i++] = tmp[--n];
    }
    buf[i] = '\0';
    ocall_print_string(buf);
}

int main(void)
{
    cntr = 0;
    ocall_flush_all();
    report("phase1 [after flush]         cntr = ", cntr);
    cntr++;
    report("phase2 [incremented]         cntr = ", cntr);
    ocall_l1d_iall();
    report("phase3 [after L1D INVD]      cntr = ", cntr);
    EAPP_RETURN(0);
}