/*
 * The A extension: every AMO in both widths, LR/SC success and the ways SC
 * must fail, and the compiler's own atomic builtins (which expand to LR/SC
 * loops). Build with -march=rv64ima.
 */
#include "guest.h"

/* amoXXX rd, rs2, (rs1): rd gets the old value, memory gets old OP rs2. */
#define AMO(name, insn)							\
static u64 name(void *p, u64 v)						\
{									\
	u64 r;								\
	__asm__ volatile(insn " %0, %2, (%1)"				\
			 : "=&r"(r) : "r"(p), "r"(v) : "memory");	\
	return r;							\
}

AMO(amoswap_w, "amoswap.w") AMO(amoswap_d, "amoswap.d")
AMO(amoadd_w, "amoadd.w")   AMO(amoadd_d, "amoadd.d")
AMO(amoxor_w, "amoxor.w")   AMO(amoxor_d, "amoxor.d")
AMO(amoand_w, "amoand.w")   AMO(amoand_d, "amoand.d")
AMO(amoor_w, "amoor.w")     AMO(amoor_d, "amoor.d")
AMO(amomin_w, "amomin.w")   AMO(amomin_d, "amomin.d")
AMO(amomax_w, "amomax.w")   AMO(amomax_d, "amomax.d")
AMO(amominu_w, "amominu.w") AMO(amominu_d, "amominu.d")
AMO(amomaxu_w, "amomaxu.w") AMO(amomaxu_d, "amomaxu.d")

static u64 (*const amos[])(void *, u64) = {
	amoswap_w, amoswap_d, amoadd_w, amoadd_d, amoxor_w, amoxor_d,
	amoand_w, amoand_d, amoor_w, amoor_d, amomin_w, amomin_d,
	amomax_w, amomax_d, amominu_w, amominu_d, amomaxu_w, amomaxu_d,
};
#define NAMOS (sizeof(amos) / sizeof(amos[0]))

static const u64 values[] = {
	0, 1, 0x7f, 0x80, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000,
	0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff,
	0x0123456789abcdef, 0xfedcba9876543210,
};
#define NVALUES (sizeof(values) / sizeof(values[0]))

static u64 test_amo(void)
{
	u64 h = MIX_INIT;

	for (unsigned op = 0; op < NAMOS; op++) {
		for (unsigned i = 0; i < NVALUES; i++) {
			for (unsigned j = 0; j < NVALUES; j++) {
				/* Neighbours on both sides catch a 32-bit op writing 64 bits. */
				volatile u64 cell[3] = { ~0UL, values[i], ~0UL };

				h = mix(h, amos[op]((void *)&cell[1], values[j]));
				h = mix(h, cell[0]);
				h = mix(h, cell[1]);
				h = mix(h, cell[2]);
			}
		}
	}
	return h;
}

/* lr then sc on the same address: must succeed (sc writes 0 to its rd). */
static u64 lr_sc_d(u64 *p, u64 v, u64 *loaded)
{
	u64 fail, old;

	__asm__ volatile("lr.d %1, (%2)\n\tsc.d %0, %3, (%2)"
			 : "=&r"(fail), "=&r"(old) : "r"(p), "r"(v) : "memory");
	*loaded = old;
	return fail;
}

static u64 lr_sc_w(u32 *p, u64 v, u64 *loaded)
{
	u64 fail, old;

	__asm__ volatile("lr.w %1, (%2)\n\tsc.w %0, %3, (%2)"
			 : "=&r"(fail), "=&r"(old) : "r"(p), "r"(v) : "memory");
	*loaded = old;
	return fail;
}

/* sc with no reservation at all. */
static u64 sc_alone(u64 *p, u64 v)
{
	u64 fail;

	__asm__ volatile("sc.d %0, %2, (%1)" : "=&r"(fail) : "r"(p), "r"(v) : "memory");
	return fail;
}

/* lr on one address, sc on another. */
static u64 lr_sc_other(u64 *p, u64 *q, u64 v)
{
	u64 fail, old;

	__asm__ volatile("lr.d %1, (%2)\n\tsc.d %0, %4, (%3)"
			 : "=&r"(fail), "=&r"(old) : "r"(p), "r"(q), "r"(v) : "memory");
	return fail;
}

static void test_lr_sc(void)
{
	u64 cell = 0x1111111111111111, other = 0x2222222222222222, loaded;
	u32 cell32 = 0x80000001;

	put_hex("lrsc_d_fail", lr_sc_d(&cell, 0xabcdef, &loaded));
	put_hex("lrsc_d_loaded", loaded);
	put_hex("lrsc_d_stored", cell);

	put_hex("lrsc_w_fail", lr_sc_w(&cell32, 0x7fffffff12345678, &loaded));
	put_hex("lrsc_w_loaded", loaded);	/* sign-extended */
	put_hex("lrsc_w_stored", cell32);

	/* The successful sc above consumed the reservation. */
	put_hex("sc_alone_fail", sc_alone(&cell, 0x99));
	put_hex("sc_alone_kept", cell);

	put_hex("sc_other_fail", lr_sc_other(&cell, &other, 0x77));
	put_hex("sc_other_kept", other);
}

/* What compilers actually emit for C11 atomics. */
static void test_builtins(void)
{
	u64 v64 = 10, expect64;
	u32 v32 = 0xfffffff0, expect32;

	put_hex("fetch_add64", __atomic_fetch_add(&v64, 5, __ATOMIC_SEQ_CST));
	put_hex("fetch_sub64", __atomic_fetch_sub(&v64, 20, __ATOMIC_SEQ_CST));
	put_hex("exchange64", __atomic_exchange_n(&v64, 42, __ATOMIC_SEQ_CST));
	expect64 = 42;
	put_hex("cas64_hit", __atomic_compare_exchange_n(&v64, &expect64, 43, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
	expect64 = 1;
	put_hex("cas64_miss", __atomic_compare_exchange_n(&v64, &expect64, 44, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
	put_hex("cas64_seen", expect64);
	put_hex("value64", v64);

	put_hex("fetch_add32", __atomic_fetch_add(&v32, 0x20, __ATOMIC_SEQ_CST));
	put_hex("fetch_or32", __atomic_fetch_or(&v32, 0xf00, __ATOMIC_SEQ_CST));
	expect32 = 0xf10;
	put_hex("cas32_hit", __atomic_compare_exchange_n(&v32, &expect32, 7, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
	put_hex("value32", v32);
}

void _start(void)
{
	put_hex("amo", test_amo());
	test_lr_sc();
	test_builtins();
	finish();
}
