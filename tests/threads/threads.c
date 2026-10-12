/*
 * Guest threads: a pthread program whose output does not depend on how the
 * threads are scheduled, so that it can be compared between emulators.
 * Only the main thread prints. Built against glibc and against musl
 * (tools/build-thread-tests.sh), run by tools/thread-check.sh.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define N 8
#define ROUNDS 20000

static long elapsed_ms(const struct timespec *since)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - since->tv_sec) * 1000 + (now.tv_nsec - since->tv_nsec) / 1000000;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, ms % 1000 * 1000000 };

	nanosleep(&ts, NULL);
}

/* 1-3: counters under a mutex, with atomics, and under a spinlock. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static long mutex_count, atomic_count, spin_count;
static char spin;

static void *count(void *arg)
{
	(void)arg;
	for (int i = 0; i < ROUNDS; i++) {
		pthread_mutex_lock(&mutex);
		mutex_count++;
		pthread_mutex_unlock(&mutex);
		__atomic_fetch_add(&atomic_count, 1, __ATOMIC_SEQ_CST);
		while (__atomic_test_and_set(&spin, __ATOMIC_ACQUIRE))
			sched_yield();
		spin_count++;
		__atomic_clear(&spin, __ATOMIC_RELEASE);
	}
	return NULL;
}

/* 4: producers and consumers on a small queue, with condition variables. */
#define QUEUE 4
#define ITEMS 500
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER, not_full = PTHREAD_COND_INITIALIZER;
static int queue[QUEUE], qhead, qlen, producers_left;
static long consumed_sum, consumed_n;

static void *produce(void *arg)
{
	int base = (int)(intptr_t)arg;

	for (int i = 1; i <= ITEMS; i++) {
		pthread_mutex_lock(&qlock);
		while (qlen == QUEUE)
			pthread_cond_wait(&not_full, &qlock);
		queue[(qhead + qlen++) % QUEUE] = base + i;
		pthread_cond_signal(&not_empty);
		pthread_mutex_unlock(&qlock);
	}
	pthread_mutex_lock(&qlock);
	producers_left--;
	pthread_cond_broadcast(&not_empty);
	pthread_mutex_unlock(&qlock);
	return NULL;
}

static void *consume(void *arg)
{
	(void)arg;
	for (;;) {
		int item;

		pthread_mutex_lock(&qlock);
		while (!qlen && producers_left)
			pthread_cond_wait(&not_empty, &qlock);
		if (!qlen) {
			pthread_mutex_unlock(&qlock);
			return NULL;
		}
		item = queue[qhead];
		qhead = (qhead + 1) % QUEUE;
		qlen--;
		consumed_sum += item;
		consumed_n++;
		pthread_cond_signal(&not_full);
		pthread_mutex_unlock(&qlock);
	}
}

/* 5-6: thread-local storage and return values. */
static __thread long tls_value = 5;

static void *tls_check(void *arg)
{
	long id = (long)(intptr_t)arg, ok = tls_value == 5;

	tls_value = id * 1000;
	for (int i = 0; i < 50; i++) {
		sched_yield();
		ok &= tls_value == id * 1000 + i;
		tls_value++;
	}
	return (void *)(intptr_t)(ok ? id * id : -1);
}

/* 7: sleeping threads sleep at the same time. */
static void *sleeper(void *arg)
{
	sleep_ms((long)(intptr_t)arg);
	return NULL;
}

/* 9: a thread blocked in read() while others run. */
static int pipe_fds[2];

static void *reader(void *arg)
{
	static char buf[32];
	ssize_t n = read(pipe_fds[0], buf, sizeof(buf) - 1);

	(void)arg;
	buf[n > 0 ? n : 0] = 0;
	return buf;
}

/* 10-11: signals for one thread, and for the process with one thread accepting them. */
static sem_t handled;
static pthread_t handler_thread[2];

static void on_signal(int sig)
{
	handler_thread[sig == SIGUSR2] = pthread_self();
	sem_post(&handled);
}

static sem_t ready;

static void *wait_for_signal(void *arg)
{
	sigset_t set;

	/* Take only the signal named; everything is blocked while waiting to be told to stop. */
	sigemptyset(&set);
	sigaddset(&set, (int)(intptr_t)arg);
	pthread_sigmask(SIG_UNBLOCK, &set, NULL);
	sem_post(&ready);
	for (;;)
		pause();
	return NULL;
}

/* 12: two threads handing a token back and forth with semaphores. */
static sem_t ping, pong;
static long volleys;

