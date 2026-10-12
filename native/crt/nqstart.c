/*
 * What every native program does between libc's setup and main
 * (bin/nq-clang links this object into each). Compiled against musl's own
 * internal headers (build-runtimes.sh), for struct pthread.
 *
 * 1. Static constructors run where libc runs them, not before libc is up.
 *    linux-wasm's runtime calls a program's exported __wasm_call_ctors right
 *    before _start, that is before musl has set itself up (__init_libc: the
 *    page size from the auxiliary vector, TLS, the environment). A
 *    constructor that allocates then gets NULL from malloc (mmap of a length
 *    rounded to a page size of 0 is EINVAL): Qt's static plugin registration
 *    does, and std::bad_alloc came out of the program before main. So
 *    bin/nq-clang does not export __wasm_call_ctors (the runtime then leaves
 *    it alone), and musl's __libc_start_main calls them here, where it would
 *    run .init_array (which Wasm has not got).
 *
 * 2. A main stack of 8 MiB instead of binfmt_wasm's 256 KiB. That stack has no
 *    guard page and nothing protects what lies below it (kernel memory
 *    included), and code written for an 8 MiB stack runs off it: Qt's QML
 *    engine sizes its recursion checks from RLIMIT_STACK (8 MiB), and a Qt
 *    Quick program corrupted the kernel (indirect calls through a garbage
 *    table index in fbcon and in a system call). The new stack is put into
 *    the main thread's struct pthread too, so pthread_getattr_np tells the
 *    truth. Threads get 1 MiB by default instead of musl's 128 KiB.
 *    NQ_MAIN_STACK_KB / NQ_THREAD_STACK_KB in the environment change both.
 */
#define _GNU_SOURCE
#include "pthread_impl.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>

hidden void __wasm_call_ctors(void);
hidden void __nq_set_stack_pointer(void *top);	/* crt/nqstack.S */

static size_t nq_size_kb(const char *name, size_t kb)
{
	const char *v = getenv(name);
	if (v && atol(v) >= 64)
		kb = atol(v);
	return kb << 10;
}

static __attribute__((noinline)) void nq_thread_stacks(void)
{
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, nq_size_kb("NQ_THREAD_STACK_KB", 1024));
	pthread_setattr_default_np(&a);
}

/* No locals whose address is taken: this function has no frame on the stack it
 * leaves, and nothing restores the old stack pointer when it returns. */
hidden void __libc_start_init(void)
{
	size_t size = nq_size_kb("NQ_MAIN_STACK_KB", 8192);
	char *stack = MAP_FAILED;

	nq_thread_stacks();
	/* No MMU: an mmap is that many contiguous pages; take less if 8 MiB are not there. */
	for (; size >= (1 << 20); size /= 2) {
		stack = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (stack != MAP_FAILED)
			break;
	}
	if (stack != MAP_FAILED) {
		pthread_t self = pthread_self();
		self->stack = stack + size;
		self->stack_size = size;
		__nq_set_stack_pointer(stack + size);
	}
	__wasm_call_ctors();
}
