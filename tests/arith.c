/*
 * RV64IM exercise for differential testing. Freestanding: no libc, only the
 * write, exit, brk and mmap system calls. Build with -march=rv64im so the
 * compiler emits neither compressed nor floating-point instructions.
 *
 * Inputs go through volatile so the compiler cannot fold the operations away.
 */
#include "guest.h"

static const u64 values[] = {
	0, 1, 2, 3, 7, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff,
	0x7fffffff, 0x80000000, 0xffffffff, 0x100000000,
	0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff,
	0x0123456789abcdef, 0xfedcba9876543210, 0xdeadbeefcafebabe,
};
#define NVALUES (sizeof(values) / sizeof(values[0]))

/* Every register-register operation in RV64IM, over all pairs of values. */
static u64 test_ops(void)
{
	u64 h = 0xcbf29ce484222325UL;

	for (unsigned i = 0; i < NVALUES; i++) {
		for (unsigned j = 0; j < NVALUES; j++) {
			volatile u64 va = values[i], vb = values[j];
			u64 a = va, b = vb;
			s64 sa = (s64)a, sb = (s64)b;
			u32 a32 = (u32)a, b32 = (u32)b;
			s32 sa32 = (s32)a;

			h = mix(h, a + b);
			h = mix(h, a - b);
			h = mix(h, a ^ b);
			h = mix(h, a | b);
			h = mix(h, a & b);
			h = mix(h, a << (b & 63));
			h = mix(h, a >> (b & 63));
			h = mix(h, (u64)(sa >> (b & 63)));
			h = mix(h, sa < sb);
			h = mix(h, a < b);

			h = mix(h, a * b);

			h = mix(h, (u64)(s64)(s32)(a32 + b32));
			h = mix(h, (u64)(s64)(s32)(a32 - b32));
			h = mix(h, (u64)(s64)(s32)(a32 << (b32 & 31)));
			h = mix(h, (u64)(s64)(s32)(a32 >> (b32 & 31)));
			h = mix(h, (u64)(s64)(sa32 >> (b32 & 31)));
			h = mix(h, (u64)(s64)(s32)(a32 * b32));
		}
	}
	return h;
}

/*
 * Division is written in assembly because C leaves x/0 and INT_MIN/-1
 * undefined, while RISC-V defines both and those are the cases worth testing.
 */
#define DIVOP(name, insn)						\
static u64 name(u64 a, u64 b)						\
{									\
	u64 r;								\
	__asm__(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));	\
	return r;							\
}
DIVOP(op_div, "div")
DIVOP(op_divu, "divu")
DIVOP(op_rem, "rem")
DIVOP(op_remu, "remu")
DIVOP(op_divw, "divw")
DIVOP(op_divuw, "divuw")
DIVOP(op_remw, "remw")
DIVOP(op_remuw, "remuw")
DIVOP(op_mulh, "mulh")
DIVOP(op_mulhsu, "mulhsu")
DIVOP(op_mulhu, "mulhu")
DIVOP(op_mulw, "mulw")

static u64 test_div(void)
{
	u64 h = 0xcbf29ce484222325UL;

	for (unsigned i = 0; i < NVALUES; i++) {
		for (unsigned j = 0; j < NVALUES; j++) {
			volatile u64 va = values[i], vb = values[j];
			u64 a = va, b = vb;

			h = mix(h, op_div(a, b));
			h = mix(h, op_divu(a, b));
			h = mix(h, op_rem(a, b));
			h = mix(h, op_remu(a, b));
			h = mix(h, op_divw(a, b));
			h = mix(h, op_divuw(a, b));
			h = mix(h, op_remw(a, b));
			h = mix(h, op_remuw(a, b));
			h = mix(h, op_mulh(a, b));
			h = mix(h, op_mulhsu(a, b));
			h = mix(h, op_mulhu(a, b));
			h = mix(h, op_mulw(a, b));
		}
	}
	return h;
}