static void *ponger(void *arg)
{
	(void)arg;
	for (int i = 0; i < 1000; i++) {
		sem_wait(&ping);
		volleys++;
		sem_post(&pong);
	}
	return NULL;
}

/* 13: barrier, once, rwlock. */
static pthread_barrier_t barrier;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static long once_runs, serial_threads, rw_value, rw_reads_ok;

static void once_fn(void)
{
	once_runs++;
}

static void *barrier_fn(void *arg)
{
	(void)arg;
	pthread_once(&once, once_fn);
	if (pthread_barrier_wait(&barrier) == PTHREAD_BARRIER_SERIAL_THREAD)
		__atomic_fetch_add(&serial_threads, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < 200; i++) {
		if (i % 10 == 0) {
			pthread_rwlock_wrlock(&rwlock);
			rw_value += 2;
			pthread_rwlock_unlock(&rwlock);
		} else {
			pthread_rwlock_rdlock(&rwlock);
			if (rw_value % 2 == 0)
				__atomic_fetch_add(&rw_reads_ok, 1, __ATOMIC_SEQ_CST);
			pthread_rwlock_unlock(&rwlock);
		}
	}
	return NULL;
}

/* 14: a detached thread. */
static sem_t detached_done;

static void *detached(void *arg)
{
	(void)arg;
	sleep_ms(5);
	sem_post(&detached_done);
	return NULL;
}

/* 15: fork and wait from a thread while another thread is busy. */
static void *forker(void *arg)
{
	int status = 0;
	pid_t pid = fork();

	(void)arg;
	if (pid == 0) {
		sleep_ms(20);
		_exit(42);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return (void *)(intptr_t)-1;
	return (void *)(intptr_t)(WIFEXITED(status) ? WEXITSTATUS(status) : -2);
}

/* 16: poll() with a timeout in one thread, satisfied by another. */
static void *poller(void *arg)
{
	struct pollfd p = { .fd = pipe_fds[0], .events = POLLIN };
	int first = poll(&p, 1, 10);	/* nothing yet */
	int second = poll(&p, 1, 5000);	/* main writes meanwhile */
	char c = 0;

	(void)arg;
	if (second == 1 && read(pipe_fds[0], &c, 1) != 1)
		c = '?';
	return (void *)(intptr_t)(first * 1000 + second * 256 + c);
}

/* 17: a cancelled thread. */
static void *cancel_me(void *arg)
{
	(void)arg;
	for (;;)
		sleep_ms(1000);
	return NULL;
}

static void *nothing(void *arg)
{
	return arg;
}

/* pthread_create() that does not fail quietly. */
static void create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
	int err = pthread_create(t, attr, fn, arg);

	if (err) {
		printf("pthread_create: %s\n", strerror(err));
		exit(1);
	}
}

