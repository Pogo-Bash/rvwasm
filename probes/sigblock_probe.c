/*
 * Is a signal that arrives while blocked delivered when it is unblocked? (The lost-USR1 bug: tests/smoke.sh.)
 *
 *   sigblock_probe N [block]
 *   sigblock_probe N pipe      as rv's fork does it: the child (this program again) writes a byte into a pipe
 *                              the parent reads, then sends the parent SIGUSR1 and exits
 *
 * N times: (with "block": block SIGUSR1,) start /bin/kill -USR1 <our pid> with posix_spawn, wait for it, unblock,
 * and see whether our handler ran. Prints how many of the N were lost.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static volatile sig_atomic_t got;

static volatile sig_atomic_t chld;

static void on_usr1(int sig)
{
	(void)sig;
	got++;
}

static void on_chld(int sig)
{
	(void)sig;
	chld++;
}

int main(int argc, char **argv)
{
	if (argc > 3 && !strcmp(argv[1], "child")) {
		char byte = 0;

		write(atoi(argv[2]), &byte, 1);
		kill(atoi(argv[3]), SIGUSR1);
		return 0;
	}
	int n = argc > 1 ? atoi(argv[1]) : 10, block = argc > 2 && !strcmp(argv[2], "block"), lost = 0;
	struct sigaction sa;
	sigset_t usr1, old;
	char pid[16];

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, NULL);
	if (getenv("CHLD")) {	/* a SIGCHLD handler too, as a shell has */
		sa.sa_handler = on_chld;
		sigaction(SIGCHLD, &sa, NULL);
	}
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	snprintf(pid, sizeof(pid), "%d", (int)getpid());
	for (int i = 0; i < n; i++) {
		char *args[] = { "kill", "-USR1", pid, NULL };
		pid_t child;
		int status;
		volatile int spin;

		got = 0;
		if (block)
			sigprocmask(SIG_BLOCK, &usr1, &old);
		int fds[2];
		char fdtext[16], byte;
		char *cargs[] = { "sigblock_probe", "child", fdtext, pid, NULL };

		if (argc > 2 && !strcmp(argv[2], "pipe")) {
			if (pipe(fds))
				return 1;
			snprintf(fdtext, sizeof(fdtext), "%d", fds[1]);
			if (posix_spawn(&child, "/proc/self/exe", NULL, NULL, cargs, environ)) {
				perror("posix_spawn");
				return 1;
			}
			close(fds[1]);
			while (read(fds[0], &byte, 1) < 0)
				;
			close(fds[0]);
		} else if (posix_spawn(&child, "/bin/kill", NULL, NULL, args, environ)) {
			perror("posix_spawn");
			return 1;
		}
		while (waitpid(child, &status, 0) < 0)
			;
		if (block)
			sigprocmask(SIG_SETMASK, &old, NULL);
		for (spin = 0; spin < 1000 && !got; spin++)
			getppid();	/* a few system calls for the signal to come at */
		if (!got) {
			lost++;
			printf("lost %d\n", i);
		}
	}
	printf("sigblock_probe %s: %d of %d lost\n", argc > 2 ? argv[2] : "noblock", lost, n);
	return 0;
}
