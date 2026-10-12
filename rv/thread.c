/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Guest threads.
 *
 * A thread of the guest is a thread of the host, running its own copy of the
 * run loop on its own struct rv_cpu: the threads of a guest run at the same
 * time, on as many cores as the host gives them. (On Linux/Wasm a thread is
 * a Web Worker with its own instance of this program; the memory, and so the
 * guest's memory and everything static in here, is the same in all.)
 *
 * What that takes:
 *
 *   The guest's memory is the same memory in every thread, so its atomic
 *   instructions are the host's (interp.c), and a futex of the guest is a
 *   futex of the host at the same place: FUTEX_WAIT and FUTEX_WAKE go to the
 *   host kernel as they are. Thread ids are the host's.
 *
 *   A system call that waits simply waits; only its thread does.
 *
 *   Each thread has its own translated code (jit.c): on Linux/Wasm code
 *   lives in a function table, and every instance has its own.
 *
 *   What the threads share besides memory (struct rv_proc: the page map, the
 *   layout of the address space, signal dispositions, the list of threads)
 *   is guarded by one lock, rv_lock().
 *
 * Stopping the world. Two things cannot be done while other threads run:
 * moving the arena when it has to grow (mm.c), since every thread computes
 * with addresses inside it, and taking the copy of the guest that a fork
 * needs. For those a thread holding the lock calls rv_stop_world(), which
 * returns when every other thread is parked: stopped between two blocks of
 * guest code, or waiting for the lock, in either case holding no address
 * inside the arena. A thread learns that it is to park from a bit in its
 * sig_pending, which the run loops look at anyway; one that is waiting in a
 * system call is got out of it with a signal reserved for this
 * (RV_SIG_KICK), after which it finds nothing to deliver, parks, and makes
 * the call again (see the end of rv_syscall()). A thread may park wherever
 * it holds no address inside the arena; a function that takes the lock or
 * calls rv_safepoint() translates the guest's addresses again afterwards.
 *
 * The kick can come too early: on a host that runs handlers at any time, it
 * may arrive after the thread last looked and before it is inside the
 * system call, and then the call waits after all. So the thread stopping the
 * world kicks again every few milliseconds until all are parked. (On
 * Linux/Wasm a handler only runs when a system call returns, and the bit is
 * looked at before every system call, so there the first kick is enough.)
 *
 * Signals: every thread has the guest's mask for it as its host mask, so the
 * host kernel picks the thread for a signal sent to the process. A signal
 * sent to one thread by another goes through the host too, with two
 * exceptions that never leave here: the numbers the host's C library keeps
 * for itself, which the guest's C library uses for cancelling threads; and
 * the kick itself. For those the bit is set in the thread's sig_pending and
 * the thread is kicked.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "rv.h"

#define RV_ETIMEDOUT	110

#define RV_CLONE_VM		0x00000100
#define RV_CLONE_FS		0x00000200
#define RV_CLONE_FILES		0x00000400
#define RV_CLONE_SIGHAND	0x00000800
#define RV_CLONE_THREAD		0x00010000
#define RV_CLONE_SETTLS		0x00080000
#define RV_CLONE_PARENT_SETTID	0x00100000
#define RV_CLONE_CHILD_CLEARTID	0x00200000
#define RV_CLONE_CHILD_SETTID	0x01000000

#define RV_FUTEX_WAIT		0
#define RV_FUTEX_WAKE		1
#define RV_FUTEX_REQUEUE	3
#define RV_FUTEX_CMP_REQUEUE	4
#define RV_FUTEX_WAKE_OP	5
#define RV_FUTEX_WAIT_BITSET	9
#define RV_FUTEX_WAKE_BITSET	10
#define RV_FUTEX_CMD_MASK	0x7f
#define RV_FUTEX_PRIVATE	128

#define RV_SIGKILL	9
#define RV_SIGSTOP	19

/* The emulator's own stack in a thread: the run loop and a system call need little. */
#define THREAD_STACK	(256 << 10)

#define BIT(sig)	(1ULL << ((sig) - 1))

/* The cpu of the thread that is running: for the host's signal handler (signal.c). */
__thread struct rv_cpu *rv_self;

static uint64_t neg(int err)
{
	return (uint64_t)-(int64_t)err;
}

static int32_t host_tid(void)
{
	return (int32_t)rv_host_syscall(SYS_gettid, 0, 0, 0, 0, 0, 0);
}

/* The first thread of a process: the one that loaded the program, or the one a fork left. */
void rv_thread_first(struct rv_cpu *cpu, struct rv_proc *proc)
{
	pthread_mutexattr_t attr;

	/* The lock can be taken again by the thread that has it: giving a page memory takes it, wherever that comes up. */
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&proc->lock, &attr);
	pthread_mutexattr_destroy(&attr);
	pthread_mutex_init(&proc->park_lock, NULL);
	pthread_cond_init(&proc->park_cond, NULL);
	proc->threads = cpu;
	proc->nthreads = 1;
	proc->stop = proc->parked = 0;
	cpu->proc = proc;
	cpu->next_thread = NULL;
	cpu->tid = host_tid();
	cpu->parked = 0;
	cpu->started = 1;
	rv_self = cpu;
}

