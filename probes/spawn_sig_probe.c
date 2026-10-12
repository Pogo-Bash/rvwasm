/*
 * What can a native wasm64 program on Linux/Wasm do with signals and with
 * starting other programs? The emulator's signal delivery and its fork (a
 * second emulator fed a snapshot) both rest on the answers.
 *
 * Each step announces itself first, so a hang or crash shows where it was.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static volatile sig_atomic_t got[65];
static volatile int info_signo, info_pid;

static void handler(int sig)
{
	got[sig]++;
}

static void info_handler(int sig, siginfo_t *si, void *uc)
{
	(void)uc;
	got[sig]++;
	info_signo = si->si_signo;
	info_pid = si->si_pid;
}

static int child_exec(void *arg)
{
	char **argv = arg;

	execve(argv[0], argv, environ);
	_exit(127);
}

static void step(const char *what)
{
	printf("%s: ", what);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	sigset_t set, pending;
	char *echo1[] = { "/bin/echo", "(child started by posix_spawn)", NULL };
	char *echo2[] = { "/bin/echo", "(child started by clone+exec)", NULL };
	char *fail[] = { "/bin/false", NULL };
	int status, r, fds[2];
	pid_t pid;

	(void)argc;
	(void)argv;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);

	step("1 handler via raise");
	sigaction(SIGUSR1, &sa, NULL);
	raise(SIGUSR1);
	printf("ran %d time(s)\n", (int)got[SIGUSR1]);

	step("3 blocked signal stays pending");
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigprocmask(SIG_BLOCK, &set, NULL);
	raise(SIGUSR1);
	sigpending(&pending);
	printf("ran %d (want 1), pending %d; ", (int)got[SIGUSR1], sigismember(&pending, SIGUSR1));
	sigprocmask(SIG_UNBLOCK, &set, NULL);
	printf("after unblock ran %d (want 2)\n", (int)got[SIGUSR1]);

	step("4 alarm interrupts pause");
	sa.sa_handler = handler;
	sa.sa_flags = 0;
	sigaction(SIGALRM, &sa, NULL);
	alarm(1);
	r = pause();
	printf("pause returned %d errno %d, handler ran %d\n", r, errno, (int)got[SIGALRM]);

	step("5 alarm during a loop with no system calls");
	alarm(1);
	{
		volatile unsigned long i;

		for (i = 0; i < 3000000000UL && got[SIGALRM] < 2; i++)
			;
		printf("after %lu iterations handler ran %d; ", (unsigned long)i, (int)got[SIGALRM] - 1);
	}
	getppid();
	printf("after one system call %d\n", (int)got[SIGALRM] - 1);

	step("6 posix_spawn");
	fflush(stdout);
	r = posix_spawn(&pid, echo1[0], NULL, NULL, echo1, environ);
	if (r) {
		printf("failed: %s\n", strerror(r));
	} else {
		waitpid(pid, &status, 0);
		printf("  -> exit status %d\n", WEXITSTATUS(status));
	}

	step("7 clone(CLONE_VM|CLONE_VFORK) + execve, with a SIGCHLD handler");
	fflush(stdout);
	sigaction(SIGCHLD, &sa, NULL);
	pid = clone(child_exec, NULL, CLONE_VM | CLONE_VFORK | SIGCHLD, echo2);
	if (pid < 0) {
		printf("failed: %s\n", strerror(errno));
	} else {
		r = waitpid(pid, &status, 0);
		printf("  -> waitpid %s, exit status %d, SIGCHLD handler ran %d\n",
		       r == pid ? "ok" : "failed", WEXITSTATUS(status), (int)got[SIGCHLD]);
	}

	step("8 child exit status and inherited pipe");
	if (pipe(fds)) {
		printf("pipe failed\n");
		return 1;
	}
	pid = clone(child_exec, NULL, CLONE_VM | CLONE_VFORK | SIGCHLD, fail);
	waitpid(pid, &status, 0);
	printf("false exited %d; ", WEXITSTATUS(status));
	/* The read end is still open in us only: nothing was written, so no data. */
	close(fds[1]);
	{
		char c;

		printf("read on our pipe gives %d (want 0, end of file)\n", (int)read(fds[0], &c, 1));
	}

	step("9 SA_SIGINFO via kill(self) (crashes on wasm64: handlers are always called with one argument)");
	fflush(stdout);
	sa.sa_sigaction = info_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR2, &sa, NULL);
	kill(getpid(), SIGUSR2);
	printf("ran %d, si_signo %d, si_pid %s\n", (int)got[SIGUSR2], info_signo,
	       info_pid == getpid() ? "is ours" : "is wrong");

	printf("done\n");
	return 0;
}
