/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "rv.h"

extern char **environ;

/*
 * Address space, not memory: what the guest starts with, and how much it
 * may come to have. Memory is only taken for pages the guest touches (mm.c);
 * the address space itself costs a page table entry, 8 bytes, per page.
 */
#define DEFAULT_MEM_MB		64
#define DEFAULT_MEM_MAX_MB	8192
#define DEFAULT_STACK_KB	1024

/* The guest has ended, in this thread: turn how it ended into our exit status. */
int rv_finish(struct rv_cpu *cpu)
{
	if (cpu->trace)
		fclose(cpu->trace);
	if (cpu->verbose) {
		fprintf(stderr, "rv: %llu instructions\n", (unsigned long long)cpu->insn_count);
		fprintf(stderr, "rv: memory: %llu KiB of its own, %llu KiB of files' that other processes can have too\n",
			(unsigned long long)cpu->proc->host_pages * 4, (unsigned long long)cpu->proc->shared_pages * 4);
		if (cpu->proc->reserve_lookups)
			fprintf(stderr, "rv: accesses beyond the page table, in reservations: %llu, of which %llu took the lock to find the page\n",
				(unsigned long long)cpu->proc->reserve_lookups, (unsigned long long)cpu->proc->reserve_misses);
		rv_jit_report(cpu);
		rv_jit_report_threads(cpu);
#ifdef RV_HEAPCHECK
		rv_hc_report();
#endif
	}
	rv_zb_report();		/* if asked for: RV_ZB_COUNT */
	/* RV_CRASH_MAPS=1: with a fault that kills the program, where it was (the return address too) and the
	 * process's mappings, to find the library and the offset in it. */
	if (cpu->trap == RV_TRAP_SEGV && getenv("RV_CRASH_MAPS")) {
		fprintf(stderr, "rv: fault at pc 0x%llx, ra 0x%llx, sp 0x%llx; mappings:\n", (unsigned long long)cpu->pc,
			(unsigned long long)cpu->x[1], (unsigned long long)cpu->x[2]);
		rv_mm_maps(cpu, 2);
	}
	rv_mm_exit(cpu);

	switch (cpu->trap) {
	case RV_TRAP_EXIT:
		return cpu->exit_code;
	/* A fault the guest did not handle kills it with the signal, as on real Linux. */
	case RV_TRAP_SEGV:
		fprintf(stderr, "rv: segmentation fault: address 0x%llx at pc 0x%llx\n",
			(unsigned long long)cpu->trap_addr, (unsigned long long)cpu->pc);
		rv_signal_die(11);
		return 128 + 11;
	case RV_TRAP_ILL:
		fprintf(stderr, "rv: illegal instruction 0x%08x at pc 0x%llx\n",
			cpu->trap_insn, (unsigned long long)cpu->trap_addr);
		rv_signal_die(4);
		return 128 + 4;
	case RV_TRAP_BREAK:
		fprintf(stderr, "rv: breakpoint at pc 0x%llx\n", (unsigned long long)cpu->trap_addr);
		rv_signal_die(5);
		return 128 + 5;
	default:
		return 1;
	}
}

#ifdef __wasm__
/*
 * Keep musl's malloc out of our stack. Linux/Wasm has no MMU, and its loader
 * (binfmt_wasm.c) gives a program one piece of memory for its stack, puts
 * the break at the bottom of it and lets the break grow to the top
 * (start_brk is the piece, end_brk its end), while the stack grows down from
 * the top. musl's malloc takes the pages for its own records of the blocks
 * it hands out (a struct meta for each group of them) from the break for as
 * long as the break grows, a page at a time. The piece is 256 KiB: enough
 * records, as in a process with tens of thousands of translated regions,
 * and they are in the pages the stack is using, the two write over each
 * other, and malloc aborts or faults on records that are no longer its own
 * (the g++ crash with RV_JIT_HOT=1: the break at the top of the stack, the
 * stack pointer in the same page as the list of a size class).
 *
 * So, before anything is allocated, the break is moved as far up as the
 * kernel lets it go, which only changes a number in the kernel: from then
 * on it cannot grow, and malloc takes those pages with mmap(), like all
 * its other memory. How far: the kernel takes a new break anywhere up to
 * the end and answers anything beyond with the old one, so a binary search.
 */
static void keep_the_break_off_the_stack(void)
{
	uintptr_t lo = (uintptr_t)syscall(SYS_brk, (long)0), hi = lo + ((uintptr_t)1 << 30);

	if (!lo || lo & (RV_PAGE_SIZE - 1))
		return;
	while (hi - lo > RV_PAGE_SIZE) {
		uintptr_t mid = lo + ((hi - lo) / 2 & ~(uintptr_t)(RV_PAGE_SIZE - 1));

		if ((uintptr_t)syscall(SYS_brk, (long)mid) == mid)
			lo = mid;
		else
			hi = mid;
	}
	syscall(SYS_brk, (long)lo);
}
#endif

