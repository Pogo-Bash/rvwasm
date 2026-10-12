/*
 * Zba, Zbb and Zbs: every instruction over a set of values that has the edge
 * cases in it (0, -1, the sign bits of both widths, values that differ only
 * above bit 31), and every immediate form with the shift amounts and bit
 * numbers at the edges (0, 1, 31, 32, 63). One hash per instruction is
 * printed, so a difference points at the instruction.
 *
 * Built with -march=rv64gc_zba_zbb_zbs. With -DREPEAT=n the whole run is done
 * n times (the hashes are the same each time): often enough for a JIT to
 * translate it, for RV_JIT_CHECK=1 to compare translated and interpreted.
 */
#include "guest.h"

#ifndef REPEAT
#define REPEAT 1
#endif

static const u64 values[] = {
	0, 1, 2, 3, 5, 31, 32, 33, 63, 64, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
	0x7fffffff, 0x80000000, 0xffffffff, 0x100000000, 0x17fffffff, 0x180000000,
	0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff, 0xffffffff80000000,
	0xffffffff7fffffff, 0x00ff00ff00ff00ff, 0x0123456789abcdef, 0xfedcba9876543210,
	0xdeadbeefcafebabe, 0x8000000000000001, 0x0000000100000001, 0x8080808080808080,
};
#define NVALUES (sizeof(values) / sizeof(values[0]))

enum {
	ADD_UW, ZEXT_W, SH1ADD, SH2ADD, SH3ADD, SH1ADD_UW, SH2ADD_UW, SH3ADD_UW, SLLI_UW,
	ANDN, ORN, XNOR, CLZ, CLZW, CTZ, CTZW, CPOP, CPOPW, MAX, MAXU, MIN, MINU,
	SEXT_B, SEXT_H, ZEXT_H, ROL, ROLW, ROR, RORW, RORI, RORIW, ORC_B, REV8,
	BCLR, BCLRI, BEXT, BEXTI, BINV, BINVI, BSET, BSETI, SAME, NOPS
};

static const char *const names[NOPS] = {
	"add.uw", "zext.w", "sh1add", "sh2add", "sh3add", "sh1add.uw", "sh2add.uw", "sh3add.uw", "slli.uw",
	"andn", "orn", "xnor", "clz", "clzw", "ctz", "ctzw", "cpop", "cpopw", "max", "maxu", "min", "minu",
	"sext.b", "sext.h", "zext.h", "rol", "rolw", "ror", "rorw", "rori", "roriw", "orc.b", "rev8",
	"bclr", "bclri", "bext", "bexti", "binv", "binvi", "bset", "bseti", "rd=rs1=rs2",
};

static u64 h[NOPS];

#define R(op, insn)							\
	do {								\
		u64 r_;							\
		__asm__ volatile(insn " %0, %1, %2" : "=r"(r_) : "r"(a), "r"(b)); \
		h[op] = mix(h[op], r_);					\
	} while (0)

#define U(op, insn)							\
	do {								\
		u64 r_;							\
		__asm__ volatile(insn " %0, %1" : "=r"(r_) : "r"(a));	\
		h[op] = mix(h[op], r_);					\
	} while (0)

