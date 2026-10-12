/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rv: a riscv64 Linux user-mode emulator.
 *
 * Portable C with no host dependencies beyond libc, so the same sources build
 * natively (for differential testing against qemu-riscv64) and as a native
 * wasm program for Linux/Wasm.
 *
 * The guest's address space is made in software: a page table says where in
 * the host's memory each of its pages is (mm.c). That lets non-PIE guests
 * live at their linked address on a host without an MMU, and lets memory be
 * taken from the host only when the guest touches it.
 *
 * Every thread of the guest is a thread of the host with a struct rv_cpu of
 * its own; what the threads share is in struct rv_proc (see thread.c).
 */
#ifndef RV_H
#define RV_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef RV_HEAPCHECK
/* A build for hunting memory corruption: every allocation goes through heapcheck.c. */
#include <stdlib.h>
void *rv_hc_malloc(size_t size, const char *file, int line);
void *rv_hc_calloc(size_t n, size_t size, const char *file, int line);
void *rv_hc_realloc(void *p, size_t size, const char *file, int line);
void rv_hc_free(void *p, const char *file, int line);
void rv_hc_check(const char *where);
void rv_hc_syscall_done(uint64_t nr);
void rv_hc_light(const char *where);
void rv_hc_report(void);
#define malloc(n)	rv_hc_malloc((n), __FILE__, __LINE__)
#define calloc(n, s)	rv_hc_calloc((n), (s), __FILE__, __LINE__)
#define realloc(p, n)	rv_hc_realloc((p), (n), __FILE__, __LINE__)
#define free(p)		rv_hc_free((p), __FILE__, __LINE__)
#endif

#define RV_PAGE_SHIFT	12
#define RV_PAGE_SIZE	(1ULL << RV_PAGE_SHIFT)
#define RV_PAGE_ALIGN(x) (((x) + RV_PAGE_SIZE - 1) & ~(RV_PAGE_SIZE - 1))

/*
 * A page's entry in the page table (rv_cpu.map, see mm.c): these in its low
 * 12 bits, and in the rest the distance from the page's guest address to its
 * host address. The low three bits match Linux's PROT_*: what may be done
 * with the page right now, which is nothing while it has no memory.
 */
#define RV_PROT_READ	0x01
#define RV_PROT_WRITE	0x02
#define RV_PROT_EXEC	0x04
#define RV_PROT_MASK	0x07
#define RV_PAGE_MAPPED	0x08	/* part of a mapping, even if PROT_NONE */
#define RV_PAGE_BACKED	0x10	/* has host memory */
#define RV_PAGE_CODE	0x20	/* translated code was made from this page (jit.c): not to be written as it stands */
#define RV_PAGE_WANT_SHIFT 6	/* three bits from here: the protection its mapping has */
#define RV_PAGE_SHARED	0x200	/* its memory is a file's, shared with other processes: not to be written */
#define RV_PAGE_LIVE	0x400	/* its memory is a part of a file that processes share to write to (mm.c: live_attach()) */
#define RV_PAGE_FLAGS	0xfff

/* Linux ABI constants as the guest sees them (the host's may differ). */
#define RV_MAP_SHARED		0x01
#define RV_MAP_PRIVATE		0x02
#define RV_MAP_FIXED		0x10
#define RV_MAP_ANONYMOUS	0x20
#define RV_MAP_FIXED_NOREPLACE	0x100000
#define RV_MREMAP_MAYMOVE	1

#define RV_EPERM	1
#define RV_ENOENT	2
#define RV_EINTR	4
#define RV_EIO		5
#define RV_EBADF	9
#define RV_EAGAIN	11
#define RV_ENOMEM	12
#define RV_EACCES	13
#define RV_EFAULT	14
#define RV_EEXIST	17
#define RV_ENODEV	19
#define RV_ENOTDIR	20
#define RV_EINVAL	22
#define RV_ENOTTY	25
#define RV_ERANGE	34
#define RV_ENOSYS	38
#define RV_EBADFD	77

#define RV_NSIG		64
#define RV_SYSROOT_MAX	256

/*
 * What the processor says it is: /proc/cpuinfo (identity.c) and
 * riscv_hwprobe (syscall.c). RV64GC and the bit-manipulation extensions
 * Zba, Zbb and Zbs (bitmanip.c; not said with RV_BITMANIP=0; the V
 * instructions vector.c has are never said), with the counters (Zicntr: cycle, time and instret, fp.c's
 * rv_csr()) but not the further performance counters (Zihpm). No vendor or
 * architecture id: 0 is what an implementation without one reports. The
 * implementation id is rv's version, 0xMMmmpp for version MM.mm.pp.
 */
