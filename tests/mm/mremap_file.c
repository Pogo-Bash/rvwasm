/*
 * A mapping of a file that is made larger with mremap() shows more of the
 * file, or mremap() fails and the file can be mapped again; never pages of
 * zeros where the file has data. (SQLite grows its read-only mapping of a
 * database this way as the file grows: zeros were a malformed database.)
 * Prints the same on any correct system, whichever of the two it does.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096

static int failed;

static void check(const char *what, int ok)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failed = 1;
}

/* Is [p, p + len) what the file has at `offset`? */
static int same(const unsigned char *p, size_t len, size_t offset)
{
	for (size_t i = 0; i < len; i++)
		if (p[i] != (unsigned char)((offset + i) * 7 / PAGE + 1))
			return 0;
	return 1;
}

static void grow(const char *path, int flags, int prot)
{
	static unsigned char buf[4 * PAGE];
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	unsigned char *p, *q;
	char what[64];

	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)(i * 7 / PAGE + 1);
	/* One page of the file first; mapped; then the file grows by writing, as SQLite's does. */
	if (fd < 0 || pwrite(fd, buf, PAGE, 0) != PAGE) {
		check("the file", 0);
		return;
	}
	p = mmap(NULL, PAGE, prot, flags, fd, 0);
	if (p == MAP_FAILED) {
		check("the first mapping", 0);
		return;
	}
	if (pwrite(fd, buf + PAGE, 3 * PAGE, PAGE) != 3 * PAGE) {
		check("the file grows", 0);
		return;
	}
	q = mremap(p, PAGE, 4 * PAGE, MREMAP_MAYMOVE);
	if (q == MAP_FAILED) {
		/* Allowed: then a new mapping of all of it. */
		munmap(p, PAGE);
		q = mmap(NULL, 4 * PAGE, prot, flags, fd, 0);
	}
	snprintf(what, sizeof(what), "%s, grown: the file's data", flags & MAP_SHARED ? "shared" : "private");
	check(what, q != MAP_FAILED && same(q, 4 * PAGE, 0));
	if (q != MAP_FAILED)
		munmap(q, 4 * PAGE);
	close(fd);
	unlink(path);
}

int main(void)
{
	char path[] = "/tmp/mremap_file-XXXXXX";
	int fd = mkstemp(path);

	if (fd < 0)
		return 1;
	close(fd);
	grow(path, MAP_SHARED, PROT_READ);
	grow(path, MAP_PRIVATE, PROT_READ);
	printf("mremap_file: %s\n", failed ? "FAILED" : "all ok");
	return failed;
}
