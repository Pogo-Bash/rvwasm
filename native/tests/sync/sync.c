/* Waits between threads, as Qt's Wayland client does them: a condition variable's timed wait (alone and
 * signalled from another thread), and poll() on an eventfd and on a pipe written by another thread. Each line
 * says how long the wait took. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int flag, efd, pfd[2];

static void *signaller(void *arg)
{
	usleep(50000);
	pthread_mutex_lock(&m);
	flag = 1;
	pthread_cond_signal(&c);
	pthread_mutex_unlock(&m);
	usleep(50000);
	unsigned long long one = 1;
	write(efd, &one, 8);
	usleep(50000);
	write(pfd[1], "x", 1);
	return arg;
}

static int timedwait_ms(int ms, clockid_t clock)
{
	struct timespec t;
	clock_gettime(clock, &t);
	t.tv_nsec += (ms % 1000) * 1000000L;
	t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000L;
	t.tv_nsec %= 1000000000L;
	int r = 0;
	pthread_mutex_lock(&m);
	while (!flag && r == 0)
		r = pthread_cond_timedwait(&c, &m, &t);
	pthread_mutex_unlock(&m);
	return r;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	double t0 = now();
	int r = timedwait_ms(100, CLOCK_REALTIME);
	printf("sync: cond timedwait 100 ms (realtime, nobody signals): %s after %.0f ms\n", r == ETIMEDOUT ? "ETIMEDOUT" : "other", now() - t0);
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_destroy(&c);
	pthread_cond_init(&c, &ca);
	t0 = now();
	r = timedwait_ms(100, CLOCK_MONOTONIC);
	printf("sync: cond timedwait 100 ms (monotonic, nobody signals): %s after %.0f ms\n", r == ETIMEDOUT ? "ETIMEDOUT" : "other", now() - t0);
	efd = eventfd(0, EFD_NONBLOCK);
	pipe(pfd);
	pthread_t th;
	t0 = now();
	pthread_create(&th, NULL, signaller, NULL);
	r = timedwait_ms(5000, CLOCK_MONOTONIC);
	printf("sync: cond signalled by a thread at 50 ms: r=%d after %.0f ms\n", r, now() - t0);
	struct pollfd p = { efd, POLLIN, 0 };
	r = poll(&p, 1, 5000);
	printf("sync: poll eventfd written by a thread at 100 ms: r=%d after %.0f ms\n", r, now() - t0);
	struct pollfd q = { pfd[0], POLLIN, 0 };
	r = poll(&q, 1, 5000);
	printf("sync: poll pipe written by a thread at 150 ms: r=%d after %.0f ms\n", r, now() - t0);
	pthread_join(th, NULL);
	t0 = now();
	r = poll(NULL, 0, 200);
	printf("sync: poll timeout 200 ms: after %.0f ms\n", now() - t0);
	return 0;
}
