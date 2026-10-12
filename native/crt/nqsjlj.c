/*
 * Provenance: written to match the functions of Emscripten's emscripten_setjmp.c (MIT or UoI-NCSA, Copyright
 * The Emscripten authors); treated as derived from it, so that notice applies (see LICENSE.md).
 *
 * setjmp/longjmp for native programs: LLVM's Wasm SjLj lowering (bin/nq-clang
 * compiles with -mllvm -wasm-enable-sjlj) and the functions it calls, as
 * Emscripten's compiler-rt has them for LLVM 18 (emscripten_setjmp.c).
 *
 * linux-wasm's musl has a setjmp that returns 0 and a longjmp that aborts.
 * FreeType's rasterizer longjmps on an ordinary path (a band that does not fit
 * its cell pool is split and drawn again), so the first text a Qt program drew
 * aborted it.
 *
 * How the lowering works: a function that calls setjmp gets a table of the
 * setjmps it has done (pairs of int: an id, the label of the call site);
 * saveSetjmp() gives each dynamic setjmp a new id, kept in the jmp_buf.
 * longjmp(env, val) becomes __wasm_longjmp(), which throws the tag
 * __c_longjmp (crt/sjlj_tag.S) with &{env, val}. Every function with setjmps
 * catches that tag around its calls and asks testSetjmp() whether the jmp_buf's
 * id is one of its own: if so it goes on after that setjmp, returning val;
 * if not it throws on to its caller.
 */
#include <stdint.h>
#include <stdlib.h>

static _Thread_local uint32_t setjmp_id;
static _Thread_local uint32_t temp_ret0;

void setTempRet0(uint32_t value)
{
	temp_ret0 = value;
}

uint32_t getTempRet0(void)
{
	return temp_ret0;
}

int32_t *saveSetjmp(void *env, uint32_t label, int32_t *table, uint32_t size)
{
	uint32_t i;

	setjmp_id++;
	*(uintptr_t *)env = setjmp_id;
	for (i = 0; i < size; i++) {
		if (table[i * 2] == 0) {
			table[i * 2] = setjmp_id;
			table[i * 2 + 1] = label;
			table[i * 2 + 2] = 0;	/* the next free slot */
			setTempRet0(size);
			return table;
		}
	}
	/* Full: twice the size, and again. */
	size *= 2;
	table = realloc(table, sizeof(int32_t) * (2 * size + 1));
	if (!table)
		abort();
	setjmp_id--;
	table = saveSetjmp(env, label, table, size);
	setTempRet0(size);
	return table;
}

uint32_t testSetjmp(uintptr_t id, int32_t *table, uint32_t size)
{
	uint32_t i;

	for (i = 0; i < size; i++) {
		uint32_t current = table[i * 2];
		if (current == 0)
			break;
		if (current == (uint32_t)id)
			return table[i * 2 + 1];
	}
	return 0;
}

struct __WasmLongjmpArgs {
	void *env;
	int val;
};

static _Thread_local struct __WasmLongjmpArgs longjmp_args;

_Noreturn void __wasm_longjmp(void *env, int val)
{
	longjmp_args.env = env;
	longjmp_args.val = val ? val : 1;
	__builtin_wasm_throw(1, &longjmp_args);
}
