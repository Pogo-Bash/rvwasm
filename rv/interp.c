/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * RV64IMAFDC (and Zba, Zbb, Zbs) interpreter: the slow tier and the correctness oracle.
 */
#include <stdlib.h>
#include <string.h>

#include "rv.h"

#define LOAD_PROT(cpu, addr, type, prot) ({			\
	type v_;						\
	memcpy(&v_, rv_ptr((cpu), (addr), sizeof(type), (prot)), sizeof(type)); \
	v_; })
#define LOAD(cpu, addr, type) LOAD_PROT(cpu, addr, type, RV_PROT_READ)

#define STORE(cpu, addr, type, val) do {			\
	type v_ = (type)(val);					\
	uint64_t a_ = (addr);					\
	uint8_t *p_ = rv_ptr((cpu), a_, sizeof(type), RV_PROT_WRITE); \
	memcpy(p_, &v_, sizeof(type));				\
	/* Across two pages that are apart for the host (mm.c). */ \
	if (__builtin_expect(p_ == (cpu)->scratch, 0))		\
		rv_store_split((cpu), a_, sizeof(type));	\
} while (0)

static inline uint64_t sext32(uint64_t v)
{
	return (uint64_t)(int64_t)(int32_t)v;
}

static uint64_t mulhu(uint64_t a, uint64_t b)
{
	uint64_t a_lo = (uint32_t)a, a_hi = a >> 32;
	uint64_t b_lo = (uint32_t)b, b_hi = b >> 32;
	uint64_t p0 = a_lo * b_lo;
	uint64_t p1 = a_lo * b_hi;
	uint64_t p2 = a_hi * b_lo;
	uint64_t p3 = a_hi * b_hi;
	uint64_t carry = ((p0 >> 32) + (uint32_t)p1 + (uint32_t)p2) >> 32;

	return p3 + (p1 >> 32) + (p2 >> 32) + carry;
}

static uint64_t mulh(int64_t a, int64_t b)
{
	uint64_t hi = mulhu((uint64_t)a, (uint64_t)b);

	if (a < 0)
		hi -= (uint64_t)b;
	if (b < 0)
		hi -= (uint64_t)a;
	return hi;
}

static uint64_t mulhsu(int64_t a, uint64_t b)
{
	uint64_t hi = mulhu((uint64_t)a, b);

	if (a < 0)
		hi -= b;
	return hi;
}

/* RISC-V division never traps: the spec defines results for /0 and overflow. */
static uint64_t div64(int64_t a, int64_t b)
{
	if (b == 0)
		return UINT64_MAX;
	if (a == INT64_MIN && b == -1)
		return (uint64_t)a;
	return (uint64_t)(a / b);
}

static uint64_t rem64(int64_t a, int64_t b)
{
	if (b == 0)
		return (uint64_t)a;
	if (a == INT64_MIN && b == -1)
		return 0;
	return (uint64_t)(a % b);
}

static uint64_t div32(int32_t a, int32_t b)
{
	if (b == 0)
		return UINT64_MAX;
	if (a == INT32_MIN && b == -1)
		return sext32((uint32_t)a);
	return sext32((uint32_t)(a / b));
}

static uint64_t rem32(int32_t a, int32_t b)
{
	if (b == 0)
		return sext32((uint32_t)a);
	if (a == INT32_MIN && b == -1)
		return 0;
	return sext32((uint32_t)(a % b));
}

/*
 * The operations with no single Wasm instruction behind them, for translated
 * code to call (jit.c imports them by name).
 */
uint64_t rv_op_mulh(uint64_t a, uint64_t b) { return mulh((int64_t)a, (int64_t)b); }
uint64_t rv_op_mulhsu(uint64_t a, uint64_t b) { return mulhsu((int64_t)a, b); }
uint64_t rv_op_mulhu(uint64_t a, uint64_t b) { return mulhu(a, b); }
uint64_t rv_op_div(uint64_t a, uint64_t b) { return div64((int64_t)a, (int64_t)b); }
uint64_t rv_op_divu(uint64_t a, uint64_t b) { return b ? a / b : UINT64_MAX; }
uint64_t rv_op_rem(uint64_t a, uint64_t b) { return rem64((int64_t)a, (int64_t)b); }
uint64_t rv_op_remu(uint64_t a, uint64_t b) { return b ? a % b : a; }
uint64_t rv_op_divw(uint64_t a, uint64_t b) { return div32((int32_t)a, (int32_t)b); }
uint64_t rv_op_divuw(uint64_t a, uint64_t b)
{
	return (uint32_t)b ? sext32((uint32_t)a / (uint32_t)b) : UINT64_MAX;
}
uint64_t rv_op_remw(uint64_t a, uint64_t b) { return rem32((int32_t)a, (int32_t)b); }
uint64_t rv_op_remuw(uint64_t a, uint64_t b)
{
	return sext32((uint32_t)b ? (uint32_t)a % (uint32_t)b : (uint32_t)a);
}

