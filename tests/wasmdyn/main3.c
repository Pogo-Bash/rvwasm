/* Threads: a thread started before dlopen calls into the library loaded after it started (the library
 * has to be loaded in that thread's Worker too); each thread has its own copy of the library's
 * thread-local variable; a thread started after dlopen has the library from the start. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (c) printf("ok   %s\n", #c); else { printf("FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int (*add)(int, int);
static int (*bump)(void);
static int result_add, result_bump1, result_bump2;
static int done;

static void *early(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&lock);
	while (!add)
		pthread_cond_wait(&cond, &lock);	/* asleep in the kernel while main dlopens */
	pthread_mutex_unlock(&lock);
	result_add = add(10, 20);
	result_bump1 = bump();			/* its own copy: 7 -> 8, whatever main did */
	result_bump2 = bump();
	pthread_mutex_lock(&lock);
	done = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);
	return 0;
}

static void *late(void *arg)
{
	(void)arg;
	return (void *)(long)(bump() * 100 + add(1, 1));
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "libfoo.so";
	pthread_t t1, t2;
	CHECK(pthread_create(&t1, 0, early, 0) == 0);
	void *h = dlopen(path, RTLD_NOW);
	if (!h) {
		printf("FAIL dlopen: %s\n", dlerror());
		return 1;
	}
	int (*b)(void) = (int (*)(void))dlsym(h, "foo_tls_bump");
	CHECK(b() == 8 && b() == 9 && b() == 10);	/* main's copy */
	pthread_mutex_lock(&lock);
	bump = b;
	add = (int (*)(int, int))dlsym(h, "foo_add");
	pthread_cond_broadcast(&cond);
	while (!done)
		pthread_cond_wait(&cond, &lock);
	pthread_mutex_unlock(&lock);
	pthread_join(t1, 0);
	CHECK(result_add == 72);
	CHECK(result_bump1 == 8 && result_bump2 == 9);
	CHECK(b() == 11);				/* main's copy, untouched by the thread */
	void *r;
	CHECK(pthread_create(&t2, 0, late, 0) == 0);
	pthread_join(t2, &r);
	CHECK((long)r == 844);				/* 8 * 100 + 44 */
	printf("main3: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
