/*
 * Waits with every signal blocked, while the world is stopped around them.
 *
 * rv stops a process's other threads for a fork (where it starts a second
 * emulator, as on Linux/Wasm or with RV_FORK=snapshot) and when its arena
 * grows, and gets a thread out of a system call it waits in with a host
 * signal of its own (thread.c). Three threads here wait in ppoll, pselect6
 * and epoll_pwait with a temporary mask of all 64 signals, the guest's way of
 * saying "nothing interrupts this": that must not keep rv's own signal out,
 * or the stop never ends and the process stands still. Meanwhile the main
 * thread forks, grows its memory, and forks again; then it wakes them.
 *
 * Then the mask across exec: blocked signals stay blocked in the program an
 * exec starts (signals 1 to 31 are compared, but for SIGBUS and SIGSEGV, which
 * qemu keeps for itself; rv cannot carry the guest's 32 to 34, which the host's
 * C library keeps, nor 64, which is rv's).
 *
 * The calls are made directly, so that the masks are exactly all 64 bits.
 * Output does not depend on scheduling. Run by tools/thread-check.sh.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define MB (1024 * 1024)

static const uint64_t all = ~0ULL;
static int pipes[3][2];

static void *in_ppoll(void *arg)
{
	struct pollfd p = { .fd = pipes[0][0], .events = POLLIN };
	long n = syscall(SYS_ppoll, &p, 1, NULL, &all, 8);

	(void)arg;
	return (void *)n;
}

static void *in_pselect(void *arg)
{
	fd_set r;
	/* pselect6's last argument: the mask's address and its size. */
	struct { const uint64_t *mask; size_t size; } m = { &all, 8 };
	long n;

	(void)arg;
	FD_ZERO(&r);
	FD_SET(pipes[1][0], &r);
	n = syscall(SYS_pselect6, pipes[1][0] + 1, &r, NULL, NULL, NULL, &m);
	return (void *)n;
}

static void *in_epoll(void *arg)
{
	struct epoll_event e = { .events = EPOLLIN }, got;
	int ep = epoll_create1(0);
	long n;

	(void)arg;
	e.data.fd = pipes[2][0];
	if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, pipes[2][0], &e))
		return (void *)-100L;
	n = syscall(SYS_epoll_pwait, ep, &got, 1, -1, &all, 8);
	close(ep);
	return (void *)n;
}

static int fork_and_wait(int code)
{
	int status;
	pid_t pid = fork();

	if (pid == 0)
		_exit(code);
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* After exec: what is blocked now (signals 1 to 31, but for SIGBUS and SIGSEGV, which qemu keeps for itself). */
static int child(void)
{
	uint64_t now = 0;

	syscall(SYS_rt_sigprocmask, SIG_BLOCK, NULL, &now, 8);
	printf("after exec: blocked 0x%llx\n", (unsigned long long)(now & 0x7fffffffULL & ~0x440ULL));
	return 0;
}

int main(int argc, char **argv)
{
	void *(*waiters[3])(void *) = { in_ppoll, in_pselect, in_epoll };
	const char *names[3] = { "ppoll", "pselect6", "epoll_pwait" };
	pthread_t t[3];
	uint8_t *big;
	long zero_bad = 0;

	if (argc > 1 && !strcmp(argv[1], "child"))
		return child();
	/* A stop that never ends is a hang: end it as a failure instead. */
	alarm(60);

	for (int i = 0; i < 3; i++) {
		if (pipe(pipes[i]))
			return 1;
		pthread_create(&t[i], NULL, waiters[i], NULL);
	}
	usleep(100000);		/* let them get into their calls */

	printf("fork: child exit %d\n", fork_and_wait(7));
	/* A mapping larger than the arena: it grows. */
	big = mmap(NULL, 256 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (big == MAP_FAILED)
		return 1;
	for (size_t i = 0; i < 256 * MB; i += 4096)
		zero_bad += big[i] != 0;
	big[256 * MB - 1] = 3;
	printf("mapping: %ld pages not zero, end %d\n", zero_bad, big[256 * MB - 1]);
	printf("fork again: child exit %d\n", fork_and_wait(big[256 * MB - 1] == 3 ? 9 : 1));
	munmap(big, 256 * MB);

	for (int i = 0; i < 3; i++) {
		void *ret;

		if (write(pipes[i][1], "w", 1) != 1)
			return 1;
		pthread_join(t[i], &ret);
		printf("%s with every signal blocked: returned %ld\n", names[i], (long)ret);
	}

	/* Every signal blocked (as far as the kernel lets), then exec: the mask stays. */
	fflush(stdout);
	syscall(SYS_rt_sigprocmask, SIG_SETMASK, &all, NULL, 8);
	execl(argv[0], argv[0], "child", (char *)NULL);
	printf("exec failed: %s\n", strerror(errno));
	return 1;
}
