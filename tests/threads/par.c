/*
 * Do threads run at the same time? The same amount of computing per thread,
 * on 1, 2, 4 and 8 threads: with enough cores the time stays the same, and
 * the work done per second grows with the number of threads.
 *
 *   par [ROUNDS]   (millions of rounds per thread, default 20)
 *
 * Not for comparing output between emulators: it prints times.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static long rounds;

static void *work(void *arg)
{
	uint64_t x = (uint64_t)(uintptr_t)arg + 1, sum = 0;

	/* xorshift: nothing a compiler can fold away, nothing shared between threads. */
	for (long i = 0; i < rounds; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		sum += x;
	}
	return (void *)(uintptr_t)sum;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void *nothing(void *arg)
{
	return arg;
}

/* Two threads hand a byte back and forth through two pipes: each turn is a wake-up and a wait. */
static int there[2], back[2];

static void *echo(void *arg)
{
	char c;

	while (read(there[0], &c, 1) == 1 && c != 'q')
		if (write(back[1], &c, 1) != 1)
			break;
	return arg;
}

/* What threaded programs do all the time, apart from computing. */
static void small_things(void)
{
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_t t;
	long start, ms;
	char c = 'x';
	volatile long counter = 0;

	start = now_ms();
	for (int i = 0; i < 200; i++) {
		pthread_create(&t, NULL, nothing, NULL);
		pthread_join(t, NULL);
	}
	ms = now_ms() - start;
	printf("200 threads made and joined, one after the other: %ld ms (%ld us each)\n", ms, ms * 1000 / 200);

	if (pipe(there) || pipe(back))
		return;
	pthread_create(&t, NULL, echo, NULL);
	start = now_ms();
	for (int i = 0; i < 5000; i++)
		if (write(there[1], &c, 1) != 1 || read(back[0], &c, 1) != 1)
			break;
	ms = now_ms() - start;
	c = 'q';
	if (write(there[1], &c, 1) != 1)
		return;
	pthread_join(t, NULL);
	printf("5000 times there and back between two threads: %ld ms (%ld us a round)\n", ms, ms * 1000 / 5000);

	start = now_ms();
	for (long i = 0; i < 2000000; i++) {
		pthread_mutex_lock(&lock);
		counter++;
		pthread_mutex_unlock(&lock);
	}
	ms = now_ms() - start;
	printf("2 million times a lock that is free, taken and released: %ld ms (%ld ns each)\n", ms, ms / 2);
}

int main(int argc, char **argv)
{
	long base = 0;

	rounds = (argc > 1 ? atol(argv[1]) : 20) * 1000000L;
	for (int n = 1; n <= 8; n *= 2) {
		pthread_t t[8];
		uint64_t check = 0;
		long start = now_ms(), ms;

		for (int i = 0; i < n; i++)
			pthread_create(&t[i], NULL, work, (void *)(uintptr_t)i);
		for (int i = 0; i < n; i++) {
			void *r;

			pthread_join(t[i], &r);
			check ^= (uint64_t)(uintptr_t)r;
		}
		ms = now_ms() - start;
		if (n == 1)
			base = ms;
		printf("%d thread%s: %ld ms, %ld%% of one thread's rate, check %016llx\n", n, n == 1 ? " " : "s", ms,
		       ms ? base * n * 100 / ms : 0, (unsigned long long)check);
	}
	small_things();
	return 0;
}
