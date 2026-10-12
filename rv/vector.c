/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * The least of the V extension: setting vl and vtype, and unit-stride loads
 * and stores of vector registers, with vectors of 128 bits.
 *
 * The processor does not have the V extension, and says so (AT_HWCAP and
 * riscv_hwprobe in elf.c and syscall.c): a program that asks uses none of
 * it. V8's deoptimization entry does not ask the second time. It saves every
 * register on the way into the deoptimizer and copies the saved values into
 * a description of the frame on the way out; the vector registers are saved
 * only on a processor that has them, but copied out always, by vle8.v and
 * vse8.v. On a processor without V that is an illegal instruction, and
 * Chromium's renderer dies the first time optimized code is left. Here it
 * copies 16 bytes of whatever is on the stack into a slot nothing reads.
 *
 * The JIT translates none of this (jit.c, classify()): these instructions
 * are the interpreter's, which is fine for how rarely they run.
 */
#include <string.h>

#include "rv.h"

#define VLEN_BYTES	16

/* VLMAX for a vtype (vlmul in bits 2:0, vsew in 5:3, vta 6, vma 7), or 0 for one that is not valid. */
static uint64_t vlmax_of(uint64_t vtype)
{
	unsigned lmul = vtype & 7, sew = (vtype >> 3) & 7;
	uint64_t elems;

	if ((vtype >> 8) || sew > 3 || lmul == 4)
		return 0;
	elems = VLEN_BYTES >> sew;		/* in one register */
	if (lmul < 4)
		elems <<= lmul;			/* 1, 2, 4, 8 registers */
	else
		elems >>= 8 - lmul;		/* 1/8, 1/4, 1/2 of one */
	return elems;
}

/* vsetvli, vsetivli, vsetvl: the new vtype, and the application vector length if one is given. */
static void vset(struct rv_cpu *cpu, uint32_t insn, uint64_t vtype, int avl_given, uint64_t avl)
{
	uint32_t rd = (insn >> 7) & 31;
	uint64_t vlmax = vlmax_of(vtype);

	if (!vlmax || (!avl_given && !rd && cpu->vl > vlmax)) {
		/* Not a vtype the processor has, or the old vl does not fit it: vill, and nothing works until the next one. */
		cpu->vtype = 1ULL << 63;
		cpu->vl = 0;
	} else {
		cpu->vtype = vtype;
		if (avl_given)
			cpu->vl = avl < vlmax ? avl : vlmax;
		else if (rd)
			cpu->vl = vlmax;	/* rs1 = x0, rd not: as many as there are */
		/* else: rs1 = rd = x0 keeps vl */
	}
	if (rd)
		cpu->x[rd] = cpu->vl;
}

/*
 * OP-V (0x57) with funct3 7, and LOAD-FP/STORE-FP (0x07/0x27) with a width
 * that is not FLW's or FLD's. 0, or -1 for what is not here, which is an
 * illegal instruction.
 */
int rv_exec_vector(struct rv_cpu *cpu, uint32_t insn)
{
	uint32_t op = insn & 0x7f, f3 = (insn >> 12) & 7;
	uint32_t rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
	uint32_t nf = insn >> 29, mop = (insn >> 26) & 3, vm = (insn >> 25) & 1;
	uint64_t addr = cpu->x[rs1], bytes;
	uint8_t *regs = (uint8_t *)cpu->v + rd * VLEN_BYTES;
	unsigned ew;
	int store = op == 0x27;

	if (op == 0x57) {
		if (f3 != 7)
			return -1;	/* arithmetic: none of it */
		if (!(insn >> 31))
			vset(cpu, insn, (insn >> 20) & 0x7ff, rs1 != 0, cpu->x[rs1]);		/* vsetvli */
		else if ((insn >> 30) == 3)
			vset(cpu, insn, (insn >> 20) & 0x3ff, 1, rs1);				/* vsetivli */
		else if ((insn >> 25) == 0x40)
			vset(cpu, insn, cpu->x[rs2], rs1 != 0, cpu->x[rs1]);			/* vsetvl */
		else
			return -1;
		return 0;
	}
	if ((op != 0x07 && op != 0x27) || (f3 != 0 && f3 < 5) || mop != 0 || !vm)
		return -1;	/* strided, indexed and masked forms: none of them */
	ew = f3 == 0 ? 1 : 1u << (f3 - 4);			/* e8, e16, e32, e64 */
	if (rs2 == 0x08) {
		/* vl1r.v .. vl8r.v, vs1r.v .. vs8r.v: nf + 1 whole registers (1, 2, 4 or 8), whatever vl and vtype say. */
		if (nf != 0 && nf != 1 && nf != 3 && nf != 7)
			return -1;
		bytes = (nf + 1) * VLEN_BYTES;
	} else if (rs2 == 0) {
		/* vle<ew>.v, vse<ew>.v: vl elements of ew bytes, one after the other, into the register group from rd on. */
		if (nf || (cpu->vtype >> 63))
			return -1;
		bytes = cpu->vl * ew;
	} else {
		return -1;
	}
	if (rd * VLEN_BYTES + bytes > 32 * VLEN_BYTES)
		return -1;
	for (uint64_t i = 0; i < bytes; i += ew) {
		uint8_t *p = rv_ptr(cpu, addr + i, ew, store ? RV_PROT_WRITE : RV_PROT_READ);

		if (store) {
			memcpy(p, regs + i, ew);
			if (__builtin_expect(p == cpu->scratch, 0))
				rv_store_split(cpu, addr + i, ew);
		} else {
			memcpy(regs + i, p, ew);
		}
		if (cpu->trap != RV_TRAP_NONE)
			break;	/* a fault: recorded, and the run loop stops before the next instruction */
	}
	return 0;
}