/* After the host forked, in the child: only the thread that called fork exists there. */
void rv_thread_after_fork(struct rv_cpu *cpu)
{
	cpu->sig_pending = cpu->sig_tkill = cpu->sig_held = 0;
	rv_thread_first(cpu, cpu->proc);
}

static void set_parked(struct rv_cpu *cpu, int parked)
{
	struct rv_proc *proc = cpu->proc;

	pthread_mutex_lock(&proc->park_lock);
	if (cpu->parked != parked) {
		cpu->parked = parked;
		proc->parked += parked ? 1 : -1;
		if (parked)
			pthread_cond_broadcast(&proc->park_cond);
	}
	pthread_mutex_unlock(&proc->park_lock);
}

/*
 * Take the lock on what the threads share. The caller holds no address
 * inside the arena: while it waits it counts as parked, and the arena may
 * have moved by the time it has the lock. From then on the arena stays.
 */
void rv_lock(struct rv_cpu *cpu)
{
	struct rv_proc *proc = cpu->proc;

	if (proc->nthreads <= 1) {	/* (nobody else to make it more) */
		pthread_mutex_lock(&proc->lock);
		return;
	}
	set_parked(cpu, 1);
	pthread_mutex_lock(&proc->lock);
	set_parked(cpu, 0);
}

void rv_unlock(struct rv_cpu *cpu)
{
	pthread_mutex_unlock(&cpu->proc->lock);
}

/*
 * Take a lock of the emulator's own other than the one on what the threads
 * share: the JIT's buffers (jit.c). A thread that has such a lock may wait
 * for the shared one in the middle of what it does with it (the JIT's log
 * asks which file the code is from), and then counts as parked; if the
 * world is being stopped meanwhile, a thread that waits here for that thread
 * would hold the stop up for good, neither parked nor able to park, while
 * the thread it waits for cannot go on before the world does. So whoever
 * waits here counts as parked as well, and once it has the lock parks for as
 * long as the world stays stopped. As with rv_lock(), the caller holds no
 * address inside the arena.
 */
void rv_lock_other(struct rv_cpu *cpu, pthread_mutex_t *lock)
{
	if (cpu->proc->nthreads <= 1) {
		pthread_mutex_lock(lock);
		return;
	}
	if (!pthread_mutex_trylock(lock))
		return;
	set_parked(cpu, 1);
	pthread_mutex_lock(lock);
	set_parked(cpu, 0);
	rv_safepoint(cpu);
}

