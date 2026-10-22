/* Scale: what loading a library with many exported functions costs (libbig.so, made by build.sh: as many
 * functions as a big Qt library exports), in the thread that loads it and in each thread started after. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int (*last)(int);
static void *run(void *arg) { (void)arg; return (void *)(long)(last ? last(1) : 0); }

static double thread_ms(void)
{
	double best = 1e9;
	for (int i = 0; i < 5; i++) {
		pthread_t t;
		double s = now();
		pthread_create(&t, 0, run, 0);
		pthread_join(t, 0);
		s = now() - s;
		if (s < best)
			best = s;
	}
	return best;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "libbig.so";
	const char *symbol = argc > 2 ? argv[2] : "big_last";
	printf("thread start+join before: %.1f ms (fastest of 5)\n", thread_ms());
	double t = now();
	void *h = dlopen(path, RTLD_NOW);
	if (!h) {
		printf("FAIL dlopen: %s\n", dlerror());
		return 1;
	}
	printf("dlopen %s: %.1f ms\n", path, now() - t);
	last = (int (*)(int))dlsym(h, symbol);
	printf("thread start+join after: %.1f ms (fastest of 5)\n", thread_ms());
	void *r;
	pthread_t th;
	pthread_create(&th, 0, run, 0);
	pthread_join(th, &r);
	printf("%s(1) in a thread: %ld\n", symbol, (long)r);
	return 0;
}
