/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Guest signals.
 *
 * The guest's dispositions and its signal mask are mirrored onto the host
 * process, so the host kernel does the queueing, the blocking and the default
 * actions. For signals the guest handles, the host handler only records that
 * the signal arrived; the interpreter then builds a RISC-V signal frame on
 * the guest stack at the next instruction boundary and runs the guest's
 * handler, which returns through rt_sigreturn.
 *
 * Every guest thread is a host thread (thread.c) and has its guest mask as
 * its host mask, so the host kernel also picks the thread that takes a
 * signal sent to the process. The handler runs in that thread and records
 * the signal in that thread's cpu.
 *
 * A thread's sig_pending holds what has arrived for it. Normally the guest
 * does not block any of that (the host would not have delivered it), but a
 * handler that is entered blocks more, and the few signals that threads send
 * each other without the host (thread.c) arrive whatever the mask. Those
 * wait in sig_held until the mask lets them through.
 *
 * The host handler takes one argument: on Linux/Wasm a three-argument
 * SA_SIGINFO handler cannot be called at all. The guest's siginfo is
 * therefore synthesised.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "rv.h"

#define RV_SIG_DFL	0
#define RV_SIG_IGN	1

#define RV_SA_ONSTACK	0x08000000u
#define RV_SA_RESTART	0x10000000u
#define RV_SA_NODEFER	0x40000000u
#define RV_SA_RESETHAND	0x80000000u

#define RV_SIG_BLOCK	0
#define RV_SIG_UNBLOCK	1
#define RV_SIG_SETMASK	2

#define RV_SS_ONSTACK	1
#define RV_SS_DISABLE	2
#define RV_MINSIGSTKSZ	2048

#define RV_SIGILL	4
#define RV_SIGTRAP	5
#define RV_SIGKILL	9
#define RV_SIGSEGV	11
#define RV_SIGCHLD	17
#define RV_SIGCONT	18
#define RV_SIGSTOP	19
#define RV_SIGURG	23
#define RV_SIGWINCH	28

#define BIT(sig)	(1ULL << ((sig) - 1))
#define UNBLOCKABLE	(BIT(RV_SIGKILL) | BIT(RV_SIGSTOP))

/*
 * struct rt_sigframe as the riscv64 kernel lays it out:
 *
 *   siginfo (128 bytes)
 *   ucontext: uc_flags, uc_link, uc_stack (24), uc_sigmask (8), padding up
 *             to 1024 bits of mask, then, aligned to 16, the machine context:
 *             pc and x1..x31, then f0..f31 and fcsr.
 */
#define FRAME_INFO	0
#define FRAME_UC	128
#define UC_STACK	(FRAME_UC + 16)
#define UC_SIGMASK	(FRAME_UC + 40)
#define UC_REGS		(FRAME_UC + 176)	/* pc, x1..x31 */
#define UC_FPREGS	(UC_REGS + 256)		/* f0..f31 */
#define UC_FCSR		(UC_FPREGS + 256)
#define FRAME_SIZE	(FRAME_UC + 176 + 784)

#define SI_SIGNO	0
#define SI_CODE		8
#define SI_ADDR		16	/* or si_pid (4 bytes) and si_uid (4 bytes) */
#define RV_SI_TKILL	(-6)

/* A signal arrived, in this thread: it waits in the thread's cpu for the run loop. */
static void host_handler(int sig)
{
	struct rv_cpu *cpu = rv_self;

	rv_stack_note("a signal");
	if (cpu)
		__atomic_or_fetch(&cpu->sig_pending, BIT(sig), __ATOMIC_SEQ_CST);
}

/* The kick (thread.c): all it has to do is end the system call the thread waits in. */
static void kick_handler(int sig)
{
	(void)sig;
	rv_stack_note("the kick");
}

/*
 * The kick's handler, and the kick let through: a mask is kept across exec,
 * and the program before may have had it blocked (spawn_child() in fork.c
 * does, until it has put the guest's mask back).
 */
