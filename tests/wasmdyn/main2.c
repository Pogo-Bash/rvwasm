/* dlopen/dlsym on libfoo.so, with RTLD_LOCAL: its symbols are not in the global scope. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (c) printf("ok   %s\n", #c); else { printf("FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "libfoo.so";
	CHECK(dlsym(RTLD_DEFAULT, "foo_add") == NULL);	/* not loaded yet */
	void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		printf("FAIL dlopen: %s\n", dlerror());
		return 1;
	}
	int (*add)(int, int) = (int (*)(int, int))dlsym(h, "foo_add");
	int *counter = (int *)dlsym(h, "foo_counter");
	const char *(*greeting)(void) = (const char *(*)(void))dlsym(h, "foo_greeting");
	int (*checked)(int) = (int (*)(int))dlsym(h, "foo_checked_area");
	CHECK(add && counter && greeting && checked);
	CHECK(*counter == 42);
	CHECK(add(1, 2) == 45);
	CHECK(strcmp(greeting(), "hello from libfoo") == 0);
	CHECK(checked(4) == 16);
	CHECK(dlsym(RTLD_DEFAULT, "foo_add") == NULL);	/* RTLD_LOCAL */
	CHECK(dlsym(h, "no_such_symbol") == NULL && dlerror() != NULL);
	void *h2 = dlopen(path, RTLD_NOW | RTLD_GLOBAL);	/* again: the same module, now global */
	CHECK(h2 == h);
	CHECK(dlsym(RTLD_DEFAULT, "foo_add") == (void *)add);
	CHECK(dlopen("/no/such/lib.so", RTLD_NOW) == NULL && dlerror() != NULL);
	CHECK(dlclose(h) == 0);
	Dl_info info;
	CHECK(dladdr((void *)add, &info) && strstr(info.dli_fname, "libfoo") && !strcmp(info.dli_sname, "foo_add"));
	printf("main2: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
