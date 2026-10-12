/*
 * Several signals pending at once: does each handler run? (What hostnode
 * found: of three signals sent while a program slept, only the handler of
 * the last one ran.) And two things a program can use instead of handlers:
 * signalfd, and a descriptor for a child (pidfd).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static volatile unsigned long seen;
static volatile int order[8], count;

static void note(int sig)
{
	seen |= 1UL << sig;
	if (count < 8)
		order[count++] = sig;
}

static void show(const char *what)
{
	int i;

	printf("%s: handlers ran for", what);
	for (i = 0; i < count; i++)
		printf(" %d", order[i]);
	printf(" (%d of 3)\n", count);
	fflush(stdout);
	seen = 0;
	count = 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	sigset_t set, old;
	char command[128];
	char *sh[] = { "/bin/sh", "-c", command, NULL };
	struct pollfd p;
	int status, fd, r;
	pid_t pid;

	(void)argc;
	(void)argv;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = note;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* 1: three raised while blocked, then unblocked together. */
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigaddset(&set, SIGTERM);
	sigprocmask(SIG_BLOCK, &set, &old);
	raise(SIGUSR1);
	raise(SIGUSR2);
	raise(SIGTERM);
	sigprocmask(SIG_SETMASK, &old, NULL);
	getppid();
	getppid();
	getppid();
	show("1 raised while blocked");

	/* 2: three sent by another program while this one sleeps in poll. */
	snprintf(command, sizeof(command), "kill -USR1 %d; kill -USR2 %d; kill -TERM %d", getpid(), getpid(), getpid());
	posix_spawn(&pid, sh[0], NULL, NULL, sh, environ);
	r = poll(NULL, 0, 2000);
	printf("  (poll returned %d, errno %d)\n", r, r < 0 ? errno : 0);
	waitpid(pid, &status, 0);
	getppid();
	getppid();
	show("2 sent while in poll");

	/* 3: signalfd. */
	sigprocmask(SIG_BLOCK, &set, &old);
	fd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
	printf("3 signalfd: %d%s\n", fd, fd < 0 ? strerror(errno) : "");
	if (fd >= 0) {
		struct signalfd_siginfo si;
		int n = 0;

		posix_spawn(&pid, sh[0], NULL, NULL, sh, environ);
		waitpid(pid, &status, 0);
		p.fd = fd;
		p.events = POLLIN;
		r = poll(&p, 1, 1000);
		printf("  poll %d revents %x; read:", r, p.revents);
		while (read(fd, &si, sizeof(si)) == sizeof(si) && n++ < 8)
			printf(" %u", si.ssi_signo);
		printf(" (%d of 3)\n", n);
		close(fd);
	}
	sigprocmask(SIG_SETMASK, &old, NULL);
	show("  (handlers after unblocking)");

	/* 4: a descriptor for a child that becomes readable when it ends. */
	snprintf(command, sizeof(command), "exit 7");
	posix_spawn(&pid, sh[0], NULL, NULL, sh, environ);
	fd = syscall(SYS_pidfd_open, (long)pid, 0L);	/* (longs: see sched_bench.c) */
	printf("4 pidfd_open: %d%s\n", fd, fd < 0 ? strerror(errno) : "");
	if (fd >= 0) {
		p.fd = fd;
		p.events = POLLIN;
		r = poll(&p, 1, 2000);
		printf("  poll %d revents %x\n", r, p.revents);
	}
	waitpid(pid, &status, 0);
	printf("  exit status %d\n", WEXITSTATUS(status));
	return 0;
}