void rv_signal_init(void)
{
	struct sigaction sa;
	sigset_t kick;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = kick_handler;
	sigfillset(&sa.sa_mask);
	sigaction(RV_SIG_KICK, &sa, NULL);
	sigemptyset(&kick);
	sigaddset(&kick, RV_SIG_KICK);
	pthread_sigmask(SIG_UNBLOCK, &kick, NULL);
}

/*
 * A guest signal mask as one for the host. Never with the kick, whatever the
 * guest asks: a thread that the kick cannot reach holds up every stop of the
 * world for good (thread.c). Every mask of the guest's that reaches the host
 * comes through here: the thread's own (sync_host_mask()), and the temporary
 * ones of rt_sigsuspend, ppoll, pselect6 and epoll_pwait, and signalfd's
 * (syscall.c).
 */
void rv_signal_host_set(uint64_t mask, sigset_t *set)
{
	sigemptyset(set);
	for (int sig = 1; sig <= RV_NSIG; sig++)
		if ((mask & BIT(sig)) && sig != RV_SIG_KICK)
			sigaddset(set, sig);	/* fails for the few a libc reserves; fine */
}

/*
 * A program started by exec (not the child of a fork, which has its parent's
 * mask): blocked signals stay blocked across exec, and the host has kept what
 * the program before blocked. That is the guest's mask to start with, as far
 * as the host can hold it (not the few its C library keeps, nor the kick).
 */
void rv_signal_inherit_mask(struct rv_cpu *cpu)
{
	sigset_t host;

	if (pthread_sigmask(SIG_BLOCK, NULL, &host))
		return;
	cpu->sig_blocked = 0;
	for (int sig = 1; sig <= RV_NSIG; sig++)
		if (sig != RV_SIG_KICK && sigismember(&host, sig) == 1)
			cpu->sig_blocked |= BIT(sig);
	cpu->sig_blocked &= ~UNBLOCKABLE;
}

static void sync_host_mask(struct rv_cpu *cpu)
{
	uint64_t free_now = cpu->sig_held & ~cpu->sig_blocked;
	sigset_t set;

	rv_signal_host_set(cpu->sig_blocked, &set);
	pthread_sigmask(SIG_SETMASK, &set, NULL);
	/* What waited for the mask to let it through. */
	if (free_now) {
		cpu->sig_held &= ~free_now;
		__atomic_or_fetch(&cpu->sig_pending, free_now, __ATOMIC_SEQ_CST);
	}
}

void rv_signal_sync_mask(struct rv_cpu *cpu)
{
	sync_host_mask(cpu);
}

/* Give the host process the disposition the guest asked for. */
static void sync_host_action(struct rv_cpu *cpu, int sig)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	if (sig == RV_SIG_KICK)
		return;		/* ours (thread.c) */
	if (cpu->proc->sigact[sig].handler == RV_SIG_DFL) {
		sa.sa_handler = SIG_DFL;
	} else if (cpu->proc->sigact[sig].handler == RV_SIG_IGN) {
		sa.sa_handler = SIG_IGN;
	} else {
		sa.sa_handler = host_handler;
		/* Never SA_RESTART: an interrupted host call must come back to
		 * us so that the guest handler runs before any restart. */
		sigfillset(&sa.sa_mask);
	}
	sigaction(sig, &sa, NULL);
}

/* After exec the host forgot our handlers (fork-by-snapshot child). */
void rv_signal_reinstall(struct rv_cpu *cpu)
{
	for (int sig = 1; sig <= RV_NSIG; sig++)
		if (cpu->proc->sigact[sig].handler > RV_SIG_IGN)
			sync_host_action(cpu, sig);
	sync_host_mask(cpu);
}

static uint64_t neg(int err)
{
	return (uint64_t)-(int64_t)err;
}