/* Run the guest in the first thread, to its end or the thread's. */
static int run(struct rv_cpu *cpu)
{
	/*
	 * The stack below here, as far as it surely goes (thread.c, "The
	 * emulator's own stack"): binfmt_wasm gives 256 KiB, the arguments and
	 * the environment on top, and the C library's start has taken a little.
	 */
	uintptr_t top = (uintptr_t)__builtin_frame_address(0);

	rv_stack_init(top - (240 << 10), top);
	rv_run(cpu);
	rv_stack_report();
	/* Only this thread ended (thread.c): the process goes on for as long as the others do. */
	if (cpu->trap == RV_TRAP_THREAD)
		pthread_exit(NULL);
	return rv_finish(cpu);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: rv [options] program [args...]\n"
		"  -m MiB    guest memory to start with (default %d); it grows as needed\n"
		"  -M MiB    how far it may grow (default %d)\n"
		"  -s KiB    guest stack size (default %d)\n"
		"  -t FILE   write a per-instruction register trace to FILE\n"
		"  -L DIR    look for the guest's absolute paths under DIR first (or RV_SYSROOT)\n"
		"  -S        log every system call\n"
		"  -v        report unimplemented syscalls and the instruction count\n"
		"The RV_MEM_MB and RV_MEM_MAX_MB environment variables set the defaults for -m and -M.\n"
		"RV_FORK=snapshot makes fork start a second emulator even where the host can fork.\n"
		"RV_JIT=0 turns the JIT off (Linux/Wasm); RV_JIT_HOT=N sets how often a block runs first,\n"
		"RV_JIT_HOT_MADE=N the same for code the program made itself.\n",
		DEFAULT_MEM_MB, DEFAULT_MEM_MAX_MB, DEFAULT_STACK_KB);
}

