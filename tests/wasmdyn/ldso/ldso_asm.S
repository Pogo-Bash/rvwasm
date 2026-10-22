/* ld-wasm's own __memory_base (where the kernel put its data): the base of the addresses its exported
 * data symbols are relative to. */
	.globaltype __memory_base, i64, immutable

	.globl __dl_self_memory_base
	.hidden __dl_self_memory_base
__dl_self_memory_base:
	.functype __dl_self_memory_base () -> (i64)
	global.get __memory_base
	end_function
