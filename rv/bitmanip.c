/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The bit-manipulation extensions Zba, Zbb and Zbs (the B extension, version
 * 1.0.0, without Zbc's carry-less multiplication): address arithmetic
 * (sh1add and its kin, add.uw, slli.uw), counting and rotating bits (clz,
 * ctz, cpop, rol, ror), minimum and maximum, sign and zero extension, byte
 * reversal (rev8), orc.b, and setting, clearing, inverting and reading single
 * bits. JavaScript engines ask for them (riscv_hwprobe) and, when they are
 * there, generate shorter code: V8 decompresses a pointer with one sh*add.uw
 * or add.uw instead of a shift pair and an add.
 *
 * Here: which instruction an encoding is (rv_zb_kind()), and what it computes
 * (rv_zb_exec()), for the interpreter; jit.c translates the same kinds into
 * WebAssembly (gen_bitmanip()), and RV_JIT_CHECK compares the two.
 *
 * RV_BITMANIP=0 keeps the instructions but stops saying that the processor
 * has them (riscv_hwprobe, /proc/cpuinfo): programs that look then take their
 * code paths for plain RV64GC, for measuring what the extensions bring.
 * RV_ZB_COUNT=1 counts the instructions run, by kind, interpreted and
 * translated, and prints the counts when the program ends.
 */
#include <stdlib.h>
#include <string.h>

#include "rv.h"

const char *const rv_zb_names[RV_ZB_KINDS] = {
	[RV_ZB_ADD_UW] = "add.uw", [RV_ZB_SH1ADD] = "sh1add", [RV_ZB_SH2ADD] = "sh2add",
	[RV_ZB_SH3ADD] = "sh3add", [RV_ZB_SH1ADD_UW] = "sh1add.uw", [RV_ZB_SH2ADD_UW] = "sh2add.uw",
	[RV_ZB_SH3ADD_UW] = "sh3add.uw", [RV_ZB_SLLI_UW] = "slli.uw",
	[RV_ZB_ANDN] = "andn", [RV_ZB_ORN] = "orn", [RV_ZB_XNOR] = "xnor",
	[RV_ZB_CLZ] = "clz", [RV_ZB_CLZW] = "clzw", [RV_ZB_CTZ] = "ctz", [RV_ZB_CTZW] = "ctzw",
	[RV_ZB_CPOP] = "cpop", [RV_ZB_CPOPW] = "cpopw",
	[RV_ZB_MAX] = "max", [RV_ZB_MAXU] = "maxu", [RV_ZB_MIN] = "min", [RV_ZB_MINU] = "minu",
	[RV_ZB_SEXT_B] = "sext.b", [RV_ZB_SEXT_H] = "sext.h", [RV_ZB_ZEXT_H] = "zext.h",
	[RV_ZB_ROL] = "rol", [RV_ZB_ROLW] = "rolw", [RV_ZB_ROR] = "ror", [RV_ZB_RORW] = "rorw",
	[RV_ZB_RORI] = "rori", [RV_ZB_RORIW] = "roriw", [RV_ZB_ORC_B] = "orc.b", [RV_ZB_REV8] = "rev8",
	[RV_ZB_BCLR] = "bclr", [RV_ZB_BCLRI] = "bclri", [RV_ZB_BEXT] = "bext", [RV_ZB_BEXTI] = "bexti",
	[RV_ZB_BINV] = "binv", [RV_ZB_BINVI] = "binvi", [RV_ZB_BSET] = "bset", [RV_ZB_BSETI] = "bseti",
};

uint64_t rv_zb_count[RV_ZB_KINDS];
int rv_zb_counting = -1;

/*
 * Which of the instructions `insn` is, or 0 if it is none of them. Only the
 * exact encodings: what the specification leaves reserved stays an illegal
 * instruction.
 */
int rv_zb_kind(uint32_t insn)
{
	uint32_t f3 = (insn >> 12) & 7, f7 = insn >> 25, f6 = insn >> 26;
	uint32_t rs2 = (insn >> 20) & 31, imm = insn >> 20;

	switch (insn & 0x7f) {
	case 0x33: /* OP */
		switch (f7) {
		case 0x10:
			return f3 == 2 ? RV_ZB_SH1ADD : f3 == 4 ? RV_ZB_SH2ADD : f3 == 6 ? RV_ZB_SH3ADD : 0;
		case 0x20:
			return f3 == 4 ? RV_ZB_XNOR : f3 == 6 ? RV_ZB_ORN : f3 == 7 ? RV_ZB_ANDN : 0;
		case 0x05:
			return f3 == 4 ? RV_ZB_MIN : f3 == 5 ? RV_ZB_MINU : f3 == 6 ? RV_ZB_MAX :
			       f3 == 7 ? RV_ZB_MAXU : 0;
		case 0x30:
			return f3 == 1 ? RV_ZB_ROL : f3 == 5 ? RV_ZB_ROR : 0;
		case 0x24:
			return f3 == 1 ? RV_ZB_BCLR : f3 == 5 ? RV_ZB_BEXT : 0;
		case 0x34:
			return f3 == 1 ? RV_ZB_BINV : 0;
		case 0x14:
			return f3 == 1 ? RV_ZB_BSET : 0;
		}
		return 0;

	case 0x3b: /* OP-32 */
		switch (f7) {
		case 0x04:
			return f3 == 0 ? RV_ZB_ADD_UW : f3 == 4 && rs2 == 0 ? RV_ZB_ZEXT_H : 0;
		case 0x10:
			return f3 == 2 ? RV_ZB_SH1ADD_UW : f3 == 4 ? RV_ZB_SH2ADD_UW :
			       f3 == 6 ? RV_ZB_SH3ADD_UW : 0;
		case 0x30:
			return f3 == 1 ? RV_ZB_ROLW : f3 == 5 ? RV_ZB_RORW : 0;
		}
		return 0;

	case 0x13: /* OP-IMM: the shift amount is six bits, imm[5:0] */
		if (f3 == 1) {
			switch (imm) {
			case 0x600: return RV_ZB_CLZ;
			case 0x601: return RV_ZB_CTZ;
			case 0x602: return RV_ZB_CPOP;
			case 0x604: return RV_ZB_SEXT_B;
			case 0x605: return RV_ZB_SEXT_H;
			}
			return f6 == 0x12 ? RV_ZB_BCLRI : f6 == 0x1a ? RV_ZB_BINVI : f6 == 0x0a ? RV_ZB_BSETI : 0;
		}
		if (f3 == 5) {
			if (imm == 0x287)
				return RV_ZB_ORC_B;
			if (imm == 0x6b8)
				return RV_ZB_REV8;
			return f6 == 0x18 ? RV_ZB_RORI : f6 == 0x12 ? RV_ZB_BEXTI : 0;
		}
		return 0;

	case 0x1b: /* OP-IMM-32 */
		if (f3 == 1) {
			switch (imm) {
			case 0x600: return RV_ZB_CLZW;
			case 0x601: return RV_ZB_CTZW;
			case 0x602: return RV_ZB_CPOPW;
			}
			return f6 == 0x02 ? RV_ZB_SLLI_UW : 0;
		}
		if (f3 == 5)
			return f7 == 0x30 ? RV_ZB_RORIW : 0;
		return 0;
	}
	return 0;
}

static inline uint64_t sext32(uint64_t v)
{
	return (uint64_t)(int64_t)(int32_t)v;
}

static inline uint64_t rotr64(uint64_t v, unsigned n)
{
	n &= 63;
	return n ? v >> n | v << (64 - n) : v;
}

static inline uint32_t rotr32(uint32_t v, unsigned n)
{
	n &= 31;
	return n ? v >> n | v << (32 - n) : v;
}

/*
 * What instruction `insn`, of kind `kind`, computes from rs1's value `a`
 * and rs2's value `b`. (The immediate forms take theirs from the
 * instruction: the shift amount, the bit's index.)
 */
uint64_t rv_zb_exec(int kind, uint32_t insn, uint64_t a, uint64_t b)
{
	unsigned shamt = (insn >> 20) & 63;

	switch (kind) {
	case RV_ZB_ADD_UW:	return b + (uint32_t)a;
	case RV_ZB_SH1ADD:	return b + (a << 1);
	case RV_ZB_SH2ADD:	return b + (a << 2);
	case RV_ZB_SH3ADD:	return b + (a << 3);
	case RV_ZB_SH1ADD_UW:	return b + ((uint64_t)(uint32_t)a << 1);
	case RV_ZB_SH2ADD_UW:	return b + ((uint64_t)(uint32_t)a << 2);
	case RV_ZB_SH3ADD_UW:	return b + ((uint64_t)(uint32_t)a << 3);
	case RV_ZB_SLLI_UW:	return (uint64_t)(uint32_t)a << shamt;
	case RV_ZB_ANDN:	return a & ~b;
	case RV_ZB_ORN:		return a | ~b;
	case RV_ZB_XNOR:	return ~(a ^ b);
	case RV_ZB_CLZ:		return a ? (uint64_t)__builtin_clzll(a) : 64;
	case RV_ZB_CLZW:	return (uint32_t)a ? (uint64_t)__builtin_clz((uint32_t)a) : 32;
	case RV_ZB_CTZ:		return a ? (uint64_t)__builtin_ctzll(a) : 64;
	case RV_ZB_CTZW:	return (uint32_t)a ? (uint64_t)__builtin_ctz((uint32_t)a) : 32;
	case RV_ZB_CPOP:	return (uint64_t)__builtin_popcountll(a);
	case RV_ZB_CPOPW:	return (uint64_t)__builtin_popcount((uint32_t)a);
	case RV_ZB_MAX:		return (int64_t)a < (int64_t)b ? b : a;
	case RV_ZB_MAXU:	return a < b ? b : a;
	case RV_ZB_MIN:		return (int64_t)a < (int64_t)b ? a : b;
	case RV_ZB_MINU:	return a < b ? a : b;
	case RV_ZB_SEXT_B:	return (uint64_t)(int64_t)(int8_t)a;
	case RV_ZB_SEXT_H:	return (uint64_t)(int64_t)(int16_t)a;
	case RV_ZB_ZEXT_H:	return (uint16_t)a;
	case RV_ZB_ROL:		return rotr64(a, 64 - (b & 63));
	case RV_ZB_ROLW:	return sext32(rotr32((uint32_t)a, 32 - (b & 31)));
	case RV_ZB_ROR:		return rotr64(a, b & 63);
	case RV_ZB_RORW:	return sext32(rotr32((uint32_t)a, b & 31));
	case RV_ZB_RORI:	return rotr64(a, shamt);
	case RV_ZB_RORIW:	return sext32(rotr32((uint32_t)a, shamt & 31));
	case RV_ZB_ORC_B: {
		uint64_t r = 0;

		for (int k = 0; k < 64; k += 8)
			if ((a >> k) & 0xff)
				r |= 0xffULL << k;
		return r;
	}
	case RV_ZB_REV8:	return __builtin_bswap64(a);
	case RV_ZB_BCLR:	return a & ~(1ULL << (b & 63));
	case RV_ZB_BCLRI:	return a & ~(1ULL << shamt);
	case RV_ZB_BEXT:	return (a >> (b & 63)) & 1;
	case RV_ZB_BEXTI:	return (a >> shamt) & 1;
	case RV_ZB_BINV:	return a ^ (1ULL << (b & 63));
	case RV_ZB_BINVI:	return a ^ (1ULL << shamt);
	case RV_ZB_BSET:	return a | (1ULL << (b & 63));
	case RV_ZB_BSETI:	return a | (1ULL << shamt);
	}
	return 0;
}

/* Does the processor say it has them? (RV_BITMANIP=0: no.) */
int rv_zb_advertised(void)
{
	static int on = -1;

	if (on < 0) {
		const char *e = getenv("RV_BITMANIP");

		on = !(e && !strcmp(e, "0"));
	}
	return on;
}

/* RV_ZB_COUNT=1: count them. */
int rv_zb_count_on(void)
{
	if (rv_zb_counting < 0) {
		const char *e = getenv("RV_ZB_COUNT");

		rv_zb_counting = e && strcmp(e, "0");
	}
	return rv_zb_counting;
}

/*
 * The counts, for the whole process: every thread adds to the same table,
 * without atomic operations, so with many threads they are a little low.
 */
void rv_zb_report(void)
{
	uint64_t total = 0;

	if (!rv_zb_count_on())
		return;
	for (int k = 1; k < RV_ZB_KINDS; k++)
		total += rv_zb_count[k];
	fprintf(stderr, "rv: bit-manipulation instructions run: %llu\n", (unsigned long long)total);
	for (int k = 1; k < RV_ZB_KINDS; k++)
		if (rv_zb_count[k])
			fprintf(stderr, "rv:   %-10s %llu\n", rv_zb_names[k], (unsigned long long)rv_zb_count[k]);
}