static void trace_state(struct rv_cpu *cpu)
{
	fprintf(cpu->trace, "%016llx", (unsigned long long)cpu->pc);
	for (int i = 1; i < 32; i++)
		fprintf(cpu->trace, " %016llx", (unsigned long long)cpu->x[i]);
	for (int i = 0; i < 32; i++)
		fprintf(cpu->trace, " %016llx", (unsigned long long)cpu->f[i]);
	fputc('\n', cpu->trace);
}

static void illegal(struct rv_cpu *cpu, uint32_t insn)
{
	cpu->trap = RV_TRAP_ILL;
	cpu->trap_addr = cpu->pc;
	cpu->trap_insn = insn;
}

/*
 * The encodings of Zba, Zbb and Zbs (bitmanip.c) are among those of OP,
 * OP-32, OP-IMM and OP-IMM-32 that RV64GC leaves out: what the switch below
 * does not know is one of them, or illegal.
 */
static uint64_t bitmanip(struct rv_cpu *cpu, uint32_t insn, uint64_t a, uint64_t b)
{
	int kind = rv_zb_kind(insn);

	if (!kind) {
		illegal(cpu, insn);
		return 0;
	}
	if (__builtin_expect(rv_zb_counting > 0, 0))
		rv_zb_count[kind]++;
	return rv_zb_exec(kind, insn, a, b);
}

/*
 * The A extension, done with the host's atomic operations: the threads of the
 * guest are threads of the host, on the same memory (thread.c). LR/SC as
 * QEMU does it: SC succeeds when the reservation is for this address and
 * memory still holds the value LR read, which is one compare-and-swap. (A
 * value that changed and changed back goes unnoticed; real hardware would
 * fail the SC.)
 */
#define SEQ	__ATOMIC_SEQ_CST

