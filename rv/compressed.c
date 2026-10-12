/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The C extension (RV64): every 16-bit instruction is shorthand for one
 * 32-bit instruction. Expanding instead of executing them directly keeps a
 * single implementation of the semantics.
 */
#include "rv.h"

#define X0	0
#define RA	1
#define SP	2

/* Field of a 16-bit instruction: `len` bits starting at bit `lo`. */
#define BITS(v, lo, len)	(((uint32_t)(v) >> (lo)) & ((1u << (len)) - 1))

static uint32_t enc_r(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op)
{
	return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}

static uint32_t enc_i(uint32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op)
{
	return (imm & 0xfff) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}

static uint32_t enc_s(uint32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op)
{
	return ((imm >> 5) & 0x7f) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (imm & 0x1f) << 7 | op;
}

static uint32_t enc_b(uint32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3)
{
	return ((imm >> 12) & 1) << 31 | ((imm >> 5) & 0x3f) << 25 | rs2 << 20 | rs1 << 15 |
	       f3 << 12 | ((imm >> 1) & 0xf) << 8 | ((imm >> 11) & 1) << 7 | 0x63;
}

static uint32_t enc_j(uint32_t imm, uint32_t rd)
{
	return ((imm >> 20) & 1) << 31 | ((imm >> 1) & 0x3ff) << 21 | ((imm >> 11) & 1) << 20 |
	       ((imm >> 12) & 0xff) << 12 | rd << 7 | 0x6f;
}

/* Sign-extend the low `bits` bits of v. */
static uint32_t sext(uint32_t v, unsigned bits)
{
	return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits));
}

