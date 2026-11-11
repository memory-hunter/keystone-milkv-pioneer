
#include "app/eapp_utils.h"
#include "app/syscall.h"
#include "edge/edge_common.h"

#include <stdint.h>

#define INC10 asm volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
#define INC100 INC10 INC10 INC10 INC10 INC10 INC10 INC10 INC10 INC10 INC10
#define INC1K INC100 INC100 INC100 INC100 INC100 INC100 INC100 INC100 INC100 INC100
#define INC10K INC1K INC1K INC1K INC1K INC1K INC1K INC1K INC1K INC1K INC1K
#define INC100K INC10K INC10K INC10K INC10K INC10K INC10K INC10K INC10K INC10K
#define PADDING \
	INC10K;     \
	INC1K;      \
	INC1K;      \
	INC1K;      \
	INC1K;      \
	INC1K;      \
	INC1K;      \
	INC100;     \
	INC100;     \
	INC100;     \
	INC10;      \
	INC10;      \
	asm volatile("nop; nop; nop;"); // 0x10000 nops exactly

#define OCALL_PRINT_BUFFER 1
#define OCALL_PRINT_VALUE 2
#define TH_SYNC_S asm volatile(".long 0x190000b" ::: "memory");\
	asm volatile("fence rw, rw" ::: "memory");
#define SYNC_10 \
	TH_SYNC_S;  \
	TH_SYNC_S;  \
	TH_SYNC_S;  \
	TH_SYNC_S;  \
	TH_SYNC_S;  \
	TH_SYNC_S;  \
	TH_SYNC_S;  \
	TH_SYNC_S;  \
	TH_SYNC_S;  \
	TH_SYNC_S;
#define SYNC_100 SYNC_10 SYNC_10 SYNC_10 SYNC_10 SYNC_10 SYNC_10 SYNC_10 SYNC_10 SYNC_10 SYNC_10
#define SYNC_1K SYNC_100 SYNC_100 SYNC_100 SYNC_100 SYNC_100 SYNC_100 SYNC_100 SYNC_100 SYNC_100 SYNC_100

static unsigned long
ocall_print_buffer(const char *str, size_t len)
{
	unsigned long ret = 0;
	ocall(OCALL_PRINT_BUFFER, (void *)str, len,
		  &ret, sizeof(ret));
	return ret;
}

static void
ocall_print_value(unsigned long val)
{
	ocall(OCALL_PRINT_VALUE, &val, sizeof(val), 0, 0);
}

#define PRINT_STR(s) ocall_print_buffer((s), sizeof(s) - 1)

static uint64_t gcd(uint64_t a, uint64_t b)
{
	while (b != 0)
	{
		uint64_t t = b;
		b = a % b;
		a = t;
	}
	return a;
}

#define KEEP_FUNCTION \
	__attribute__((noinline, used, noipa))

KEEP_FUNCTION
static uint64_t modpow1(uint64_t base, uint64_t exp, uint64_t mod)
{
	uint64_t res = 1;
	base %= mod;

	while (exp > 0)
	{
		if (exp & 1)
			res = (res * base) % mod;

		base = (base * base) % mod;
		exp >>= 1;
	}
	return res;
}

KEEP_FUNCTION
static uint64_t modpow2(uint64_t base, uint64_t exp, uint64_t mod)
{
	uint64_t res = 1;
	base %= mod;

	while (exp > 0)
	{
		if (exp & 1)
			res = (res * base) % mod;

		base = (base * base) % mod;
		exp >>= 1;
	}
	return res;
}

KEEP_FUNCTION
static uint64_t modpow3(uint64_t base, uint64_t exp, uint64_t mod)
{
	uint64_t res = 1;
	base %= mod;

	while (exp > 0)
	{
		if (exp & 1)
			res = (res * base) % mod;

		base = (base * base) % mod;
		exp >>= 1;
	}
	return res;
}

static int64_t egcd(int64_t a, int64_t b, int64_t *x, int64_t *y)
{
	if (b == 0)
	{
		*x = 1;
		*y = 0;
		return a;
	}

	int64_t x1, y1;
	int64_t g = egcd(b, a % b, &x1, &y1);

	*x = y1;
	*y = x1 - (a / b) * y1;
	return g;
}

static uint64_t modinv(uint64_t a, uint64_t m)
{
	int64_t x, y;
	int64_t g = egcd((int64_t)a, (int64_t)m, &x, &y);

	if (g != 1)
		return 0;

	return (uint64_t)((x % (int64_t)m + (int64_t)m) % (int64_t)m);
}

int main(void)
{

	for (volatile int i = 0; i < 0xffff; i++)
	{
		i = i;
	}

	volatile uint64_t p = 61;
	TH_SYNC_S;
	volatile uint64_t q = 53;
	TH_SYNC_S;
	volatile uint64_t e = 17;
	TH_SYNC_S;
	volatile uint64_t N = p * q;
	TH_SYNC_S;
	volatile uint64_t phi = (p - 1) * (q - 1);
	TH_SYNC_S;
	volatile uint64_t d = modinv(e, phi);
	TH_SYNC_S;
	volatile uint64_t dp = d % (p - 1);
	TH_SYNC_S;
	volatile uint64_t dq = d % (q - 1);
	TH_SYNC_S;
	volatile uint64_t qinv = modinv(q, p);

	// PRINT_STR("RSA parameters:\np = 61\nq = 53\n");

	// ocall_print_value(N);	 // 3233
	// ocall_print_value(phi);	 // 3120
	// ocall_print_value(e);	 // 17
	// ocall_print_value(d);	 // 2753
	// ocall_print_value(dp);	 // 53
	// ocall_print_value(dq);	 // 49
	// ocall_print_value(qinv); // 38
	TH_SYNC_S;
	// Sign
	volatile uint64_t m = 2;

	TH_SYNC_S;
	// PRINT_STR("Doing sp\n");
	volatile uint64_t sp = modpow1(m, dp, p); // 8

	TH_SYNC_S;
	// PRINT_STR("Doing sq\n");
	volatile uint64_t sq = modpow2(m, dq, q); // 30

	// ocall_print_value(sp); // 8
	// ocall_print_value(sq); // 30
	TH_SYNC_S;

	volatile uint64_t h = (qinv * ((sp + p - sq) % p)) % p;
	// ocall_print_value(h); // 13

	TH_SYNC_S;

	volatile uint64_t s = sq + h * q;
	// ocall_print_value(s); // 719

	TH_SYNC_S;

	// PRINT_STR("Signing details:\n");
	// ocall_print_value(m); // 2

	// Verify
	volatile uint64_t verified = modpow3(s, e, N);

	TH_SYNC_S;

	// PRINT_STR("Verification result:\n");
	// ocall_print_value(verified); // 2

	EAPP_RETURN(0);
}
