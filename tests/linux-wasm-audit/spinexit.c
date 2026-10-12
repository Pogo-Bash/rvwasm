/*
 * main() returns (exit_group) while another thread computes without making system calls.
 * Expect the program to end at once and the shell to print "exited". With "exec": main executes /bin/echo
 * instead of returning (exec has to end the other thread first); expect "exec ran".
 */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static void *spin(void *arg)
{
	volatile unsigned long n = 0;

	(void)arg;
	for (;;)
		n++;
	return 0;
}

int main(int argc, char **argv)
{
	pthread_t t;

	pthread_create(&t, 0, spin, 0);
	usleep(200000);
	if (argc > 1) {
		execl("/bin/echo", "echo", "exec ran", (char *)0);
		perror("execl");
		return 1;
	}
	printf("main returns\n");
	return 0;
}
