/*
 * madvise(MADV_DONTNEED): memory of the process's own reads as zeros
 * afterwards, a private mapping of a file shows the file again or still,
 * and a shared mapping keeps what was written. (riscv64, static.)
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SIZE (64 * 4096UL)

static void check(int ok, const char *what)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		exit(1);
}

static int all(const unsigned char *p, unsigned long n, unsigned char value)
{
	for (unsigned long i = 0; i < n; i++)
		if (p[i] != value)
			return 0;
	return 1;
}

int main(void)
{
	unsigned char *p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), *f, *s;
	int fd;

	check(p != MAP_FAILED, "mmap");
	memset(p, 0xaa, SIZE);
	check(madvise(p + 4096, SIZE - 2 * 4096, MADV_DONTNEED) == 0, "madvise");
	check(all(p, 4096, 0xaa) && all(p + SIZE - 4096, 4096, 0xaa), "the pages around keep theirs");
	check(all(p + 4096, SIZE - 2 * 4096, 0), "zeros afterwards");
	memset(p, 0x55, SIZE);
	check(all(p, SIZE, 0x55), "and can be written again");
	check(madvise(p, SIZE, MADV_DONTNEED) == 0 && all(p, SIZE, 0), "a second time");
	check(madvise(p + 1, 4096, MADV_DONTNEED) < 0, "an address inside a page is refused");
	check(madvise(p, SIZE, MADV_WILLNEED) == 0 && madvise(p, SIZE, MADV_FREE) == 0, "other advice");

	/* The heap, as malloc has it. */
	{
		unsigned char *h = sbrk(8 * 4096);

		memset(h, 0x77, 8 * 4096);
		h = (unsigned char *)(((unsigned long)h + 4095) & ~4095UL);
		check(madvise(h, 4 * 4096, MADV_DONTNEED) == 0 && all(h, 4 * 4096, 0), "the heap");
	}

	fd = open("madvise.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
	check(fd >= 0 && ftruncate(fd, SIZE) == 0 && pwrite(fd, "file", 4, 0) == 4, "a file");
	f = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	check(f != MAP_FAILED && !memcmp(f, "file", 4), "mapped private");
	check(madvise(f, SIZE, MADV_DONTNEED) == 0 && !memcmp(f, "file", 4), "still the file's");
	s = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(s != MAP_FAILED, "mapped shared");
	memcpy(s + 8192, "kept", 4);
	check(madvise(s, SIZE, MADV_DONTNEED) == 0 && !memcmp(s + 8192, "kept", 4), "a shared mapping keeps what was written");
	unlink("madvise.tmp");
	puts("madvise: all ok");
	return 0;
}