static void exec_atomic(struct rv_cpu *cpu, uint32_t insn)
{
	uint64_t *x = cpu->x;
	uint32_t rd = (insn >> 7) & 31, f3 = (insn >> 12) & 7;
	uint32_t rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
	uint32_t f5 = insn >> 27;
	uint64_t addr = x[rs1], src = x[rs2];
	uint64_t old, val;
	uint64_t *p64;
	uint32_t *p32;
	uint8_t *p;
	int wide, ok;

	if (f3 != 2 && f3 != 3) {
		illegal(cpu, insn);
		return;
	}
	wide = f3 == 3;

	/* The operand has to be aligned, for the guest's processor as for the host's. */
	if (addr & (wide ? 7 : 3)) {
		rv_fault(cpu, addr);
		return;
	}
	p = rv_ptr(cpu, addr, wide ? 8 : 4, f5 == 0x02 ? RV_PROT_READ : RV_PROT_READ | RV_PROT_WRITE);
	if (cpu->trap != RV_TRAP_NONE)
		return;
	p64 = (uint64_t *)(void *)p;
	p32 = (uint32_t *)(void *)p;

#define RMW(op)	(wide ? __atomic_##op(p64, src, SEQ) : sext32(__atomic_##op(p32, (uint32_t)src, SEQ)))

	switch (f5) {
	case 0x02: /* LR */
		if (rs2 != 0) {
			illegal(cpu, insn);
			return;
		}
		old = wide ? __atomic_load_n(p64, SEQ) : sext32(__atomic_load_n(p32, SEQ));
		cpu->lr_addr = addr;
		cpu->lr_value = old;
		cpu->lr_valid = 1;
		x[rd] = old;
		return;

	case 0x03: /* SC: rd is 0 on success, 1 on failure */
		ok = cpu->lr_valid && cpu->lr_addr == addr;
		cpu->lr_valid = 0;
		if (ok && wide) {
			uint64_t expected = cpu->lr_value;

			ok = __atomic_compare_exchange_n(p64, &expected, src, 0, SEQ, SEQ);
		} else if (ok) {
			uint32_t expected = (uint32_t)cpu->lr_value;

			ok = __atomic_compare_exchange_n(p32, &expected, (uint32_t)src, 0, SEQ, SEQ);
		}
		x[rd] = !ok;
		return;

	case 0x01: old = RMW(exchange_n); break;	/* AMOSWAP */
	case 0x00: old = RMW(fetch_add); break;		/* AMOADD */
	case 0x04: old = RMW(fetch_xor); break;		/* AMOXOR */
	case 0x0c: old = RMW(fetch_and); break;		/* AMOAND */
	case 0x08: old = RMW(fetch_or); break;		/* AMOOR */

	case 0x10: /* AMOMIN */
	case 0x14: /* AMOMAX */
	case 0x18: /* AMOMINU */
	case 0x1c: /* AMOMAXU */
		/* No host operation for these: try until nothing came in between. */
		for (;;) {
			int take_src;

			old = wide ? __atomic_load_n(p64, SEQ) : sext32(__atomic_load_n(p32, SEQ));
			if (f5 == 0x10)
				take_src = wide ? (int64_t)src < (int64_t)old : (int32_t)src < (int32_t)old;
			else if (f5 == 0x14)
				take_src = wide ? (int64_t)src > (int64_t)old : (int32_t)src > (int32_t)old;
			else if (f5 == 0x18)
				take_src = wide ? src < old : (uint32_t)src < (uint32_t)old;
			else
				take_src = wide ? src > old : (uint32_t)src > (uint32_t)old;
			val = take_src ? src : old;
			if (wide) {
				uint64_t expected = old;

				if (__atomic_compare_exchange_n(p64, &expected, val, 0, SEQ, SEQ))
					break;
			} else {
				uint32_t expected = (uint32_t)old;

				if (__atomic_compare_exchange_n(p32, &expected, (uint32_t)val, 0, SEQ, SEQ))
					break;
			}
		}
		break;

	default:
		illegal(cpu, insn);
		return;
	}
#undef RMW
	x[rd] = old;
}

/*
 * Interpret until the guest stops or, with `one_block`, until the end of the
 * current stretch of straight-line code: that is where the dispatcher gets to
 * look for translated code again.
 */
