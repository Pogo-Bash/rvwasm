/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The F and D extensions and the floating-point CSRs.
 *
 * The reference is arithmetic done in software (Berkeley SoftFloat, RISC-V
 * flavour): WebAssembly has neither rounding-mode control nor exception
 * flags, and results have to be exact and the same on every host.
 * Conveniently SoftFloat numbers its rounding modes and exception flags
 * exactly as RISC-V does.
 *
 * The host's own floating point is used where it is certain to give the
 * very same result and flags (fp_fast()), which is most of the time:
 *   - The host rounds to nearest, ties to even, and nothing else; so only
 *     when that is the rounding the instruction asks for.
 *   - The host does not say whether a result was exact. The flag for that
 *     is sticky, though: once it is set, an inexact result changes nothing.
 *     So arithmetic goes to the host only while the flag is set, which in a
 *     program that computes it is from its first division on. (A result
 *     that is plainly exact, an integer to a double, say, needs no flag.)
 *   - The other flags (overflow, underflow, division by zero, invalid)
 *     cannot come up when the operands are zero or normal numbers and the
 *     result is one too, which is checked. Anything with an infinity, a
 *     NaN or a subnormal number in it is SoftFloat's.
 * QEMU does the same for the same reasons. RV_FP_CHECK=1 computes everything
 * both ways and stops at the first difference.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "softfloat.h"

#include "rv.h"

#define F32_SIGN	0x80000000u
#define F64_SIGN	0x8000000000000000ull
#define F32_CANON_NAN	0x7fc00000u
#define F64_CANON_NAN	0x7ff8000000000000ull

#define CSR_FFLAGS	0x001
#define CSR_FRM		0x002
#define CSR_FCSR	0x003
#define CSR_CYCLE	0xc00
#define CSR_TIME	0xc01
#define CSR_INSTRET	0xc02

#define FFLAGS_MASK	0x1f
#define FRM_SHIFT	5
#define FRM_DYNAMIC	7

/*
 * A single-precision value lives in the low half of a 64-bit register with
 * the upper half all ones ("NaN boxing"). Anything else read as a single is
 * the canonical NaN.
 */
static uint64_t box(uint32_t v)
{
	return 0xffffffff00000000ull | v;
}

static float32_t get_f32(struct rv_cpu *cpu, uint32_t reg)
{
	uint64_t r = cpu->f[reg];
	float32_t v = { (r >> 32) == 0xffffffffu ? (uint32_t)r : F32_CANON_NAN };

	return v;
}

static float64_t get_f64(struct rv_cpu *cpu, uint32_t reg)
{
	float64_t v = { cpu->f[reg] };

	return v;
}

static int is_nan32(uint32_t v)
{
	return (v & 0x7fffffffu) > 0x7f800000u;
}

static int is_nan64(uint64_t v)
{
	return (v & ~F64_SIGN) > 0x7ff0000000000000ull;
}

/* The ten-way classification FCLASS returns as a one-hot mask. */
static uint64_t classify(int sign, int exp_zero, int exp_ones, int frac_zero, int quiet)
{
	if (exp_ones) {
		if (frac_zero)
			return sign ? 1u << 0 : 1u << 7;	/* infinity */
		return quiet ? 1u << 9 : 1u << 8;		/* NaN */
	}
	if (exp_zero) {
		if (frac_zero)
			return sign ? 1u << 3 : 1u << 4;	/* zero */
		return sign ? 1u << 2 : 1u << 5;		/* subnormal */
	}
	return sign ? 1u << 1 : 1u << 6;			/* normal */
}

static uint64_t fclass32(uint32_t v)
{
	uint32_t exp = (v >> 23) & 0xff, frac = v & 0x7fffff;

	return classify(v >> 31, exp == 0, exp == 0xff, frac == 0, (frac >> 22) & 1);
}

static uint64_t fclass64(uint64_t v)
{
	uint64_t exp = (v >> 52) & 0x7ff, frac = v & 0xfffffffffffffull;

	return classify((int)(v >> 63), exp == 0, exp == 0x7ff, frac == 0, (int)(frac >> 51) & 1);
}

/*
 * FMIN/FMAX: a quiet NaN operand is ignored in favour of the other operand,
 * two NaNs give the canonical NaN, and -0 orders below +0. The comparison
 * raises "invalid" for signalling NaNs, as required.
 */
static uint32_t minmax32(float32_t a, float32_t b, int is_max)
{
	int pick_a;

	if (is_nan32(a.v) && is_nan32(b.v)) {
		(void)f32_lt_quiet(a, b);
		return F32_CANON_NAN;
	}
	if (is_max)
		pick_a = f32_lt_quiet(b, a) || (f32_eq(b, a) && (b.v & F32_SIGN));
	else
		pick_a = f32_lt_quiet(a, b) || (f32_eq(a, b) && (a.v & F32_SIGN));
	return (pick_a || is_nan32(b.v)) ? a.v : b.v;
}

static uint64_t minmax64(float64_t a, float64_t b, int is_max)
{
	int pick_a;

	if (is_nan64(a.v) && is_nan64(b.v)) {
		(void)f64_lt_quiet(a, b);
		return F64_CANON_NAN;
	}
	if (is_max)
		pick_a = f64_lt_quiet(b, a) || (f64_eq(b, a) && (b.v & F64_SIGN));
	else
		pick_a = f64_lt_quiet(a, b) || (f64_eq(a, b) && (a.v & F64_SIGN));
	return (pick_a || is_nan64(b.v)) ? a.v : b.v;
}

/* Resolve an instruction's rounding-mode field; -1 if it is not a valid mode. */
static int rounding_mode(struct rv_cpu *cpu, uint32_t rm)
{
	if (rm == FRM_DYNAMIC)
		rm = (cpu->fcsr >> FRM_SHIFT) & 7;
	return rm <= softfloat_round_near_maxMag ? (int)rm : -1;
}

static int fp_load_store(struct rv_cpu *cpu, uint32_t insn)
{
	uint32_t opcode = insn & 0x7f, f3 = (insn >> 12) & 7;
	uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
	uint32_t v32;
	uint64_t v64;

	if (f3 != 2 && f3 != 3)
		return -1;

	if (opcode == 0x07) { /* FLW, FLD */
		uint64_t addr = cpu->x[rs1] + (uint64_t)(int64_t)((int32_t)insn >> 20);

		if (f3 == 2) {
			memcpy(&v32, rv_ptr(cpu, addr, 4, RV_PROT_READ), 4);
			v64 = box(v32);
		} else {
			memcpy(&v64, rv_ptr(cpu, addr, 8, RV_PROT_READ), 8);
		}
		if (cpu->trap == RV_TRAP_NONE)
			cpu->f[rd] = v64;
	} else { /* FSW, FSD: the raw bits, with no NaN-box check */
		int64_t imm = (int32_t)(((int32_t)insn >> 25 << 5) | ((insn >> 7) & 31));
		uint64_t addr = cpu->x[rs1] + (uint64_t)imm;

		uint64_t len = f3 == 2 ? 4 : 8;
		uint8_t *p = rv_ptr(cpu, addr, len, RV_PROT_WRITE);

		if (f3 == 2) {
			v32 = (uint32_t)cpu->f[rs2];
			memcpy(p, &v32, 4);
		} else {
			v64 = cpu->f[rs2];
			memcpy(p, &v64, 8);
		}
		if (p == cpu->scratch)	/* across two pages that are apart for the host (mm.c) */
			rv_store_split(cpu, addr, len);
	}
	return 0;
}

/* FMADD, FMSUB, FNMSUB, FNMADD: (+-)(rs1 * rs2) (+-) rs3 with a single rounding. */
static int fp_fused(struct rv_cpu *cpu, uint32_t insn)
{
	uint32_t opcode = insn & 0x7f, fmt = (insn >> 25) & 3;
	uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31, rs3 = insn >> 27;
	int negate_product = opcode == 0x4b || opcode == 0x4f;
	int negate_addend = opcode == 0x47 || opcode == 0x4f;
	int rm = rounding_mode(cpu, (insn >> 12) & 7);

	if (rm < 0 || fmt > 1)
		return -1;
	softfloat_roundingMode = (uint_fast8_t)rm;

	if (fmt == 0) {
		float32_t a = get_f32(cpu, rs1), b = get_f32(cpu, rs2), c = get_f32(cpu, rs3);

		if (negate_product)
			a.v ^= F32_SIGN;
		if (negate_addend)
			c.v ^= F32_SIGN;
		cpu->f[rd] = box(f32_mulAdd(a, b, c).v);
	} else {
		float64_t a = get_f64(cpu, rs1), b = get_f64(cpu, rs2), c = get_f64(cpu, rs3);

		if (negate_product)
			a.v ^= F64_SIGN;
		if (negate_addend)
			c.v ^= F64_SIGN;
		cpu->f[rd] = f64_mulAdd(a, b, c).v;
	}
	return 0;
}

/* ---------------------------------------------------------- the host's own */

static double to_double(uint64_t v)
{
	double d;

	memcpy(&d, &v, 8);
	return d;
}

static uint64_t double_bits(double d)
{
	uint64_t v;

	memcpy(&v, &d, 8);
	return v;
}

static float to_float(uint32_t v)
{
	float f;

	memcpy(&f, &v, 4);
	return f;
}

