/*
 * A signal sent to a new process before it has run for the first time, with a handler it inherited.
 * Expect the child to run the handler, then its function, and exit 0 ("child: handler ran, exit 0"). With an
 * argument N, N children one after the other (whether the signal comes before the child's first run is a race:
 * some of them will), and a count: expect "children: N ran the handler, 0 did not, 0 were killed". With "pool" as
 * well, the program first joins the pool of CPUs that cooperative tasks share (prctl(PR_WASM_COOPERATIVE)); its
 * children are then in the pool too, and with a pool of one CPU (wasm_pool=1 on the kernel's command line) a child
 * cannot run before its parent waits for it: the signal is always there before the child's first run.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static char stack[65536] __attribute__((aligned(16)));
static volatile int handled;

static void on_usr1(int sig)
{
	(void)sig;
	handled = 1;
}

static int child(void *arg)
{
	(void)arg;
	return handled ? 0 : 3;
}

static int one(int verbose)
{
	int status = 0;
	pid_t pid;

	handled = 0;
	pid = clone(child, stack + sizeof stack, CLONE_VM | SIGCHLD, 0);
	if (pid < 0) {
		perror("clone");
		return -1;
	}
	kill(pid, SIGUSR1);
	if (waitpid(pid, &status, 0) < 0) {
		perror("waitpid");
		return -1;
	}
	if (verbose) {
		if (WIFEXITED(status))
			printf("child: %s, exit %d\n", WEXITSTATUS(status) == 0 ? "handler ran" : "handler did not run", WEXITSTATUS(status));
		else
			printf("child: killed by signal %d\n", WTERMSIG(status));
	}
	return WIFEXITED(status) ? (WEXITSTATUS(status) == 0 ? 0 : 1) : 2;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int n = argc > 1 ? atoi(argv[1]) : 20, counts[3] = { 0, 0, 0 };

	if (argc > 2 && !strcmp(argv[2], "pool") && prctl(0x5741534d, 0, 0, 0, 0))
		perror("prctl(PR_WASM_COOPERATIVE)");

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_usr1;
	sigaction(SIGUSR1, &sa, 0);
	alarm(50);
	for (int i = 0; i < n; i++) {
		int r = one(n == 1);
		if (r < 0)
			return 1;
		counts[r]++;
	}
	if (n > 1)
		printf("children: %d ran the handler, %d did not, %d were killed\n", counts[0], counts[1], counts[2]);
	return 0;
}
