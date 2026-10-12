/*
 * A guest that outgrows the memory it starts with: rv's arena has to grow,
 * and move, several times while the program holds pointers into it, has a
 * thread parked in poll() on an array inside it, and forks.
 * Output does not depend on scheduling. Run by tools/thread-check.sh.
 */
#define _GNU_SOURCE
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define MB (1024 * 1024)
#define MAX_CHUNKS 64
#define CHUNK (4 * MB)

/* bigmem [PIECES [MAPPING_MIB]]: 40 pieces of 4 MiB and a 200 MiB mapping unless told otherwise. */
static int CHUNKS = 40;
static size_t BIG = 200;

static int fds[2];
static struct pollfd *parked;	/* on the heap: inside the arena that moves */

static void *poller(void *arg)
{
	char c = 0;
	int n = poll(parked, 1, 20000);

	(void)arg;
	if (n == 1 && read(fds[0], &c, 1) != 1)
		c = '?';
	return (void *)(intptr_t)(n * 256 + c);
}

static uint64_t pattern(int chunk, size_t i)
{
	return ((uint64_t)chunk << 40) ^ (i * 0x9e3779b97f4a7c15ULL);
}

int main(int argc, char **argv)
{
	static uint64_t *chunk[MAX_CHUNKS];
	pthread_t t;
	void *ret;
	long bad = 0, zero_bad = 0;
	uint8_t *big;
	int status;
	pid_t pid;

	if (argc > 1 && atoi(argv[1]) > 0 && atoi(argv[1]) <= MAX_CHUNKS)
		CHUNKS = atoi(argv[1]);
	if (argc > 2 && atoi(argv[2]) > 0)
		BIG = (size_t)atoi(argv[2]);
	if (pipe(fds))
		return 1;
	parked = malloc(sizeof(*parked));
	parked->fd = fds[0];
	parked->events = POLLIN;
	parked->revents = 0;
	pthread_create(&t, NULL, poller, NULL);
	usleep(20000);		/* let it park */

	/* Many pieces (160 MiB by default), each written on every page as it is made. */
	for (int c = 0; c < CHUNKS; c++) {
		chunk[c] = malloc(CHUNK);
		if (!chunk[c]) {
			printf("malloc failed at chunk %d\n", c);
			return 1;
		}
		for (size_t i = 0; i < CHUNK / 8; i += 512)
			chunk[c][i] = pattern(c, i);
	}
	/* All of it still there, after however many moves. */
	for (int c = 0; c < CHUNKS; c++)
		for (size_t i = 0; i < CHUNK / 8; i += 512)
			bad += chunk[c][i] != pattern(c, i);
	printf("heap: %d MiB in %d pieces, %ld words wrong\n", CHUNKS * (CHUNK / MB), CHUNKS, bad);

	/* One mapping larger than anything so far: fresh memory must read as zero. */
	big = mmap(NULL, BIG * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (big == MAP_FAILED) {
		printf("mmap of %zu MiB failed\n", BIG);
		return 1;
	}
	for (size_t i = 0; i < BIG * MB; i += 4096)
		zero_bad += big[i] != 0;
	big[0] = 1;
	big[BIG * MB - 1] = 2;
	printf("mapping: %zu MiB, %ld pages not zero, ends %d %d\n", BIG, zero_bad, big[0], big[BIG * MB - 1]);

	/* The thread parked before all this is woken through its array, which has moved. */
	if (write(fds[1], "w", 1) != 1)
		return 1;
	pthread_join(t, &ret);
	printf("parked thread: poll returned %ld, read '%c'\n", (long)(intptr_t)ret / 256, (char)((intptr_t)ret % 256));

	/* A fork takes all of it along. */
	pid = fork();
	if (pid == 0) {
		long wrong = 0;

		for (int c = 0; c < CHUNKS; c++)
			for (size_t i = 0; i < CHUNK / 8; i += 512)
				wrong += chunk[c][i] != pattern(c, i);
		_exit(wrong ? 1 : big[BIG * MB - 1] == 2 ? 42 : 2);
	}
	waitpid(pid, &status, 0);
	printf("forked child: exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);

	munmap(big, BIG * MB);
	for (int c = 0; c < CHUNKS; c++)
		free(chunk[c]);
	printf("done\n");
	return 0;
}
