/* timeit N PROGRAM [ARGS...]: start PROGRAM N times one after the other (posix_spawn, wait), its output
 * thrown away; print the mean and the fastest wall time per run. */
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	int n = atoi(argv[1]);
	double sum = 0, best = 1e9;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
	for (int i = 0; i < n; i++) {
		pid_t pid;
		int status;
		double t = now();
		if (posix_spawn(&pid, argv[2], &fa, 0, argv + 2, environ) != 0) {
			perror("posix_spawn");
			return 1;
		}
		waitpid(pid, &status, 0);
		t = now() - t;
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			fprintf(stderr, "%s failed (status %d)\n", argv[2], status);
			return 1;
		}
		sum += t;
		if (t < best)
			best = t;
	}
	printf("%s: %d runs, mean %.1f ms, fastest %.1f ms\n", argv[2], n, sum / n, best);
	return 0;
}
