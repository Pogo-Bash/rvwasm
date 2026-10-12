/*
 * Repro: does printf of a floating-point value work on wasm64?
 *
 * musl formats floats through long double, which needs the compiler-rt
 * comparison helpers (__netf2, __eqtf2, __unordtf2). wasm-ld warns about a
 * signature mismatch for those on wasm64 (i32 vs i64 result) and replaces
 * mismatched calls with a trap.
 */
#include <stdio.h>

int main(int argc, char **argv)
{
	volatile double d = 1.5;
	volatile long double ld = 2.25L;

	(void)argv;
	printf("integer: %d\n", argc);
	fflush(stdout);
	printf("long double compare: %d\n", ld != 0.0L);
	fflush(stdout);
	printf("double: %f\n", d);
	return 0;
}
