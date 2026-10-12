/*
 * How long the kernel and the runtime take to block a task and wake it, and
 * to switch between tasks (workstream "sched").
 *
 *   sched_bench [-p] futex N     two threads hand a futex word back and forth N times
 *   sched_bench [-p] pipe N      the same through two pipes (one byte each way)
 *   sched_bench [-p] thru MIB    one thread writes MIB MiB into a pipe in 64 KiB pieces, the other reads
 *   sched_bench [-p] yield N     two threads call sched_yield() N times each (a switch each, where they
 *                                share a CPU: boot with wasm_pool=1 and -p)
 *   sched_bench [-p] sleep N     N times nanosleep(1 ms): how late the timer wakes a sleeping task
 *   sched_bench [-p] storm N     N rounds of: 16 threads made at once, each passing a token round a ring of futexes
 *                                200 times and yielding, then all joined (a program starting up, like Firefox's
 *                                threads coming up while the pool's CPUs do)
 *   sched_bench evdev N          read N pointer positions from /dev/input/event1 that carry the time they were
 *                                put into the input ring (build/evdev-hook.js under node-machine: x | y << 15 is
 *                                that time in microseconds, modulo 2^30), and say how long they took to be
 *                                reported by the kernel (the event's time stamp) and to be read here
 *
 * -p: the program first asks to share CPUs (prctl PR_WASM_COOPERATIVE), as rv does, so that it and its
 * threads run on the pool's CPUs like every emulated program; without it every thread has a CPU of its own.
 *
 * Prints one line per run: what, how many, the time per round trip (or per call) in microseconds as
 * min / median / mean, and the total. Integer arithmetic only (floating-point printf traps on wasm64).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/input.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_WASM_COOPERATIVE
#define PR_WASM_COOPERATIVE 0x5741534d
#endif

static long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static long futex(atomic_int *word, int op, int val)
{
	/* (Every argument as a long: syscall() takes them as such, and on wasm64 an int is a narrower vararg.) */
	return syscall(SYS_futex, (long)word, (long)op, (long)val, 0L, 0L, 0L);
}

static long n_rounds;
static long *samples;
static atomic_int turn;		/* futex: 0 the main thread's turn, 1 the other's */
static int ping[2], pong[2];
static atomic_int ready;

static int cmp_long(const void *a, const void *b)
{
	long x = *(const long *)a, y = *(const long *)b;

	return x < y ? -1 : x > y;
}

static void report(const char *what, long n, long total_ns)
{
	long sum = 0;

	qsort(samples, n, sizeof(long), cmp_long);
	for (long i = 0; i < n; i++)
		sum += samples[i];
	printf("%s n=%ld per-trip-us min=%ld.%01ld median=%ld.%01ld mean=%ld.%01ld p90=%ld.%01ld total-ms=%ld\n", what, n,
	       samples[0] / 1000, samples[0] / 100 % 10, samples[n / 2] / 1000, samples[n / 2] / 100 % 10,
	       sum / n / 1000, sum / n / 100 % 10, samples[n * 9 / 10] / 1000, samples[n * 9 / 10] / 100 % 10,
	       total_ns / 1000000);
	fflush(stdout);
}

static void *futex_other(void *arg)
{
	for (long i = 0; i < n_rounds; i++) {
		while (atomic_load(&turn) != 1)
			futex(&turn, FUTEX_WAIT_PRIVATE, 0);
		atomic_store(&turn, 0);
		futex(&turn, FUTEX_WAKE_PRIVATE, 1);
	}
	return NULL;
}

static void *pipe_other(void *arg)
{
	char c;

	for (long i = 0; i < n_rounds; i++) {
		if (read(ping[0], &c, 1) != 1 || write(pong[1], &c, 1) != 1)
			break;
	}
	return NULL;
}

static void *thru_other(void *arg)
{
	static char buf[65536];
	long left = n_rounds;

	while (left > 0) {
		long n = read(ping[0], buf, sizeof(buf));
		if (n <= 0)
			break;
		left -= n;
	}
	return NULL;
}

#define STORM_THREADS 16
static atomic_int storm_token;

static void *storm_thread(void *arg)
{
	long me = (long)arg;

	for (int i = 0; i < 200; i++) {
		while (atomic_load(&storm_token) % STORM_THREADS != me)
			futex(&storm_token, FUTEX_WAIT_PRIVATE, atomic_load(&storm_token));
		atomic_fetch_add(&storm_token, 1);
		futex(&storm_token, FUTEX_WAKE_PRIVATE, STORM_THREADS);
		if (i % 7 == 0)
			sched_yield();
	}
	return NULL;
}