static uint32_t float_bits(float f)
{
	uint32_t v;

	memcpy(&v, &f, 4);
	return v;
}

/* A normal number: not zero, subnormal, infinite or NaN. */
static int normal64(uint64_t v)
{
	return ((v >> 52) & 0x7ff) - 1 < 0x7fe;
}

static int normal32(uint32_t v)
{
	return ((v >> 23) & 0xff) - 1 < 0xfe;
}

static int zero64(uint64_t v)
{
	return !(v << 1);
}

static int zero32(uint32_t v)
{
	return !(v << 1);
}

static int finite64(uint64_t v)
{
	return ((v >> 52) & 0x7ff) != 0x7ff;
}

static int finite32(uint32_t v)
{
	return ((v >> 23) & 0xff) != 0xff;
}

/* A single in a register, if it is boxed as one. */
static int unbox(uint64_t r, uint32_t *v)
{
	*v = (uint32_t)r;
	return (r >> 32) == 0xffffffffu;
}

/*
 * Execute the instruction with the host's floating point if that is certain
 * to give SoftFloat's result and leave the flags as SoftFloat would (see the
 * top of the file). Returns 1 if it did, 0 if SoftFloat has to.
 */
static int fp_fast(struct rv_cpu *cpu, uint32_t insn)
{
	uint32_t f7 = insn >> 25, f3 = (insn >> 12) & 7;
	uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
	uint32_t frm = (cpu->fcsr >> FRM_SHIFT) & 7;
	int dbl = f7 & 1;
	int nearest = f3 == 0 || (f3 == FRM_DYNAMIC && frm == 0);
	int inexact_set = cpu->fcsr & 1;
	uint64_t a = cpu->f[rs1], b = cpu->f[rs2], r, x;
	uint32_t a32, b32, r32;
	double t;

	if (f7 & 2)
		return 0;

	switch (f7 >> 2) {
	case 0x00: /* FADD */
	case 0x01: /* FSUB */
	case 0x02: /* FMUL */
	case 0x03: /* FDIV */
		if (!nearest || !inexact_set)
			return 0;
		if (dbl) {
			if (!(normal64(a) || zero64(a)) || !(normal64(b) || zero64(b)))
				return 0;
			switch (f7 >> 2) {
			case 0x00:
			case 0x01:
				/* A sum that is tiny is exact; one that is too large is SoftFloat's. */
				r = double_bits((f7 >> 2) ? to_double(a) - to_double(b) : to_double(a) + to_double(b));
				if (!finite64(r))
					return 0;
				break;
			case 0x02:
				r = double_bits(to_double(a) * to_double(b));
				if (!normal64(r) && !zero64(a) && !zero64(b))
					return 0;
				break;
			default:
				if (zero64(b))
					return 0;
				r = double_bits(to_double(a) / to_double(b));
				if (!normal64(r) && !zero64(a))
					return 0;
			}
			cpu->f[rd] = r;
			return 1;
		}
		if (!unbox(a, &a32) || !unbox(b, &b32) ||
		    !(normal32(a32) || zero32(a32)) || !(normal32(b32) || zero32(b32)))
			return 0;
		switch (f7 >> 2) {
		case 0x00:
		case 0x01:
			r32 = float_bits((f7 >> 2) ? to_float(a32) - to_float(b32) : to_float(a32) + to_float(b32));
			if (!finite32(r32))
				return 0;
			break;
		case 0x02:
			r32 = float_bits(to_float(a32) * to_float(b32));
			if (!normal32(r32) && !zero32(a32) && !zero32(b32))
				return 0;
			break;
		default:
			if (zero32(b32))
				return 0;
			r32 = float_bits(to_float(a32) / to_float(b32));
			if (!normal32(r32) && !zero32(a32))
				return 0;
		}
		cpu->f[rd] = box(r32);
		return 1;

	case 0x0b: /* FSQRT: of a positive normal number, which is one again */
		if (!nearest || !inexact_set || rs2 != 0)
			return 0;
		if (dbl) {
			if (!normal64(a) || (a >> 63))
				return 0;
			cpu->f[rd] = double_bits(__builtin_sqrt(to_double(a)));
			return 1;
		}
		if (!unbox(a, &a32) || !normal32(a32) || (a32 >> 31))
			return 0;
		cpu->f[rd] = box(float_bits(__builtin_sqrtf(to_float(a32))));
		return 1;

	case 0x08: /* FCVT.S.D, FCVT.D.S */
		if (!dbl && rs2 == 1) {
			float narrow;

			if (!(normal64(a) || zero64(a)))
				return 0;
			narrow = (float)to_double(a);
			r32 = float_bits(narrow);
			if (!normal32(r32) && !zero64(a))
				return 0;
			if ((double)narrow != to_double(a) && !(nearest && inexact_set))
				return 0;
			cpu->f[rd] = box(r32);
			return 1;
		}
		if (dbl && rs2 == 0) {
			/* Widening is exact; only a NaN has more to it. */
			if (!unbox(a, &a32) || (!finite32(a32) && (a32 << 9)))
				return 0;
			cpu->f[rd] = double_bits((double)to_float(a32));
			return 1;
		}
		return 0;

	case 0x14: /* FLE, FLT, FEQ: flags only for a NaN */
		if (f3 > 2)
			return 0;
		if (dbl) {
			double p = to_double(a), q = to_double(b);

			if (p != p || q != q)
				return 0;
			cpu->x[rd] = f3 == 0 ? p <= q : f3 == 1 ? p < q : p == q;
			return 1;
		}
		if (unbox(a, &a32) && unbox(b, &b32)) {
			float p = to_float(a32), q = to_float(b32);

			if (p != p || q != q)
				return 0;
			cpu->x[rd] = f3 == 0 ? p <= q : f3 == 1 ? p < q : p == q;
			return 1;
		}
		return 0;

	case 0x18: { /* FCVT.{W,WU,L,LU}.{S,D}: to an integer, if it fits, rounded the host's four ways (not to nearest away from zero) */
		uint32_t rm = f3 == FRM_DYNAMIC ? frm : f3;

		if (rm > 3)
			return 0;
		if (dbl) {
			if (!finite64(a))
				return 0;
			t = to_double(a);
		} else {
			if (!unbox(a, &a32) || !finite32(a32))
				return 0;
			t = (double)to_float(a32);
		}
		{
			double whole = rm == 0 ? __builtin_rint(t) : rm == 1 ? __builtin_trunc(t) : rm == 2 ? __builtin_floor(t) : __builtin_ceil(t);

			if (whole != t && !inexact_set)
				return 0;
			switch (rs2) {
			case 0:
				if (!(whole >= -2147483648.0 && whole <= 2147483647.0))
					return 0;
				cpu->x[rd] = (uint64_t)(int64_t)(int32_t)whole;
				return 1;
			case 1:
				if (!(whole >= 0.0 && whole <= 4294967295.0))
					return 0;
				cpu->x[rd] = (uint64_t)(int64_t)(int32_t)(uint32_t)whole;
				return 1;
			case 2:
				if (!(whole >= -9223372036854775808.0 && whole < 9223372036854775808.0))
					return 0;
				cpu->x[rd] = (uint64_t)(int64_t)whole;
				return 1;
			case 3:
				if (!(whole >= 0.0 && whole < 18446744073709551616.0))
					return 0;
				cpu->x[rd] = (uint64_t)whole;
				return 1;
			}
		}
		return 0;
	}

	case 0x1a: /* FCVT.{S,D}.{W,WU,L,LU}: from an integer; exact if it has few enough digits */
		x = cpu->x[rs1];
		if (dbl) {
			switch (rs2) {
			case 0: cpu->f[rd] = double_bits((double)(int32_t)x); return 1;
			case 1: cpu->f[rd] = double_bits((double)(uint32_t)x); return 1;
			case 2:
				if (!(nearest && inexact_set) && (uint64_t)((int64_t)x + (1LL << 53)) > (1ULL << 54))
					return 0;
				cpu->f[rd] = double_bits((double)(int64_t)x);
				return 1;
			case 3:
				if (!(nearest && inexact_set) && x > (1ULL << 53))
					return 0;
				cpu->f[rd] = double_bits((double)x);
				return 1;
			}
			return 0;
		}
		switch (rs2) {
		case 0:
			if (!(nearest && inexact_set) && (uint32_t)((int32_t)x + (1 << 24)) > (1u << 25))
				return 0;
			cpu->f[rd] = box(float_bits((float)(int32_t)x));
			return 1;
		case 1:
			if (!(nearest && inexact_set) && (uint32_t)x > (1u << 24))
				return 0;
			cpu->f[rd] = box(float_bits((float)(uint32_t)x));
			return 1;
		case 2:
			if (!(nearest && inexact_set) && (uint64_t)((int64_t)x + (1LL << 24)) > (1ULL << 25))
				return 0;
			cpu->f[rd] = box(float_bits((float)(int64_t)x));
			return 1;
		case 3:
			if (!(nearest && inexact_set) && x > (1ULL << 24))
				return 0;
			cpu->f[rd] = box(float_bits((float)x));
			return 1;
		}
		return 0;
	}
	return 0;
}