/* With the lock held: returns when every other thread is parked. They stay so until rv_start_world(). */
void rv_stop_world(struct rv_cpu *cpu)
{
	struct rv_proc *proc = cpu->proc;
	pid_t pid = getpid();

	if (proc->nthreads <= 1)
		return;
	pthread_mutex_lock(&proc->park_lock);
	proc->stop = 1;
	for (struct rv_cpu *t = proc->threads; t; t = t->next_thread)
		if (t != cpu)
			__atomic_or_fetch(&t->sig_pending, RV_SIG_STOP, __ATOMIC_SEQ_CST);
	int kicked = 0, told = 0;
	struct timespec began;

	clock_gettime(CLOCK_MONOTONIC, &began);

	while (proc->parked < proc->nthreads - 1) {
		struct timespec until;

		/*
		 * A stop takes milliseconds, or a few seconds when a thread waits
		 * for a CPU. One that has not ended after 5 s may never end: say
		 * which threads hold it up, and where (once).
		 */
		clock_gettime(CLOCK_MONOTONIC, &until);
		if (!told && until.tv_sec - began.tv_sec >= 5 && (told = 1))
			for (struct rv_cpu *t = proc->threads; t; t = t->next_thread)
				if (t != cpu && !t->parked)
					fprintf(stderr, "rv[%d]: thread %d stops the world, and thread %d has not parked "
						"after 5 s (pc 0x%llx, last system call at 0x%llx)\n", (int)pid, cpu->tid,
						t->tid, (unsigned long long)t->pc, (unsigned long long)t->sys_pc);

		/*
		 * On Linux/Wasm the first kick is enough (see above), and a second
		 * one is not harmless: it is delivered as another handler to a
		 * thread that has not returned from the first, which happens to
		 * one on a starved CPU, and every 2 ms another, until the
		 * Worker's stack is gone.
		 */
#ifndef __wasm__
		kicked = 0;
#endif
		if (!kicked)
			for (struct rv_cpu *t = proc->threads; t; t = t->next_thread)
				if (t != cpu && !t->parked)
					rv_host_syscall(SYS_tgkill, pid, t->tid, RV_SIG_KICK, 0, 0, 0);
		kicked = 1;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 2000000;
		if (until.tv_nsec >= 1000000000) {
			until.tv_nsec -= 1000000000;
			until.tv_sec++;
		}
		pthread_cond_timedwait(&proc->park_cond, &proc->park_lock, &until);
	}
	pthread_mutex_unlock(&proc->park_lock);
}

void rv_start_world(struct rv_cpu *cpu)
{
	struct rv_proc *proc = cpu->proc;

	pthread_mutex_lock(&proc->park_lock);
	if (proc->stop) {
		proc->stop = 0;
		for (struct rv_cpu *t = proc->threads; t; t = t->next_thread)
			__atomic_and_fetch(&t->sig_pending, ~RV_SIG_STOP, __ATOMIC_SEQ_CST);
		pthread_cond_broadcast(&proc->park_cond);
	}
	pthread_mutex_unlock(&proc->park_lock);
}

/* Park here for as long as the world is stopped. The arena may have moved afterwards. */
void rv_safepoint(struct rv_cpu *cpu)
{
	struct rv_proc *proc = cpu->proc;

	if (!(cpu->sig_pending & RV_SIG_STOP))
		return;
	pthread_mutex_lock(&proc->park_lock);
	while (proc->stop) {
		if (!cpu->parked) {
			cpu->parked = 1;
			proc->parked++;
			pthread_cond_broadcast(&proc->park_cond);
		}
		pthread_cond_wait(&proc->park_cond, &proc->park_lock);
	}
	if (cpu->parked) {
		cpu->parked = 0;
		proc->parked--;
	}
	pthread_mutex_unlock(&proc->park_lock);
}

/*
 * The emulator's own stack. On Linux/Wasm a program's C stack is memory like
 * any other, with nothing below it that stops a frame from going past its
 * end: the main thread gets what binfmt_wasm gives every program (256 KiB),
 * a thread what THREAD_STACK asks for, and a frame below that writes over
 * whatever is there, which, all processes being in one memory, may be
 * another program's or the kernel's. So the lowest 4 KiB of each are filled
 * with a known word when the thread starts, and looked at where the stack is
 * deepest (translating, a system call, a signal's handler): if they are no
 * longer that word, or a frame is in them, the process says so and ends at
 * once rather than go on writing over others. RV_STACK=1: every thread says
 * how deep its stack went, when it ends.
 */
