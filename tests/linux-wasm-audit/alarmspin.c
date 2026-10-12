/*
 * alarm(1), then 3 s of computing without system calls, then getpid(). The timer expired while the program
 * computed; Linux delivers SIGALRM by the return of getpid() at the latest. Says where the handler ran.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int handled;

static void on_alarm(int sig)
{
	(void)sig;
	handled = 1;
}

static long ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	volatile unsigned long n;
	unsigned long loops = 1000000, i;
	long t0, t;
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_alarm;
	sigaction(SIGALRM, &sa, 0);
	/* Calibrate: grow the loop until it takes 3 s. */
	for (;;) {
		t0 = ms();
		for (i = 0, n = 0; i < loops; i++)
			n++;
		t = ms() - t0;
		if (t > 300)
			break;
		loops *= 4;
	}
	loops = loops * 3000 / t;
	alarm(1);
	t0 = ms();
	for (i = 0, n = 0; i < loops; i++)
		n++;
	t = ms() - t0;
	getpid();
	printf("computed %ld ms; after getpid(): %s\n", t, handled ? "handler ran (ok)" : "NOT delivered");
	if (!handled) {
		usleep(100000);
		printf("after a 100 ms sleep: %s\n", handled ? "handler ran" : "still not delivered");
	}
	return 0;
}
