/*
 * The fused multiply-add instructions on millions of operands, of every
 * kind: what a compiler for RISC-V makes of a * b + c wherever it may, so
 * numeric code is full of them. Prints a hash of every result and of the
 * exception flags along the way, which is the same on any correct system;
 * under rv's translator it is the test of doing them with the host's own
 * instruction (fma_probe() in rv/jit.c). (riscv64, static.)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUSED(name, insn)							\
	static inline double name(double a, double b, double c)		\
	{									\
		double r;							\
		__asm__ volatile(insn " %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(b), "f"(c)); \
		return r;							\
	}
FUSED(madd, "fmadd.d")
FUSED(msub, "fmsub.d")
FUSED(nmsub, "fnmsub.d")
FUSED(nmadd, "fnmadd.d")

static uint64_t seed = 0x243f6a8885a308d3ULL;

static uint64_t next(void)
{
	seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
	return seed ^ seed >> 29;
}

static double from_bits(uint64_t bits)
{
	double d;

	memcpy(&d, &bits, 8);
	return d;
}

static uint64_t bits_of(double d)
{
	uint64_t bits;

	memcpy(&bits, &d, 8);
	return bits;
}

/* A number with any fraction and sign and an exponent within `spread` of 1. */
static double near_one(unsigned spread)
{
	uint64_t r = next();

	return from_bits((r & 0x800fffffffffffffULL) | (0x3ffULL - spread + (r >> 52) % (2 * spread + 1)) << 52);
}

static double any(void)
{
	static const uint64_t special[] = {
		0, 0x8000000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000000ULL,
		0x7ff4000000000000ULL, 1, 0x000fffffffffffffULL, 0x0010000000000000ULL, 0x7fefffffffffffffULL,
		0x3ff0000000000000ULL, 0xbff0000000000000ULL,
	};
	uint64_t r = next();

	return r % 3 ? from_bits(next()) : from_bits(special[(r >> 8) % (sizeof(special) / sizeof(special[0]))]);
}

int main(void)
{
	uint64_t hash = 0;
	unsigned long flags = 0;

	for (long i = 0; i < 1500000; i++) {
		double a, b, c;
		unsigned kind = (unsigned)(next() >> 40) % 16;

		if (kind < 9) {			/* ordinary numbers */
			a = near_one(40), b = near_one(40), c = near_one(60);
		} else if (kind < 12) {		/* a sum that nearly or wholly cancels */
			a = near_one(20), b = near_one(20);
			c = -from_bits(bits_of(a * b) + (next() % 5) - 2);
		} else if (kind < 13) {		/* small whole numbers: exact, and zeros */
			a = (double)(long)(next() % 9) - 4, b = (double)(long)(next() % 9) - 4, c = (double)(long)(next() % 33) - 16;
		} else if (kind < 14) {		/* very large and very small */
			a = near_one(1000), b = near_one(1000), c = near_one(1000);
		} else {			/* anything at all */
			a = any(), b = any(), c = any();
		}
		hash = hash * 0x100000001b3ULL ^ bits_of(madd(a, b, c));
		hash = hash * 0x100000001b3ULL ^ bits_of(msub(a, b, c));
		hash = hash * 0x100000001b3ULL ^ bits_of(nmsub(a, b, c));
		hash = hash * 0x100000001b3ULL ^ bits_of(nmadd(a, b, c));
		/* The flags: looked at now and then, and cleared at times, so that there are stretches with and without each. */
		if (i % 1024 == 1023) {
			__asm__ volatile("frflags %0" : "=r"(flags));
			hash = hash * 0x100000001b3ULL ^ flags;
			if (next() % 3 == 0)
				__asm__ volatile("fsflags x0");
		}
	}
	printf("fma: %016llx\n", (unsigned long long)hash);
	return 0;
}