/*
 * a * b + c with one rounding, on a host that has no instruction for it
 * (WebAssembly). Not as (a * b) + c, which rounds twice. The exact product
 * is two doubles (Dekker), adding c to the larger of them leaves a third
 * (Knuth), and the two small ones are added with rounding to odd, after
 * which rounding the sum of the large and the small one to nearest is
 * rounding the exact result to nearest (Boldo and Melquiond, "Emulation of
 * a FMA and correctly rounded sums: proved algorithms using rounding to
 * odd"; the C library's fma() of glibc is built the same way).
 *
 * All of that is exact only while nothing overflows or underflows on the
 * way, which the callers make sure of by the operands' exponents.
 * (-ffp-contract=off, see the Makefile: a compiler that fused any of these
 * operations would break it.)
 */
static double odd_sum(double x, double y)
{
	double s = x + y, part = s - x, rest = (x - (s - part)) + (y - part);
	uint64_t bits = double_bits(s);

	/* Not exact, and the nearest is even: the odd one is its neighbour on the side of the rest. */
	if (rest != 0 && !(bits & 1))
		bits += ((rest > 0) == (s > 0)) ? 1 : -1;
	return to_double(bits);
}

static double fused64(double a, double b, double c)
{
	const double split = 134217729.0;	/* 2^27 + 1 */
	double high = a * b;
	double ta = a * split, ah = ta - (ta - a), al = a - ah;
	double tb = b * split, bh = tb - (tb - b), bl = b - bh;
	double low = (((ah * bh - high) + ah * bl) + al * bh) + al * bl;
	double sum = c + high, part = sum - c, rest = (c - (sum - part)) + (high - part);

	return sum + odd_sum(rest, low);
}

