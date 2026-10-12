/*
 * Strings from high memory, handed to the kernel: a native Linux/Wasm
 * program (tools/wasm-cc), whose pointers are the machine's own addresses.
 * Takes memory in pieces of 64 MiB until it has one at or above LIMIT
 * (default 3 GiB), copies a path and arguments there, and calls open() and
 * execve() with them: the kernel reads such strings with
 * strncpy_from_user() and strnlen_user(), which refuse any at TASK_SIZE or
 * above (EFAULT). The pieces below the first high one are kept so that it
 * stays high.
 *
 *   highstr [LIMIT_MIB]	prints "highstr: open ok" and then what the
 *				program it execs (busybox echo) prints, or
 *				what failed.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PIECE	(64UL << 20)

int main(int argc, char **argv)
{
	uint64_t limit = (argc > 1 ? strtoull(argv[1], NULL, 0) : 3072) << 20;
	char *high = NULL;
	int pieces = 0;

	while (pieces < 512) {
		char *p = mmap(NULL, PIECE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p == MAP_FAILED) {
			printf("highstr: no piece above 0x%llx after %d pieces (errno %d)\n",
			       (unsigned long long)limit, pieces, errno);
			return 1;
		}
		pieces++;
		if ((uintptr_t)p >= limit) {
			high = p + PIECE - 4096;
			break;
		}
	}
	if (!high)
		return 1;
	printf("highstr: a piece at 0x%llx after %d pieces\n", (unsigned long long)(uintptr_t)high, pieces);

	char *path = high, *arg0 = high + 256, *arg1 = high + 512, *arg2 = high + 768, *env0 = high + 1024;

	strcpy(path, "/bin/busybox");
	strcpy(arg0, "echo");
	strcpy(arg1, "highstr: execve ok, argument from");
	strcpy(arg2, "high memory");
	strcpy(env0, "HIGHSTR=1");
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		printf("highstr: open of a path at 0x%llx FAILED (errno %d)\n", (unsigned long long)(uintptr_t)path, errno);
		return 1;
	}
	close(fd);
	printf("highstr: open ok\n");
	fflush(stdout);

	char **args = (char **)(high + 2048);

	args[0] = arg0;
	args[1] = arg1;
	args[2] = arg2;
	args[3] = NULL;
	char **env = args + 4;

	env[0] = env0;
	env[1] = NULL;
	execve(path, args, env);
	printf("highstr: execve with a path and arguments at 0x%llx FAILED (errno %d)\n",
	       (unsigned long long)(uintptr_t)path, errno);
	return 1;
}