uint64_t rv_sys_rt_sigaction(struct rv_cpu *cpu, uint64_t sig, uint64_t act_addr,
			     uint64_t old_addr, uint64_t size)
{
	struct rv_sigaction old;

	if (size != 8 || sig < 1 || sig > RV_NSIG)
		return neg(RV_EINVAL);

	rv_lock(cpu);
	old = cpu->proc->sigact[sig];
	if (act_addr) {
		struct rv_sigaction *act = rv_user_ptr(cpu, act_addr, sizeof(*act), RV_PROT_READ);

		if (!act || (BIT(sig) & UNBLOCKABLE)) {
			rv_unlock(cpu);
			return neg(act ? RV_EINVAL : RV_EFAULT);
		}
		memcpy(&cpu->proc->sigact[sig], act, sizeof(*act));
		sync_host_action(cpu, (int)sig);
	}
	rv_unlock(cpu);
	if (old_addr) {
		void *p = rv_user_ptr(cpu, old_addr, sizeof(old), RV_PROT_WRITE);

		if (!p)
			return neg(RV_EFAULT);
		memcpy(p, &old, sizeof(old));
	}
	return 0;
}

uint64_t rv_sys_rt_sigprocmask(struct rv_cpu *cpu, uint64_t how, uint64_t set_addr,
			       uint64_t old_addr, uint64_t size)
{
	uint64_t old = cpu->sig_blocked;

	if (size != 8)
		return neg(RV_EINVAL);
	if (set_addr) {
		uint64_t *p = rv_user_ptr(cpu, set_addr, 8, RV_PROT_READ), set;

		if (!p)
			return neg(RV_EFAULT);
		memcpy(&set, p, 8);
		switch (how) {
		case RV_SIG_BLOCK:	cpu->sig_blocked |= set; break;
		case RV_SIG_UNBLOCK:	cpu->sig_blocked &= ~set; break;
		case RV_SIG_SETMASK:	cpu->sig_blocked = set; break;
		default:		return neg(RV_EINVAL);
		}
		cpu->sig_blocked &= ~UNBLOCKABLE;
		sync_host_mask(cpu);
	}
	if (old_addr) {
		void *p = rv_user_ptr(cpu, old_addr, 8, RV_PROT_WRITE);

		if (!p)
			return neg(RV_EFAULT);
		memcpy(p, &old, 8);
	}
	return 0;
}

uint64_t rv_sys_rt_sigpending(struct rv_cpu *cpu, uint64_t addr, uint64_t size)
{
	uint64_t *p = rv_user_ptr(cpu, addr, 8, RV_PROT_WRITE);
	uint64_t mask = ((cpu->sig_pending & ~RV_SIG_INTERNAL) | cpu->sig_held) & cpu->sig_blocked;
	sigset_t host;

	if (size != 8)
		return neg(RV_EINVAL);
	if (!p)
		return neg(RV_EFAULT);
	/* Blocked signals wait in the host kernel, not here. */
	if (!sigpending(&host))
		for (int sig = 1; sig <= RV_NSIG; sig++)
			if (sigismember(&host, sig) == 1)
				mask |= BIT(sig);
	memcpy(p, &mask, 8);
	return 0;
}

/*
 * A system call that waits with a temporary signal mask (rt_sigsuspend,
 * ppoll, pselect6) was interrupted. A handler for a signal that only the
 * temporary mask let through must still run, and the original mask comes
 * back when it returns: keep the temporary mask in force until the next
 * delivery point and remember what to restore.
 */
void rv_signal_temp_mask(struct rv_cpu *cpu, uint64_t temp_mask)
{
	uint64_t free_now;

	cpu->sig_saved_mask = cpu->sig_blocked;
	cpu->sig_restore_mask = 1;
	cpu->sig_blocked = temp_mask & ~UNBLOCKABLE;
	/* (The host's mask is not touched: it was the temporary one inside the call, and is the old one again.) */
	free_now = cpu->sig_held & ~cpu->sig_blocked;
	if (free_now) {
		cpu->sig_held &= ~free_now;
		__atomic_or_fetch(&cpu->sig_pending, free_now, __ATOMIC_SEQ_CST);
	}
}