#define RV_VERSION_MAJOR	0
#define RV_VERSION_MINOR	1
#define RV_VERSION_PATCH	0
#define RV_MVENDORID	0x0
#define RV_MARCHID	0x0
#define RV_MIMPID	((RV_VERSION_MAJOR << 16) | (RV_VERSION_MINOR << 8) | RV_VERSION_PATCH)
#define RV_ISA		"rv64imafdc_zicntr_zicsr_zifencei_zba_zbb_zbs"
#define RV_ISA_NO_ZB	"rv64imafdc_zicntr_zicsr_zifencei"
#define RV_UARCH	"rvwasm,rv"

/* The kernel's struct sigaction on riscv64 (no restorer field). */
struct rv_sigaction {
	uint64_t handler;
	uint64_t flags;
	uint64_t mask;
};

struct rv_jit;
struct rv_cpu;
struct rv_chunk;

/* Why pages that code was translated from are no longer what they were (rv_jit_code_changed()). */
enum rv_code_cause {
	RV_CODE_MOVED,		/* the page table is elsewhere (counted only: translated code reads where it is) */
	RV_CODE_WRITE,		/* the guest is about to write to the page */
	RV_CODE_PROTECT,	/* mprotect() took away the right to execute it */
	RV_CODE_UNMAP,		/* munmap(), mmap() over it, mremap(), madvise(): the page is gone or is other memory */
	RV_CODE_FLUSH,		/* riscv_flush_icache(): the guest says the code changed */
	RV_CODE_CAUSES
};
#define RV_CODE_LOG	64

/* A system call's buffer that is a copy, because the guest's pages are not in one piece (mm.c). */
struct rv_bounce {
	uint64_t addr, len;
	uint8_t *buf;
	int write;		/* the host may have written to it */
};

/*
 * What the threads of a guest process share, apart from the memory itself.
 * `lock` guards all of it, the page map and the layout of the address space;
 * rv_lock() takes it. See thread.c for the rest.
 */
struct rv_proc {
	pthread_mutex_t lock;
	struct rv_cpu *threads;		/* all of them, linked by rv_cpu.next_thread */
	int nthreads;

	/* Stopping the world (thread.c). */
	pthread_mutex_t park_lock;
	pthread_cond_t park_cond;
	int stop;			/* a thread wants every other one parked */
	int parked;			/* how many are */

	/*
	 * Counts up whenever translated code may have become wrong: pages it
	 * was made from changed, or the page table moved. Every thread has its
	 * own translations (jit.c) and looks at what happened when it sees a
	 * new number: code_log has the last RV_CODE_LOG such events, the one
	 * numbered n at n % RV_CODE_LOG, as the pages concerned (count 0: all
	 * there is). code_events counts them by cause, for -v.
	 */
	volatile uint32_t code_epoch;
	struct {
		volatile uint64_t first, count;
	} code_log[RV_CODE_LOG];
	uint64_t code_events[RV_CODE_CAUSES];

	/* The host memory the guest's pages are in (mm.c). */
	struct rv_chunk **chunks;
	uint32_t nchunks, chunks_cap, pool_hint, pool_chunks;
	uint64_t host_pages;		/* pages of memory of our own */
	uint64_t shared_pages;		/* pages of files' memory attached, which other processes may have too */

	uint64_t brk;			/* current program break */

	/* The process has said (unshare(), setns()) that its children are to be in another namespace of process ids. */
	int children_apart;

	/*
	 * The seals of files in memory, where the host has no such files and
	 * a plain file stands in for one (syscall.c: memory_file()): by the
	 * file's identity. Kept, not enforced.
	 */
	struct {
		uint64_t dev, ino;
		uint32_t seals;
	} seals[48];
	uint32_t nseals;

	/*
	 * Reservations: very large stretches of addresses that a program asks
	 * for and mostly never uses, beyond what the page table spans (mm.c).
	 */
	struct rv_reserve {
		uint64_t base, count;	/* the addresses: where, and how many pages */
		uint64_t flat, off, n;	/* pages [off, off + n) of it are the page table's pages from `flat` on */
	} reserves[64];
	uint32_t nreserves;
	uint64_t reserve_next;		/* where the next one goes */
	/*
	 * Counts up whenever a page of a reservation may have come to stand for
	 * another page of the table, or for none (mm.c: what the threads have
	 * remembered of where those pages are is then out of date).
	 */
	uint64_t reserve_gen;
	uint64_t reserve_lookups, reserve_misses;	/* (for RV_VERBOSE: accesses in reservations, and those that took the lock) */

