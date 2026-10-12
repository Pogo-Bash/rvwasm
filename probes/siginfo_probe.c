/* A native program's signal handlers, one without SA_SIGINFO and one with, each raised three times (musl's __libc_handle_signal once read SA_SIGINFO from the wrong place: tools/patch-runtime.py 18). */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile int plain, info, info_ok;

static void on_plain(int sig)
{
	plain += sig == SIGUSR1;
}

static void on_info(int sig, siginfo_t *si, void *uc)
{
	info++;
	info_ok += sig == SIGUSR2 && si && si->si_signo == SIGUSR2 && si->si_pid == getpid() && uc;
}

int main(int argc, char **argv)
{
	struct sigaction sa;

	(void)argc;
	(void)argv;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_plain;
	sigaction(SIGUSR1, &sa, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_info;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR2, &sa, NULL);
	for (int i = 0; i < 3; i++) {
		kill(getpid(), SIGUSR1);
		kill(getpid(), SIGUSR2);
	}
	printf("siginfo_probe: plain %d, siginfo %d (%d right)\n", plain, info, info_ok);
	return !(plain == 3 && info == 3 && info_ok == 3);
}
