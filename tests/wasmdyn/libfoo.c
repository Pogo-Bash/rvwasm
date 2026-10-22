/* libfoo: the C half of the dynamic-linking prototype's library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int foo_counter = 40;			/* data, exported: GOT.mem in its users */
static const char *greeting = "hello from libfoo";	/* a pointer in data: a data relocation */
_Thread_local int foo_tls = 7;		/* thread-local, per thread and per module */
int (*foo_callback)(int);		/* a function pointer set by the program */

int foo_add(int a, int b) { return a + b + foo_counter; }

const char *foo_greeting(void) { return greeting; }

int foo_call(int x) { return foo_callback ? foo_callback(x) : -1; }

int *foo_tls_addr(void) { return &foo_tls; }

int foo_tls_bump(void) { return ++foo_tls; }

/* Address of a libc function taken inside the library: GOT.func strlen. */
size_t (*foo_strlen_ptr(void))(const char *) { return strlen; }

__attribute__((weak)) int foo_weak(void) { return 1; }

/* (Not something the compiler can work out ahead: GlobalOpt would fold `foo_counter += 2` into the initial
 * value and drop the constructor.) */
__attribute__((constructor)) static void foo_init(void) { foo_counter += getenv("FOO_NEVER_SET") ? 1 : 2; }

/* For bench.c: calls inside the library, to its own weak function (bound through a trampoline, as
 * wasm-ld imports a shared library's weak definitions), and through a pointer. */
__attribute__((noinline)) int foo_inc(int x) { return x + 1; }

int foo_loop_direct(int n)
{
	int s = 0;
	for (int i = 0; i < n; i++)
		s = foo_inc(s);
	return s;
}

int foo_loop_weak(int n)
{
	int s = 0;
	for (int i = 0; i < n; i++)
		s += foo_weak();
	return s;
}

int foo_loop_ptr(int n, int (*f)(int))
{
	int s = 0;
	for (int i = 0; i < n; i++)
		s = f(s);
	return s;
}

int foo_loop_counter(int n)
{
	int s = 0;
	for (int i = 0; i < n; i++) {
		/* through GOT.mem (the program may have its own copy); volatile, or it is one multiplication */
		(*(volatile int *)&foo_counter)++;
		s += *(volatile int *)&foo_counter;
	}
	return s;
}