	/* Which files the guest's file mappings are of, for /proc/self/maps (mm.c). */
	struct rv_named *named;
	uint32_t nnamed, named_cap;
	int file_copies;	/* some of them are shared mappings that are copies, which pwrite() keeps up to date */

	/* The display, while the guest has it mapped (see mm.c): our descriptor for it, and where in the arena. */
	int fb_fd;
	uint64_t fb_addr, fb_len;

	/*
	 * Our own program, held open from the guest's first chroot() on
	 * (syscall.c): the new root need not have a /proc, and exec and fork
	 * start emulators by way of /proc/self/exe otherwise. -1 until then.
	 */
	int self_fd;

	/* Signals (see signal.c). */
	struct rv_sigaction sigact[RV_NSIG + 1];
	uint64_t sig_trampoline;	/* guest address of the sigreturn code */
};

enum rv_trap {
	RV_TRAP_NONE = 0,
	RV_TRAP_EXIT,		/* guest called exit/exit_group */
	RV_TRAP_SEGV,		/* guest touched memory it has no access to */
	RV_TRAP_ILL,		/* unknown or unsupported instruction */
	RV_TRAP_BREAK,		/* ebreak */
	RV_TRAP_THREAD,		/* this thread called exit, and it is not the last */
};

struct rv_cpu {
	uint64_t x[32];
	uint64_t f[32];		/* raw bits; single-precision values are NaN-boxed */
	uint64_t pc;
	uint32_t fcsr;		/* fflags in bits 4:0, frm in bits 7:5 */
	/* The V extension, what little of it there is (vector.c): 32 registers of 128 bits, vl, vtype. */
	uint8_t v[32][16];
	uint64_t vl, vtype;

	/* LR/SC reservation: address, and the value seen there by LR. */
	uint64_t lr_addr;
	uint64_t lr_value;
	int lr_valid;

	struct rv_proc *proc;	/* what this thread shares with the others */

	/*
	 * The guest's page table (see mm.c): an entry for each of npages
	 * pages, and one more at the end that allows nothing, for the JIT's
	 * checks. Every thread has these two for itself, all the same: they
	 * change only when the table is replaced by a longer one, which
	 * happens with the world stopped.
	 */
	uint64_t *map;
	uint64_t npages;
	uint64_t mem_max;	/* how much address space the guest may come to have (see mm.c) */
	/* This thread's memory of which page of the table stands for a page of a reservation (mm.c: rv_reserve_xlat()). */
	struct rv_reserve_seen {
		uint64_t page, flat, gen;
	} reserve_seen[256];

	/* What rv_user_ptr() gave out as copies, until rv_user_done(). */
	struct rv_bounce *bounces;
	int nbounces, bounces_cap;

	struct rv_jit *jit;	/* this thread's translations; NULL when the JIT is off or not yet started */
	uint8_t *jit_used;	/* by a hash of a region's address: entered since the last look (translated code sets it, jit.c: used_hash()) */
	int jit_wanted;		/* the JIT is on: a thread starts its own when it has run for a while */
	int jit_fault;		/* a helper called by translated code could not execute its instruction */

	/* Guest address space layout, all guest addresses. */
	uint64_t brk_start;	/* end of the loaded image */
	uint64_t mmap_top;	/* mmap() places mappings below this */
	uint64_t stack_low;	/* lowest address of the stack mapping */
	uint64_t stack_size;	/* its size when the program was loaded */

	/* From the ELF loader, for the auxiliary vector. */
	uint64_t elf_entry;
	uint64_t elf_phdr;
	uint64_t elf_phnum;
	uint64_t elf_base;	/* where the interpreter was loaded, or 0 */

	/*
	 * Directory looked in first for the guest's absolute paths (like
	 * qemu's -L), or empty. Lets riscv64 libraries live beside the host's.
	 */
	char sysroot[RV_SYSROOT_MAX];

