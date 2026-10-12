/*
 * Address-space semantics: what mmap, munmap, mprotect, mremap and brk
 * promise a program, checked by outcome rather than by address so that the
 * output is the same on any correct implementation.
 */
#include "guest.h"

#define PAGE 4096UL

static void *map(void *addr, u64 len, long flags)
{
	return (void *)sys(SYS_mmap, (long)addr, (long)len, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | flags, -1, 0);
}

static int is_zero(volatile unsigned char *p, u64 len)
{
	for (u64 i = 0; i < len; i++)
		if (p[i])
			return 0;
	return 1;
}

static void fill(volatile unsigned char *p, u64 len, unsigned char v)
{
	for (u64 i = 0; i < len; i++)
		p[i] = v;
}

static int all(volatile unsigned char *p, u64 len, unsigned char v)
{
	for (u64 i = 0; i < len; i++)
		if (p[i] != v)
			return 0;
	return 1;
}

void _start(void)
{
	unsigned char *p, *q, *r;
	u64 brk0;

	/* Fresh anonymous memory is zero. */
	p = map(0, 3 * PAGE, 0);
	put_hex("map_ok", (s64)p > 0);
	put_hex("map_zero", is_zero(p, 3 * PAGE));
	fill(p, 3 * PAGE, 0xaa);

	/* A hole punched in the middle can be refilled at the same address, zeroed. */
	put_hex("unmap_middle", (u64)sys(SYS_munmap, (long)(p + PAGE), PAGE, 0, 0, 0, 0));
	q = map(p + PAGE, PAGE, 0);
	put_hex("hint_honoured", q == p + PAGE);
	put_hex("refill_zero", is_zero(q, PAGE));
	put_hex("neighbours_kept", all(p, PAGE, 0xaa) && all(p + 2 * PAGE, PAGE, 0xaa));

	/* MAP_FIXED replaces what is there; MAP_FIXED_NOREPLACE refuses to. */
	q = map(p, PAGE, MAP_FIXED);
	put_hex("fixed_same_addr", q == p);
	put_hex("fixed_zero", is_zero(p, PAGE));
	put_hex("fixed_noreplace", (u64)map(p, PAGE, MAP_FIXED_NOREPLACE));
	put_hex("fixed_unaligned", (u64)map(p + 1, PAGE, MAP_FIXED));
	put_hex("zero_length", (u64)map(0, 0, 0));

	/* mprotect needs a mapping; munmap needs alignment. */
	put_hex("mprotect_ro", (u64)sys(SYS_mprotect, (long)p, PAGE, PROT_READ, 0, 0, 0));
	put_hex("mprotect_rw", (u64)sys(SYS_mprotect, (long)p, PAGE, PROT_READ | PROT_WRITE, 0, 0, 0));
	sys(SYS_munmap, (long)(p + PAGE), PAGE, 0, 0, 0, 0);
	put_hex("mprotect_hole", (u64)sys(SYS_mprotect, (long)p, 3 * PAGE, PROT_READ, 0, 0, 0));
	put_hex("munmap_unaligned", (u64)sys(SYS_munmap, (long)(p + 1), PAGE, 0, 0, 0, 0));
	put_hex("munmap_twice", (u64)sys(SYS_munmap, (long)(p + PAGE), PAGE, 0, 0, 0, 0));

	/* mremap: growing keeps the contents and zeroes the new part. */
	r = map(0, 2 * PAGE, 0);
	fill(r, 2 * PAGE, 0x55);
	q = (unsigned char *)sys(SYS_mremap, (long)r, 2 * PAGE, 8 * PAGE, MREMAP_MAYMOVE, 0, 0);
	put_hex("mremap_grow_ok", (s64)q > 0);
	put_hex("mremap_kept", all(q, 2 * PAGE, 0x55));
	put_hex("mremap_new_zero", is_zero(q + 2 * PAGE, 6 * PAGE));
	r = (unsigned char *)sys(SYS_mremap, (long)q, 8 * PAGE, PAGE, MREMAP_MAYMOVE, 0, 0);
	/* (Whether a shrunk mapping stays in place is up to the implementation.) */
	put_hex("mremap_shrink_ok", (s64)r > 0);
	put_hex("mremap_shrink_kept", all(r, PAGE, 0x55));

	/* Growing without permission to move, into an occupied neighbour, fails. */
	p = map(0, 2 * PAGE, 0);
	sys(SYS_munmap, (long)(p + PAGE), PAGE, 0, 0, 0, 0);
	q = map(p + PAGE, PAGE, 0);
	put_hex("mremap_blocked", (u64)sys(SYS_mremap, (long)p, PAGE, 2 * PAGE, 0, 0, 0));

	/* brk: grows, keeps data, and memory given back comes back zeroed. */
	brk0 = (u64)sys(SYS_brk, 0, 0, 0, 0, 0, 0);
	put_hex("brk_aligned", (brk0 & (PAGE - 1)) == 0);
	put_hex("brk_grow", (u64)sys(SYS_brk, (long)(brk0 + 3 * PAGE), 0, 0, 0, 0, 0) - brk0);
	fill((unsigned char *)brk0, 3 * PAGE, 0x77);
	put_hex("brk_shrink", (u64)sys(SYS_brk, (long)(brk0 + PAGE), 0, 0, 0, 0, 0) - brk0);
	put_hex("brk_regrow", (u64)sys(SYS_brk, (long)(brk0 + 3 * PAGE), 0, 0, 0, 0, 0) - brk0);
	put_hex("brk_kept", all((unsigned char *)brk0, PAGE, 0x77));
	put_hex("brk_regrow_zero", is_zero((unsigned char *)(brk0 + PAGE), 2 * PAGE));
	put_hex("brk_below_start", (u64)sys(SYS_brk, (long)(brk0 - PAGE), 0, 0, 0, 0, 0) - brk0);

	finish();
}
