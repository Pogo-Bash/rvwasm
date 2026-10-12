/*
 * The F and D extensions: every arithmetic instruction in every rounding
 * mode (static and dynamic), conversions, comparisons, sign injection,
 * min/max, classification, fused multiply-add and NaN boxing, with the
 * accumulated exception flags read back after every operation.
 * Build with -march=rv64imafd -mabi=lp64d.
 *
 * Instructions are written out in assembly so that the test covers exactly
 * the instruction named, whatever the compiler would have chosen.
 */
#include "guest.h"

static inline double d_from(u64 v)
{
	union { u64 u; double d; } x = { .u = v };
	return x.d;
}

static inline u64 d_bits(double d)
{
	union { double d; u64 u; } x = { .d = d };
	return x.u;
}

static inline float s_from(u32 v)
{
	union { u32 u; float f; } x = { .u = v };
	return x.f;
}

static inline u64 s_bits(float f)
{
	union { float f; u32 u; } x = { .f = f };
	return x.u;
}

/* Read the accumulated exception flags and clear them. */
static u64 take_flags(void)
{
	u64 flags;

	__asm__ volatile("frflags %0\n\tfsflags x0" : "=r"(flags));
	return flags;
}

static void set_rm(u64 rm)
{
	__asm__ volatile("fsrm %0" : : "r"(rm));
}

#define D2(name, text)							\
static double name(double a, double b)					\
{									\
	double r;							\
	__asm__ volatile(text : "=f"(r) : "f"(a), "f"(b));		\
	return r;							\
}
#define S2(name, text)							\
static float name(float a, float b)					\
{									\
	float r;							\
	__asm__ volatile(text : "=f"(r) : "f"(a), "f"(b));		\
	return r;							\
}
#define D1(name, text)							\
static double name(double a)						\
{									\
	double r;							\
	__asm__ volatile(text : "=f"(r) : "f"(a));			\
	return r;							\
}
#define S1(name, text)							\
static float name(float a)						\
{									\
	float r;							\
	__asm__ volatile(text : "=f"(r) : "f"(a));			\
	return r;							\
}
/* Float to integer register. */
#define D2X(name, text)							\
static u64 name(double a)						\
{									\
	u64 r;								\
	__asm__ volatile(text : "=r"(r) : "f"(a));			\
	return r;							\
}
#define S2X(name, text)							\
static u64 name(float a)						\
{									\
	u64 r;								\
	__asm__ volatile(text : "=r"(r) : "f"(a));			\
	return r;							\
}
/* Integer register to float. */
#define X2D(name, text)							\
static double name(u64 a)						\
{									\
	double r;							\
	__asm__ volatile(text : "=f"(r) : "r"(a));			\
	return r;							\
}
#define X2S(name, text)							\
static float name(u64 a)						\
{									\
	float r;							\
	__asm__ volatile(text : "=f"(r) : "r"(a));			\
	return r;							\
}
/* Two floats to an integer register (comparisons). */
#define DD2X(name, text)						\
static u64 name(double a, double b)					\
{									\
	u64 r;								\
	__asm__ volatile(text : "=r"(r) : "f"(a), "f"(b));		\
	return r;							\
}
#define SS2X(name, text)						\
static u64 name(float a, float b)					\
{									\
	u64 r;								\
	__asm__ volatile(text : "=r"(r) : "f"(a), "f"(b));		\
	return r;							\
}
#define D3(name, text)							\
static double name(double a, double b, double c)			\
{									\
	double r;							\
	__asm__ volatile(text : "=f"(r) : "f"(a), "f"(b), "f"(c));	\
	return r;							\
}
#define S3(name, text)							\
static float name(float a, float b, float c)				\
{									\
	float r;							\
	__asm__ volatile(text : "=f"(r) : "f"(a), "f"(b), "f"(c));	\
	return r;							\
}