/* For translated code (jit.c), which has checked the operands as fused_fast() does and checks the result. */
uint64_t rv_jit_fused64(uint64_t a, uint64_t b, uint64_t c)
{
	return double_bits(fused64(to_double(a), to_double(b), to_double(c)));
}

/* An exponent safely in the middle: products and their errors neither overflow nor underflow. */
static int moderate64(uint64_t v)
{
	return ((v >> 52) & 0x7ff) - (0x3ff - 400) <= 800;
}

/* FMADD, FMSUB, FNMSUB, FNMADD with the host's arithmetic, on the same terms as fp_fast(). */
static int fused_fast(struct rv_cpu *cpu, uint32_t insn)
{
	uint32_t opcode = insn & 0x7f, fmt = (insn >> 25) & 3, f3 = (insn >> 12) & 7;
	uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31, rs3 = insn >> 27;
	uint64_t a = cpu->f[rs1], b = cpu->f[rs2], c = cpu->f[rs3], r;
	uint32_t a32, b32, c32, r32;
	double exact;

	if (!(cpu->fcsr & 1) || !(f3 == 0 || (f3 == FRM_DYNAMIC && !((cpu->fcsr >> FRM_SHIFT) & 7))))
		return 0;
	if (fmt == 1) {
		if (!moderate64(a) || !moderate64(b) || !(moderate64(c) || zero64(c)))
			return 0;
		if (opcode == 0x4b || opcode == 0x4f)
			a ^= F64_SIGN;
		if (opcode == 0x47 || opcode == 0x4f)
			c ^= F64_SIGN;
		r = double_bits(fused64(to_double(a), to_double(b), to_double(c)));
		if (!normal64(r) && !zero64(r))
			return 0;
		cpu->f[rd] = r;
		return 1;
	}
	if (fmt != 0 || !unbox(a, &a32) || !unbox(b, &b32) || !unbox(c, &c32) ||
	    !(normal32(a32) || zero32(a32)) || !(normal32(b32) || zero32(b32)) || !(normal32(c32) || zero32(c32)))
		return 0;
	if (opcode == 0x4b || opcode == 0x4f)
		a32 ^= F32_SIGN;
	if (opcode == 0x47 || opcode == 0x4f)
		c32 ^= F32_SIGN;
	/*
	 * Singles: their product is exact as a double, and the sum rounded to
	 * odd as a double rounds to the right single (a double has more than two
	 * digits to spare).
	 */
	exact = odd_sum((double)to_float(a32) * (double)to_float(b32), (double)to_float(c32));
	r32 = float_bits((float)exact);
	if (!normal32(r32) && exact != 0)
		return 0;
	cpu->f[rd] = box(r32);
	return 1;
}

/* RV_FP_CHECK: the instruction both ways, from the same state; they have to agree. */
static int fp_check = -1;
static int fp_fused(struct rv_cpu *cpu, uint32_t insn);

static int fp_op(struct rv_cpu *cpu, uint32_t insn);