#define I1(op, insn, n)							\
	do {								\
		u64 r_;							\
		__asm__ volatile(insn " %0, %1, " #n : "=r"(r_) : "r"(a)); \
		h[op] = mix(h[op], r_);					\
	} while (0)

/* The immediate forms with six-bit amounts, and with five (the w forms). */
#define I64(op, insn)							\
	do {								\
		I1(op, insn, 0); I1(op, insn, 1); I1(op, insn, 7); I1(op, insn, 8); \
		I1(op, insn, 15); I1(op, insn, 16); I1(op, insn, 31); I1(op, insn, 32); \
		I1(op, insn, 33); I1(op, insn, 48); I1(op, insn, 62); I1(op, insn, 63); \
	} while (0)
#define I32(op, insn)							\
	do {								\
		I1(op, insn, 0); I1(op, insn, 1); I1(op, insn, 7); I1(op, insn, 8); \
		I1(op, insn, 15); I1(op, insn, 16); I1(op, insn, 30); I1(op, insn, 31); \
	} while (0)

static void run(void)
{
	for (unsigned i = 0; i < NVALUES; i++) {
		volatile u64 va = values[i];
		u64 a = va;

		U(ZEXT_W, "zext.w");
		U(CLZ, "clz");
		U(CLZW, "clzw");
		U(CTZ, "ctz");
		U(CTZW, "ctzw");
		U(CPOP, "cpop");
		U(CPOPW, "cpopw");
		U(SEXT_B, "sext.b");
		U(SEXT_H, "sext.h");
		U(ZEXT_H, "zext.h");
		U(ORC_B, "orc.b");
		U(REV8, "rev8");
		I64(SLLI_UW, "slli.uw");
		I64(RORI, "rori");
		I32(RORIW, "roriw");
		I64(BCLRI, "bclri");
		I64(BEXTI, "bexti");
		I64(BINVI, "binvi");
		I64(BSETI, "bseti");

		for (unsigned j = 0; j < NVALUES; j++) {
			volatile u64 vb = values[j];
			u64 b = vb, c;

			R(ADD_UW, "add.uw");
			R(SH1ADD, "sh1add");
			R(SH2ADD, "sh2add");
			R(SH3ADD, "sh3add");
			R(SH1ADD_UW, "sh1add.uw");
			R(SH2ADD_UW, "sh2add.uw");
			R(SH3ADD_UW, "sh3add.uw");
			R(ANDN, "andn");
			R(ORN, "orn");
			R(XNOR, "xnor");
			R(MAX, "max");
			R(MAXU, "maxu");
			R(MIN, "min");
			R(MINU, "minu");
			R(ROL, "rol");
			R(ROLW, "rolw");
			R(ROR, "ror");
			R(RORW, "rorw");
			R(BCLR, "bclr");
			R(BEXT, "bext");
			R(BINV, "binv");
			R(BSET, "bset");

			/* The destination one of the sources, and both sources the same register. */
			c = a;
			__asm__ volatile("sh3add.uw %0, %0, %1" : "+r"(c) : "r"(b));
			h[SAME] = mix(h[SAME], c);
			c = b;
			__asm__ volatile("min %0, %1, %0" : "+r"(c) : "r"(a));
			h[SAME] = mix(h[SAME], c);
			c = a;
			__asm__ volatile("rol %0, %0, %0" : "+r"(c));
			h[SAME] = mix(h[SAME], c);
			c = a;
			__asm__ volatile("andn %0, %0, %0" : "+r"(c));
			h[SAME] = mix(h[SAME], c);
			c = b;
			__asm__ volatile("bext %0, %1, %0" : "+r"(c) : "r"(a));
			h[SAME] = mix(h[SAME], c);
		}
	}
	/* x0 as a source, and as the destination (the result goes nowhere). */
	{
		u64 r, a = values[NVALUES - 2];

		__asm__ volatile("sh2add %0, zero, %1" : "=r"(r) : "r"(a));
		h[SAME] = mix(h[SAME], r);
		__asm__ volatile("add.uw %0, %1, zero" : "=r"(r) : "r"(a));
		h[SAME] = mix(h[SAME], r);
		__asm__ volatile("max %0, zero, %1" : "=r"(r) : "r"(a));
		h[SAME] = mix(h[SAME], r);
		__asm__ volatile("clz %0, zero" : "=r"(r));
		h[SAME] = mix(h[SAME], r);
		r = 5;
		__asm__ volatile("rev8 zero, %0\n\tmv %0, zero" : "+r"(r));
		h[SAME] = mix(h[SAME], r);
	}
}

void _start(void)
{
	for (int k = 0; k < NOPS; k++)
		h[k] = MIX_INIT;
	for (int n = 0; n < REPEAT; n++)
		run();
	for (int k = 0; k < NOPS; k++)
		put_hex(names[k], h[k]);
	finish();
}
