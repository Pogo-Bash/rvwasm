/*
 * chroot(): into a directory made for the occasion, then what a program
 * does there: list /, open, readlink /proc/self/exe (there is no /proc in
 * the new root), fork, and exec a program that is inside it (a copy of
 * this one, static), which forks in its turn. And the errors chroot()
 * gives. Needs the right to chroot (root, or a user namespace of its own).
 * The output is compared with a real kernel's (qemu-riscv64 passes the
 * call through). riscv64, static.
 *
 * Usage: chroot DIR    (DIR must not exist; it is made, and left behind)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failed;

static void check(int ok, const char *what)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		failed = 1;
}

static int cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names in / , sorted, on one line. */
static void list_root(const char *who)
{
	char *names[64];
	int n = 0;
	DIR *d = opendir("/");
	struct dirent *e;

	if (!d) {
		printf("%s: opendir /: %s\n", who, strerror(errno));
		failed = 1;
		return;
	}
	while ((e = readdir(d)) && n < 64)
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			names[n++] = strdup(e->d_name);
	closedir(d);
	qsort(names, (size_t)n, sizeof(names[0]), cmp);
	printf("%s: / has", who);
	for (int k = 0; k < n; k++)
		printf(" %s", names[k]);
	printf("\n");
}

static int copy(const char *from, const char *to)
{
	char buf[65536];
	int in = open(from, O_RDONLY), out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	ssize_t n;

	if (in < 0 || out < 0)
		return -1;
	while ((n = read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, (size_t)n) != n)
			return -1;
	close(in);
	return close(out) || n < 0 ? -1 : 0;
}

/* The copy, started inside the new root by exec. */
static int inside(void)
{
	pid_t pid;
	int status;

	list_root("after exec");
	pid = fork();
	if (pid == 0) {
		list_root("child of the exec");
		_exit(9);
	}
	check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 9,
	      "fork after exec");
	return failed;
}

int main(int argc, char **argv)
{
	char path[4096], link[64];
	pid_t pid;
	int status, fd;
	ssize_t n;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc == 2 && !strcmp(argv[1], "--inside"))
		return inside();
	if (argc != 2) {
		fprintf(stderr, "usage: chroot DIR\n");
		return 2;
	}

	/* The new root: a file, a directory, and this program. */
	check(mkdir(argv[1], 0755) == 0, "mkdir");
	snprintf(path, sizeof(path), "%s/sub", argv[1]);
	check(mkdir(path, 0755) == 0, "mkdir sub");
	snprintf(path, sizeof(path), "%s/hello", argv[1]);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	check(fd >= 0 && write(fd, "hello from the new root\n", 24) == 24 && close(fd) == 0, "file");
	snprintf(path, sizeof(path), "%s/self", argv[1]);
	check(copy("/proc/self/exe", path) == 0, "copy of this program");

	/* What chroot() says no to. */
	errno = 0;
	check(chroot("/no/such/directory") < 0 && errno == ENOENT, "chroot of nothing is ENOENT");
	snprintf(path, sizeof(path), "%s/hello", argv[1]);
	errno = 0;
	check(chroot(path) < 0 && errno == ENOTDIR, "chroot of a file is ENOTDIR");

	if (chroot(argv[1]) < 0) {
		printf("chroot: %s\n", strerror(errno));
		return 1;
	}
	check(chdir("/") == 0, "chdir /");
	list_root("after chroot");
	check(getcwd(path, sizeof(path)) && !strcmp(path, "/"), "getcwd is /");

	fd = open("/hello", O_RDONLY);
	n = fd >= 0 ? read(fd, path, sizeof(path) - 1) : -1;
	path[n > 0 ? n : 0] = '\0';
	check(n == 24 && !strcmp(path, "hello from the new root\n"), "open /hello");
	if (fd >= 0)
		close(fd);
	check(access("/sub", F_OK) == 0 && access("/sub/../hello", F_OK) == 0, "/sub and /sub/..");
	check(access("/../../hello", F_OK) == 0, "/.. is / again");

	/* No /proc here: the link is not there either. */
	errno = 0;
	n = readlink("/proc/self/exe", link, sizeof(link));
	check(n < 0 && errno == ENOENT, "no /proc/self/exe in the new root");

	pid = fork();
	if (pid == 0) {
		list_root("child of a fork");
		_exit(7);
	}
	check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7,
	      "fork after chroot");

	pid = fork();
	if (pid == 0) {
		execl("/self", "self", "--inside", (char *)NULL);
		printf("exec /self: %s\n", strerror(errno));
		_exit(127);
	}
	check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "exec inside the new root");

	/* A second chroot, from inside the first, relative to the working directory. */
	check(chdir("/sub") == 0 && chroot(".") == 0, "chroot . from /sub");
	list_root("after the second chroot");

	printf("chroot: %s\n", failed ? "FAILED" : "all ok");
	return failed;
}
