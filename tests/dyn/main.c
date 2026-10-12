/*
 * A dynamically linked program: the loader has to map the C library, relocate
 * everything, set up thread-local storage and run constructors before main.
 * Given a path, it also loads a shared library at run time and calls into it.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread int tls_value = 7;
static int constructed;

__attribute__((constructor)) static void init(void)
{
	constructed = 1;
}

int main(int argc, char **argv)
{
	volatile double two = 2.0;
	char *block;

	printf("dynamically linked: argc=%d constructed=%d tls=%d\n", argc, constructed, tls_value);
	printf("sqrt(2)=%.6f\n", sqrt(two));

	block = malloc(200000);
	memset(block, 'x', 200000);
	printf("malloc: %c%c\n", block[0], block[199999]);
	free(block);

	if (argc > 1) {
		void *lib = dlopen(argv[1], RTLD_NOW);
		int (*greet)(int);
		int *calls;

		if (!lib) {
			printf("dlopen failed: %s\n", dlerror());
			return 1;
		}
		greet = (int (*)(int))dlsym(lib, "greet");
		calls = dlsym(lib, "greet_calls");
		printf("greet(20)=%d greet(1)=%d calls=%d\n", greet(20), greet(1), *calls);
		printf("dlclose=%d\n", dlclose(lib));
	}
	return 3;
}
