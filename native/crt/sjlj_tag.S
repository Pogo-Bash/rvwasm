/*
 * The tag longjmp is thrown with (crt/nqsjlj.c): tag 1 of the Wasm SjLj
 * lowering. Its one parameter is the address of {env, val} (64-bit on wasm64).
 */
	.tagtype __c_longjmp i64
	.globl __c_longjmp
__c_longjmp:
