/*
 * A file mapped shared and writable by two processes: what one writes the
 * other sees, and the file has it after msync() and after the mappings are
 * gone. (riscv64, static: tools/stage.sh. RV_SHARE_HOST=0 makes rv take the
 * path of a host without an MMU on one that has one.)
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define SIZE 32768

/* A byte through a pipe, to take turns. */
static void put(int fd, char c)
{
	if (write(fd, &c, 1) != 1)
		exit(2);
}

static char get(int fd)
{
	char c = 0;

	if (read(fd, &c, 1) != 1)
		exit(2);
	return c;
}

static void check(int ok, const char *what)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		exit(1);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "live_map.tmp";
	char buf[16] = "";
	volatile int *p, *q;
	int fd, pipes[2], back[2], status;
	pid_t pid;

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	check(fd >= 0 && ftruncate(fd, SIZE) == 0, "create");
	check(pwrite(fd, "start", 5, 4096) == 5, "write");
	p = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(p != MAP_FAILED, "mmap");
	check(!memcmp((const char *)p + 4096, "start", 5), "the file's contents");

	/* A second mapping in the same process is the same memory. */
	q = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(q != MAP_FAILED && q != p, "second mmap");
	p[1] = 11;
	check(q[1] == 11, "two mappings, one memory");
	check(munmap((void *)q, SIZE) == 0, "munmap of one");

	check(pipe(pipes) == 0 && pipe(back) == 0, "pipes");
	pid = fork();
	check(pid >= 0, "fork");
	if (pid == 0) {
		char c;

		/* The child has the mapping too: tell the parent through it, and hear back. */
		p[2] = 22;
		put(back[1], 'a');
		c = get(pipes[0]);
		if (p[3] != 33)
			_exit(3);
		/* And a process that maps the file anew gets the same memory. */
		if (c == 'x') {
			int fd2 = open(path, O_RDWR);
			volatile int *r = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);

			if (r == MAP_FAILED || r[3] != 33 || r[1] != 11)
				_exit(4);
			r[4] = 44;
		}
		put(back[1], 'b');
		_exit(0);
	}
	get(back[0]);
	check(p[2] == 22, "the child's write, seen by the parent");
	p[3] = 33;
	put(pipes[1], 'x');
	get(back[0]);
	check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "the parent's write, seen by the child");
	check(p[4] == 44, "a new mapping in the child, seen by the parent");

	memcpy((char *)p + 8192, "synced", 6);
	check(msync((void *)p, SIZE, MS_SYNC) == 0, "msync");
	check(pread(fd, buf, 6, 8192) == 6 && !memcmp(buf, "synced", 6), "the file after msync");

	memcpy((char *)p + 12288, "unmapped", 8);
	check(munmap((void *)p, SIZE) == 0, "munmap");
	check(pread(fd, buf, 8, 12288) == 8 && !memcmp(buf, "unmapped", 8), "the file after munmap");
	check(pread(fd, buf, 4, 4) == 4 && *(int *)buf == 11, "the first write, in the file");

	/* Not open for writing: no shared writable mapping. */
	close(fd);
	fd = open(path, O_RDONLY);
	check(mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED, "read-only file refused");
	unlink(path);
	puts("live_map: all ok");
	return 0;
}
