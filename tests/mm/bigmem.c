/*
 * A lot of memory, used: MIB mebibytes in pieces of 64 MiB, every page
 * written with a pattern of its own, then read back; and every piece handed
 * to system calls (written to a pipe and read back from it, read from
 * /dev/zero into), which the kernel only does for a buffer it takes to be
 * the program's. Under rv the guest's pages are the emulator's memory,
 * wherever the kernel found it, so several of these at once cover most of a
 * machine's memory (tests/mm: the machine's size, tools/www/dev.html).
 *
 *   bigmem MIB [SEED [HOLD]]	prints "bigmem: ok ..." or what went wrong; exit 1 then.
 *				HOLD: seconds to keep the memory afterwards.
 * (riscv64, static.)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PIECE	(64UL << 20)
#define PAGE	4096UL

static uint64_t word(uint64_t seed, uint64_t piece, uint64_t page, uint64_t i)
{
	uint64_t x = seed * 0x9e3779b97f4a7c15ULL ^ piece << 40 ^ page << 12 ^ i;

	x ^= x >> 31;
	x *= 0xbf58476d1ce4e5b9ULL;
	return x ^ x >> 29;
}

static int fail(const char *what, uint64_t piece, uint64_t page)
{
	printf("bigmem: FAILED: %s (piece %llu, page %llu, errno %d)\n", what,
	       (unsigned long long)piece, (unsigned long long)page, errno);
	return 1;
}

int main(int argc, char **argv)
{
	uint64_t mib = argc > 1 ? strtoull(argv[1], NULL, 0) : 256;
	uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	uint64_t pieces = (mib << 20) / PIECE;
	uint64_t **at = calloc(pieces, sizeof(*at));
	int zero = open("/dev/zero", O_RDONLY);
	int fds[2];
	static uint64_t back[PAGE / 8];

	if (!at || zero < 0 || pipe(fds))
		return fail("setup", 0, 0);
	for (uint64_t p = 0; p < pieces; p++) {
		at[p] = mmap(NULL, PIECE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (at[p] == MAP_FAILED)
			return fail("mmap", p, 0);
		for (uint64_t g = 0; g < PIECE / PAGE; g++)
			for (uint64_t i = 0; i < PAGE / 8; i++)
				at[p][g * PAGE / 8 + i] = word(seed, p, g, i);
	}
	/* The system calls: each piece's first and last page through a pipe, the page after the first from /dev/zero. */
	for (uint64_t p = 0; p < pieces; p++) {
		uint64_t pages[2] = { 0, PIECE / PAGE - 1 };

		for (int k = 0; k < 2; k++) {
			uint64_t *page = at[p] + pages[k] * PAGE / 8;

			if (write(fds[1], page, PAGE) != (ssize_t)PAGE)
				return fail("write from it", p, pages[k]);
			if (read(fds[0], back, PAGE) != (ssize_t)PAGE)
				return fail("read back", p, pages[k]);
			if (memcmp(back, page, PAGE))
				return fail("what came back through the pipe", p, pages[k]);
			if (read(fds[0], page, 0) != 0)
				return fail("a read of nothing into it", p, pages[k]);
		}
		if (read(zero, at[p] + PAGE / 8, PAGE) != (ssize_t)PAGE)
			return fail("read into it", p, 1);
		for (uint64_t i = 0; i < PAGE / 8; i++) {
			if (at[p][PAGE / 8 + i])
				return fail("zeros from /dev/zero", p, 1);
			at[p][PAGE / 8 + i] = word(seed, p, 1, i);
		}
	}
	uint64_t sum = 0;

	for (uint64_t p = 0; p < pieces; p++)
		for (uint64_t g = 0; g < PIECE / PAGE; g++)
			for (uint64_t i = 0; i < PAGE / 8; i++) {
				uint64_t w = at[p][g * PAGE / 8 + i];

				if (w != word(seed, p, g, i))
					return fail("read back what was written", p, g);
				sum += w;
			}
	printf("bigmem: ok: %llu MiB written and read back, sum %016llx\n",
	       (unsigned long long)(pieces * PIECE >> 20), (unsigned long long)sum);
	fflush(stdout);
	if (argc > 3)
		sleep((unsigned)strtoul(argv[3], NULL, 0));
	return 0;
}
