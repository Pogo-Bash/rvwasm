/*
 * fork(). A NOMMU system cannot give the child a copy of the parent's memory; Linux on NOMMU (uClinux) fails
 * fork() instead. Expect either a child that exits 42, or fork() failing (ENOSYS/EINVAL) so a program can fall
 * back to vfork() or posix_spawn().
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int status = 0;
	pid_t pid = fork();

	if (pid == 0)
		_exit(42);
	if (pid < 0) {
		printf("fork failed: %s (ok)\n", strerror(errno));
		return 0;
	}
	waitpid(pid, &status, 0);
	if (WIFEXITED(status))
		printf("child exited %d (%s)\n", WEXITSTATUS(status), WEXITSTATUS(status) == 42 ? "ok" : "WRONG");
	else
		printf("fork returned %d, and the child was killed by signal %d (WRONG)\n", (int)pid, WTERMSIG(status));
	return 0;
}
