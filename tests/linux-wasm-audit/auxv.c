/*
 * The auxiliary vector binfmt_wasm gives a program. Run as root: the program sets its effective group to 5 and
 * executes itself, which then compares AT_EGID with getegid(), and says whether AT_RANDOM is there.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/auxv.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc == 1) {
		if (setresgid(-1, 5, -1)) { perror("setresgid"); return 1; }
		execl("/proc/self/exe", argv[0], "child", (char *)0);
		execl(argv[0], argv[0], "child", (char *)0);
		perror("exec");
		return 1;
	}
	printf("getgid %u getegid %u; AT_GID %lu AT_EGID %lu (%s); AT_RANDOM %#lx (%s)\n", getgid(), getegid(),
	       getauxval(AT_GID), getauxval(AT_EGID), getauxval(AT_EGID) == getegid() ? "ok" : "WRONG",
	       getauxval(AT_RANDOM), getauxval(AT_RANDOM) ? "ok" : "missing");
	return 0;
}
