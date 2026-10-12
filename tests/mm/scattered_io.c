/*
 * read() and write() with a buffer whose pages the emulator has in many
 * pieces of host memory: the pages are touched in an order that makes them
 * so (every other one first), then a file is written from the buffer and
 * read back into another one made the same way, in one call each and at
 * odd offsets. (riscv64, static: tools/stage.sh.)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGES 3000
#define SIZE (PAGES * 4096UL)

static uint8_t *scattered(void)
{
	uint8_t *p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		exit(2);
	for (int pass = 0; pass < 2; pass++)
		for (unsigned long i = pass ? 0 : 37; i < PAGES; i += 37 * 2)
			for (unsigned long k = i; k < i + 37 && k < PAGES; k++)
				p[k * 4096] = 1;
	return p;
}

static void check(int ok, const char *what)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		exit(1);
}

int main(void)
{
	uint8_t *a = scattered(), *b = scattered();
	int fd = open("scattered_io.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
	void *volatile bad = (void *)8;
	int pipes[2];

	for (unsigned long i = 0; i < SIZE; i++)
		a[i] = (uint8_t)(i * 2654435761UL >> 13);
	check(fd >= 0, "create");
	check(write(fd, a + 5, SIZE - 9) == (ssize_t)(SIZE - 9), "write");
	check(lseek(fd, 0, SEEK_SET) == 0, "seek");
	memset(b, 0, SIZE);
	check(read(fd, b + 5, SIZE - 5) == (ssize_t)(SIZE - 9), "read");
	check(!memcmp(a + 5, b + 5, SIZE - 9), "what was read");
	memset(b, 0, SIZE);
	check(pread(fd, b + 4099, SIZE - 8200, 4094) == (ssize_t)(SIZE - 8200), "pread");
	check(!memcmp(a + 4099, b + 4099, SIZE - 8200), "what pread read");
	check(pwrite(fd, a + 1000, SIZE - 2000, 123) == (ssize_t)(SIZE - 2000), "pwrite");
	check(pread(fd, b, 4096, 123) == 4096 && !memcmp(a + 1000, b, 4096), "what pwrite wrote");
	check(lseek(fd, 0, SEEK_CUR) == (off_t)(SIZE - 9), "the position, untouched by pread and pwrite");
	check(read(fd, b, 0) == 0 && write(fd, a, 0) == 0, "nothing");
	check(read(fd, bad, 100) < 0, "a bad address");
	/* A pipe gives what it has: less than was asked for. */
	check(pipe(pipes) == 0 && write(pipes[1], a, 30000) == 30000, "pipe");
	check(read(pipes[0], b, SIZE) == 30000 && !memcmp(a, b, 30000), "a short read");
	unlink("scattered_io.tmp");
	puts("scattered_io: all ok");
	return 0;
}
