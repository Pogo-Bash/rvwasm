/*
 * Phase 0 verification: how does NOMMU mmap behave with large regions?
 *
 * Walks a ladder of anonymous mapping sizes and reports which succeed, then
 * checks the operations a user-mode translator would want from the kernel:
 * address hints, MAP_FIXED, partial munmap, mprotect, brk, file mappings.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MB (1024UL * 1024UL)

/* Integer microseconds: floating-point printf traps on wasm64 (see fp_probe.c). */
static long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static void *try_map(size_t len, int extra_flags, const char *label)
{
	long t0 = now_us();
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | extra_flags, -1, 0);
	long t1 = now_us();

	if (p == MAP_FAILED) {
		printf("  %-14s %6lu MiB: FAIL errno=%d (%s)\n", label,
		       (unsigned long)(len / MB), errno, strerror(errno));
		return NULL;
	}
	printf("  %-14s %6lu MiB: ok at %p, %ld us\n", label,
	       (unsigned long)(len / MB), p, t1 - t0);
	return p;
}

/* Not main(void): clang's wasm target only gives libc the entry it calls
 * (__main_argc_argv) when main takes argc and argv. */
int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	static const size_t ladder[] = { 1, 2, 3, 4, 5, 8, 16, 32, 64, 65, 128,
					 256, 512, 1024, 2048 };
	long page = sysconf(_SC_PAGESIZE);
	void *p, *q;

	printf("page size: %ld\n", page);

	printf("anonymous mmap ladder (each unmapped again):\n");
	for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
		size_t len = ladder[i] * MB;

		p = try_map(len, 0, "anon");
		if (!p)
			continue;
		/* Is it really zeroed and writable at both ends? */
		if (((unsigned char *)p)[0] || ((unsigned char *)p)[len - 1])
			printf("    not zeroed!\n");
		((unsigned char *)p)[0] = 1;
		((unsigned char *)p)[len - 1] = 1;
		if (munmap(p, len))
			printf("    munmap failed: %s\n", strerror(errno));
	}

#ifdef MAP_UNINITIALIZED
	printf("MAP_UNINITIALIZED (skips zeroing if the kernel allows it):\n");
	p = try_map(64 * MB, MAP_UNINITIALIZED, "uninit");
	if (p)
		munmap(p, 64 * MB);
#endif

	printf("address control:\n");
	p = try_map(2 * MB, 0, "base");
	if (p) {
		/* Hint: an address we know is free once we unmap it. */
		munmap(p, 2 * MB);
		q = mmap(p, 1 * MB, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		printf("  hint %p -> %p (%s)\n", p, q,
		       q == MAP_FAILED ? strerror(errno) : q == p ? "honoured" : "ignored");
		if (q != MAP_FAILED)
			munmap(q, 1 * MB);

		q = mmap(p, 1 * MB, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		printf("  MAP_FIXED at %p -> %s\n", p,
		       q == MAP_FAILED ? strerror(errno) : "ok");
		if (q != MAP_FAILED)
			munmap(q, 1 * MB);
	}

	q = mmap((void *)0x10000, 1 * MB, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	printf("  MAP_FIXED at 0x10000 (non-PIE riscv load address) -> %s\n",
	       q == MAP_FAILED ? strerror(errno) : "ok");

	printf("partial munmap / mprotect / mremap:\n");
	p = try_map(4 * MB, 0, "region");
	if (p) {
		int r;

		r = munmap((char *)p + 3 * MB, 1 * MB);
		printf("  munmap tail 1 MiB   -> %s\n", r ? strerror(errno) : "ok");
		r = munmap((char *)p + 1 * MB, 1 * MB);
		printf("  munmap middle 1 MiB -> %s\n", r ? strerror(errno) : "ok");
		r = mprotect(p, 1 * MB, PROT_READ);
		printf("  mprotect PROT_READ  -> %s\n", r ? strerror(errno) : "ok");
		r = mprotect(p, 1 * MB, PROT_READ | PROT_WRITE | PROT_EXEC);
		printf("  mprotect RWX        -> %s\n", r ? strerror(errno) : "ok");
		q = mremap(p, 1 * MB, 2 * MB, MREMAP_MAYMOVE);
		printf("  mremap grow 1->2    -> %s\n",
		       q == MAP_FAILED ? strerror(errno) : "ok");
	}

	printf("brk:\n");
	p = sbrk(0);
	q = sbrk(1 * MB);
	printf("  sbrk(0) = %p, sbrk(1 MiB) -> %s\n", p,
	       q == (void *)-1 ? strerror(errno) : "ok");

	printf("file mapping (private, of our own binary):\n");
	{
		int fd = open("/proc/self/exe", O_RDONLY);

		if (fd < 0) {
			printf("  open /proc/self/exe: %s\n", strerror(errno));
		} else {
			q = mmap(NULL, 64 * 1024, PROT_READ, MAP_PRIVATE, fd, 0);
			if (q == MAP_FAILED)
				printf("  MAP_PRIVATE file -> %s\n", strerror(errno));
			else
				printf("  MAP_PRIVATE file -> ok, magic %02x %02x %02x %02x\n",
				       ((unsigned char *)q)[0], ((unsigned char *)q)[1],
				       ((unsigned char *)q)[2], ((unsigned char *)q)[3]);
			q = mmap(NULL, 64 * 1024, PROT_READ, MAP_SHARED, fd, 0);
			printf("  MAP_SHARED file  -> %s\n",
			       q == MAP_FAILED ? strerror(errno) : "ok");
			close(fd);
		}
	}

	printf("fragmentation: hold 8 x 4 MiB, free every other, then ask for 8 MiB:\n");
	{
		void *held[8];

		for (int i = 0; i < 8; i++)
			held[i] = mmap(NULL, 4 * MB, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		for (int i = 0; i < 8; i += 2)
			if (held[i] != MAP_FAILED)
				munmap(held[i], 4 * MB);
		p = try_map(8 * MB, 0, "after-frag");
		if (p)
			munmap(p, 8 * MB);
		for (int i = 1; i < 8; i += 2)
			if (held[i] != MAP_FAILED)
				munmap(held[i], 4 * MB);
	}

	printf("done\n");
	return 0;
}
