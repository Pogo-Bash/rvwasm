/*
 * Descriptors across fork and exec: one marked close-on-exec has to survive
 * a fork (the child uses it) and not survive the exec after it.
 * This is what a program does that starts a helper connected by pipes, apt
 * for one. Run by tools/thread-check.sh.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int to_child[2], from_child[2], status, kept;
	char buf[128] = "";
	pid_t pid;

	/* The helper: it was exec'd; report on the descriptors it was told about. */
	if (argc == 4 && !strcmp(argv[1], "helper")) {
		int closed = fcntl(atoi(argv[2]), F_GETFD) < 0 && fcntl(atoi(argv[3]), F_GETFD) < 0;
		char line[32];
		ssize_t n = read(0, line, sizeof(line) - 1);

		line[n > 0 ? n : 0] = 0;
		printf("helper got \"%s\"; marked descriptors %s\n", line, closed ? "closed by exec" : "STILL OPEN");
		return 7;
	}

	if (pipe2(to_child, O_CLOEXEC) || pipe2(from_child, O_CLOEXEC))
		return 1;
	kept = open("/dev/null", O_RDONLY);		/* not marked: the helper inherits it */
	pid = fork();
	if (pid == 0) {
		char a[16], b[16];

		/* In the child the marked descriptors are there: wire them to stdin and stdout. */
		if (dup2(to_child[0], 0) < 0 || dup2(from_child[1], 1) < 0)
			_exit(99);
		snprintf(a, sizeof(a), "%d", to_child[1]);
		snprintf(b, sizeof(b), "%d", from_child[0]);
		execl("/proc/self/exe", argv[0], "helper", a, b, (char *)NULL);
		_exit(98);
	}
	close(to_child[0]);
	close(from_child[1]);
	if (write(to_child[1], "through the pipe", 16) != 16)
		return 1;
	close(to_child[1]);
	if (read(from_child[0], buf, sizeof(buf) - 1) < 0)
		return 1;
	waitpid(pid, &status, 0);
	printf("%s", buf);
	printf("child exit %d; parent still has its marks: %s; unmarked one inherited: %s\n",
	       WIFEXITED(status) ? WEXITSTATUS(status) : -1,
	       (fcntl(from_child[0], F_GETFD) & FD_CLOEXEC) ? "yes" : "NO",
	       fcntl(kept, F_GETFD) == 0 ? "yes" : "NO");
	return 0;
}