uint64_t rv_sys_rt_sigsuspend(struct rv_cpu *cpu, uint64_t mask_addr, uint64_t size)
{
	uint64_t *p = rv_user_ptr(cpu, mask_addr, 8, RV_PROT_READ), mask;
	sigset_t host;

	if (size != 8)
		return neg(RV_EINVAL);
	if (!p)
		return neg(RV_EFAULT);
	memcpy(&mask, p, 8);
	mask &= ~UNBLOCKABLE;

	/*
	 * Something may have arrived already; if not, sleep until it does. A
	 * kick ends the sleep too (thread.c): then the thread parks if it is
	 * asked to, and sleeps again.
	 */
	rv_signal_host_set(mask, &host);
	while (!(((cpu->sig_pending & ~RV_SIG_INTERNAL) | cpu->sig_held) & ~mask)) {
		rv_safepoint(cpu);
		sigsuspend(&host);
	}
	rv_signal_temp_mask(cpu, mask);
	return neg(RV_EINTR);
}

uint64_t rv_sys_sigaltstack(struct rv_cpu *cpu, uint64_t ss_addr, uint64_t old_addr)
{
	/* stack_t: ss_sp (8), ss_flags (4, padded to 8), ss_size (8) */
	uint64_t old[3] = { cpu->sigalt_sp, cpu->sigalt_flags, cpu->sigalt_size };
	uint64_t sp = cpu->x[2];
	int on_stack = cpu->sigalt_size && sp - cpu->sigalt_sp < cpu->sigalt_size;

	if (!cpu->sigalt_size)
		old[1] = RV_SS_DISABLE;
	else if (on_stack)
		old[1] = RV_SS_ONSTACK;

	if (ss_addr) {
		uint64_t *p = rv_user_ptr(cpu, ss_addr, 24, RV_PROT_READ), ss[3];

		if (!p)
			return neg(RV_EFAULT);
		memcpy(ss, p, 24);
		ss[1] &= 0xffffffffu;
		if (on_stack)
			return neg(RV_EPERM);
		if (ss[1] & RV_SS_DISABLE) {
			cpu->sigalt_sp = cpu->sigalt_size = 0;
		} else {
			if (ss[1] & ~(uint64_t)RV_SS_ONSTACK)
				return neg(RV_EINVAL);
			if (ss[2] < RV_MINSIGSTKSZ)
				return neg(RV_ENOMEM);
			cpu->sigalt_sp = ss[0];
			cpu->sigalt_size = ss[2];
		}
		cpu->sigalt_flags = 0;
	}
	if (old_addr) {
		void *p = rv_user_ptr(cpu, old_addr, 24, RV_PROT_WRITE);

		if (!p)
			return neg(RV_EFAULT);
		memcpy(p, old, 24);
	}
	return 0;
}

/*
 * A handler returns to this code, which the kernel normally provides in the
 * vDSO: `li a7, 139; ecall` (rt_sigreturn). Mapped on first use, so programs
 * that never take a signal keep the layout they had.
 */
static uint64_t trampoline(struct rv_cpu *cpu)
{
	static const uint32_t code[2] = { 0x08b00893, 0x00000073 };
	struct rv_proc *proc = cpu->proc;
	uint64_t addr;

	if (proc->sig_trampoline)
		return proc->sig_trampoline;
	addr = rv_mm_mmap(cpu, 0, RV_PAGE_SIZE, RV_PROT_READ | RV_PROT_EXEC,
			  RV_MAP_PRIVATE | RV_MAP_ANONYMOUS, -1, 0);
	if (addr >= (uint64_t)-4095)
		return 0;
	rv_lock(cpu);
	if (!proc->sig_trampoline && !rv_mm_fill(cpu, addr, sizeof(code), code, -1, 0)) {
		proc->sig_trampoline = addr;
		addr = 0;
	}
	rv_unlock(cpu);
	if (addr)	/* another thread was first */
		rv_mm_munmap(cpu, addr, RV_PAGE_SIZE);
	return proc->sig_trampoline;
}

static void put64(uint8_t *at, uint64_t v)
{
	memcpy(at, &v, 8);
}

static uint64_t get64(const uint8_t *at)
{
	uint64_t v;

	memcpy(&v, at, 8);
	return v;
}