static int checked_fast(struct rv_cpu *cpu, uint32_t insn)
{
	uint32_t rd = (insn >> 7) & 31;
	uint64_t f_before = cpu->f[rd], x_before = cpu->x[rd], f_fast, x_fast;
	uint32_t fcsr_before = cpu->fcsr;
	int fused = (insn & 0x7f) != 0x53;

	if (!(fused ? fused_fast(cpu, insn) : fp_fast(cpu, insn)))
		return 0;
	f_fast = cpu->f[rd];
	x_fast = cpu->x[rd];
	cpu->f[rd] = f_before;
	cpu->x[rd] = x_before;

	softfloat_exceptionFlags = 0;
	if (fused ? fp_fused(cpu, insn) : fp_op(cpu, insn))
		return 0;
	cpu->fcsr |= softfloat_exceptionFlags & FFLAGS_MASK;
	if (cpu->f[rd] != f_fast || cpu->x[rd] != x_fast || cpu->fcsr != fcsr_before) {
		uint32_t rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;

		fprintf(stderr, "rv: fp check: instruction %08x at pc 0x%llx: f%u=%016llx f%u=%016llx x%u=%016llx fcsr %x\n"
			"  the host: f %016llx x %016llx fcsr %x\n  SoftFloat: f %016llx x %016llx fcsr %x\n",
			insn, (unsigned long long)cpu->pc, rs1, (unsigned long long)(rd == rs1 ? f_before : cpu->f[rs1]),
			rs2, (unsigned long long)(rd == rs2 ? f_before : cpu->f[rs2]), rs1,
			(unsigned long long)(rd == rs1 ? x_before : cpu->x[rs1]), fcsr_before,
			(unsigned long long)f_fast, (unsigned long long)x_fast, fcsr_before,
			(unsigned long long)cpu->f[rd], (unsigned long long)cpu->x[rd], cpu->fcsr);
		_exit(98);
	}
	return 1;
}

/* ------------------------------------------------------------- SoftFloat's */

