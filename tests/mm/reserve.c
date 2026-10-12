/*
 * A reservation: 16 GiB of addresses that nothing is in, of which parts are
 * made memory, used, given back, and trimmed, the way a WebAssembly runtime
 * does for a sandbox's memory. (riscv64, static.)
 */
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define GIB (1UL << 30)

static sigjmp_buf jump;
static volatile unsigned long fault_at;

static void on_fault(int sig, siginfo_t *info, void *context)
{
	fault_at = (unsigned long)info->si_addr;
	siglongjmp(jump, 1);
}

static void check(int ok, const char *what)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAILED");
	if (!ok)
		exit(1);
}

static int faults(volatile unsigned char *p)
{
	fault_at = 0;
	if (sigsetjmp(jump, 1))
		return fault_at == (unsigned long)p;
	*p = 1;
	return 0;
}

int main(void)
{
	struct sigaction sa = { .sa_sigaction = on_fault, .sa_flags = SA_SIGINFO };
	unsigned char *all, *mem, *second;
	char name[32] = "reserve.tmp";
	unsigned long sum = 0;
	int fd;

	sigaction(SIGSEGV, &sa, NULL);
	all = mmap(NULL, 16 * GIB + 4 * GIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	check(all != MAP_FAILED, "20 GiB of addresses");
	/* Cut down to 16 GiB at a multiple of 4 GiB. */
	mem = (unsigned char *)(((unsigned long)all + 4 * GIB - 1) & ~(4 * GIB - 1));
	if (mem > all)
		check(munmap(all, (unsigned long)(mem - all)) == 0, "the part before");
	check(munmap(mem + 16 * GIB, (unsigned long)(all + 20 * GIB - (mem + 16 * GIB))) == 0 || mem == all, "the part after");
	check(faults(mem) && faults(mem + 5 * GIB), "nothing is there");

	check(mprotect(mem, 65536, PROT_READ | PROT_WRITE) == 0, "64 KiB of memory");
	check(mem[0] == 0 && mem[65535] == 0, "zeros");
	for (unsigned i = 0; i < 65536; i++)
		mem[i] = (unsigned char)(i * 7);
	check(faults(mem + 65536), "and nothing after it");
	/* It grows, many times. */
	for (unsigned long size = 131072; size <= 64 * 1048576UL; size *= 2) {
		if (mprotect(mem, size, PROT_READ | PROT_WRITE))
			check(0, "more memory");
		mem[size - 1] = 0x5a;
	}
	for (unsigned i = 0; i < 65536; i++)
		sum += mem[i] == (unsigned char)(i * 7);
	check(sum == 65536 && mem[131071] == 0x5a && mem[64 * 1048576UL - 1] == 0x5a, "what was there is there after growing");

	/* A system call with a buffer there, and a string. */
	fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0600);
	strcpy((char *)mem + 4096, name);
	check(fd >= 0 && write(fd, mem, 65536) == 65536 && pread(fd, mem + 1048576, 65536, 0) == 65536, "write and read");
	check(!memcmp(mem, mem + 1048576, 65536), "the same bytes");
	check(access((char *)mem + 4096, F_OK) == 0, "a name");
	unlink(name);

	/* Far into it, and a second reservation. */
	check(mprotect(mem + 6 * GIB, 8192, PROT_READ | PROT_WRITE) == 0, "memory 6 GiB in");
	memcpy(mem + 6 * GIB + 4090, "straddle", 8);
	check(!memcmp(mem + 6 * GIB + 4090, "straddle", 8) && mem[64 * 1048576UL - 1] == 0x5a, "written across two pages");
	second = mmap(NULL, 8 * GIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(second != MAP_FAILED && (second + 8 * GIB <= mem || second >= mem + 16 * GIB), "a second one, elsewhere");
	check(mmap(second + GIB, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == second + GIB, "a mapping placed in it");
	second[GIB + 5000] = 9;
	check(second[GIB + 5000] == 9 && faults(second + GIB + 8192), "used");

	check(madvise(mem, 65536, MADV_DONTNEED) == 0 && mem[100] == 0 && mem[131071] == 0x5a, "madvise");
	check(mprotect(mem, 65536, PROT_NONE) == 0 && faults(mem + 10), "taken away again");
	check(munmap(mem, 16 * GIB) == 0 && munmap(second, 8 * GIB) == 0, "unmapped");
	check(faults(mem + 131071), "gone");
	all = mmap(NULL, 16 * GIB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(all != MAP_FAILED && mprotect(all, 4096, PROT_READ | PROT_WRITE) == 0 && all[0] == 0, "and once more");
	puts("reserve: all ok");
	return 0;
}
