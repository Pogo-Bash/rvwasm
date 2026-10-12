/*
 * mmap() of a file at a nonzero offset. The file is three pages: page n is filled with the letter 'A' + n.
 * Expect the mapping at offset 4096 to begin with 'B'.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	char page[4096];
	int fd = open("/tmp/mmapoff", O_CREAT | O_RDWR | O_TRUNC, 0644), i;
	char *m;

	for (i = 0; i < 3; i++) {
		memset(page, 'A' + i, sizeof page);
		write(fd, page, sizeof page);
	}
	m = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
	if (m == MAP_FAILED) {
		perror("mmap at offset 4096");
		return 1;
	}
	printf("mmap at offset 4096 begins with '%c' (%s)\n", m[0], m[0] == 'B' ? "ok" : "WRONG");
	munmap(m, 4096);
	close(fd);
	unlink("/tmp/mmapoff");
	return 0;
}