static int fp_op(struct rv_cpu *cpu, uint32_t insn)
{
	uint32_t f7 = insn >> 25, f3 = (insn >> 12) & 7;
	uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
	int dbl = f7 & 1;
	float32_t a32 = get_f32(cpu, rs1), b32 = get_f32(cpu, rs2), r32;
	float64_t a64 = get_f64(cpu, rs1), b64 = get_f64(cpu, rs2), r64;
	uint64_t x = cpu->x[rs1];
	int rm;

	if (f7 & 2)
		return -1;	/* half and quad precision are not implemented */

	switch (f7 >> 2) {
	case 0x00: /* FADD */
	case 0x01: /* FSUB */
	case 0x02: /* FMUL */
	case 0x03: /* FDIV */
	case 0x0b: /* FSQRT */
		rm = rounding_mode(cpu, f3);
		if (rm < 0 || ((f7 >> 2) == 0x0b && rs2 != 0))
			return -1;
		softfloat_roundingMode = (uint_fast8_t)rm;
		if (dbl) {
			switch (f7 >> 2) {
			case 0x00: r64 = f64_add(a64, b64); break;
			case 0x01: r64 = f64_sub(a64, b64); break;
			case 0x02: r64 = f64_mul(a64, b64); break;
			case 0x03: r64 = f64_div(a64, b64); break;
			default:   r64 = f64_sqrt(a64); break;
			}
			cpu->f[rd] = r64.v;
		} else {
			switch (f7 >> 2) {
			case 0x00: r32 = f32_add(a32, b32); break;
			case 0x01: r32 = f32_sub(a32, b32); break;
			case 0x02: r32 = f32_mul(a32, b32); break;
			case 0x03: r32 = f32_div(a32, b32); break;
			default:   r32 = f32_sqrt(a32); break;
			}
			cpu->f[rd] = box(r32.v);
		}
		return 0;

	case 0x04: /* FSGNJ, FSGNJN, FSGNJX: magnitude of rs1, sign from rs2 */
		if (f3 > 2)
			return -1;
		if (dbl) {
			uint64_t sign = f3 == 0 ? b64.v : f3 == 1 ? ~b64.v : a64.v ^ b64.v;

			cpu->f[rd] = (a64.v & ~F64_SIGN) | (sign & F64_SIGN);
		} else {
			uint32_t sign = f3 == 0 ? b32.v : f3 == 1 ? ~b32.v : a32.v ^ b32.v;

			cpu->f[rd] = box((a32.v & ~F32_SIGN) | (sign & F32_SIGN));
		}
		return 0;

	case 0x05: /* FMIN, FMAX */
		if (f3 > 1)
			return -1;
		if (dbl)
			cpu->f[rd] = minmax64(a64, b64, (int)f3);
		else
			cpu->f[rd] = box(minmax32(a32, b32, (int)f3));
		return 0;

	case 0x08: /* FCVT.S.D, FCVT.D.S */
		rm = rounding_mode(cpu, f3);
		if (rm < 0)
			return -1;
		softfloat_roundingMode = (uint_fast8_t)rm;
		if (!dbl && rs2 == 1)
			cpu->f[rd] = box(f64_to_f32(a64).v);
		else if (dbl && rs2 == 0)
			cpu->f[rd] = f32_to_f64(a32).v;
		else
			return -1;
		return 0;

	case 0x14: /* FLE, FLT, FEQ: only FEQ is quiet on quiet NaNs */
		switch (f3) {
		case 0: cpu->x[rd] = dbl ? f64_le(a64, b64) : f32_le(a32, b32); break;
		case 1: cpu->x[rd] = dbl ? f64_lt(a64, b64) : f32_lt(a32, b32); break;
		case 2: cpu->x[rd] = dbl ? f64_eq(a64, b64) : f32_eq(a32, b32); break;
		default: return -1;
		}
		return 0;

	case 0x18: /* FCVT.{W,WU,L,LU}.{S,D}: float to integer */
		rm = rounding_mode(cpu, f3);
		if (rm < 0)
			return -1;
		switch (rs2) {
		case 0: cpu->x[rd] = (uint64_t)(int64_t)(int32_t)(dbl ? f64_to_i32(a64, (uint_fast8_t)rm, true)
								       : f32_to_i32(a32, (uint_fast8_t)rm, true)); break;
		case 1: cpu->x[rd] = (uint64_t)(int64_t)(int32_t)(dbl ? f64_to_ui32(a64, (uint_fast8_t)rm, true)
								       : f32_to_ui32(a32, (uint_fast8_t)rm, true)); break;
		case 2: cpu->x[rd] = (uint64_t)(dbl ? f64_to_i64(a64, (uint_fast8_t)rm, true)
						    : f32_to_i64(a32, (uint_fast8_t)rm, true)); break;
		case 3: cpu->x[rd] = dbl ? f64_to_ui64(a64, (uint_fast8_t)rm, true)
					 : f32_to_ui64(a32, (uint_fast8_t)rm, true); break;
		default: return -1;
		}
		return 0;

	case 0x1a: /* FCVT.{S,D}.{W,WU,L,LU}: integer to float */
		rm = rounding_mode(cpu, f3);
		if (rm < 0)
			return -1;
		softfloat_roundingMode = (uint_fast8_t)rm;
		if (dbl) {
			switch (rs2) {
			case 0: r64 = i32_to_f64((int32_t)x); break;
			case 1: r64 = ui32_to_f64((uint32_t)x); break;
			case 2: r64 = i64_to_f64((int64_t)x); break;
			case 3: r64 = ui64_to_f64(x); break;
			default: return -1;
			}
			cpu->f[rd] = r64.v;
		} else {
			switch (rs2) {
			case 0: r32 = i32_to_f32((int32_t)x); break;
			case 1: r32 = ui32_to_f32((uint32_t)x); break;
			case 2: r32 = i64_to_f32((int64_t)x); break;
			case 3: r32 = ui64_to_f32(x); break;
			default: return -1;
			}
			cpu->f[rd] = box(r32.v);
		}
		return 0;

	case 0x1c: /* FMV.X.W / FMV.X.D (raw bits), FCLASS */
		if (rs2 != 0)
			return -1;
		if (f3 == 0)
			cpu->x[rd] = dbl ? cpu->f[rs1] : (uint64_t)(int64_t)(int32_t)cpu->f[rs1];
		else if (f3 == 1)
			cpu->x[rd] = dbl ? fclass64(a64.v) : fclass32(a32.v);
		else
			return -1;
		return 0;

	case 0x1e: /* FMV.W.X / FMV.D.X */
		if (rs2 != 0 || f3 != 0)
			return -1;
		cpu->f[rd] = dbl ? x : box((uint32_t)x);
		return 0;

	default:
		return -1;
	}
}

/* Returns 0 if the instruction was executed, -1 if it is not a valid FP instruction. */
int rv_exec_fp(struct rv_cpu *cpu, uint32_t insn)
{
	int ret;

	switch (insn & 0x7f) {
	case 0x07:
	case 0x27:
		return fp_load_store(cpu, insn);
	case 0x53:
		if (__builtin_expect(fp_check < 0, 0))
			fp_check = getenv("RV_FP_CHECK") != NULL;
		if (fp_check ? checked_fast(cpu, insn) : fp_fast(cpu, insn))
			return 0;
		break;
	case 0x43:
	case 0x47:
	case 0x4b:
	case 0x4f:
		if (__builtin_expect(fp_check < 0, 0))
			fp_check = getenv("RV_FP_CHECK") != NULL;
		if (fp_check ? checked_fast(cpu, insn) : fused_fast(cpu, insn))
			return 0;
		break;
	}

	softfloat_exceptionFlags = 0;
	switch (insn & 0x7f) {
	case 0x43:
	case 0x47:
	case 0x4b:
	case 0x4f:
		ret = fp_fused(cpu, insn);
		break;
	default:
		ret = fp_op(cpu, insn);
	}
	/* Exception flags are sticky: they accumulate until the guest clears them. */
	cpu->fcsr |= softfloat_exceptionFlags & FFLAGS_MASK;
	return ret;
}