/* End the process the way the default action of a signal would. */
void rv_signal_die(int sig)
{
	sigset_t set;

	rv_mm_die();
	signal(sig, SIG_DFL);
	sigemptyset(&set);
	sigaddset(&set, sig);
	pthread_sigmask(SIG_UNBLOCK, &set, NULL);
	raise(sig);
	_exit(128 + sig);	/* the default action was not fatal after all */
}

/*
 * Enter the guest's handler for `sig`: push a frame holding everything
 * rt_sigreturn needs to resume at `resume_pc`, then redirect the cpu.
 */
static int push_frame_with(struct rv_cpu *cpu, int sig, uint64_t resume_pc, int code, uint64_t info, const uint32_t *more);

static int push_frame(struct rv_cpu *cpu, int sig, uint64_t resume_pc, int code, uint64_t info)
{
	return push_frame_with(cpu, sig, resume_pc, code, info, NULL);
}

/* `more`: si_errno, and the two words after the address (for SIGSYS: the system call and the architecture). */
static int push_frame_with(struct rv_cpu *cpu, int sig, uint64_t resume_pc, int code, uint64_t info, const uint32_t *more)
{
	struct rv_sigaction act = cpu->proc->sigact[sig], *sa = &act;
	uint64_t saved_mask = cpu->sig_restore_mask ? cpu->sig_saved_mask : cpu->sig_blocked;
	uint64_t sp = cpu->x[2], frame, tramp = trampoline(cpu);
	int on_alt = cpu->sigalt_size && sp - cpu->sigalt_sp < cpu->sigalt_size;
	uint32_t fcsr = cpu->fcsr, flags;
	uint8_t *f;

	if ((sa->flags & RV_SA_ONSTACK) && cpu->sigalt_size && !on_alt)
		sp = cpu->sigalt_sp + cpu->sigalt_size;
	frame = (sp - FRAME_SIZE) & ~15ULL;

	/* No room for the frame: the kernel would kill the process with SIGSEGV. */
	if (!tramp || !(f = rv_user_ptr(cpu, frame, FRAME_SIZE, RV_PROT_WRITE)))
		return -1;

	memset(f, 0, FRAME_SIZE);
	memcpy(f + FRAME_INFO + SI_SIGNO, &sig, 4);
	memcpy(f + FRAME_INFO + SI_CODE, &code, 4);
	put64(f + FRAME_INFO + SI_ADDR, info);
	if (more) {
		memcpy(f + FRAME_INFO + 4, &more[0], 4);
		memcpy(f + FRAME_INFO + SI_ADDR + 8, &more[1], 8);
	}

	put64(f + UC_STACK, cpu->sigalt_sp);
	flags = !cpu->sigalt_size ? RV_SS_DISABLE : on_alt ? RV_SS_ONSTACK : 0;
	memcpy(f + UC_STACK + 8, &flags, 4);
	put64(f + UC_STACK + 16, cpu->sigalt_size);
	put64(f + UC_SIGMASK, saved_mask);
	put64(f + UC_REGS, resume_pc);
	memcpy(f + UC_REGS + 8, &cpu->x[1], 31 * 8);
	memcpy(f + UC_FPREGS, cpu->f, 32 * 8);
	memcpy(f + UC_FCSR, &fcsr, 4);
	rv_user_done(cpu);	/* (the frame may have been a copy: mm.c) */

	cpu->x[1] = tramp;
	cpu->x[2] = frame;
	cpu->x[10] = (uint64_t)sig;
	cpu->x[11] = frame + FRAME_INFO;
	cpu->x[12] = frame + FRAME_UC;
	cpu->pc = sa->handler;

	/* The handler runs with its own mask added, and normally itself blocked. */
	cpu->sig_blocked |= sa->mask;
	if (!(sa->flags & RV_SA_NODEFER))
		cpu->sig_blocked |= BIT(sig);
	cpu->sig_blocked &= ~UNBLOCKABLE;
	cpu->sig_restore_mask = 0;
	sync_host_mask(cpu);

	if (sa->flags & RV_SA_RESETHAND) {
		rv_lock(cpu);
		cpu->proc->sigact[sig].handler = RV_SIG_DFL;
		sync_host_action(cpu, sig);
		rv_unlock(cpu);
	}
	return 0;
}

static int default_ignores(int sig)
{
	return sig == RV_SIGCHLD || sig == RV_SIGCONT || sig == RV_SIGURG || sig == RV_SIGWINCH;
}

/*
 * Called between instructions whenever something may be deliverable. Enters
 * at most one handler; further pending signals are taken at the following
 * instruction boundary, which stacks their frames the way the kernel does.
 */
void rv_signal_deliver(struct rv_cpu *cpu)
{
	uint64_t pending, ready, resume_pc, info = 0;
	struct rv_sigaction act;
	int sig, code = 0;	/* SI_USER */

	/* First of all: park, if that is what is asked (thread.c). */
	rv_safepoint(cpu);
	/* Code changed (jit.c): being here is all that was asked for; the run loop looks at what changed. */
	if (cpu->sig_pending & RV_SIG_CODE)
		__atomic_and_fetch(&cpu->sig_pending, ~RV_SIG_CODE, __ATOMIC_SEQ_CST);

	/* What the guest blocks at the moment waits elsewhere, so that sig_pending means "look here". */
	pending = cpu->sig_pending & ~RV_SIG_INTERNAL;
	if (pending & cpu->sig_blocked) {
		__atomic_and_fetch(&cpu->sig_pending, ~(pending & cpu->sig_blocked), __ATOMIC_SEQ_CST);
		cpu->sig_held |= pending & cpu->sig_blocked;
	}
	ready = pending & ~cpu->sig_blocked;
	resume_pc = cpu->pc;

	if (!ready) {
		if (cpu->sig_restore_mask) {
			/* Nothing ran under the temporary mask: back to the original. */
			cpu->sig_blocked = cpu->sig_saved_mask;
			cpu->sig_restore_mask = 0;
			sync_host_mask(cpu);
		}
		cpu->sys_interrupted = 0;
		return;
	}

	sig = __builtin_ctzll(ready) + 1;
	__atomic_and_fetch(&cpu->sig_pending, ~BIT(sig), __ATOMIC_SEQ_CST);
	if (__atomic_fetch_and(&cpu->sig_tkill, ~BIT(sig), __ATOMIC_SEQ_CST) & BIT(sig)) {
		/*
		 * Sent to this thread with tkill/tgkill, by this process: say so
		 * (si_code, si_pid, si_uid). C libraries check it before they act
		 * on the signals they use themselves, such as the one that cancels
		 * a thread.
		 */
		code = RV_SI_TKILL;
		info = (uint32_t)getpid() | (uint64_t)getuid() << 32;
	}

	act = cpu->proc->sigact[sig];
	if (act.handler == RV_SIG_IGN || (act.handler == RV_SIG_DFL && default_ignores(sig)))
		return;	/* the disposition changed after the signal arrived */
	if (act.handler == RV_SIG_DFL)
		rv_signal_die(sig);

	/*
	 * The signal interrupted a system call that just returned EINTR. With
	 * SA_RESTART the call is made again after the handler: resume at the
	 * ecall with its first argument back in a0.
	 */
	if (cpu->sys_interrupted && cpu->pc == cpu->sys_pc + 4 && (act.flags & RV_SA_RESTART)) {
		resume_pc = cpu->sys_pc;
		cpu->x[10] = cpu->sys_a0;
	}
	cpu->sys_interrupted = 0;

	if (push_frame(cpu, sig, resume_pc, code, info))
		rv_signal_die(RV_SIGSEGV);
}

/*
 * The guest faulted (bad memory access, illegal instruction, breakpoint).
 * If it handles the corresponding signal, enter the handler with the faulting
 * instruction as the place to resume, and report 1; otherwise 0.
 */
int rv_signal_fault(struct rv_cpu *cpu)
{
	int sig, code = 1;	/* SEGV_MAPERR, ILL_ILLOPC and TRAP_BRKPT are all 1 */
	uint64_t addr = cpu->trap_addr;

	switch (cpu->trap) {
	case RV_TRAP_SEGV:	sig = RV_SIGSEGV; break;
	case RV_TRAP_ILL:	sig = RV_SIGILL; break;
	case RV_TRAP_BREAK:	sig = RV_SIGTRAP; break;
	default:		return 0;
	}
	if (cpu->proc->sigact[sig].handler <= RV_SIG_IGN || (cpu->sig_blocked & BIT(sig)))
		return 0;
	/* (A program that handles its own faults says nothing about them, or not what they were.) */
	if (cpu->verbose || cpu->strace) {
		const uint8_t *insn = rv_user_ptr(cpu, cpu->pc, 4, RV_PROT_READ);
		uint32_t raw = 0;

		if (insn)
			memcpy(&raw, insn, 4);
		fprintf(stderr, "rv[%d]: signal %d to the program's handler: pc 0x%llx (instruction 0x%08x), address 0x%llx, ra 0x%llx\n",
			cpu->tid, sig, (unsigned long long)cpu->pc, raw, (unsigned long long)addr,
			(unsigned long long)cpu->x[1]);
		rv_user_done(cpu);
	}
	if (push_frame(cpu, sig, cpu->pc, code, addr))
		return 0;
	cpu->trap = RV_TRAP_NONE;
	return 1;
}

/*
 * A system call filter answered a call with "trap" (seccomp.c): the thread
 * gets SIGSYS in place of the call, with the call's number and where it was
 * made, and its registers as they were before (the handler may do the
 * call's work itself and put a result into the saved a0). The signal cannot
 * be held back or ignored: a program that does not handle it dies of it.
 * Returns 1 if the handler was entered.
 */
int rv_signal_sys(struct rv_cpu *cpu, uint64_t nr, uint32_t data)
{
	const uint32_t more[3] = { data, (uint32_t)nr, 0xc00000f3U };	/* (AUDIT_ARCH_RISCV64) */

	if (cpu->proc->sigact[31].handler <= RV_SIG_IGN || (cpu->sig_blocked & BIT(31)) ||
	    push_frame_with(cpu, 31, cpu->pc + 4, 1 /* SYS_SECCOMP */, cpu->pc + 4, more))
		rv_signal_die(31);
	cpu->pc_set = 1;
	return 1;
}

/* rt_sigreturn: undo push_frame. The handler may have edited the saved context. */
void rv_sys_rt_sigreturn(struct rv_cpu *cpu)
{
	uint64_t frame = cpu->x[2];
	const uint8_t *f = rv_user_ptr(cpu, frame, FRAME_SIZE, RV_PROT_READ);
	uint32_t fcsr;

	if (!f) {
		cpu->trap = RV_TRAP_SEGV;
		cpu->trap_addr = frame;
		return;
	}

	cpu->sig_blocked = get64(f + UC_SIGMASK) & ~UNBLOCKABLE;
	cpu->pc = get64(f + UC_REGS);
	memcpy(&cpu->x[1], f + UC_REGS + 8, 31 * 8);
	memcpy(cpu->f, f + UC_FPREGS, 32 * 8);
	memcpy(&fcsr, f + UC_FCSR, 4);
	cpu->fcsr = fcsr & 0xff;
	cpu->pc_set = 1;
	sync_host_mask(cpu);
}

/* kill, tkill, tgkill: the guest's process and thread ids are the host's. */
uint64_t rv_sys_kill(int64_t pid, int sig)
{
	return kill((pid_t)pid, sig) < 0 ? neg(errno) : 0;
}

/*
 * On Linux/Wasm a task cannot be interrupted: signals are only acted on when
 * it returns from a system call. A guest that computes without making any
 * would never see one (or be killed by one), so the interpreter calls this
 * regularly to give the kernel the opportunity.
 */
void rv_signal_poll(void)
{
#ifdef __wasm__
	(void)getppid();
#endif
}