#define STACK_GUARD	4096
#define STACK_WORD	0x6b636174735f7672ULL	/* "rv_stack" */
static __thread uintptr_t stack_low, stack_top, stack_deepest;
static int stack_say = -1;

void rv_stack_init(uintptr_t low, uintptr_t top)
{
	stack_low = low;
	stack_top = top;
	stack_deepest = (uintptr_t)__builtin_frame_address(0);
	for (uintptr_t at = low; at < low + STACK_GUARD; at += 8)
		*(volatile uint64_t *)at = STACK_WORD;
	if (stack_say < 0)
		stack_say = getenv("RV_STACK") && atoi(getenv("RV_STACK"));
}

void rv_stack_note(const char *where)
{
	uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
	int broken = 0;

	if (!stack_top)
		return;
	if (sp < stack_deepest)
		stack_deepest = sp;
	for (uintptr_t at = stack_low; at < stack_low + STACK_GUARD; at += 8)
		broken |= *(volatile uint64_t *)at != STACK_WORD;
	if (broken || sp < stack_low + STACK_GUARD) {
		char line[160];
		int n = snprintf(line, sizeof(line), "rv: the emulator's stack ran out (%s, %lu bytes deep of %lu)\n", where,
				 (unsigned long)(stack_top - sp), (unsigned long)(stack_top - stack_low));

		if (n > 0)
			(void)!write(2, line, (size_t)n);
		abort();
	}
}

void rv_stack_report(void)
{
	if (stack_say > 0 && stack_top)
		fprintf(stderr, "rv: stack: thread %d went %lu bytes deep of %lu\n", (int)host_tid(),
			(unsigned long)(stack_top - stack_deepest), (unsigned long)(stack_top - stack_low));
}

/* What a new host thread does: run the guest's thread to its end. */
static void *thread_main(void *arg)
{
	struct rv_cpu *cpu = arg;
	struct rv_proc *proc = cpu->proc;

	rv_self = cpu;
	{
		pthread_attr_t attr;
		void *addr;
		size_t size;

		if (!pthread_getattr_np(pthread_self(), &attr)) {
			if (!pthread_attr_getstack(&attr, &addr, &size))
				rv_stack_init((uintptr_t)addr, (uintptr_t)addr + size);
			pthread_attr_destroy(&attr);
		}
	}
	/* Tell the creator who we are, and wait until it has told the guest. */
	pthread_mutex_lock(&proc->park_lock);
	cpu->tid = host_tid();
	pthread_cond_broadcast(&proc->park_cond);
	while (!cpu->started)
		pthread_cond_wait(&proc->park_cond, &proc->park_lock);
	pthread_mutex_unlock(&proc->park_lock);
	/* The creator had every signal blocked for us; now the mask is the guest's. */
	rv_signal_sync_mask(cpu);

	rv_run(cpu);

	/* The guest ended in this thread (exit_group, or a fault): the process ends. */
	rv_stack_report();
	if (cpu->trap != RV_TRAP_THREAD)
		exit(rv_finish(cpu));
	/* Only the thread did: rv_thread_exit() has taken it off the list. */
	if (cpu->verbose)
		rv_jit_report_thread(cpu);
	rv_jit_free(cpu);
	free(cpu->bounces);
	free(cpu);
	return NULL;
}