int main(int argc, char **argv)
{
	static struct rv_cpu cpu;
	static struct rv_proc proc;
	uint64_t mem_mb = DEFAULT_MEM_MB, mem_max_mb = DEFAULT_MEM_MAX_MB, stack_kb = DEFAULT_STACK_KB;
	const char *trace_path = NULL;
	const char *env_mem = getenv("RV_MEM_MB");
	const char *env_mem_max = getenv("RV_MEM_MAX_MB");
	const char *env_fork = getenv("RV_FORK");
	const char *env_jit = getenv("RV_JIT");
	const char *sysroot = getenv("RV_SYSROOT");
	static char exe_path[4096];
	int i;

#ifdef __wasm__
	/* First: nothing has been allocated yet. */
	keep_the_break_off_the_stack();
#endif
	rv_thread_first(&cpu, &proc);
	proc.self_fd = -1;
	rv_signal_init();

#ifdef __wasm__
	/*
	 * On Linux/Wasm a task normally has a CPU to itself, because nothing
	 * can take one away from a task that computes; that makes a task dear,
	 * and their number small. We tell the kernel that this program never
	 * goes long without a system call (rv_signal_poll() sees to that), so
	 * that it, its threads and the programs it starts can share a few CPUs
	 * like tasks do elsewhere. (A kernel without the request says no, and
	 * nothing changes.)
	 */
#ifndef PR_WASM_COOPERATIVE
#define PR_WASM_COOPERATIVE 0x5741534d
#endif
	prctl(PR_WASM_COOPERATIVE, 1UL, 0UL, 0UL, 0UL);
#endif

	/* Where the kernel keeps our arguments, which are to show the guest's (identity.c). */
	rv_args_init(argc, argv);

	/* The child of a guest fork: everything comes from the parent's snapshot (a sixth argument is only room, fork.c). */
	if ((argc == 5 || argc == 6) && !strcmp(argv[1], "--resume")) {
		if (rv_fork_resume(&cpu, &proc, argv[2], atoi(argv[3]), atoi(argv[4]))) {
			fprintf(stderr, "rv: cannot copy the guest from the parent of a fork\n");
			return 127;
		}
		return run(&cpu);
	}
	rv_signal_inherit_mask(&cpu);
	if (env_fork && !strcmp(env_fork, "snapshot"))
		cpu.fork_snapshot = 1;
	/* binfmt_misc starts us without options: -v by way of the environment. */
	if (getenv("RV_VERBOSE"))
		cpu.verbose = 1;
	if (getenv("RV_STRACE"))	/* -S */
		cpu.strace = 1;
	rv_zb_count_on();	/* RV_ZB_COUNT (bitmanip.c) */

	/* binfmt_misc starts us without options, so the environment can set the arena size. */
	if (env_mem && strtoull(env_mem, NULL, 0))
		mem_mb = strtoull(env_mem, NULL, 0);
	if (env_mem_max && strtoull(env_mem_max, NULL, 0))
		mem_max_mb = strtoull(env_mem_max, NULL, 0);

	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		} else if (!strcmp(argv[i], "-v")) {
			cpu.verbose = 1;
		} else if (!strcmp(argv[i], "-S")) {
			cpu.strace = 1;
		} else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
			mem_mb = strtoull(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-M") && i + 1 < argc) {
			mem_max_mb = strtoull(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			stack_kb = strtoull(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			trace_path = argv[++i];
		} else if (!strcmp(argv[i], "-L") && i + 1 < argc) {
			sysroot = argv[++i];
		} else {
			usage();
			return 2;
		}
	}
	if (i >= argc || !mem_mb || !stack_kb) {
		usage();
		return 2;
	}

	if (sysroot && strlen(sysroot) >= sizeof(cpu.sysroot)) {
		fprintf(stderr, "rv: sysroot path too long\n");
		return 2;
	}
	if (sysroot)
		strcpy(cpu.sysroot, sysroot);

	cpu.mem_max = (mem_max_mb > mem_mb ? mem_max_mb : mem_mb) << 20;
	if (rv_mm_init(&cpu, mem_mb << 20)) {
		fprintf(stderr, "rv: cannot allocate the page table\n");
		return 1;
	}
	/* Not realpath(): its PATH_MAX buffers do not fit the 8 KiB process stack on Linux/Wasm. */
	if (argv[i][0] == '/') {
		/* (A copy: the arguments are moved about below.) */
		snprintf(exe_path, sizeof(exe_path), "%s", argv[i]);
		cpu.exe_path = exe_path;
	} else if (getcwd(exe_path, sizeof(exe_path)) &&
		   strlen(exe_path) + strlen(argv[i]) + 2 <= sizeof(exe_path)) {
		strcat(exe_path, "/");
		strcat(exe_path, argv[i]);
		cpu.exe_path = exe_path;
	}
	/*
	 * And with the links in it followed: a program looks for its own files
	 * from where it really is (java's libraries are next to what
	 * /usr/bin/java points to, by way of /etc/alternatives). The kernel
	 * knows the name: it is what it calls the open file.
	 */
	if (cpu.exe_path) {
		static char real[4096];
		char link[40];
		int fd = open(cpu.exe_path, O_PATH | O_CLOEXEC);

		if (fd >= 0) {
			ssize_t n;

			snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
			n = readlink(link, real, sizeof(real) - 1);
			if (n > 0 && real[0] == '/') {
				real[n] = '\0';
				cpu.exe_path = real;
			}
			close(fd);
		}
	}

	/*
	 * Our own program, held by the emulator before us after a chroot()
	 * (syscall.c: sys_chroot()), named in the environment as RV_SELF: ours
	 * to keep for the same reason, and not the guest's to see.
	 */
	if (getenv("RV_SELF")) {
		int fd = atoi(getenv("RV_SELF"));

		unsetenv("RV_SELF");
		if (fd >= 3 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0)
			proc.self_fd = fd;
	}

	/* The system call filters of the program that executed this one, if it had any. */
	if (rv_seccomp_load(&cpu)) {
		fprintf(stderr, "rv: cannot take up the system call filters of the program before\n");
		return 126;
	}
	if (rv_load_program(&cpu, argv[i], stack_kb << 10))
		return 1;
	/* For ps and the like, the process is the guest's program, not the emulator. */
	prctl(PR_SET_NAME, (unsigned long)(strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i]), 0UL, 0UL, 0UL);
	if (rv_setup_stack(&cpu, argc - i, argv + i, environ)) {
		fprintf(stderr, "rv: cannot set up the guest stack\n");
		return 1;
	}

	/*
	 * The JIT needs a host that can compile Wasm: on by default on
	 * Linux/Wasm (RV_JIT=0 turns it off). Elsewhere RV_JIT_DUMP=DIR makes
	 * the translator run and write its modules there, for inspection, and
	 * RV_JIT_FAKE=1 makes it do everything but run them (see jit.c).
	 */
#ifdef __wasm__
	if (!env_jit || strcmp(env_jit, "0"))
#else
	(void)env_jit;
	if (getenv("RV_JIT_DUMP") || getenv("RV_JIT_FAKE") || getenv("RV_JIT_EMU"))
#endif
		if (rv_jit_init(&cpu))
			fprintf(stderr, "rv: cannot set up the JIT, interpreting\n");

	if (trace_path) {
		cpu.trace = fopen(trace_path, "w");
		if (!cpu.trace) {
			fprintf(stderr, "rv: cannot open %s: %s\n", trace_path, strerror(errno));
			return 1;
		}
	}

	/*
	 * Last, since it moves the strings argv points to: what other programs
	 * see as our command line is the guest's arguments, which are the last
	 * of ours (identity.c).
	 */
	{
		size_t len = 0;

		for (int k = i; k < argc; k++)
			len += strlen(argv[k]) + 1;
		rv_args_show(argv[i], len);
	}

	return run(&cpu);
}