	/*
	 * Signals (see signal.c). sig_pending is what the run loops and
	 * translated code look at between blocks: the signals that have
	 * arrived for this thread, set by the host's handler and by other
	 * threads. Two bits of it are not signals: RV_SIG_STOP asks the thread
	 * to park (thread.c), RV_SIG_CODE to look at what became of the code it
	 * has translated (jit.c).
	 */
	volatile uint64_t sig_pending;
	volatile uint64_t sig_tkill;	/* which of them a thread of this process sent with tkill (for si_code) */
	uint64_t sig_held;		/* arrived while the guest blocks them: taken when it no longer does */
	uint64_t sig_blocked;
	uint64_t sig_saved_mask;	/* mask to restore after a temporary one */
	int sig_restore_mask;		/* sig_saved_mask is to be put back */
	uint64_t sigalt_sp, sigalt_size;
	uint32_t sigalt_flags;

	/* Threads (see thread.c). */
	struct rv_cpu *next_thread;
	int32_t tid;			/* the host's id for this thread, which is the guest's too */
	/* The system call filters that apply to this thread (seccomp.c), newest first; and the old strict mode. */
	struct rv_filter *seccomp;
	int seccomp_strict;
	int parked;
	int started;			/* a new thread: it may run (its creator has finished with it) */
	uint64_t clear_child_tid;	/* where the thread's exit is announced, or 0 */

	/* The system call being made, for restarting it after a signal. */
	uint64_t sys_pc, sys_a0;
	int sys_interrupted;
	int pc_set;			/* a system call set pc itself */

	enum rv_trap trap;
	uint64_t trap_addr;	/* faulting address or pc */
	uint32_t trap_insn;
	int exit_code;

	uint64_t insn_count;
	uint64_t steps_left;	/* for rv_interp_steps() */
	int undo_on;		/* the JIT's self-check wants the interpreter's writes noted (jit.c) */
	FILE *trace;		/* per-instruction register trace, or NULL */
	int verbose;
	int strace;		/* log every system call */
	int fork_snapshot;	/* fork by starting a second emulator (see fork.c) */
	const char *exe_path;	/* absolute path of the guest program */

	/* Target of faulting accesses, so a fault needs no unwinding. */
	uint8_t scratch[16];
};

/* interp.c */
void rv_run(struct rv_cpu *cpu);
void rv_interp_block(struct rv_cpu *cpu);
void rv_interp_steps(struct rv_cpu *cpu, uint64_t count);

/* jit.c */
int rv_jit_init(struct rv_cpu *cpu);
void rv_jit_run(struct rv_cpu *cpu);
void rv_jit_flush(struct rv_cpu *cpu);
void rv_jit_code_changed(struct rv_cpu *cpu, uint64_t first, uint64_t count, enum rv_code_cause cause);
void rv_jit_report(struct rv_cpu *cpu);
void rv_jit_report_threads(struct rv_cpu *cpu);
void rv_jit_report_thread(struct rv_cpu *t);
void rv_jit_count_fp(struct rv_cpu *cpu, uint32_t insn);
void rv_jit_free(struct rv_cpu *cpu);
void rv_jit_after_fork(void);
void rv_undo_note(struct rv_cpu *cpu, uint64_t addr, uint64_t len);

/* bitmanip.c: Zba, Zbb, Zbs */
enum {
	RV_ZB_NONE, RV_ZB_ADD_UW, RV_ZB_SH1ADD, RV_ZB_SH2ADD, RV_ZB_SH3ADD,
	RV_ZB_SH1ADD_UW, RV_ZB_SH2ADD_UW, RV_ZB_SH3ADD_UW, RV_ZB_SLLI_UW,
	RV_ZB_ANDN, RV_ZB_ORN, RV_ZB_XNOR, RV_ZB_CLZ, RV_ZB_CLZW, RV_ZB_CTZ, RV_ZB_CTZW,
	RV_ZB_CPOP, RV_ZB_CPOPW, RV_ZB_MAX, RV_ZB_MAXU, RV_ZB_MIN, RV_ZB_MINU,
	RV_ZB_SEXT_B, RV_ZB_SEXT_H, RV_ZB_ZEXT_H, RV_ZB_ROL, RV_ZB_ROLW, RV_ZB_ROR, RV_ZB_RORW,
	RV_ZB_RORI, RV_ZB_RORIW, RV_ZB_ORC_B, RV_ZB_REV8,
	RV_ZB_BCLR, RV_ZB_BCLRI, RV_ZB_BEXT, RV_ZB_BEXTI, RV_ZB_BINV, RV_ZB_BINVI, RV_ZB_BSET, RV_ZB_BSETI,
	RV_ZB_KINDS
};
extern const char *const rv_zb_names[RV_ZB_KINDS];
extern uint64_t rv_zb_count[RV_ZB_KINDS];
extern int rv_zb_counting;
int rv_zb_kind(uint32_t insn);
uint64_t rv_zb_exec(int kind, uint32_t insn, uint64_t a, uint64_t b);
int rv_zb_advertised(void);
int rv_zb_count_on(void);
void rv_zb_report(void);

