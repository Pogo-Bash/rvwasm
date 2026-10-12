/*
 * A Wasm trap of each kind, with a handler for SIGSEGV, SIGFPE and SIGILL (or with "nohandler", none; with "return",
 * a handler that returns). Linux would give an out-of-bounds access SIGSEGV, a division by zero SIGFPE and an
 * undefined instruction SIGILL. Expect "KIND: handler got signal N", or death by that signal (status 128 + N); a
 * handler that returns cannot resume a trap, so the program then dies by the signal as well.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *kind;
static int returns;

static void on_trap(int sig, siginfo_t *si, void *uc)
{
	char msg[64];
	int n;

	(void)si;
	(void)uc;
	n = snprintf(msg, sizeof msg, "%s: handler got signal %d\n", kind, sig);
	write(1, msg, n);
	if (!returns)
		_exit(0);
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	volatile int zero = 0, one = 1;

	kind = argc > 1 ? argv[1] : "oob";
	returns = argc > 2 && !strcmp(argv[2], "return");
	if (!(argc > 2 && !strcmp(argv[2], "nohandler"))) {
		memset(&sa, 0, sizeof sa);
		sa.sa_sigaction = on_trap;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, 0);
		sigaction(SIGFPE, &sa, 0);
		sigaction(SIGILL, &sa, 0);
	}
	if (!strcmp(kind, "oob"))
		return *(volatile char *)(~0UL - 0xffff);
	if (!strcmp(kind, "div0"))
		return one / zero;
	__builtin_trap();
	return 0;
}
