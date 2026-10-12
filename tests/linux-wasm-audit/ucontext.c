/*
 * The ucontext an SA_SIGINFO handler gets. Linux: uc_stack says the alternate stack is off (SS_DISABLE), uc_mcontext
 * holds the interrupted stack pointer, and a signal added to uc_sigmask is blocked once the handler returns.
 * The same is then read through the kernel's layout (the proposed fix for musl's arch/wasm/bits/signal.h).
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>

/* The kernel's struct ucontext (asm-generic/ucontext.h, with wasm's struct sigcontext): the proposed musl layout. */
struct kernel_ucontext {
	unsigned long uc_flags;
	struct kernel_ucontext *uc_link;
	stack_t uc_stack;
	struct { unsigned long stack_pointer, tls; } uc_mcontext;
	sigset_t uc_sigmask;
};

static volatile int k_ss_flags = -1;
static volatile unsigned long k_ss_size = 1, k_sp;
static volatile int ss_flags = -1;
static volatile unsigned long ss_size = 1, sp;

static void on_usr1(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = ctx;

	(void)sig;
	(void)si;
	ss_flags = uc->uc_stack.ss_flags;
	ss_size = uc->uc_stack.ss_size;
	sp = uc->uc_mcontext.stack_pointer;
	if (k_ss_flags == -1) {
		struct kernel_ucontext *k = ctx;

		k_ss_flags = k->uc_stack.ss_flags;
		k_ss_size = k->uc_stack.ss_size;
		k_sp = k->uc_mcontext.stack_pointer;
		sigaddset(&uc->uc_sigmask, SIGUSR2);
	} else {
		sigaddset(&((struct kernel_ucontext *)ctx)->uc_sigmask, SIGUSR2);
	}
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	sigset_t now;
	int here;

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_usr1;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &sa, 0);
	raise(SIGUSR1);
	sigprocmask(SIG_BLOCK, 0, &now);
	printf("uc_stack.ss_flags %d (want %d), ss_size %lu (want 0); uc_mcontext sp %#lx (a local of main is at %p)\n",
	       ss_flags, SS_DISABLE, ss_size, sp, (void *)&here);
	printf("SIGUSR2 added to uc_sigmask is %s after the handler (want blocked)\n",
	       sigismember(&now, SIGUSR2) ? "blocked" : "NOT blocked");
	printf("read with the kernel's layout: ss_flags %d, ss_size %lu, sp %#lx\n", k_ss_flags, k_ss_size, k_sp);
	/* Again, adding SIGUSR2 through the kernel's layout. */
	sigprocmask(SIG_UNBLOCK, &now, 0);
	raise(SIGUSR1);
	sigprocmask(SIG_BLOCK, 0, &now);
	printf("SIGUSR2 added through the kernel's layout is %s after the handler\n",
	       sigismember(&now, SIGUSR2) ? "blocked (ok)" : "NOT blocked");
	return 0;
}
