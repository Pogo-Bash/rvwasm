/*
 * SA_RESTART: the handler must run when the signal arrives, and the interrupted read() is restarted after it.
 * A thread (SIGALRM blocked in it) writes to the pipe 3 s after start; SIGALRM comes at 1 s. Expect the handler at ~1000 ms and the read at ~3000 ms.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static struct timespec t0;
static volatile long handler_at = -1;

/* Milliseconds since start (printf of a double traps on this toolchain: compiler-rt __eqtf2, filed as #22). */
static long now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
}

static void on_alarm(int sig)
{
	(void)sig;
	handler_at = now();
}

static void *writer(void *arg)
{
	sleep(3);
	write(((int *)arg)[1], "x", 1);
	return 0;
}

int main(int argc, char **argv)
{
	int p[2];
	char c;
	struct sigaction sa;
	ssize_t n;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	pipe(p);
	{
		pthread_t t;
		sigset_t set, old;

		sigemptyset(&set);
		sigaddset(&set, SIGALRM);
		pthread_sigmask(SIG_BLOCK, &set, &old);
		pthread_create(&t, 0, writer, p);
		pthread_sigmask(SIG_SETMASK, &old, 0);
	}
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_alarm;
	sa.sa_flags = argc > 1 ? 0 : SA_RESTART;
	sigaction(SIGALRM, &sa, 0);
	alarm(1);
	n = read(p[0], &c, 1);
	printf("%s: read returned %zd at %ld ms; handler ran at %ld ms (%s)\n", argc > 1 ? "no SA_RESTART" : "SA_RESTART",
	       n, now(), handler_at, handler_at > 0 && handler_at < 2000 ? "ok" : "LATE: the handler waited for the restarted read");
	return 0;
}