/* Returns the equivalent 32-bit instruction, or 0 for an illegal encoding. */
uint32_t rv_expand_compressed(uint16_t c)
{
	uint32_t op = BITS(c, 0, 2);
	uint32_t f3 = BITS(c, 13, 3);
	uint32_t rd = BITS(c, 7, 5);		/* also rs1 in the two-address forms */
	uint32_t rs2 = BITS(c, 2, 5);
	uint32_t rdp = 8 + BITS(c, 2, 3);	/* the three-bit register fields name x8..x15 */
	uint32_t rs1p = 8 + BITS(c, 7, 3);
	uint32_t imm6 = sext(BITS(c, 12, 1) << 5 | BITS(c, 2, 5), 6);
	uint32_t shamt = BITS(c, 12, 1) << 5 | BITS(c, 2, 5);
	uint32_t imm;

	switch (op << 3 | f3) {
	/* Quadrant 0 */
	case 000: /* C.ADDI4SPN: addi rd', sp, nzuimm */
		imm = BITS(c, 11, 2) << 4 | BITS(c, 7, 4) << 6 | BITS(c, 6, 1) << 2 | BITS(c, 5, 1) << 3;
		return imm ? enc_i(imm, SP, 0, rdp, 0x13) : 0;
	case 001: /* C.FLD: fld rd', off(rs1') */
		imm = BITS(c, 10, 3) << 3 | BITS(c, 5, 2) << 6;
		return enc_i(imm, rs1p, 3, rdp, 0x07);
	case 002: /* C.LW: lw rd', off(rs1') */
		imm = BITS(c, 10, 3) << 3 | BITS(c, 6, 1) << 2 | BITS(c, 5, 1) << 6;
		return enc_i(imm, rs1p, 2, rdp, 0x03);
	case 003: /* C.LD: ld rd', off(rs1') */
		imm = BITS(c, 10, 3) << 3 | BITS(c, 5, 2) << 6;
		return enc_i(imm, rs1p, 3, rdp, 0x03);
	case 005: /* C.FSD: fsd rs2', off(rs1') */
		imm = BITS(c, 10, 3) << 3 | BITS(c, 5, 2) << 6;
		return enc_s(imm, rdp, rs1p, 3, 0x27);
	case 006: /* C.SW: sw rs2', off(rs1') */
		imm = BITS(c, 10, 3) << 3 | BITS(c, 6, 1) << 2 | BITS(c, 5, 1) << 6;
		return enc_s(imm, rdp, rs1p, 2, 0x23);
	case 007: /* C.SD: sd rs2', off(rs1') */
		imm = BITS(c, 10, 3) << 3 | BITS(c, 5, 2) << 6;
		return enc_s(imm, rdp, rs1p, 3, 0x23);

	/* Quadrant 1 */
	case 010: /* C.ADDI (C.NOP when rd is x0): addi rd, rd, imm */
		return enc_i(imm6, rd, 0, rd, 0x13);
	case 011: /* C.ADDIW: addiw rd, rd, imm */
		return rd ? enc_i(imm6, rd, 0, rd, 0x1b) : 0;
	case 012: /* C.LI: addi rd, x0, imm */
		return enc_i(imm6, X0, 0, rd, 0x13);
	case 013:
		if (rd == SP) { /* C.ADDI16SP: addi sp, sp, nzimm */
			imm = sext(BITS(c, 12, 1) << 9 | BITS(c, 6, 1) << 4 | BITS(c, 5, 1) << 6 |
				   BITS(c, 3, 2) << 7 | BITS(c, 2, 1) << 5, 10);
			return imm ? enc_i(imm, SP, 0, SP, 0x13) : 0;
		}
		/* C.LUI: lui rd, nzimm */
		return imm6 ? (imm6 << 12 | rd << 7 | 0x37) : 0;
	case 014:
		switch (BITS(c, 10, 2)) {
		case 0: /* C.SRLI */
			return enc_i(shamt, rs1p, 5, rs1p, 0x13);
		case 1: /* C.SRAI */
			return enc_i(0x400 | shamt, rs1p, 5, rs1p, 0x13);
		case 2: /* C.ANDI */
			return enc_i(imm6, rs1p, 7, rs1p, 0x13);
		default:
			switch (BITS(c, 12, 1) << 2 | BITS(c, 5, 2)) {
			case 0: return enc_r(0x20, rdp, rs1p, 0, rs1p, 0x33);	/* C.SUB */
			case 1: return enc_r(0x00, rdp, rs1p, 4, rs1p, 0x33);	/* C.XOR */
			case 2: return enc_r(0x00, rdp, rs1p, 6, rs1p, 0x33);	/* C.OR */
			case 3: return enc_r(0x00, rdp, rs1p, 7, rs1p, 0x33);	/* C.AND */
			case 4: return enc_r(0x20, rdp, rs1p, 0, rs1p, 0x3b);	/* C.SUBW */
			case 5: return enc_r(0x00, rdp, rs1p, 0, rs1p, 0x3b);	/* C.ADDW */
			default: return 0;
			}
		}
	case 015: /* C.J: jal x0, off */
		imm = sext(BITS(c, 12, 1) << 11 | BITS(c, 11, 1) << 4 | BITS(c, 9, 2) << 8 |
			   BITS(c, 8, 1) << 10 | BITS(c, 7, 1) << 6 | BITS(c, 6, 1) << 7 |
			   BITS(c, 3, 3) << 1 | BITS(c, 2, 1) << 5, 12);
		return enc_j(imm, X0);
	case 016: /* C.BEQZ: beq rs1', x0, off */
	case 017: /* C.BNEZ: bne rs1', x0, off */
		imm = sext(BITS(c, 12, 1) << 8 | BITS(c, 10, 2) << 3 | BITS(c, 5, 2) << 6 |
			   BITS(c, 3, 2) << 1 | BITS(c, 2, 1) << 5, 9);
		return enc_b(imm, X0, rs1p, f3 & 1);

	/* Quadrant 2 */
	case 020: /* C.SLLI: slli rd, rd, shamt */
		return enc_i(shamt, rd, 1, rd, 0x13);
	case 021: /* C.FLDSP: fld rd, off(sp) */
		imm = BITS(c, 12, 1) << 5 | BITS(c, 5, 2) << 3 | BITS(c, 2, 3) << 6;
		return enc_i(imm, SP, 3, rd, 0x07);
	case 022: /* C.LWSP: lw rd, off(sp) */
		imm = BITS(c, 12, 1) << 5 | BITS(c, 4, 3) << 2 | BITS(c, 2, 2) << 6;
		return rd ? enc_i(imm, SP, 2, rd, 0x03) : 0;
	case 023: /* C.LDSP: ld rd, off(sp) */
		imm = BITS(c, 12, 1) << 5 | BITS(c, 5, 2) << 3 | BITS(c, 2, 3) << 6;
		return rd ? enc_i(imm, SP, 3, rd, 0x03) : 0;
	case 024:
		if (!BITS(c, 12, 1)) {
			if (!rs2) /* C.JR: jalr x0, 0(rs1) */
				return rd ? enc_i(0, rd, 0, X0, 0x67) : 0;
			return enc_r(0, rs2, X0, 0, rd, 0x33);	/* C.MV: add rd, x0, rs2 */
		}
		if (!rs2) {
			if (!rd)
				return 0x00100073;		/* C.EBREAK */
			return enc_i(0, rd, 0, RA, 0x67);	/* C.JALR: jalr ra, 0(rs1) */
		}
		return enc_r(0, rs2, rd, 0, rd, 0x33);		/* C.ADD: add rd, rd, rs2 */
	case 025: /* C.FSDSP: fsd rs2, off(sp) */
		imm = BITS(c, 10, 3) << 3 | BITS(c, 7, 3) << 6;
		return enc_s(imm, rs2, SP, 3, 0x27);
	case 026: /* C.SWSP: sw rs2, off(sp) */
		imm = BITS(c, 9, 4) << 2 | BITS(c, 7, 2) << 6;
		return enc_s(imm, rs2, SP, 2, 0x23);
	case 027: /* C.SDSP: sd rs2, off(sp) */
		imm = BITS(c, 10, 3) << 3 | BITS(c, 7, 3) << 6;
		return enc_s(imm, rs2, SP, 3, 0x23);

	default:
		return 0;
	}
}
