/*
 * Floating point on operands made up at random, every operation the
 * compiler has an instruction for, in double and single precision. Prints
 * one number per kind of operation, made of the bits of all its results
 * and of the exception flags at the end: an emulator that gets a single
 * bit of a single result wrong prints another number.
 *
 * The operands are of the kinds that make rounding hard: of similar size,
 * of very different size, sums that nearly cancel, products right at the
 * edge of what is exact, numbers at the edges of the range, and a few
 * zeros, infinities, NaNs and subnormal numbers.
 *
 *   fprand [MILLIONS]   (of rounds, default 1)
 */
#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t state = 0x9e3779b97f4a7c15ull;

static uint64_t next(void)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}

static double make_double(uint64_t bits)
{
	double d;

	memcpy(&d, &bits, 8);
	return d;
}

static uint64_t double_bits(double d)
{
	uint64_t bits;

	memcpy(&bits, &d, 8);
	return bits;
}

static float make_float(uint32_t bits)
{
	float f;

	memcpy(&f, &bits, 4);
	return f;
}

static uint32_t float_bits(float f)
{
	uint32_t bits;

	memcpy(&bits, &f, 4);
	return bits;
}

/* A double: mostly near 1 in size, sometimes anywhere, now and then something special. */
static double any_double(void)
{
	uint64_t r = next(), fraction = next() >> 12, sign = r >> 63;
	unsigned kind = (r >> 8) & 255;
	uint64_t exponent;

	if (kind < 150)
		exponent = 1023 - 30 + (r >> 20) % 61;
	else if (kind < 220)
		exponent = 1 + (r >> 20) % 2046;
	else if (kind < 232)
		exponent = 1023 - 420 + (r >> 20) % 841;	/* around where the short way stops being taken */
	else if (kind < 238)
		return make_double(sign << 63);			/* zero */
	else if (kind < 242)
		return make_double(sign << 63 | 0x7ffull << 52);	/* infinity */
	else if (kind < 246)
		return make_double(sign << 63 | 0x7ffull << 52 | fraction | 1);	/* a NaN */
	else if (kind < 252)
		return make_double(sign << 63 | fraction);	/* subnormal */
	else
		fraction = (r >> 30) & 1 ? 0 : (1ull << 52) - 1, exponent = 1023 + (r >> 20) % 3;
	/* Few digits set makes for exact results and exact ties. */
	if ((r >> 16) & 1)
		fraction &= ~0ull << ((r >> 40) % 52);
	return make_double(sign << 63 | exponent << 52 | fraction);
}

static float any_float(void)
{
	uint64_t r = next();
	uint32_t fraction = (uint32_t)(next() >> 41), sign = (uint32_t)(r >> 63);
	unsigned kind = (r >> 8) & 255;
	uint32_t exponent;

	if (kind < 170)
		exponent = 127 - 20 + (r >> 20) % 41;
	else if (kind < 232)
		exponent = 1 + (r >> 20) % 254;
	else if (kind < 238)
		return make_float(sign << 31);
	else if (kind < 242)
		return make_float(sign << 31 | 0xffu << 23);
	else if (kind < 246)
		return make_float(sign << 31 | 0xffu << 23 | fraction | 1);
	else
		return make_float(sign << 31 | fraction);
	if ((r >> 16) & 1)
		fraction &= ~0u << ((r >> 40) % 23);
	return make_float(sign << 31 | exponent << 23 | fraction);
}

#define KINDS 24
static uint64_t sums[KINDS];
static const char *const names[KINDS] = {
	"add", "sub", "mul", "div", "sqrt", "fma", "fms", "fnma", "fnms", "fma cancel", "compare", "to int",
	"from int", "narrow", "min max", "addf", "subf", "mulf", "divf", "sqrtf", "fmaf", "comparef", "to intf",
	"widen",
};

static void note(int kind, uint64_t bits)
{
	sums[kind] = (sums[kind] ^ bits) * 0x100000001b3ull + (sums[kind] >> 29);
}

int main(int argc, char **argv)
{
	long rounds = (argc > 1 ? atol(argv[1]) : 1) * 1000000L;
	volatile double sink = 1.0;

	/* The flag for inexact results, set from the start, as it is in any program that has computed a little. */
	sink = sink / 3.0;

	for (long i = 0; i < rounds; i++) {
		double a = any_double(), b = any_double(), c = any_double();
		float p = any_float(), q = any_float(), s = any_float();
		int64_t n = (int64_t)next() >> (next() % 64);

		/* Now and then operands that belong together: c near -(a*b), b near a. */
		if ((i & 7) == 0)
			c = make_double(double_bits(-(a * b)) + (next() % 5) - 2);
		if ((i & 15) == 1)
			b = make_double(double_bits(a) + (next() % 9) - 4);

		note(0, double_bits(a + b));
		note(1, double_bits(a - b));
		note(2, double_bits(a * b));
		note(3, double_bits(a / b));
		note(4, double_bits(sqrt(a)));
		note(5, double_bits(__builtin_fma(a, b, c)));
		note(6, double_bits(__builtin_fma(a, b, -c)));
		note(7, double_bits(__builtin_fma(-a, b, c)));
		note(8, double_bits(-__builtin_fma(a, b, c)));
		note(9, double_bits(__builtin_fma(a, b, -(a * b))));
		note(10, (uint64_t)((a < b) | (a <= b) << 1 | (a == b) << 2 | (a > c) << 3));
		if (a > -1e19 && a < 1e19)
			note(11, (uint64_t)(int64_t)a ^ (uint64_t)(int32_t)(a > -2e9 && a < 2e9 ? a : 0) ^
			     (a > -1 && a < 1.8e19 ? (uint64_t)a : 0) ^ (a > -1 && a < 4e9 ? (uint32_t)a : 0));
		note(12, double_bits((double)n) ^ double_bits((double)(uint64_t)n) ^ double_bits((double)(int32_t)n) ^
		     float_bits((float)n) ^ (uint64_t)float_bits((float)(uint64_t)n) << 32 ^ float_bits((float)(int32_t)n));
		note(13, float_bits((float)a));
		note(14, double_bits(fmin(a, b)) ^ double_bits(fmax(a, c)));

		if ((i & 7) == 2)
			s = make_float(float_bits(-(p * q)) + (uint32_t)(next() % 5) - 2);
		note(15, float_bits(p + q));
		note(16, float_bits(p - q));
		note(17, float_bits(p * q));
		note(18, float_bits(p / q));
		note(19, float_bits(sqrtf(p)));
		note(20, float_bits(__builtin_fmaf(p, q, s)) ^ (uint64_t)float_bits(__builtin_fmaf(-p, q, -s)) << 32);
		note(21, (uint64_t)((p < q) | (p <= q) << 1 | (p == q) << 2));
		if (p > -9e18f && p < 9e18f)
			note(22, (uint64_t)(int64_t)p ^ (uint64_t)(int32_t)(p > -2e9f && p < 2e9f ? p : 0));
		note(23, double_bits((double)p));

		/* Half of the time with the flags cleared, so that both ways of computing get their turn. */
		if ((i & 1023) == 1023 && (next() & 1))
			feclearexcept(FE_ALL_EXCEPT);
	}
	for (int k = 0; k < KINDS; k++)
		printf("%-10s %016llx\n", names[k], (unsigned long long)sums[k]);
	printf("flags      %x\n", fetestexcept(FE_ALL_EXCEPT));
	return 0;
}