/* Immediate forms, including the sign-extending 32-bit ones. */
static u64 test_imm(void)
{
	u64 h = 0xcbf29ce484222325UL;

	for (unsigned i = 0; i < NVALUES; i++) {
		volatile u64 va = values[i];
		u64 a = va;
		s64 sa = (s64)a;
		u32 a32 = (u32)a;
		s32 sa32 = (s32)a;

		h = mix(h, a + 2047);
		h = mix(h, a - 2048);
		h = mix(h, a ^ 0x555);
		h = mix(h, a | 0x2aa);
		h = mix(h, a & 0x7f0);
		h = mix(h, a ^ ~0UL);
		h = mix(h, sa < -5);
		h = mix(h, a < 100);
		h = mix(h, a << 1);
		h = mix(h, a << 63);
		h = mix(h, a >> 1);
		h = mix(h, a >> 63);
		h = mix(h, (u64)(sa >> 1));
		h = mix(h, (u64)(sa >> 63));
		h = mix(h, (u64)(s64)(s32)(a32 + 1));
		h = mix(h, (u64)(s64)(s32)(a32 << 31));
		h = mix(h, (u64)(s64)(s32)(a32 >> 31));
		h = mix(h, (u64)(s64)(sa32 >> 31));
		h = mix(h, a + 0x12345000);
		h = mix(h, a + 0xfffffffffff00000UL);
	}
	return h;
}

/* Loads and stores of every width, at unaligned addresses too. */
static u64 test_mem(void)
{
	static unsigned char buf[64];
	u64 h = 0xcbf29ce484222325UL;

	for (unsigned i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)(i * 37 + 0x81);

	for (unsigned off = 0; off < 9; off++) {
		volatile unsigned char *p = buf + off;

		h = mix(h, *(volatile signed char *)p);
		h = mix(h, *(volatile unsigned char *)p);
		h = mix(h, (u64)*(volatile short *)p);
		h = mix(h, *(volatile unsigned short *)p);
		h = mix(h, (u64)*(volatile int *)p);
		h = mix(h, *(volatile unsigned int *)p);
		h = mix(h, *(volatile unsigned long *)p);

		*(volatile unsigned char *)(p + 16) = (unsigned char)h;
		*(volatile unsigned short *)(p + 24) = (unsigned short)h;
		*(volatile unsigned int *)(p + 32) = (unsigned int)h;
		*(volatile unsigned long *)(p + 40) = h;
	}
	for (unsigned i = 0; i < sizeof(buf); i++)
		h = mix(h, buf[i]);
	return h;
}

/* Calls, recursion and every branch condition. */
static u64 fib(u64 n)
{
	return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static u64 test_branch(void)
{
	u64 h = 0xcbf29ce484222325UL;

	for (unsigned i = 0; i < NVALUES; i++) {
		for (unsigned j = 0; j < NVALUES; j++) {
			volatile u64 va = values[i], vb = values[j];
			u64 a = va, b = vb;
			unsigned bits = 0;

			if (a == b) bits |= 1;
			if (a != b) bits |= 2;
			if ((s64)a < (s64)b) bits |= 4;
			if ((s64)a >= (s64)b) bits |= 8;
			if (a < b) bits |= 16;
			if (a >= b) bits |= 32;
			h = mix(h, bits);
		}
	}
	return mix(h, fib(15));
}

/* brk and anonymous mmap: grow, write, read back. */
static u64 test_mm(void)
{
	u64 h = 0xcbf29ce484222325UL;
	u64 brk0 = (u64)sys(SYS_brk, 0, 0, 0, 0, 0, 0);
	u64 brk1 = (u64)sys(SYS_brk, (long)(brk0 + 8192), 0, 0, 0, 0, 0);
	volatile u64 *heap = (volatile u64 *)brk0;
	volatile unsigned char *map;

	h = mix(h, brk1 - brk0);
	for (unsigned i = 0; i < 1024; i++)
		heap[i] = i * 0x9e3779b97f4a7c15UL;
	for (unsigned i = 0; i < 1024; i++)
		h = mix(h, heap[i]);

	map = (volatile unsigned char *)sys(SYS_mmap, 0, 65536, 3 /* RW */,
					    0x22 /* PRIVATE|ANONYMOUS */, -1, 0);
	h = mix(h, (s64)map > 0);
	if ((s64)map > 0) {
		u64 zeros = 0;

		for (unsigned i = 0; i < 65536; i += 4096)
			zeros += map[i] == 0;
		h = mix(h, zeros);
		for (unsigned i = 0; i < 65536; i += 97)
			map[i] = (unsigned char)i;
		for (unsigned i = 0; i < 65536; i += 97)
			h = mix(h, map[i]);
	}
	return h;
}

void _start(void)
{
	put_hex("ops", test_ops());
	put_hex("div", test_div());
	put_hex("imm", test_imm());
	put_hex("mem", test_mem());
	put_hex("branch", test_branch());
	put_hex("mm", test_mm());
	flush();
	sys(SYS_exit, 0, 0, 0, 0, 0, 0);
	for (;;)
		;
}