/* clone() with CLONE_THREAD: a new thread, which starts as a copy of the caller. */
uint64_t rv_thread_clone(struct rv_cpu *cpu, uint64_t flags, uint64_t stack, uint64_t ptid,
			 uint64_t tls, uint64_t ctid)
{
	uint64_t need = RV_CLONE_VM | RV_CLONE_FS | RV_CLONE_FILES | RV_CLONE_SIGHAND | RV_CLONE_THREAD;
	struct rv_proc *proc = cpu->proc;
	struct rv_cpu *t;
	pthread_attr_t attr;
	pthread_t host;
	sigset_t all, old;
	int32_t tid;
	void *p;
	int err;

	if ((flags & need) != need)
		return neg(RV_EINVAL);
	if (!(t = malloc(sizeof(*t))))
		return neg(RV_ENOMEM);

	rv_lock(cpu);
	if ((flags & RV_CLONE_PARENT_SETTID) && !rv_user_ptr(cpu, ptid, sizeof(tid), RV_PROT_WRITE)) {
		rv_unlock(cpu);
		free(t);
		return neg(RV_EFAULT);
	}

	/* It returns 0 from the same clone() call, on its own stack. */
	memcpy(t, cpu, sizeof(*t));
	t->x[10] = 0;
	t->pc = cpu->pc + 4;
	if (stack)
		t->x[2] = stack;
	if (flags & RV_CLONE_SETTLS)
		t->x[4] = tls;
	t->lr_valid = 0;
	t->sigalt_sp = t->sigalt_size = 0;
	t->sig_restore_mask = 0;
	t->sys_interrupted = 0;
	t->sig_pending = t->sig_tkill = t->sig_held = 0;
	t->clear_child_tid = (flags & RV_CLONE_CHILD_CLEARTID) ? ctid : 0;
	t->jit_wanted = cpu->jit || cpu->jit_wanted;
	t->jit = NULL;
	t->jit_used = NULL;	/* (the creator's table, which goes when the creator does) */
	/* (Not the creator's copies of system call buffers: those are its own to give back.) */
	t->bounces = NULL;
	t->nbounces = t->bounces_cap = 0;
	t->undo_on = 0;
	t->insn_count = 0;
	t->tid = 0;
	t->parked = t->started = 0;
	t->next_thread = proc->threads;
	proc->threads = t;
	proc->nthreads++;

	/* No signal for the new thread before it knows which cpu is its own. */
	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &old);
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, THREAD_STACK);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	err = pthread_create(&host, &attr, thread_main, t);
	pthread_attr_destroy(&attr);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	if (err) {
		proc->threads = t->next_thread;
		proc->nthreads--;
		rv_unlock(cpu);
		free(t);
		return neg(RV_EAGAIN);
	}

	/* Its id is the host's, which only the new thread can say. */
	pthread_mutex_lock(&proc->park_lock);
	while (!t->tid)
		pthread_cond_wait(&proc->park_cond, &proc->park_lock);
	tid = t->tid;
	if ((flags & RV_CLONE_PARENT_SETTID) && (p = rv_user_ptr(cpu, ptid, sizeof(tid), RV_PROT_WRITE)))
		memcpy(p, &tid, sizeof(tid));
	if ((flags & RV_CLONE_CHILD_SETTID) && (p = rv_user_ptr(cpu, ctid, sizeof(tid), RV_PROT_WRITE)))
		memcpy(p, &tid, sizeof(tid));
	t->started = 1;
	pthread_cond_broadcast(&proc->park_cond);
	pthread_mutex_unlock(&proc->park_lock);

	rv_unlock(cpu);
	return (uint64_t)tid;
}

/*
 * exit() of one thread. Returns 0 if it is the only one, in which case the
 * process ends the usual way; otherwise the thread is off the list and its
 * run loop ends (RV_TRAP_THREAD).
 */
int rv_thread_exit(struct rv_cpu *cpu)
{
	struct rv_proc *proc = cpu->proc;
	uint32_t *word;

	rv_lock(cpu);
	if (proc->nthreads <= 1) {
		rv_unlock(cpu);
		return 0;
	}
	for (struct rv_cpu **link = &proc->threads; *link; link = &(*link)->next_thread) {
		if (*link == cpu) {
			*link = cpu->next_thread;
			break;
		}
	}
	proc->nthreads--;
	/* What pthread_join() waits for. */
	if (cpu->clear_child_tid && !(cpu->clear_child_tid & 3) &&
	    (word = rv_user_ptr(cpu, cpu->clear_child_tid, 4, RV_PROT_WRITE))) {
		__atomic_store_n(word, 0, __ATOMIC_SEQ_CST);
		rv_host_syscall(SYS_futex, word, RV_FUTEX_WAKE | RV_FUTEX_PRIVATE, 1, 0, 0, 0);
	}
	rv_unlock(cpu);
	cpu->trap = RV_TRAP_THREAD;
	return 1;
}

