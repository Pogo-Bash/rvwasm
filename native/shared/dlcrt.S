/*
 * Linked into every module ld-wasm loads (programs and shared libraries): what the dynamic linker
 * needs to give each thread its own copy of the module's thread-local variables. A module's
 * __tls_base is a global of its own instance, and every thread (Worker) has its own instances, so
 * setting it in one thread's instance is giving that thread its block.
 */
	.globaltype __tls_base, i64
	.globaltype __tls_size, i64, immutable
	.globaltype __tls_align, i64, immutable
	.functype __wasm_init_tls (i64) -> ()

/* A new block: the module's initial TLS image copied into it, and __tls_base pointed at it. */
	.globl __dl_tls_init
	.export_name __dl_tls_init, __dl_tls_init
__dl_tls_init:
	.functype __dl_tls_init (i64) -> ()
	local.get 0
	call __wasm_init_tls
	end_function

/* A block that is there already (a vfork child running on its parent's): only __tls_base. */
	.globl __dl_tls_set
	.export_name __dl_tls_set, __dl_tls_set
__dl_tls_set:
	.functype __dl_tls_set (i64) -> ()
	local.get 0
	global.set __tls_base
	end_function

	.globl __dl_tls_size
	.export_name __dl_tls_size, __dl_tls_size
__dl_tls_size:
	.functype __dl_tls_size () -> (i64)
	global.get __tls_size
	end_function

	.globl __dl_tls_align
	.export_name __dl_tls_align, __dl_tls_align
__dl_tls_align:
	.functype __dl_tls_align () -> (i64)
	global.get __tls_align
	end_function
