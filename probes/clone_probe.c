/*
 * Phase 0 verification: how does clone spawn workers?
 *
 * Starts threads through pthreads (clone under the hood), checks that memory
 * is shared, that futex wait/wake works across workers, and reports how long
 * a thread takes to come up (each one is a new Web Worker that re-instantiates
 * both vmlinux and this program).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NTHREADS 4

static atomic_int counter;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int go;
static __thread int tls_value = 42;

/* Integer microseconds: floating-point printf traps on wasm64 (see fp_probe.c). */
static long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static void *worker(void *arg)
{
	long id = (long)arg;

	/* TLS must be private per thread and start from its initialiser. */
	if (tls_value != 42)
		printf("  thread %ld: TLS not initialised (%d)\n", id, tls_value);
	tls_value = (int)id;

	pthread_mutex_lock(&lock);
	while (!go)
		pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);

	for (int i = 0; i < 100000; i++)
		atomic_fetch_add(&counter, 1);

	printf("  thread %ld: tid %ld, pid %d, tls %d\n", id,
	       (long)syscall(SYS_gettid), (int)getpid(), tls_value);
	return (void *)(id * 10);
}

/* Not main(void): clang's wasm target only gives libc the entry it calls
 * (__main_argc_argv) when main takes argc and argv. */
int main(int argc, char **argv)
{
	pthread_t th[NTHREADS];
	long t0, t1;
	int r;

	(void)argc;
	(void)argv;

	printf("main: pid %d, tid %ld\n", (int)getpid(), (long)syscall(SYS_gettid));

	t0 = now_us();
	for (long i = 0; i < NTHREADS; i++) {
		r = pthread_create(&th[i], NULL, worker, (void *)i);
		if (r) {
			printf("pthread_create %ld failed: %s\n", i, strerror(r));
			return 1;
		}
	}
	t1 = now_us();
	printf("spawned %d threads in %ld us\n", NTHREADS, t1 - t0);

	/* Give them time to block on the condvar, then release them all. */
	usleep(200 * 1000);
	pthread_mutex_lock(&lock);
	go = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);

	for (long i = 0; i < NTHREADS; i++) {
		void *ret;

		pthread_join(th[i], &ret);
		if ((long)ret != i * 10)
			printf("thread %ld returned %ld\n", i, (long)ret);
	}

	printf("counter = %d (expected %d)\n", atomic_load(&counter), NTHREADS * 100000);
	printf("main tls still %d (expected 42)\n", tls_value);
	printf("done\n");
	return 0;
}