/* compressed.c */
uint32_t rv_expand_compressed(uint16_t insn);

/* fp.c */
int rv_exec_fp(struct rv_cpu *cpu, uint32_t insn);
int rv_csr(struct rv_cpu *cpu, uint32_t insn);
int rv_exec_vector(struct rv_cpu *cpu, uint32_t insn);
int rv_fp_int_source(uint32_t insn);
int rv_fp_int_dest(uint32_t insn);
uint64_t rv_jit_fp(struct rv_cpu *cpu, uint64_t insn, uint64_t a);
uint64_t rv_jit_csr(struct rv_cpu *cpu, uint64_t insn, uint64_t a);
uint64_t rv_jit_fused64(uint64_t a, uint64_t b, uint64_t c);

/* elf.c */
int rv_load_program(struct rv_cpu *cpu, const char *path, uint64_t stack_size);
int rv_setup_stack(struct rv_cpu *cpu, int argc, char **argv, char **envp);

/* syscall.c */
void rv_syscall(struct rv_cpu *cpu);

/* virtgpu.c: an emulated virtio-gpu render node (RV_VIRTGPU) */
struct stat;
int rv_virtgpu_enabled(void);
const char *rv_virtgpu_path(const char *path);
long rv_virtgpu_open(long fd, int cloexec);
void rv_virtgpu_close(int fd);
void rv_virtgpu_fd(int fd);
void rv_virtgpu_fix_stat(struct stat *st);
void rv_virtgpu_fix_statx(void *statx);
int rv_virtgpu_ioctl(struct rv_cpu *cpu, int fd, uint64_t req, uint64_t arg, uint64_t *ret);
int rv_virtgpu_mmap(int *fd, uint64_t *offset);

/* fork.c */
int64_t rv_fork_snapshot(struct rv_cpu *cpu, uint64_t child_stack, uint64_t set_tid_addr,
			 uint64_t clear_tid_addr, uint64_t namespaces, int apart);
#define RV_CLONE_NEWPID	0x20000000ULL
#define RV_CLONE_NEW	(0x00020000ULL | 0x02000000ULL | 0x04000000ULL | 0x08000000ULL | 0x10000000ULL | RV_CLONE_NEWPID | 0x40000000ULL | 0x80ULL)
int rv_namespace_children(uint64_t namespaces, int *restore);
void rv_namespace_restore(int restore);
int rv_fork_resume(struct rv_cpu *cpu, struct rv_proc *proc, const char *addr, int done_fd, int close_fd);

/* main.c */
int rv_finish(struct rv_cpu *cpu);

/* identity.c */
void rv_args_init(int argc, char **argv);
char *rv_args_area(size_t *size);
const char *rv_args_shown(size_t *len);
void rv_args_show(const char *args, size_t len);
int rv_cpuinfo(int fd);
int rv_cpuinfo_for(int fd, const char *online);
long rv_exe_of(const char *exe_path, const char *sysroot, int dirfd, const char *path, char *out, size_t size);

/* thread.c */
#define RV_SIG_KICK	64	/* the host signal that gets a thread out of a system call it waits in */
/* Not a signal (SIGKILL is never pending for a handler): the thread is to park. */
#define RV_SIG_STOP	(1ULL << (9 - 1))
/* Nor is this one (SIGSTOP is not either): code changed, the thread is to leave translated code and look (jit.c). */
#define RV_SIG_CODE	(1ULL << (19 - 1))
#define RV_SIG_INTERNAL	(RV_SIG_STOP | RV_SIG_CODE)
extern __thread struct rv_cpu *rv_self;
void rv_thread_first(struct rv_cpu *cpu, struct rv_proc *proc);
void rv_lock(struct rv_cpu *cpu);
void rv_stack_init(uintptr_t low, uintptr_t top);
void rv_stack_note(const char *where);
void rv_stack_report(void);
void rv_lock_other(struct rv_cpu *cpu, pthread_mutex_t *lock);
void rv_unlock(struct rv_cpu *cpu);
void rv_stop_world(struct rv_cpu *cpu);
void rv_start_world(struct rv_cpu *cpu);
void rv_safepoint(struct rv_cpu *cpu);
uint64_t rv_thread_clone(struct rv_cpu *cpu, uint64_t flags, uint64_t stack, uint64_t ptid,
			 uint64_t tls, uint64_t ctid);
