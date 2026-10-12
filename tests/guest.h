/*
 * Support for freestanding riscv64 guest tests: raw system calls and a small
 * buffered writer. No libc, so a test depends on nothing but the emulator.
 */
#ifndef GUEST_H
#define GUEST_H

typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef int s32;

#define SYS_write	64
#define SYS_exit	93
#define SYS_brk		214
#define SYS_munmap	215
#define SYS_mremap	216
#define SYS_mmap	222
#define SYS_mprotect	226

#define PROT_NONE	0
#define PROT_READ	1
#define PROT_WRITE	2
#define PROT_EXEC	4
#define MAP_PRIVATE	0x02
#define MAP_FIXED	0x10
#define MAP_ANONYMOUS	0x20
#define MAP_FIXED_NOREPLACE 0x100000
#define MREMAP_MAYMOVE	1

static inline long sys(long n, long a, long b, long c, long d, long e, long f)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	register long a5 __asm__("a5") = f;

	__asm__ volatile("ecall" : "+r"(a0)
			 : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
			 : "memory");
	return a0;
}

static char out[4096];
static unsigned long out_len;

static void flush(void)
{
	sys(SYS_write, 1, (long)out, (long)out_len, 0, 0, 0);
	out_len = 0;
}

static void put(const char *s)
{
	while (*s) {
		if (out_len == sizeof(out))
			flush();
		out[out_len++] = *s++;
	}
}

static inline void put_hex(const char *label, u64 v)
{
	char buf[20];

	put(label);
	put("=");
	for (int i = 0; i < 16; i++) {
		unsigned d = (v >> (60 - 4 * i)) & 15;

		buf[i] = d < 10 ? '0' + d : 'a' + d - 10;
	}
	buf[16] = '\n';
	buf[17] = 0;
	put(buf);
}

/* FNV-style running hash, to fold many results into one printed value. */
static inline u64 mix(u64 h, u64 v)
{
	h ^= v;
	h *= 0x100000001b3UL;
	return h ^ (h >> 29);
}
#define MIX_INIT 0xcbf29ce484222325UL

static inline void finish(void)
{
	flush();
	sys(SYS_exit, 0, 0, 0, 0, 0, 0);
	for (;;)
		;
}

#endif /* GUEST_H */