/* One set of everything that takes a rounding mode, per mode. */
#define PER_MODE(rm)							\
	D2(d_add_##rm, "fadd.d %0,%1,%2," #rm)				\
	D2(d_sub_##rm, "fsub.d %0,%1,%2," #rm)				\
	D2(d_mul_##rm, "fmul.d %0,%1,%2," #rm)				\
	D2(d_div_##rm, "fdiv.d %0,%1,%2," #rm)				\
	S2(s_add_##rm, "fadd.s %0,%1,%2," #rm)				\
	S2(s_sub_##rm, "fsub.s %0,%1,%2," #rm)				\
	S2(s_mul_##rm, "fmul.s %0,%1,%2," #rm)				\
	S2(s_div_##rm, "fdiv.s %0,%1,%2," #rm)				\
	D1(d_sqrt_##rm, "fsqrt.d %0,%1," #rm)				\
	S1(s_sqrt_##rm, "fsqrt.s %0,%1," #rm)				\
	D2X(d_to_w_##rm, "fcvt.w.d %0,%1," #rm)				\
	D2X(d_to_wu_##rm, "fcvt.wu.d %0,%1," #rm)			\
	D2X(d_to_l_##rm, "fcvt.l.d %0,%1," #rm)				\
	D2X(d_to_lu_##rm, "fcvt.lu.d %0,%1," #rm)			\
	S2X(s_to_w_##rm, "fcvt.w.s %0,%1," #rm)				\
	S2X(s_to_wu_##rm, "fcvt.wu.s %0,%1," #rm)			\
	S2X(s_to_l_##rm, "fcvt.l.s %0,%1," #rm)				\
	S2X(s_to_lu_##rm, "fcvt.lu.s %0,%1," #rm)			\
	X2D(l_to_d_##rm, "fcvt.d.l %0,%1," #rm)				\
	X2D(lu_to_d_##rm, "fcvt.d.lu %0,%1," #rm)			\
	X2S(w_to_s_##rm, "fcvt.s.w %0,%1," #rm)				\
	X2S(wu_to_s_##rm, "fcvt.s.wu %0,%1," #rm)			\
	X2S(l_to_s_##rm, "fcvt.s.l %0,%1," #rm)				\
	X2S(lu_to_s_##rm, "fcvt.s.lu %0,%1," #rm)			\
	static float d_to_s_##rm(double a)				\
	{								\
		float r;						\
		__asm__ volatile("fcvt.s.d %0,%1," #rm : "=f"(r) : "f"(a)); \
		return r;						\
	}								\
	D3(d_madd_##rm, "fmadd.d %0,%1,%2,%3," #rm)			\
	D3(d_msub_##rm, "fmsub.d %0,%1,%2,%3," #rm)			\
	D3(d_nmsub_##rm, "fnmsub.d %0,%1,%2,%3," #rm)			\
	D3(d_nmadd_##rm, "fnmadd.d %0,%1,%2,%3," #rm)			\
	S3(s_madd_##rm, "fmadd.s %0,%1,%2,%3," #rm)			\
	S3(s_msub_##rm, "fmsub.s %0,%1,%2,%3," #rm)			\
	S3(s_nmsub_##rm, "fnmsub.s %0,%1,%2,%3," #rm)			\
	S3(s_nmadd_##rm, "fnmadd.s %0,%1,%2,%3," #rm)

PER_MODE(rne) PER_MODE(rtz) PER_MODE(rdn) PER_MODE(rup) PER_MODE(rmm) PER_MODE(dyn)

#define MODES(prefix) prefix##rne, prefix##rtz, prefix##rdn, prefix##rup, prefix##rmm, prefix##dyn
#define NMODES 6

static double (*const d_arith[])(double, double) = {
	MODES(d_add_), MODES(d_sub_), MODES(d_mul_), MODES(d_div_),
};
static float (*const s_arith[])(float, float) = {
	MODES(s_add_), MODES(s_sub_), MODES(s_mul_), MODES(s_div_),
};
static double (*const d_sqrt[])(double) = { MODES(d_sqrt_) };
static float (*const s_sqrt[])(float) = { MODES(s_sqrt_) };
static u64 (*const d_to_x[])(double) = {
	MODES(d_to_w_), MODES(d_to_wu_), MODES(d_to_l_), MODES(d_to_lu_),
};
static u64 (*const s_to_x[])(float) = {
	MODES(s_to_w_), MODES(s_to_wu_), MODES(s_to_l_), MODES(s_to_lu_),
};
static double (*const x_to_d[])(u64) = { MODES(l_to_d_), MODES(lu_to_d_) };
static float (*const x_to_s[])(u64) = {
	MODES(w_to_s_), MODES(wu_to_s_), MODES(l_to_s_), MODES(lu_to_s_),
};
static float (*const d_to_s[])(double) = { MODES(d_to_s_) };
static double (*const d_fused[])(double, double, double) = {
	MODES(d_madd_), MODES(d_msub_), MODES(d_nmsub_), MODES(d_nmadd_),
};
static float (*const s_fused[])(float, float, float) = {
	MODES(s_madd_), MODES(s_msub_), MODES(s_nmsub_), MODES(s_nmadd_),
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Exact conversions and everything without a rounding mode. */
X2D(w_to_d, "fcvt.d.w %0,%1")
X2D(wu_to_d, "fcvt.d.wu %0,%1")
static double s_to_d(float a)
{
	double r;

	__asm__ volatile("fcvt.d.s %0,%1" : "=f"(r) : "f"(a));
	return r;
}
D2(d_sgnj, "fsgnj.d %0,%1,%2") D2(d_sgnjn, "fsgnjn.d %0,%1,%2") D2(d_sgnjx, "fsgnjx.d %0,%1,%2")
D2(d_min, "fmin.d %0,%1,%2")   D2(d_max, "fmax.d %0,%1,%2")
S2(s_sgnj, "fsgnj.s %0,%1,%2") S2(s_sgnjn, "fsgnjn.s %0,%1,%2") S2(s_sgnjx, "fsgnjx.s %0,%1,%2")
S2(s_min, "fmin.s %0,%1,%2")   S2(s_max, "fmax.s %0,%1,%2")
DD2X(d_eq, "feq.d %0,%1,%2") DD2X(d_lt, "flt.d %0,%1,%2") DD2X(d_le, "fle.d %0,%1,%2")
SS2X(s_eq, "feq.s %0,%1,%2") SS2X(s_lt, "flt.s %0,%1,%2") SS2X(s_le, "fle.s %0,%1,%2")
D2X(d_class, "fclass.d %0,%1") S2X(s_class, "fclass.s %0,%1")
S2X(s_mv_x, "fmv.x.w %0,%1")

static double (*const d_misc[])(double, double) = { d_sgnj, d_sgnjn, d_sgnjx, d_min, d_max };
static float (*const s_misc[])(float, float) = { s_sgnj, s_sgnjn, s_sgnjx, s_min, s_max };
static u64 (*const d_cmp[])(double, double) = { d_eq, d_lt, d_le };
static u64 (*const s_cmp[])(float, float) = { s_eq, s_lt, s_le };

/*
 * A 64-bit pattern put into an FP register and then used as a single: unless
 * its upper half is all ones it must read as the canonical NaN.
 */
static u64 single_from_raw(u64 raw)
{
	u64 r;

	__asm__ volatile("fmv.d.x ft0,%1\n\tfadd.s ft1,ft0,ft0\n\tfmv.x.d %0,ft1"
			 : "=r"(r) : "r"(raw) : "ft0", "ft1");
	return r;
}

static const u64 dvals[] = {
	0x0000000000000000,	/* +0 */
	0x8000000000000000,	/* -0 */
	0x3ff0000000000000,	/* 1 */
	0xbff0000000000000,	/* -1 */
	0x3ff8000000000000,	/* 1.5 */
	0x4004000000000000,	/* 2.5: ties show RNE against RMM */
	0xc004000000000000,	/* -2.5 */
	0x3fb999999999999a,	/* 0.1 */
	0x400921fb54442d18,	/* pi */
	0x4340000000000001,	/* just above 2^53: not exactly representable as a single */
	0x41dfffffffc00000,	/* 2^31 - 1 */
	0x41e0000000000000,	/* 2^31 */
	0x43e0000000000000,	/* 2^63 */
	0x43f0000000000000,	/* 2^64 */
	0xc3e0000000000001,	/* just below -2^63 */
	0x7fefffffffffffff,	/* largest finite */
	0x0010000000000000,	/* smallest normal */
	0x000fffffffffffff,	/* largest subnormal */
	0x0000000000000001,	/* smallest subnormal */
	0x7e37e43c8800759c,	/* 1e300 */
	0x01a56e1fc2f8f359,	/* 1e-300 */
	0x7ff0000000000000,	/* +inf */
	0xfff0000000000000,	/* -inf */
	0x7ff8000000000000,	/* quiet NaN */
	0xfff8000000001234,	/* quiet NaN, sign and payload set */
	0x7ff0000000000001,	/* signalling NaN */
};
#define NDVALS COUNT(dvals)

static const u32 svals[] = {
	0x00000000, 0x80000000,	/* +-0 */
	0x3f800000, 0xbf800000,	/* +-1 */
	0x3fc00000,		/* 1.5 */
	0x40200000, 0xc0200000,	/* +-2.5 */
	0x3dcccccd,		/* 0.1 */
	0x40490fdb,		/* pi */
	0x4b800001,		/* just above 2^24 */
	0x4f000000,		/* 2^31 */
	0x5f000000,		/* 2^63 */
	0x7f7fffff,		/* largest finite */
	0x00800000,		/* smallest normal */
	0x007fffff,		/* largest subnormal */
	0x00000001,		/* smallest subnormal */
	0x7f800000, 0xff800000,	/* +-inf */
	0x7fc00000,		/* quiet NaN */
	0xffc01234,		/* quiet NaN, sign and payload set */
	0x7f800001,		/* signalling NaN */
};
#define NSVALS COUNT(svals)

static const u64 xvals[] = {
	0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000001,
	0x20000000000001,	/* 2^53 + 1: rounds when converted to double */
	0x1000001,		/* 2^24 + 1: rounds when converted to single */
	0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff,
	0xfffffffffffffc01,
};
#define NXVALS COUNT(xvals)

/* The pairwise tests walk a subset; everything else uses the full lists. */
#define PAIR_STEP 2

static u64 test_arith_d(void)
{
	u64 h = MIX_INIT;

	for (unsigned i = 0; i < NDVALS; i += PAIR_STEP) {
		for (unsigned j = 0; j < NDVALS; j++) {
			double a = d_from(dvals[i]), b = d_from(dvals[j]);

			set_rm((i + j) % 5);	/* what the "dyn" variants will use */
			for (unsigned op = 0; op < COUNT(d_arith); op++) {
				h = mix(h, d_bits(d_arith[op](a, b)));
				h = mix(h, take_flags());
			}
			for (unsigned op = 0; op < COUNT(d_misc); op++) {
				h = mix(h, d_bits(d_misc[op](a, b)));
				h = mix(h, take_flags());
			}
			for (unsigned op = 0; op < COUNT(d_cmp); op++) {
				h = mix(h, d_cmp[op](a, b));
				h = mix(h, take_flags());
			}
		}
	}
	return h;
}

static u64 test_arith_s(void)
{
	u64 h = MIX_INIT;

	for (unsigned i = 0; i < NSVALS; i += PAIR_STEP) {
		for (unsigned j = 0; j < NSVALS; j++) {
			float a = s_from(svals[i]), b = s_from(svals[j]);

			set_rm((i + j) % 5);
			for (unsigned op = 0; op < COUNT(s_arith); op++) {
				h = mix(h, s_bits(s_arith[op](a, b)));
				h = mix(h, take_flags());
			}
			for (unsigned op = 0; op < COUNT(s_misc); op++) {
				h = mix(h, s_bits(s_misc[op](a, b)));
				h = mix(h, take_flags());
			}
			for (unsigned op = 0; op < COUNT(s_cmp); op++) {
				h = mix(h, s_cmp[op](a, b));
				h = mix(h, take_flags());
			}
		}
	}
	return h;
}

static u64 test_unary(void)
{
	u64 h = MIX_INIT;

	for (unsigned i = 0; i < NDVALS; i++) {
		double a = d_from(dvals[i]);

		set_rm(i % 5);
		for (unsigned op = 0; op < COUNT(d_sqrt); op++) {
			h = mix(h, d_bits(d_sqrt[op](a)));
			h = mix(h, take_flags());
		}
		for (unsigned op = 0; op < COUNT(d_to_x); op++) {
			h = mix(h, d_to_x[op](a));
			h = mix(h, take_flags());
		}
		for (unsigned op = 0; op < COUNT(d_to_s); op++) {
			h = mix(h, s_bits(d_to_s[op](a)));
			h = mix(h, take_flags());
		}
		h = mix(h, d_class(a));
	}
	for (unsigned i = 0; i < NSVALS; i++) {
		float a = s_from(svals[i]);

		set_rm(i % 5);
		for (unsigned op = 0; op < COUNT(s_sqrt); op++) {
			h = mix(h, s_bits(s_sqrt[op](a)));
			h = mix(h, take_flags());
		}
		for (unsigned op = 0; op < COUNT(s_to_x); op++) {
			h = mix(h, s_to_x[op](a));
			h = mix(h, take_flags());
		}
		h = mix(h, d_bits(s_to_d(a)));
		h = mix(h, take_flags());
		h = mix(h, s_class(a));
		h = mix(h, s_mv_x(a));
	}
	return h;
}

static u64 test_from_int(void)
{
	u64 h = MIX_INIT;

	for (unsigned i = 0; i < NXVALS; i++) {
		set_rm(i % 5);
		for (unsigned op = 0; op < COUNT(x_to_d); op++) {
			h = mix(h, d_bits(x_to_d[op](xvals[i])));
			h = mix(h, take_flags());
		}
		for (unsigned op = 0; op < COUNT(x_to_s); op++) {
			h = mix(h, s_bits(x_to_s[op](xvals[i])));
			h = mix(h, take_flags());
		}
		h = mix(h, d_bits(w_to_d(xvals[i])));
		h = mix(h, d_bits(wu_to_d(xvals[i])));
		h = mix(h, single_from_raw(xvals[i]));
		h = mix(h, single_from_raw(0xffffffff00000000 | (u32)xvals[i]));
		h = mix(h, take_flags());
	}
	return h;
}

static u64 test_fused(void)
{
	u64 h = MIX_INIT;

	for (unsigned i = 0; i < NDVALS; i += 3) {
		for (unsigned j = 1; j < NDVALS; j += 3) {
			for (unsigned k = 2; k < NDVALS; k += 4) {
				double a = d_from(dvals[i]), b = d_from(dvals[j]), c = d_from(dvals[k]);

				set_rm((i + j + k) % 5);
				for (unsigned op = 0; op < COUNT(d_fused); op++) {
					h = mix(h, d_bits(d_fused[op](a, b, c)));
					h = mix(h, take_flags());
				}
			}
		}
	}
	for (unsigned i = 0; i < NSVALS; i += 3) {
		for (unsigned j = 1; j < NSVALS; j += 3) {
			for (unsigned k = 2; k < NSVALS; k += 4) {
				float a = s_from(svals[i]), b = s_from(svals[j]), c = s_from(svals[k]);

				set_rm((i + j + k) % 5);
				for (unsigned op = 0; op < COUNT(s_fused); op++) {
					h = mix(h, s_bits(s_fused[op](a, b, c)));
					h = mix(h, take_flags());
				}
			}
		}
	}
	return h;
}

/* The CSRs themselves: fflags, frm and their combined view fcsr. */
static void test_csr(void)
{
	u64 v;

	__asm__ volatile("li t0, 0xff\n\tfscsr t0\n\tfrcsr %0" : "=r"(v) : : "t0");
	put_hex("fcsr_all", v);
	__asm__ volatile("frflags %0" : "=r"(v));
	put_hex("fflags_view", v);
	__asm__ volatile("frrm %0" : "=r"(v));
	put_hex("frm_view", v);
	__asm__ volatile("li t0, 0x3\n\tfsrm t0\n\tli t0, 0x15\n\tfsflags t0\n\tfrcsr %0" : "=r"(v) : : "t0");
	put_hex("fcsr_combined", v);
	/* csrrs/csrrc set and clear single bits; with x0 they only read. */
	__asm__ volatile("li t0, 0x0a\n\tcsrrs %0, fflags, t0" : "=r"(v) : : "t0");
	put_hex("csrrs_old", v);
	__asm__ volatile("csrrci %0, fflags, 0x11" : "=r"(v));
	put_hex("csrrci_old", v);
	__asm__ volatile("csrrs %0, fcsr, x0" : "=r"(v));
	put_hex("fcsr_final", v);
	__asm__ volatile("fscsr x0");
}

void _start(void)
{
	test_csr();
	put_hex("arith_d", test_arith_d());
	put_hex("arith_s", test_arith_s());
	put_hex("unary", test_unary());
	put_hex("from_int", test_from_int());
	put_hex("fused", test_fused());
	finish();
}
