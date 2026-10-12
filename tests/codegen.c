/*
 * Code that changes at run time, the way a dynamic loader or a JIT inside the
 * guest produces it: write a function into a fresh mapping, make it
 * executable, call it many times, then replace it and call it again.
 *
 * An emulator that caches translations must notice every replacement; one
 * that does not prints the first function's result three times.
 */
#include "guest.h"

#define PAGE 4096UL

typedef u64 (*fn_t)(u64);

/* addi a0, a0, k ; ret */
static void write_add(u32 *code, int k)
{
	code[0] = 0x00050513u | ((u32)k << 20);
	code[1] = 0x00008067u;
}

/* slli a0, a0, k ; ret */
static void write_shift(u32 *code, int k)
{
	code[0] = 0x00051513u | ((u32)k << 20);
	code[1] = 0x00008067u;
}

static u64 run_hot(fn_t fn)
{
	u64 v = 1;

	/* Enough calls for any translation threshold. */
	for (int i = 0; i < 5000; i++)
		v = fn(v) & 0xffff;
	return v;
}

void _start(void)
{
	u32 *code = (u32 *)sys(SYS_mmap, 0, PAGE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	fn_t fn = (fn_t)code;
	u32 *other;

	/* Version 1, hot. */
	write_add(code, 3);
	put_hex("protect_rx", (u64)sys(SYS_mprotect, (long)code, PAGE, PROT_READ | PROT_EXEC, 0, 0, 0));
	put_hex("v1", run_hot(fn));

	/* Replace it in place: writable again, new code, executable again. */
	sys(SYS_mprotect, (long)code, PAGE, PROT_READ | PROT_WRITE, 0, 0, 0);
	write_add(code, 7);
	sys(SYS_mprotect, (long)code, PAGE, PROT_READ | PROT_EXEC, 0, 0, 0);
	put_hex("v2", run_hot(fn));

	/* Unmap, and map different code at the same address. */
	sys(SYS_munmap, (long)code, PAGE, 0, 0, 0, 0);
	other = (u32 *)sys(SYS_mmap, (long)code, PAGE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	put_hex("same_address", other == code);
	write_shift(other, 1);
	sys(SYS_mprotect, (long)other, PAGE, PROT_READ | PROT_EXEC, 0, 0, 0);
	put_hex("v3", run_hot(fn));

	/* A page that stays writable and executable: changed between calls, no mprotect. */
	sys(SYS_mprotect, (long)code, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, 0, 0, 0);
	{
		u64 v = 0;

		for (int i = 0; i < 3000; i++) {
			write_add(code, i & 15);
			__asm__ volatile(".word 0x0000100f");	/* fence.i */
			v = (v + fn(v)) & 0xffffff;
		}
		put_hex("v4", v);
	}

	finish();
}