static void *yield_other(void *arg)
{
	atomic_fetch_add(&ready, 1);
	while (atomic_load(&ready) < 2)
		sched_yield();
	for (long i = 0; i < n_rounds; i++)
		sched_yield();
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t thread;
	int pool = 0;
	long began;

	if (argc > 1 && !strcmp(argv[1], "-p")) {
		pool = 1;
		argv++;
		argc--;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: sched_bench [-p] futex|pipe|thru|yield|sleep N\n");
		return 2;
	}
	if (pool && prctl(PR_WASM_COOPERATIVE, 1UL, 0UL, 0UL, 0UL))
		printf("(no pool: %s)\n", strerror(errno));
	n_rounds = atol(argv[2]);
	samples = calloc(n_rounds + 1, sizeof(long));

	if (!strcmp(argv[1], "futex")) {
		pthread_create(&thread, NULL, futex_other, NULL);
		began = now_ns();
		for (long i = 0; i < n_rounds; i++) {
			long t = now_ns();
			atomic_store(&turn, 1);
			futex(&turn, FUTEX_WAKE_PRIVATE, 1);
			while (atomic_load(&turn) != 0)
				futex(&turn, FUTEX_WAIT_PRIVATE, 1);
			samples[i] = now_ns() - t;
		}
		report(pool ? "futex-pool" : "futex", n_rounds, now_ns() - began);
	} else if (!strcmp(argv[1], "pipe")) {
		char c = 'x';

		if (pipe(ping) || pipe(pong))
			return 1;
		pthread_create(&thread, NULL, pipe_other, NULL);
		began = now_ns();
		for (long i = 0; i < n_rounds; i++) {
			long t = now_ns();
			if (write(ping[1], &c, 1) != 1 || read(pong[0], &c, 1) != 1)
				return 1;
			samples[i] = now_ns() - t;
		}
		report(pool ? "pipe-pool" : "pipe", n_rounds, now_ns() - began);
	} else if (!strcmp(argv[1], "thru")) {
		static char buf[65536];
		long total = n_rounds * 1048576L, left = total, ns;

		if (pipe(ping))
			return 1;
		n_rounds = total;
		memset(buf, 'y', sizeof(buf));
		pthread_create(&thread, NULL, thru_other, NULL);
		began = now_ns();
		while (left > 0) {
			long n = write(ping[1], buf, left < (long)sizeof(buf) ? left : (long)sizeof(buf));
			if (n <= 0)
				return 1;
			left -= n;
		}
		pthread_join(thread, NULL);
		ns = now_ns() - began;
		printf("%s bytes=%ld ms=%ld MiB/s=%ld\n", pool ? "thru-pool" : "thru", total, ns / 1000000,
		       ns ? total * 1000L / ns * 1000000L / 1048576L : 0);
		return 0;
	} else if (!strcmp(argv[1], "yield")) {
		pthread_create(&thread, NULL, yield_other, NULL);
		atomic_fetch_add(&ready, 1);
		while (atomic_load(&ready) < 2)
			sched_yield();
		began = now_ns();
		for (long i = 0; i < n_rounds; i++) {
			long t = now_ns();
			sched_yield();
			samples[i] = now_ns() - t;
		}
		report(pool ? "yield-pool" : "yield", n_rounds, now_ns() - began);
	} else if (!strcmp(argv[1], "sleep")) {
		struct timespec ms = { 0, 1000000 };

		began = now_ns();
		for (long i = 0; i < n_rounds; i++) {
			long t = now_ns();
			nanosleep(&ms, NULL);
			samples[i] = now_ns() - t - 1000000;
		}
		report(pool ? "sleep1ms-late-pool" : "sleep1ms-late", n_rounds, now_ns() - began);
		return 0;
	} else if (!strcmp(argv[1], "storm")) {
		pthread_t threads[STORM_THREADS];

		began = now_ns();
		for (long r = 0; r < n_rounds; r++) {
			long t = now_ns();
			atomic_store(&storm_token, 0);
			for (long i = 0; i < STORM_THREADS; i++)
				pthread_create(&threads[i], NULL, storm_thread, (void *)i);
			for (long i = 0; i < STORM_THREADS; i++)
				pthread_join(threads[i], NULL);
			samples[r] = now_ns() - t;
		}
		report(pool ? "storm-pool" : "storm", n_rounds, now_ns() - began);
		return 0;
	} else if (!strcmp(argv[1], "evdev")) {
		int fd = open("/dev/input/event1", O_RDONLY);
		long *read_lat = calloc(n_rounds + 1, sizeof(long));
		long x = -1, y = -1, got = 0;
		struct input_event ev;

		if (fd < 0) {
			perror("/dev/input/event1");
			return 1;
		}
		while (got < n_rounds && read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type == EV_ABS && ev.code == ABS_X)
				x = ev.value;
			else if (ev.type == EV_ABS && ev.code == ABS_Y)
				y = ev.value;
			else if (ev.type == EV_SYN && x >= 0 && y >= 0) {
				long put = (x | y << 15) & 0x3fffffff;
				long reported = (ev.input_event_sec * 1000000L + ev.input_event_usec) & 0x3fffffff;
				long seen = (now_ns() / 1000) & 0x3fffffff;

				samples[got] = ((reported - put) & 0x3fffffff) * 1000;
				read_lat[got] = ((seen - put) & 0x3fffffff) * 1000;
				got++;
				x = y = -1;
			}
		}
		report("evdev-reported", got, 0);
		memcpy(samples, read_lat, got * sizeof(long));
		report("evdev-read", got, 0);
		return 0;
	} else {
		fprintf(stderr, "unknown test %s\n", argv[1]);
		return 2;
	}
	pthread_join(thread, NULL);
	return 0;
}
