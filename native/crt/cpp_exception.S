/*
 * The tag C++ exceptions are thrown with (Wasm exception handling). The
 * compiler leaves it undefined in every object; in a position independent
 * link wasm-ld would import it from the host, which linux-wasm's runtime
 * does not give. Defined here, in the program, it is the program's own.
 * Its one parameter is the thrown object's address (64-bit on wasm64).
 */
	.tagtype __cpp_exception i64
	.globl __cpp_exception
__cpp_exception:
