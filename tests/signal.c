/*
 * Signal delivery: handlers, masks, sigsuspend, nesting, one-shot handlers,
 * fault handlers that edit the saved context, the alternate stack, timers.
 *
 * Everything is synchronous (signals are sent to ourselves or waited for in a
 * system call), so the instruction stream is the same on every run and on
 * every correct implementation.
 */
#include "guest.h"

#define SYS_setitimer		103
#define SYS_kill		129
#define SYS_sigaltstack		132
#define SYS_rt_sigsuspend	133
#define SYS_rt_sigaction	134
#define SYS_rt_sigprocmask	135
#define SYS_rt_sigpending	136
#define SYS_getpid		172

#define SIGILL	4
#define SIGUSR1	10
#define SIGSEGV	11
#define SIGUSR2	12
#define SIGALRM	14

#define SA_SIGINFO	0x00000004UL
#define SA_ONSTACK	0x08000000UL
#define SA_NODEFER	0x40000000UL
#define SA_RESETHAND	0x80000000UL

#define SIG_BLOCK	0
#define SIG_UNBLOCK	1
#define SIG_SETMASK	2

#define BIT(sig)	(1UL << ((sig) - 1))

/* Offsets into the ucontext a handler is given. */
#define UC_SIGMASK	40
#define UC_PC		176
#define UC_A0		(176 + 10 * 8)
#define SI_ADDR		16

/*
 * The kernel's struct sigaction on riscv64 is the first three words: there is
 * no restorer. qemu-riscv64 8.2 reads the mask from a fourth word as if there
 * were one, so the mask is given twice to make both see the same thing.
 */
struct ksigaction {
	void (*handler)(int, void *, void *);
	u64 flags;
	u64 mask;
	u64 mask_for_qemu;
};

static volatile u64 count[32];
static volatile u64 seen_signo, seen_saved_mask, seen_live_mask, seen_addr, seen_on_alt;
static volatile u64 depth, max_depth;
static u64 pid;
static char alt_stack[16384];

static u64 kill_self(int sig)
{
	return (u64)sys(SYS_kill, (long)pid, sig, 0, 0, 0, 0);
}

static u64 set_action(int sig, void (*fn)(int, void *, void *), u64 flags, u64 mask)
{
	struct ksigaction sa = { fn, flags | SA_SIGINFO, mask, mask };

	return (u64)sys(SYS_rt_sigaction, sig, (long)&sa, 0, 8, 0, 0);
}

static u64 mask_op(int how, u64 set)
{
	u64 old = 0;

	sys(SYS_rt_sigprocmask, how, (long)&set, (long)&old, 8, 0, 0);
	return old;
}

static u64 current_mask(void)
{
	u64 old = 0;

	sys(SYS_rt_sigprocmask, SIG_BLOCK, 0, (long)&old, 8, 0, 0);
	return old;
}

static void handler(int sig, void *info, void *uc)
{
	register u64 sp __asm__("sp");

	count[sig]++;
	seen_signo = (u64)*(int *)info;
	seen_saved_mask = *(u64 *)((char *)uc + UC_SIGMASK);
	seen_live_mask = current_mask();
	seen_on_alt = sp >= (u64)alt_stack && sp < (u64)alt_stack + sizeof(alt_stack);
}

/* Sends itself the same signal once more: only possible with SA_NODEFER. */
static void nesting_handler(int sig, void *info, void *uc)
{
	(void)info;
	(void)uc;
	count[sig]++;
	depth++;
	if (depth > max_depth)
		max_depth = depth;
	if (depth < 3)
		kill_self(sig);
	depth--;
}

/* Skips the faulting instruction and plants a value in the saved a0. */
static void fault_handler(int sig, void *info, void *uc)
{
	count[sig]++;
	seen_addr = *(u64 *)((char *)info + SI_ADDR);
	*(u64 *)((char *)uc + UC_PC) += 4;
	*(u64 *)((char *)uc + UC_A0) = 0x1234;
}

static u64 load_from(u64 addr)
{
	register u64 a0 __asm__("a0") = addr;

	/* Exactly one 4-byte instruction, so that the handler can step over it. */
	__asm__ volatile(".option push\n\t.option norvc\n\tld a0, 0(a0)\n\t.option pop"
			 : "+r"(a0) : : "memory");
	return a0;
}

static u64 run_illegal(void)
{
	register u64 a0 __asm__("a0") = 7;

	__asm__ volatile(".word 0\n" : "+r"(a0));
	return a0;
}

void _start(void)
{
	u64 pending = 0, empty = 0, r;
	u64 alt[3] = { (u64)alt_stack, 0, sizeof(alt_stack) };
	struct ksigaction query;
	long timer[4] = { 0, 0, 0, 20000 };	/* one shot, 20 ms */

	pid = (u64)sys(SYS_getpid, 0, 0, 0, 0, 0, 0);

	/* A handler runs, sees the right signal, and the program carries on. */
	put_hex("action", set_action(SIGUSR1, handler, 0, BIT(SIGALRM)));
	put_hex("kill", kill_self(SIGUSR1));
	put_hex("ran", count[SIGUSR1]);
	put_hex("signo", seen_signo);
	put_hex("saved_mask", seen_saved_mask);
	put_hex("live_mask", seen_live_mask);	/* itself plus sa_mask */
	put_hex("mask_after", current_mask());

	/* A blocked signal waits; unblocking delivers it. */
	mask_op(SIG_BLOCK, BIT(SIGUSR1));
	kill_self(SIGUSR1);
	put_hex("blocked_ran", count[SIGUSR1]);
	sys(SYS_rt_sigpending, (long)&pending, 8, 0, 0, 0, 0);
	put_hex("pending", pending);
	mask_op(SIG_UNBLOCK, BIT(SIGUSR1));
	put_hex("unblocked_ran", count[SIGUSR1]);
	put_hex("unblocked_saved", seen_saved_mask);

	/* sigsuspend lets a blocked signal in for the duration of the call only. */
	set_action(SIGUSR2, handler, 0, 0);
	mask_op(SIG_BLOCK, BIT(SIGUSR2));
	kill_self(SIGUSR2);
	put_hex("before_suspend", count[SIGUSR2]);
	r = (u64)sys(SYS_rt_sigsuspend, (long)&empty, 8, 0, 0, 0, 0);
	put_hex("suspend_ret", r);
	put_hex("suspend_ran", count[SIGUSR2]);
	put_hex("suspend_saved", seen_saved_mask);	/* the mask before the call */
	put_hex("suspend_live", seen_live_mask);
	put_hex("suspend_after", current_mask());
	mask_op(SIG_SETMASK, 0);

	/* Ignored signals do nothing. */
	set_action(SIGUSR2, (void (*)(int, void *, void *))1, 0, 0);
	kill_self(SIGUSR2);
	put_hex("ignored_ran", count[SIGUSR2]);

	/* SA_NODEFER allows a handler to interrupt itself. */
	set_action(SIGUSR2, nesting_handler, SA_NODEFER, 0);
	kill_self(SIGUSR2);
	put_hex("nested_runs", count[SIGUSR2]);
	put_hex("nested_depth", max_depth);

	/* SA_RESETHAND: the handler is gone after one delivery. */
	set_action(SIGUSR1, handler, SA_RESETHAND, 0);
	kill_self(SIGUSR1);
	sys(SYS_rt_sigaction, SIGUSR1, 0, (long)&query, 8, 0, 0);
	put_hex("resethand_ran", count[SIGUSR1]);
	put_hex("resethand_left", (u64)query.handler);

	/* Faults become signals; the handler's edits to the context take effect. */
	set_action(SIGSEGV, fault_handler, 0, 0);
	put_hex("segv_result", load_from(8));
	put_hex("segv_ran", count[SIGSEGV]);
	put_hex("segv_addr", seen_addr);
	set_action(SIGILL, fault_handler, 0, 0);
	put_hex("ill_result", run_illegal());
	put_hex("ill_ran", count[SIGILL]);

	/* SA_ONSTACK runs the handler on the alternate stack. */
	put_hex("altstack", (u64)sys(SYS_sigaltstack, (long)alt, 0, 0, 0, 0, 0));
	set_action(SIGUSR1, handler, SA_ONSTACK, 0);
	kill_self(SIGUSR1);
	put_hex("on_alt", seen_on_alt);
	set_action(SIGUSR1, handler, 0, 0);
	kill_self(SIGUSR1);
	put_hex("off_alt", seen_on_alt);

	/* A timer signal ends a wait. */
	set_action(SIGALRM, handler, 0, 0);
	put_hex("setitimer", (u64)sys(SYS_setitimer, 0, (long)timer, 0, 0, 0, 0));
	r = (u64)sys(SYS_rt_sigsuspend, (long)&empty, 8, 0, 0, 0, 0);
	put_hex("alarm_ret", r);
	put_hex("alarm_ran", count[SIGALRM]);

	finish();
}
