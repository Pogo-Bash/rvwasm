/* What a call costs across modules, against the same calls inside one module. Built twice: against
 * libfoo.so (run with ld-wasm), and with libfoo.c linked in (a static PIE, as nq-clang builds them). */
#include <stdio.h>
#include <time.h>

int foo_inc(int x);
int foo_loop_direct(int n);
int foo_loop_weak(int n);
int foo_loop_ptr(int n, int (*f)(int));
int foo_loop_counter(int n);

__attribute__((noinline)) static int local_inc(int x) { return x + 1; }

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

#define N 20000000
#define RUN(label, expr) do { \
	double t = now(); volatile int r = (expr); (void)r; t = now() - t; \
	printf("%-44s %6.2f ns/call\n", label, t * 1e9 / N); } while (0)

static int loop_local(int n) { int s = 0; for (int i = 0; i < n; i++) s = local_inc(s); return s; }
static int loop_import(int n) { int s = 0; for (int i = 0; i < n; i++) s = foo_inc(s); return s; }
extern int foo_counter;
static int loop_data(int n) { int s = 0; for (int i = 0; i < n; i++) { (*(volatile int *)&foo_counter)++; s += *(volatile int *)&foo_counter; } return s; }
static int loop_ptr(int n, int (*f)(int)) { int s = 0; for (int i = 0; i < n; i++) s = f(s); return s; }

int main(void)
{
	for (int round = 0; round < 2; round++) {
		printf("round %d\n", round);
		RUN("program -> program (direct)", loop_local(N));
		RUN("program -> library (import)", loop_import(N));
		RUN("program -> library (pointer)", loop_ptr(N, foo_inc));
		RUN("library -> library (direct)", foo_loop_direct(N));
		RUN("library -> library weak (trampoline)", foo_loop_weak(N));
		RUN("library -> program (pointer)", foo_loop_ptr(N, local_inc));
		RUN("library data (GOT.mem) read+write", foo_loop_counter(N));
		RUN("program data (GOT.mem of the library's)", loop_data(N));
	}
	return 0;
}