int rv_thread_exit(struct rv_cpu *cpu);
uint64_t rv_thread_futex(struct rv_cpu *cpu, uint64_t addr, uint64_t op, uint64_t val,
			 uint64_t timeout, uint64_t addr2, uint64_t val3);
int rv_thread_kill(struct rv_cpu *cpu, int64_t tid, int sig, uint64_t *ret);
void rv_thread_after_fork(struct rv_cpu *cpu);

/* signal.c */
void rv_signal_init(void);
void rv_signal_host_set(uint64_t mask, sigset_t *set);
void rv_signal_inherit_mask(struct rv_cpu *cpu);
void rv_signal_deliver(struct rv_cpu *cpu);
int rv_signal_fault(struct rv_cpu *cpu);
void rv_signal_poll(void);
void rv_signal_die(int sig);
void rv_signal_reinstall(struct rv_cpu *cpu);
void rv_signal_sync_mask(struct rv_cpu *cpu);
void rv_signal_temp_mask(struct rv_cpu *cpu, uint64_t temp_mask);
uint64_t rv_sys_rt_sigaction(struct rv_cpu *cpu, uint64_t sig, uint64_t act_addr,
			     uint64_t old_addr, uint64_t size);
uint64_t rv_sys_rt_sigprocmask(struct rv_cpu *cpu, uint64_t how, uint64_t set_addr,
			       uint64_t old_addr, uint64_t size);
uint64_t rv_sys_rt_sigpending(struct rv_cpu *cpu, uint64_t addr, uint64_t size);
uint64_t rv_sys_rt_sigsuspend(struct rv_cpu *cpu, uint64_t mask_addr, uint64_t size);
uint64_t rv_sys_sigaltstack(struct rv_cpu *cpu, uint64_t ss_addr, uint64_t old_addr);
void rv_sys_rt_sigreturn(struct rv_cpu *cpu);
uint64_t rv_sys_kill(int64_t pid, int sig);

/* mm.c */
int rv_mm_init(struct rv_cpu *cpu, uint64_t size);
int rv_mm_reserve(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot);
int rv_mm_fill(struct rv_cpu *cpu, uint64_t addr, uint64_t len, const void *src, int fd, uint64_t offset);
int rv_mm_fork_child(struct rv_cpu *cpu, int (*copy)(void *dst, const void *src, size_t len), int apart);
void rv_mm_exit(struct rv_cpu *cpu);

/* seccomp.c: what a filter can answer (the kernel's SECCOMP_RET_ values), and one answer of our own. */
#define RV_SECCOMP_KILL_PROCESS	0x80000000U
#define RV_SECCOMP_KILL_THREAD	0x00000000U
#define RV_SECCOMP_TRAP		0x00030000U
#define RV_SECCOMP_ERRNO	0x00050000U
#define RV_SECCOMP_USER_NOTIF	0x7fc00000U
#define RV_SECCOMP_TRACE	0x7ff00000U
#define RV_SECCOMP_LOG		0x7ffc0000U
#define RV_SECCOMP_ALLOW	0x7fff0000U
#define RV_SECCOMP_KILL_STRICT	0x80010000U	/* strict mode: the process dies of SIGKILL */
uint32_t rv_seccomp_check(struct rv_cpu *cpu, uint64_t nr, const uint64_t *args);
uint64_t rv_seccomp(struct rv_cpu *cpu, uint64_t op, uint64_t flags, uint64_t arg);
uint64_t rv_seccomp_prctl(struct rv_cpu *cpu, int set, uint64_t mode, uint64_t arg);
int rv_seccomp_fork_child(struct rv_cpu *cpu, int (*copy)(void *dst, const void *src, size_t len));
void *rv_seccomp_save(struct rv_cpu *cpu, size_t *size);
int rv_seccomp_load(struct rv_cpu *cpu);
int rv_signal_sys(struct rv_cpu *cpu, uint64_t nr, uint32_t data);
void rv_mm_die(void);
void rv_mm_after_fork(struct rv_cpu *cpu, int apart);
void rv_mm_name(struct rv_cpu *cpu, uint64_t addr, uint64_t len, int fd, uint64_t offset);
void rv_mm_file_written(struct rv_cpu *cpu, int fd, uint64_t len, uint64_t offset);
int rv_mm_maps(struct rv_cpu *cpu, int fd);
int rv_mm_file_at(struct rv_cpu *cpu, uint64_t addr, uint64_t *dev, uint64_t *ino, uint64_t *offset, int *fixed);
uint64_t rv_mm_msync(struct rv_cpu *cpu, uint64_t addr, uint64_t len);
uint64_t rv_mm_madvise(struct rv_cpu *cpu, uint64_t addr, uint64_t len, int advice);
uint64_t rv_mm_flush_icache(struct rv_cpu *cpu, uint64_t start, uint64_t end);
uint64_t rv_mm_find_free(struct rv_cpu *cpu, uint64_t len);
uint64_t rv_reserve_xlat(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot);
uint64_t rv_jit_reserve(struct rv_cpu *cpu, uint64_t addr, uint64_t how);
int rv_mm_grow_to(struct rv_cpu *cpu, uint64_t bytes);
uint64_t rv_mm_mmap(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot,
		    uint64_t flags, int fd, uint64_t offset);
