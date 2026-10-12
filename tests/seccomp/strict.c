/* The old strict mode: after it, read, write and exit; anything else is death by SIGKILL. riscv64, static. */
#define _GNU_SOURCE
#include <linux/seccomp.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	int status, fine, killed;
	pid_t pid = fork();

	if (pid == 0) {
		prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT);
		syscall(SYS_write, 1, "written\n", 8);
		syscall(SYS_exit, 0);
	}
	fine = waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	pid = fork();
	if (pid == 0) {
		prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT);
		syscall(SYS_getpid);
		syscall(SYS_exit, 0);
	}
	killed = waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
	printf("strict: write and exit %s, another call %s\n", fine ? "ok" : "FAILED", killed ? "ok" : "FAILED");
	return !(fine && killed);
}