/*
 * futex(): the host's, on the same word. Always as a private one: the
 * guest's memory is not shared with any other process.
 */
uint64_t rv_thread_futex(struct rv_cpu *cpu, uint64_t addr, uint64_t op, uint64_t val,
			 uint64_t timeout, uint64_t addr2, uint64_t val3)
{
	unsigned cmd = (unsigned)op & RV_FUTEX_CMD_MASK;
	uint32_t *word = rv_user_ptr(cpu, addr, 4, RV_PROT_READ), *word2;
	int host_op = (int)op | RV_FUTEX_PRIVATE;
	void *ts = NULL;
	long ret;

	if (!word || (addr & 3))
		return neg(word ? RV_EINVAL : RV_EFAULT);

	switch (cmd) {
	case RV_FUTEX_WAIT:
	case RV_FUTEX_WAIT_BITSET:
		/* (struct timespec is two 64-bit numbers for the guest and for the host.) */
		if (timeout && !(ts = rv_user_ptr(cpu, timeout, 16, RV_PROT_READ)))
			return neg(RV_EFAULT);
		ret = rv_host_syscall(SYS_futex, word, host_op, (uint32_t)val, ts, 0, (uint32_t)val3);
		break;
	case RV_FUTEX_WAKE:
	case RV_FUTEX_WAKE_BITSET:
		ret = rv_host_syscall(SYS_futex, word, host_op, (uint32_t)val, 0, 0, (uint32_t)val3);
		break;
	case RV_FUTEX_REQUEUE:
	case RV_FUTEX_CMP_REQUEUE:
	case RV_FUTEX_WAKE_OP:
		if ((addr2 & 3) || !(word2 = rv_user_ptr(cpu, addr2, 4, cmd == RV_FUTEX_WAKE_OP
							 ? RV_PROT_READ | RV_PROT_WRITE : RV_PROT_READ)))
			return neg(RV_EFAULT);
		/* (`timeout` is a count here.) */
		ret = rv_host_syscall(SYS_futex, word, host_op, (uint32_t)val, (uint32_t)timeout, word2, (uint32_t)val3);
		break;
	default:
		return neg(RV_ENOSYS);
	}
	return ret < 0 ? neg(errno) : (uint64_t)ret;
}

/* Can the host carry this signal from one thread to another? Not the ones its C library keeps, nor the kick. */
static int host_carries(int sig)
{
	return !(sig >= 32 && sig <= 34) && sig != RV_SIG_KICK;
}

/*
 * tkill/tgkill. Returns 1 and sets *ret if tid is one of our threads; 0 if
 * the signal is for another process (or one that ends this one whatever its
 * threads do), which the host sees to.
 */
int rv_thread_kill(struct rv_cpu *cpu, int64_t tid, int sig, uint64_t *ret)
{
	struct rv_proc *proc = cpu->proc;
	struct rv_cpu *t;

	if (sig == RV_SIGKILL || sig == RV_SIGSTOP)
		return 0;
	rv_lock(cpu);
	for (t = proc->threads; t; t = t->next_thread)
		if (t->tid == tid)
			break;
	if (!t) {
		rv_unlock(cpu);
		return 0;
	}
	if (sig < 0 || sig > RV_NSIG) {
		*ret = neg(RV_EINVAL);
	} else if (!sig) {
		*ret = 0;
	} else {
		/* Sent by this process to one of its threads: the handler is told so (signal.c). */
		__atomic_or_fetch(&t->sig_tkill, BIT(sig), __ATOMIC_SEQ_CST);
		if (host_carries(sig)) {
			*ret = rv_host_syscall(SYS_tgkill, getpid(), t->tid, sig, 0, 0, 0) < 0 ? neg(errno) : 0;
		} else {
			__atomic_or_fetch(&t->sig_pending, BIT(sig), __ATOMIC_SEQ_CST);
			if (t != cpu)
				rv_host_syscall(SYS_tgkill, getpid(), t->tid, RV_SIG_KICK, 0, 0, 0);
			*ret = 0;
		}
	}
	rv_unlock(cpu);
	return 1;
}