/*
 * Zicsr: CSRRW, CSRRS, CSRRC and their immediate forms, for the CSRs user
 * code can reach. Returns 0, or -1 for an illegal instruction.
 */
int rv_csr(struct rv_cpu *cpu, uint32_t insn)
{
	uint32_t csr = insn >> 20, f3 = (insn >> 12) & 7;
	uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31;
	uint64_t arg = (f3 & 4) ? rs1 : cpu->x[rs1];
	uint32_t op = f3 & 3;
	/* CSRRS/CSRRC with a zero source only read; CSRRW always writes. */
	int writes = op == 1 || rs1 != 0;
	uint64_t old, val;
	struct timespec ts;

	if (op == 0)
		return -1;

	switch (csr) {
	case CSR_FFLAGS:
		old = cpu->fcsr & FFLAGS_MASK;
		break;
	case CSR_FRM:
		old = (cpu->fcsr >> FRM_SHIFT) & 7;
		break;
	case CSR_FCSR:
		old = cpu->fcsr & 0xff;
		break;
	case CSR_CYCLE:
	case CSR_INSTRET:
		old = cpu->insn_count;
		break;
	case CSR_TIME:
		clock_gettime(CLOCK_MONOTONIC, &ts);
		old = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
		break;
	default:
		return -1;
	}

	if (writes) {
		val = op == 1 ? arg : op == 2 ? old | arg : old & ~arg;
		switch (csr) {
		case CSR_FFLAGS:
			cpu->fcsr = (cpu->fcsr & ~(uint32_t)FFLAGS_MASK) | (uint32_t)(val & FFLAGS_MASK);
			break;
		case CSR_FRM:
			cpu->fcsr = (cpu->fcsr & FFLAGS_MASK) | (uint32_t)((val & 7) << FRM_SHIFT);
			break;
		case CSR_FCSR:
			cpu->fcsr = (uint32_t)(val & 0xff);
			break;
		default:
			return -1;	/* the counters are read-only */
		}
	}

	cpu->x[rd] = old;
	return 0;
}

/*
 * For translated code (jit.c), which keeps the integer registers in locals
 * and so cannot let the functions above read and write cpu->x[] on their own.
 * The floating-point registers and fcsr are always in *cpu, so those need no
 * such care.
 */

/* Does this OP-FP instruction read integer register rs1 (FCVT from an integer, FMV.x.X)? */
int rv_fp_int_source(uint32_t insn)
{
	return (insn & 0x7f) == 0x53 && (insn >> 27 == 0x1a || insn >> 27 == 0x1e);
}

/* Does it write integer register rd (comparisons, FCVT to an integer, FMV.X.x, FCLASS)? */
int rv_fp_int_dest(uint32_t insn)
{
	return (insn & 0x7f) == 0x53 && (insn >> 27 == 0x14 || insn >> 27 == 0x18 || insn >> 27 == 0x1c);
}

/*
 * Execute one floating-point instruction (not a load or store). `a` is the
 * value of integer register rs1 if the instruction reads it; the result is
 * the new value of integer register rd if it writes that. cpu->jit_fault
 * says whether the instruction was invalid, in which case nothing changed.
 */
uint64_t rv_jit_fp(struct rv_cpu *cpu, uint64_t insn, uint64_t a)
{
	uint32_t i = (uint32_t)insn;
	uint64_t result = 0;

	if (rv_fp_int_source(i))
		cpu->x[(i >> 15) & 31] = a;
	if (__builtin_expect(cpu->verbose, 0))
		rv_jit_count_fp(cpu, i);
	cpu->jit_fault = rv_exec_fp(cpu, i) != 0;
	if (rv_fp_int_dest(i))
		result = cpu->x[(i >> 7) & 31];
	cpu->x[0] = 0;
	return result;
}

/* The same for a Zicsr instruction; the result is the new value of rd. */
uint64_t rv_jit_csr(struct rv_cpu *cpu, uint64_t insn, uint64_t a)
{
	uint32_t i = (uint32_t)insn;
	uint64_t result;

	if (!(i & 0x4000))	/* the register forms; the others have an immediate */
		cpu->x[(i >> 15) & 31] = a;
	cpu->jit_fault = rv_csr(cpu, i) != 0;
	result = cpu->x[(i >> 7) & 31];
	cpu->x[0] = 0;
	return result;
}