uint64_t rv_mm_munmap(struct rv_cpu *cpu, uint64_t addr, uint64_t len);
uint64_t rv_mm_mprotect(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot);
uint64_t rv_mm_mremap(struct rv_cpu *cpu, uint64_t old_addr, uint64_t old_len,
		      uint64_t new_len, uint64_t flags);
uint64_t rv_mm_brk(struct rv_cpu *cpu, uint64_t addr);
uint8_t *rv_fault(struct rv_cpu *cpu, uint64_t addr);
uint8_t *rv_ptr_slow(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot);
void rv_store_split(struct rv_cpu *cpu, uint64_t addr, uint64_t len);
uint8_t *rv_host_addr(struct rv_cpu *cpu, uint64_t addr);
void *rv_user_ptr(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot);
struct iovec;
int rv_user_iov(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot,
		struct iovec *iov, int max, uint64_t *covered);
void rv_user_done(struct rv_cpu *cpu);
uint64_t rv_user_strlen(struct rv_cpu *cpu, uint64_t addr);

/*
 * Translate a guest access of at most 16 bytes to a host pointer, checking
 * the page's protection: one look into the page table for an access that
 * lies in one page and that the page allows as it stands. Everything else is
 * rv_ptr_slow()'s (mm.c): a page that has no memory yet gets some; an access
 * across two pages is put together; and a failed access records a SEGV trap
 * and returns scratch space so that the caller can finish the instruction;
 * the run loop stops before the next one. (No setjmp/longjmp: Linux/Wasm
 * does not support them.)
 *
 * A caller that stores through the pointer has to look whether it got
 * cpu->scratch, and call rv_store_split() if so: STORE() in interp.c does.
 */
static inline uint8_t *rv_ptr(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot)
{
	uint64_t page = addr >> RV_PAGE_SHIFT;

	if (__builtin_expect(page < cpu->npages && ((addr + len - 1) >> RV_PAGE_SHIFT) == page, 1)) {
		uint64_t e = cpu->map[page];

		if (__builtin_expect((e & prot) == prot, 1)) {
			/* The JIT's self-check (jit.c) wants to take back what the interpreter writes. */
			if ((prot & RV_PROT_WRITE) && __builtin_expect(cpu->undo_on, 0))
				rv_undo_note(cpu, addr, len);
			return (uint8_t *)(uintptr_t)(addr + (e & ~(uint64_t)RV_PAGE_FLAGS));
		}
	}
	return rv_ptr_slow(cpu, addr, len, prot);
}

/*
 * A system call of the host's by its number. Every argument is passed as a
 * long: syscall() takes them through `...`, where on wasm64 an int stays 32
 * bits wide, and the C library reads 64 for each.
 */
#define rv_host_syscall(nr, a, b, c, d, e, f) \
	syscall(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

/* Is there a signal this thread would take now? */
static inline int rv_signal_ready(const struct rv_cpu *cpu)
{
	return (cpu->sig_pending & ~RV_SIG_INTERNAL & ~cpu->sig_blocked) != 0;
}

#endif /* RV_H */
