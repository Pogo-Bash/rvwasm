/* setjmp/longjmp (crt/nqsjlj.c): a longjmp through two frames, one nested setjmp, and a loop of 1000. */
#include <setjmp.h>
#include <stdio.h>

static jmp_buf outer, inner;
static int depth;

static void deep(int n, jmp_buf *where, int value)
{
	if (n == 0)
		longjmp(*where, value);
	depth++;
	deep(n - 1, where, value);
}

int main(void)
{
	volatile int count = 0;
	int r = setjmp(outer);
	if (r == 0) {
		int s = setjmp(inner);
		if (s == 0)
			deep(3, &inner, 7);
		printf("sjlj: inner returned %d after depth %d\n", s, depth);
		deep(2, &outer, 42);
	}
	printf("sjlj: outer returned %d\n", r);
	jmp_buf loop;
	if (setjmp(loop) < 1000) {
		count++;
		if (count < 1000)
			longjmp(loop, count);
	}
	printf("sjlj: %d longjmps in a loop\n", count);
	printf("sjlj: %s\n", r == 42 && count == 1000 ? "ok" : "FAILED");
	return 0;
}