static void interp(struct rv_cpu *cpu, int one_block)
{
	uint64_t *x = cpu->x;

	while (cpu->trap == RV_TRAP_NONE) {
		/*
		 * Signals are taken between instructions, and that is where a thread
		 * parks when it is asked to (thread.c). (Not while the JIT's
		 * self-check has a block replayed: that has to be those instructions
		 * and nothing else.)
		 */
		if (__builtin_expect(cpu->sig_pending || cpu->sig_restore_mask, 0) && one_block != 2)
			rv_signal_deliver(cpu);
		if (__builtin_expect(!(cpu->insn_count & 0x3ffff), 0) && one_block != 2)
			rv_signal_poll();

		uint64_t pc = cpu->pc;
		uint64_t next;
		/* The low two bits tell the length: 11 is a 32-bit instruction. */
		uint32_t insn = LOAD_PROT(cpu, pc, uint16_t, RV_PROT_EXEC);
		int compressed = (insn & 3) != 3;

		if (compressed) {
			next = pc + 2;
		} else {
			insn = LOAD_PROT(cpu, pc, uint32_t, RV_PROT_EXEC);
			next = pc + 4;
		}
		if (cpu->trap != RV_TRAP_NONE)
			break;	/* pc itself is not executable memory */
		if (cpu->trace)
			trace_state(cpu);

		if (compressed) {
			uint32_t expanded = rv_expand_compressed((uint16_t)insn);

			if (!expanded) {
				illegal(cpu, insn);
				continue;
			}
			insn = expanded;
		}

		uint32_t opcode = insn & 0x7f;
		uint32_t rd = (insn >> 7) & 31;
		uint32_t f3 = (insn >> 12) & 7;
		uint32_t rs1 = (insn >> 15) & 31;
		uint32_t rs2 = (insn >> 20) & 31;
		uint32_t f7 = insn >> 25;
		int64_t imm_i = (int32_t)insn >> 20;
		uint64_t a = x[rs1], b = x[rs2];
		uint64_t res = 0;

		switch (opcode) {
		case 0x37: /* LUI */
			x[rd] = (uint64_t)(int64_t)(int32_t)(insn & 0xfffff000);
			break;

		case 0x17: /* AUIPC */
			x[rd] = pc + (uint64_t)(int64_t)(int32_t)(insn & 0xfffff000);
			break;

		case 0x6f: { /* JAL */
			/* Cast the assembled value back to int32_t: OR-ing in the
			 * unsigned fields would otherwise drop the sign extension. */
			int64_t imm = (int32_t)(((int32_t)(insn & 0x80000000) >> 11) |
						(insn & 0xff000) | ((insn >> 9) & 0x800) |
						((insn >> 20) & 0x7fe));
			x[rd] = next;
			next = pc + (uint64_t)imm;
			break;
		}

		case 0x67: /* JALR */
			if (f3 != 0) {
				illegal(cpu, insn);
				break;
			}
			x[rd] = next;
			next = (a + (uint64_t)imm_i) & ~1ULL;
			break;

		case 0x63: { /* BRANCH */
			int64_t imm = (int32_t)(((int32_t)(insn & 0x80000000) >> 19) |
						((insn & 0x80) << 4) | ((insn >> 20) & 0x7e0) |
						((insn >> 7) & 0x1e));
			int taken;

			switch (f3) {
			case 0: taken = a == b; break;				/* BEQ */
			case 1: taken = a != b; break;				/* BNE */
			case 4: taken = (int64_t)a < (int64_t)b; break;		/* BLT */
			case 5: taken = (int64_t)a >= (int64_t)b; break;	/* BGE */
			case 6: taken = a < b; break;				/* BLTU */
			case 7: taken = a >= b; break;				/* BGEU */
			default:
				illegal(cpu, insn);
				taken = 0;
			}
			if (taken)
				next = pc + (uint64_t)imm;
			break;
		}

		case 0x03: { /* LOAD */
			uint64_t addr = a + (uint64_t)imm_i;

			switch (f3) {
			case 0: res = (uint64_t)(int64_t)LOAD(cpu, addr, int8_t); break;	/* LB */
			case 1: res = (uint64_t)(int64_t)LOAD(cpu, addr, int16_t); break;	/* LH */
			case 2: res = (uint64_t)(int64_t)LOAD(cpu, addr, int32_t); break;	/* LW */
			case 3: res = LOAD(cpu, addr, uint64_t); break;				/* LD */
			case 4: res = LOAD(cpu, addr, uint8_t); break;				/* LBU */
			case 5: res = LOAD(cpu, addr, uint16_t); break;				/* LHU */
			case 6: res = LOAD(cpu, addr, uint32_t); break;				/* LWU */
			default: illegal(cpu, insn);
			}
			if (cpu->trap == RV_TRAP_NONE)
				x[rd] = res;
			break;
		}

		case 0x23: { /* STORE */
			int64_t imm = (int32_t)(((int32_t)insn >> 25 << 5) | ((insn >> 7) & 31));
			uint64_t addr = a + (uint64_t)imm;

			switch (f3) {
			case 0: STORE(cpu, addr, uint8_t, b); break;	/* SB */
			case 1: STORE(cpu, addr, uint16_t, b); break;	/* SH */
			case 2: STORE(cpu, addr, uint32_t, b); break;	/* SW */
			case 3: STORE(cpu, addr, uint64_t, b); break;	/* SD */
			default: illegal(cpu, insn);
			}
			break;
		}

		case 0x13: { /* OP-IMM */
			uint32_t shamt = (insn >> 20) & 63;
			uint32_t f6 = insn >> 26;

			switch (f3) {
			case 0: res = a + (uint64_t)imm_i; break;			/* ADDI */
			case 2: res = (int64_t)a < imm_i; break;			/* SLTI */
			case 3: res = a < (uint64_t)imm_i; break;			/* SLTIU */
			case 4: res = a ^ (uint64_t)imm_i; break;			/* XORI */
			case 6: res = a | (uint64_t)imm_i; break;			/* ORI */
			case 7: res = a & (uint64_t)imm_i; break;			/* ANDI */
			case 1:								/* SLLI */
				if (f6 != 0)
					res = bitmanip(cpu, insn, a, b);
				else
					res = a << shamt;
				break;
			case 5:
				if (f6 == 0x00)
					res = a >> shamt;				/* SRLI */
				else if (f6 == 0x10)
					res = (uint64_t)((int64_t)a >> shamt);		/* SRAI */
				else
					res = bitmanip(cpu, insn, a, b);
				break;
			}
			if (cpu->trap == RV_TRAP_NONE)
				x[rd] = res;
			break;
		}

		case 0x1b: { /* OP-IMM-32 */
			uint32_t shamt = rs2;

			switch (f3) {
			case 0: res = sext32(a + (uint64_t)imm_i); break;		/* ADDIW */
			case 1:								/* SLLIW */
				if (f7 != 0)
					res = bitmanip(cpu, insn, a, b);
				else
					res = sext32((uint32_t)a << shamt);
				break;
			case 5:
				if (f7 == 0x00)
					res = sext32((uint32_t)a >> shamt);		/* SRLIW */
				else if (f7 == 0x20)
					res = sext32((uint32_t)((int32_t)a >> shamt));	/* SRAIW */
				else
					res = bitmanip(cpu, insn, a, b);
				break;
			default:
				illegal(cpu, insn);
			}
			if (cpu->trap == RV_TRAP_NONE)
				x[rd] = res;
			break;
		}

		case 0x33: /* OP */
			if (f7 == 0x00) {
				switch (f3) {
				case 0: res = a + b; break;				/* ADD */
				case 1: res = a << (b & 63); break;			/* SLL */
				case 2: res = (int64_t)a < (int64_t)b; break;		/* SLT */
				case 3: res = a < b; break;				/* SLTU */
				case 4: res = a ^ b; break;				/* XOR */
				case 5: res = a >> (b & 63); break;			/* SRL */
				case 6: res = a | b; break;				/* OR */
				case 7: res = a & b; break;				/* AND */
				}
			} else if (f7 == 0x20 && f3 == 0) {
				res = a - b;						/* SUB */
			} else if (f7 == 0x20 && f3 == 5) {
				res = (uint64_t)((int64_t)a >> (b & 63));		/* SRA */
			} else if (f7 == 0x01) {
				switch (f3) {
				case 0: res = a * b; break;				/* MUL */
				case 1: res = mulh((int64_t)a, (int64_t)b); break;	/* MULH */
				case 2: res = mulhsu((int64_t)a, b); break;		/* MULHSU */
				case 3: res = mulhu(a, b); break;			/* MULHU */
				case 4: res = div64((int64_t)a, (int64_t)b); break;	/* DIV */
				case 5: res = b ? a / b : UINT64_MAX; break;		/* DIVU */
				case 6: res = rem64((int64_t)a, (int64_t)b); break;	/* REM */
				case 7: res = b ? a % b : a; break;			/* REMU */
				}
			} else {
				res = bitmanip(cpu, insn, a, b);
			}
			if (cpu->trap == RV_TRAP_NONE)
				x[rd] = res;
			break;

		case 0x3b: { /* OP-32 */
			uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;

			if (f7 == 0x00 && f3 == 0) {
				res = sext32(a32 + b32);				/* ADDW */
			} else if (f7 == 0x20 && f3 == 0) {
				res = sext32(a32 - b32);				/* SUBW */
			} else if (f7 == 0x00 && f3 == 1) {
				res = sext32(a32 << (b32 & 31));			/* SLLW */
			} else if (f7 == 0x00 && f3 == 5) {
				res = sext32(a32 >> (b32 & 31));			/* SRLW */
			} else if (f7 == 0x20 && f3 == 5) {
				res = sext32((uint32_t)((int32_t)a32 >> (b32 & 31)));	/* SRAW */
			} else if (f7 == 0x01) {
				switch (f3) {
				case 0: res = sext32(a32 * b32); break;			/* MULW */
				case 4: res = div32((int32_t)a32, (int32_t)b32); break;	/* DIVW */
				case 5: res = b32 ? sext32(a32 / b32) : UINT64_MAX; break; /* DIVUW */
				case 6: res = rem32((int32_t)a32, (int32_t)b32); break;	/* REMW */
				case 7: res = sext32(b32 ? a32 % b32 : a32); break;	/* REMUW */
				default: illegal(cpu, insn);
				}
			} else {
				res = bitmanip(cpu, insn, a, b);
			}
			if (cpu->trap == RV_TRAP_NONE)
				x[rd] = res;
			break;
		}

		case 0x2f: /* AMO: the A extension */
			exec_atomic(cpu, insn);
			break;

		case 0x07: /* LOAD-FP, and with a width that is not FLW's or FLD's the vector loads */
		case 0x27: /* STORE-FP, and the vector stores */
			if (f3 != 2 && f3 != 3) {
				if (rv_exec_vector(cpu, insn))
					illegal(cpu, insn);
				break;
			}
			/* fall through */
		case 0x43: /* FMADD */
		case 0x47: /* FMSUB */
		case 0x4b: /* FNMSUB */
		case 0x4f: /* FNMADD */
		case 0x53: /* OP-FP */
			if (rv_exec_fp(cpu, insn))
				illegal(cpu, insn);
			break;

		/*
		 * MISC-MEM. FENCE orders memory accesses, which one interpreter
		 * thread does anyway. FENCE.I announces changed code to the
		 * processor that executes it, and needs nothing either: what
		 * this thread wrote to a page that code was translated from was
		 * seen when it was written (see jit.c). For the other threads
		 * there is riscv_flush_icache.
		 */
		case 0x0f:
			break;

		case 0x57: /* OP-V: vsetvli and its kin, nothing else (vector.c) */
			if (rv_exec_vector(cpu, insn))
				illegal(cpu, insn);
			break;

		case 0x73: /* SYSTEM */
			if (insn == 0x00000073) {
				rv_syscall(cpu);				/* ECALL */
			} else if (insn == 0x00100073) {
				cpu->trap = RV_TRAP_BREAK;			/* EBREAK */
				cpu->trap_addr = pc;
			} else if (f3 == 0 || rv_csr(cpu, insn)) {
				illegal(cpu, insn);				/* else: Zicsr */
			}
			break;

		default:
			illegal(cpu, insn);
		}

		x[0] = 0;
		if (cpu->trap == RV_TRAP_NONE || cpu->trap == RV_TRAP_EXIT) {
			if (cpu->pc_set)
				cpu->pc_set = 0;	/* rt_sigreturn chose where to go */
			else
				cpu->pc = next;
			cpu->insn_count++;
		}

		/*
		 * Control transfers end a block, and so does anything the JIT
		 * leaves to the interpreter: translated code may start right
		 * after it.
		 */
		if (one_block == 2) {
			if (!--cpu->steps_left)
				return;
			continue;
		}
		if (one_block) {
			switch (opcode) {
			case 0x63: case 0x67: case 0x6f: case 0x73:
			case 0x2f: case 0x0f:
				return;
			}
		}
	}
}