int main(void)
{
	pthread_t t[64];
	struct timespec start, deadline;
	struct sigaction sa;
	sigset_t set;
	void *ret;
	long sum, ms;
	int err;

	setvbuf(stdout, NULL, _IOLBF, 0);

	for (int i = 0; i < N; i++)
		create(&t[i], NULL, count, NULL);
	for (int i = 0; i < N; i++)
		pthread_join(t[i], NULL);
	printf("counters: mutex %ld, atomic %ld, spinlock %ld\n", mutex_count, atomic_count, spin_count);

	producers_left = 2;
	create(&t[0], NULL, produce, (void *)(intptr_t)0);
	create(&t[1], NULL, produce, (void *)(intptr_t)100000);
	for (int i = 2; i < 5; i++)
		create(&t[i], NULL, consume, NULL);
	for (int i = 0; i < 5; i++)
		pthread_join(t[i], NULL);
	printf("queue: %ld items, sum %ld\n", consumed_n, consumed_sum);

	for (int i = 0; i < N; i++)
		create(&t[i], NULL, tls_check, (void *)(intptr_t)(i + 1));
	sum = 0;
	for (int i = 0; i < N; i++) {
		pthread_join(t[i], &ret);
		sum += (long)(intptr_t)ret;
	}
	printf("tls and return values: %ld (main's copy %ld)\n", sum, tls_value);

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; i < 4; i++)
		create(&t[i], NULL, sleeper, (void *)(intptr_t)100);
	for (int i = 0; i < 4; i++)
		pthread_join(t[i], NULL);
	ms = elapsed_ms(&start);
	printf("four 100 ms sleeps: %s\n", ms >= 100 && ms < 300 ? "overlap" : "DO NOT OVERLAP");

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_nsec += 50000000;
	if (deadline.tv_nsec >= 1000000000) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000;
	}
	clock_gettime(CLOCK_MONOTONIC, &start);
	pthread_mutex_lock(&qlock);
	err = pthread_cond_timedwait(&not_empty, &qlock, &deadline);
	pthread_mutex_unlock(&qlock);
	ms = elapsed_ms(&start);
	printf("timed wait: %s after %s\n", err == ETIMEDOUT ? "ETIMEDOUT" : strerror(err),
	       ms >= 45 && ms < 500 ? "about 50 ms" : "THE WRONG TIME");

	if (pipe(pipe_fds))
		return 1;
	create(&t[0], NULL, reader, NULL);
	sleep_ms(20);
	if (write(pipe_fds[1], "through the pipe", 16) != 16)
		return 1;
	pthread_join(t[0], &ret);
	printf("blocking read: \"%s\"\n", (char *)ret);

	/* Everything blocked here and in new threads, except what a thread unblocks for itself. */
	sem_init(&handled, 0, 0);
	sem_init(&ready, 0, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &set, NULL);
	create(&t[0], NULL, wait_for_signal, (void *)(intptr_t)SIGUSR1);
	create(&t[1], NULL, wait_for_signal, (void *)(intptr_t)SIGUSR2);
	sem_wait(&ready);
	sem_wait(&ready);
	pthread_kill(t[0], SIGUSR1);		/* to one thread */
	while (sem_wait(&handled))
		;
	kill(getpid(), SIGUSR2);		/* to the process: only t[1] accepts it */
	while (sem_wait(&handled))
		;
	printf("signals: thread-directed in %s thread, process-directed in %s thread\n",
	       pthread_equal(handler_thread[0], t[0]) ? "the right" : "THE WRONG",
	       pthread_equal(handler_thread[1], t[1]) ? "the right" : "THE WRONG");
	pthread_cancel(t[0]);
	pthread_cancel(t[1]);
	pthread_join(t[0], &ret);
	pthread_join(t[1], NULL);
	printf("cancelled while waiting for signals: %s\n", ret == PTHREAD_CANCELED ? "yes" : "NO");

	sem_init(&ping, 0, 0);
	sem_init(&pong, 0, 0);
	create(&t[0], NULL, ponger, NULL);
	for (int i = 0; i < 1000; i++) {
		sem_post(&ping);
		sem_wait(&pong);
	}
	pthread_join(t[0], NULL);
	printf("semaphores: %ld volleys\n", volleys);

	pthread_barrier_init(&barrier, NULL, N);
	for (int i = 0; i < N; i++)
		create(&t[i], NULL, barrier_fn, NULL);
	for (int i = 0; i < N; i++)
		pthread_join(t[i], NULL);
	printf("barrier: %ld serial thread, once ran %ld time, rwlock value %ld with %ld consistent reads\n",
	       serial_threads, once_runs, rw_value, rw_reads_ok);

	{
		pthread_attr_t attr;

		sem_init(&detached_done, 0, 0);
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		create(&t[0], &attr, detached, NULL);
		sem_wait(&detached_done);
		printf("detached thread: done\n");
	}

	create(&t[0], NULL, forker, NULL);
	create(&t[1], NULL, count, NULL);
	pthread_join(t[0], &ret);
	pthread_join(t[1], NULL);
	printf("fork from a thread: child exited with %ld\n", (long)(intptr_t)ret);

	create(&t[0], NULL, poller, NULL);
	sleep_ms(40);
	if (write(pipe_fds[1], "p", 1) != 1)
		return 1;
	pthread_join(t[0], &ret);
	printf("poll: first %ld, second %ld, read '%c'\n", (long)(intptr_t)ret / 1000,
	       (long)(intptr_t)ret % 1000 / 256, (char)((long)(intptr_t)ret % 256));

	create(&t[0], NULL, cancel_me, NULL);
	sleep_ms(10);
	pthread_cancel(t[0]);
	pthread_join(t[0], &ret);
	printf("cancelled in a sleep: %s\n", ret == PTHREAD_CANCELED ? "yes" : "NO");

	{
		/* Many at once, with small stacks: the default ones would not all fit in a small guest. */
		pthread_attr_t attr;

		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 128 * 1024);
		sum = 0;
		for (int i = 0; i < 64; i++)
			create(&t[i], &attr, nothing, (void *)(intptr_t)i);
		for (int i = 0; i < 64; i++) {
			pthread_join(t[i], &ret);
			sum += (long)(intptr_t)ret;
		}
		printf("64 threads: %ld\n", sum);
	}
	return 0;
}
