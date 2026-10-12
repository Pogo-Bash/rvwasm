/*
 * pthread_cancel() of a thread blocked in read() (with "sleep": in nanosleep()), both cancellation points.
 * Expect join to return PTHREAD_CANCELED.
 */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static int p[2];

static void *reader(void *arg)
{
	char c;

	if (arg)
		for (;;)
			sleep(10);
	read(p[0], &c, 1);
	return (void *)1;
}

int main(int argc, char **argv)
{
	pthread_t t;
	void *res = 0;

	pipe(p);
	pthread_create(&t, 0, reader, argc > 1 ? (void *)1 : 0);
	sleep(1);
	printf("cancelling\n");
	fflush(stdout);
	pthread_cancel(t);
	pthread_join(t, &res);
	printf("joined: %s\n", res == PTHREAD_CANCELED ? "PTHREAD_CANCELED (ok)" : "not cancelled");
	return 0;
}
