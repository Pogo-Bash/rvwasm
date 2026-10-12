/*
 * A program that traps (here: __builtin_trap(), Wasm "unreachable") with a handler for the signal the kernel turns
 * the trap into (SIGILL). Linux would run the handler (or kill the program); expect either "handler ran" and exit
 * status 7, or death by SIGILL. With "x" no handler is installed and SIGILL is blocked instead.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void on_fault(int sig)
{
	char msg[] = "handler ran\n";
	(void)sig;
	write(1, msg, sizeof msg - 1);
	_exit(7);
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	sigset_t set;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_fault;
	if (argc > 1) {
		sigemptyset(&set);
		sigaddset(&set, SIGILL);
		sigprocmask(SIG_BLOCK, &set, 0);
	} else {
		sigaction(SIGILL, &sa, 0);
		sigaction(SIGSEGV, &sa, 0);
	}
	printf("trapping\n");
	fflush(stdout);
	__builtin_trap();
	return 0;
}