void rv_interp_block(struct rv_cpu *cpu)
{
	interp(cpu, 1);
}

/* Exactly `count` instructions, unless one of them traps. */
void rv_interp_steps(struct rv_cpu *cpu, uint64_t count)
{
	cpu->steps_left = count;
	interp(cpu, 2);
}

/* What a thread other than the first interprets before it gets translations of its own (jit.c). */
#define JIT_THREAD_START	100000

/* (RV_JIT_START=n: another number, for measuring what the wait costs.) */
static uint64_t jit_thread_start(void)
{
	static uint64_t start = ~0ULL;

	if (start == ~0ULL)
		start = getenv("RV_JIT_START") ? strtoull(getenv("RV_JIT_START"), NULL, 0) : JIT_THREAD_START;
	return start;
}

void rv_run(struct rv_cpu *cpu)
{
	for (;;) {
		/*
		 * A new thread starts out interpreting: many threads never run
		 * enough for translated code, with its tables, to be worth having.
		 */
		while (!cpu->jit && cpu->jit_wanted && !cpu->trace && cpu->trap == RV_TRAP_NONE) {
			interp(cpu, 1);
			if (cpu->insn_count >= jit_thread_start() && rv_jit_init(cpu))
				cpu->jit_wanted = 0;
		}
		/* Tracing wants every instruction, so it stays in the interpreter. */
		if (cpu->jit && !cpu->trace)
			rv_jit_run(cpu);
		else
			interp(cpu, 0);
		/* A fault the guest has a handler for becomes a signal; anything else ends the run. */
		if (!rv_signal_fault(cpu))
			break;
	}
}
