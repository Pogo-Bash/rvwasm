/*
 * Code that a program makes while it runs, and changes: what a JavaScript
 * engine or a Java machine does all day. Every case makes a small function,
 * runs it until any translator has taken it up, changes it in one of the
 * ways there are, and runs it again: what runs must be the code that is
 * there. The output is the same on any correct system (qemu-riscv64 is the
 * reference), and under rv's translator it is the test of the rules at the
 * top of rv/jit.c. (riscv64, static, with threads.)
 *
 *   selfmod          all cases but "twice"
 *   selfmod NAME...  those cases
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE	4096UL
#define RWX	(PROT_READ | PROT_WRITE | PROT_EXEC)
#define ANON	(MAP_PRIVATE | MAP_ANONYMOUS)

typedef long (*fn_t)(long);

enum { ZERO = 0, RA = 1, T0 = 5, T1 = 6, T2 = 7, A0 = 10, A1 = 11 };

static uint32_t addi(int rd, int rs1, int imm)
{
	return ((uint32_t)(imm & 0xfff) << 20) | (uint32_t)rs1 << 15 | (uint32_t)rd << 7 | 0x13;
}

static uint32_t bge(int rs1, int rs2, int off)
{
	uint32_t o = (uint32_t)off;

	return ((o >> 12) & 1) << 31 | ((o >> 5) & 0x3f) << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 | 5u << 12 |
	       ((o >> 1) & 0xf) << 8 | ((o >> 11) & 1) << 7 | 0x63;
}

static uint32_t jal(int rd, int off)
{
	uint32_t o = (uint32_t)off;

	return ((o >> 20) & 1) << 31 | ((o >> 1) & 0x3ff) << 21 | ((o >> 11) & 1) << 20 | ((o >> 12) & 0xff) << 12 |
	       (uint32_t)rd << 7 | 0x6f;
}

#define RET	0x00008067u	/* jalr x0, ra, 0 */
#define FENCE_I	0x0000100fu
#define SUM_SIZE 32
#define SUM_K	12		/* where in it the instruction with k is */

/*
 * long sum(long n) { long s = 0; for (long i = 0; i < n; i++) s += k; return s; }
 * as eight instructions at `at`, which need not be a multiple of four.
 */
static void emit_sum(void *at, int k)
{
	uint32_t code[SUM_SIZE / 4] = {
		addi(T0, ZERO, 0),
		addi(T1, ZERO, 0),
		bge(T1, A0, 16),
		addi(T0, T0, k),
		addi(T1, T1, 1),
		jal(ZERO, -12),
		addi(A0, T0, 0),
		RET,
	};

	memcpy(at, code, sizeof(code));
}

/* Put another k into a function made by emit_sum(): one instruction, written in one piece if it is aligned. */
static void patch_k(void *at, int k)
{
	uint32_t insn = addi(T0, T0, k);
	uint8_t *where = (uint8_t *)at + SUM_K;

	if ((uintptr_t)where % 4 == 0)
		__atomic_store_n((uint32_t *)(void *)where, insn, __ATOMIC_SEQ_CST);
	else
		memcpy(where, &insn, 4);
}

static void flush(void *from, unsigned long len)
{
	syscall(SYS_riscv_flush_icache, from, (char *)from + len, 0UL);
}

static void fence_i(void)
{
	__asm__ volatile(".4byte 0x0000100f");
}

static fn_t fn(void *at)
{
	return (fn_t)at;
}

/* Run it often enough for a translator to take it up; the sum of what it returned. */
static long hot(void *at, long n)
{
	long total = 0;

	for (int i = 0; i < 8; i++)
		total += fn(at)(n);
	return total;
}

static void *map(unsigned long len, int prot)
{
	void *p = mmap(NULL, len, prot, ANON, -1, 0);

	if (p == MAP_FAILED) {
		printf("mmap: %s\n", strerror(errno));
		exit(1);
	}
	return p;
}

/* A page that can be written and executed at once, the code changed where it is. */
static void t_rwx(void)
{
	uint8_t *p = map(PAGE, RWX);

	emit_sum(p, 3);
	flush(p, SUM_SIZE);
	printf("rwx: %ld", hot(p, 1000));
	patch_k(p, 5);
	flush(p, SUM_SIZE);
	printf(" %ld", hot(p, 1000));
	/* Told to the processor only, as a program with one thread may. */
	patch_k(p, 7);
	fence_i();
	printf(" %ld", hot(p, 1000));
	/* Many times over. */
	{
		long total = 0;

		for (int k = 1; k <= 300; k++) {
			patch_k(p, k);
			flush(p, SUM_SIZE);
			total += fn(p)(400);
		}
		printf(" %ld\n", total);
	}
	munmap(p, PAGE);
}

/* Written while it may not be executed, executed while it may not be written. */
static void t_wx(void)
{
	uint8_t *p = map(2 * PAGE, PROT_READ | PROT_WRITE);
	long total = 0;

	emit_sum(p, 2);
	emit_sum(p + PAGE, 20);
	mprotect(p, 2 * PAGE, PROT_READ | PROT_EXEC);
	flush(p, 2 * PAGE);
	printf("wx: %ld %ld", hot(p, 1000), hot(p + PAGE, 1000));
	for (int k = 3; k < 200; k++) {
		/* One page changes; the other is looked at the same way, and left alone. */
		mprotect(p, 2 * PAGE, PROT_READ | PROT_WRITE);
		patch_k(p, k);
		mprotect(p, 2 * PAGE, PROT_READ | PROT_EXEC);
		flush(p, SUM_SIZE);
		total += fn(p)(300) + fn(p + PAGE)(300);
	}
	printf(" %ld", total);
	/* Protections that leave it executable, and nothing changed. */
	mprotect(p, 2 * PAGE, RWX);
	total = hot(p, 500);
	mprotect(p, 2 * PAGE, PROT_READ | PROT_EXEC);
	total += hot(p, 500);
	mprotect(p, 2 * PAGE, PROT_EXEC);
	total += hot(p + PAGE, 500);
	printf(" %ld\n", total);
	munmap(p, 2 * PAGE);
}

static sigjmp_buf jump;

static void on_fault(int sig)
{
	(void)sig;
	siglongjmp(jump, 1);
}

/* Code that may no longer be executed is not, however often it ran before. */
static void t_noexec(void)
{
	uint8_t *p = map(PAGE, RWX);
	struct sigaction act, old;
	volatile int faulted = 0;
	long r = 0;

	memset(&act, 0, sizeof(act));
	act.sa_handler = on_fault;
	sigaction(SIGSEGV, &act, &old);
	emit_sum(p, 4);
	flush(p, SUM_SIZE);
	r = hot(p, 1000);
	mprotect(p, PAGE, PROT_READ | PROT_WRITE);
	if (sigsetjmp(jump, 1))
		faulted = 1;
	else
		r += fn(p)(10);
	mprotect(p, PAGE, PROT_READ | PROT_EXEC);
	printf("noexec: %ld faulted=%d %ld\n", r, faulted, hot(p, 1000));
	sigaction(SIGSEGV, &old, NULL);
	munmap(p, PAGE);
}

/* Code next to code that changes, on the same page and on the next, and next to data that is written all the time. */
static void t_neighbours(void)
{
	uint8_t *p = map(2 * PAGE, RWX);
	volatile long *counter = (volatile long *)(void *)(p + 2048);
	long total = 0;

	emit_sum(p, 1);			/* stays */
	emit_sum(p + 256, 2);		/* changes, on the page of the first */
	emit_sum(p + PAGE, 3);		/* changes, on the next page */
	flush(p, 2 * PAGE);
	printf("neighbours: %ld %ld %ld", hot(p, 1000), hot(p + 256, 1000), hot(p + PAGE, 1000));
	for (int k = 4; k < 400; k++) {
		patch_k(k & 1 ? p + 256 : p + PAGE, k);
		flush(p, 2 * PAGE);
		total += fn(p)(200) + fn(p + 256)(200) + fn(p + PAGE)(200);
	}
	printf(" %ld", total);
	/* New code put behind the old, as a compiler fills a page. */
	total = 0;
	for (int i = 2; i < 60; i++) {
		emit_sum(p + i * 64, i);
		flush(p + i * 64, SUM_SIZE);
		for (int j = 0; j <= i; j++)
			total += fn(p + j * 64 + (j == 1 ? 192 : 0))(100);
	}
	printf(" %ld", total);
	/* Data on the code's page, written between any two calls. */
	total = 0;
	*counter = 0;
	for (int i = 0; i < 3000; i++) {
		*counter += i;
		total += fn(p)(60) + *counter;
	}
	printf(" %ld\n", total);
	munmap(p, 2 * PAGE);
}

/* A function whose instructions lie across the end of a page, one of them in both pages. */
static void t_straddle(void)
{
	uint8_t *p = map(2 * PAGE, RWX);
	uint8_t *at = p + PAGE - SUM_K - 2;	/* the instruction with k: two bytes in each page */
	long total;

	emit_sum(at, 6);
	flush(at, SUM_SIZE);
	total = hot(at, 1000);
	for (int k = 7; k < 120; k++) {
		patch_k(at, k);
		flush(at, SUM_SIZE);
		total += fn(at)(250);
	}
	printf("straddle: %ld\n", total);
	munmap(p, 2 * PAGE);
}

/* The addresses given back, and other code put where the old was. */
static void t_remap(void)
{
	uint8_t *p = map(4 * PAGE, RWX), *q;
	long a, b, c, d;

	emit_sum(p, 3);
	flush(p, SUM_SIZE);
	a = hot(p, 1000);
	munmap(p, 4 * PAGE);
	q = mmap(p, 4 * PAGE, RWX, ANON | MAP_FIXED, -1, 0);
	emit_sum(q, 9);
	flush(q, SUM_SIZE);
	b = hot(q, 1000);
	/* A new mapping right over the old one. */
	q = mmap(p, 4 * PAGE, RWX, ANON | MAP_FIXED, -1, 0);
	emit_sum(q, 11);
	flush(q, SUM_SIZE);
	c = hot(q, 1000);
	/* The memory thrown away, the mapping kept. */
	madvise(q, 4 * PAGE, MADV_DONTNEED);
	emit_sum(q, 13);
	flush(q, SUM_SIZE);
	d = hot(q, 1000);
	printf("remap: %d %ld %ld %ld %ld", q == p, a, b, c, d);
	munmap(q, 4 * PAGE);
	/* The same code somewhere else (it does not care where it is), and changed there. */
	p = map(PAGE, RWX);
	emit_sum(p, 15);
	flush(p, SUM_SIZE);
	a = hot(p, 1000);
	q = mremap(p, PAGE, 64 * PAGE, MREMAP_MAYMOVE);
	if (q == MAP_FAILED) {
		printf(" mremap: %s\n", strerror(errno));
		return;
	}
	b = hot(q, 1000);
	patch_k(q, 17);
	flush(q, SUM_SIZE);
	printf(" %ld %ld %ld\n", a, b, hot(q, 1000));
	munmap(q, 64 * PAGE);
}

/* Code that the kernel writes: read() into the page the code is on. */
static void t_syscall(void)
{
	uint8_t *p = map(PAGE, RWX);
	uint8_t code[SUM_SIZE];
	int fds[2];
	long total;

	emit_sum(p, 2);
	flush(p, SUM_SIZE);
	total = hot(p, 1000);
	if (pipe(fds)) {
		printf("pipe: %s\n", strerror(errno));
		exit(1);
	}
	for (int k = 3; k < 40; k++) {
		emit_sum(code, k);
		if (write(fds[1], code, sizeof(code)) != sizeof(code) || read(fds[0], p, sizeof(code)) != sizeof(code)) {
			printf("pipe: %s\n", strerror(errno));
			exit(1);
		}
		flush(p, SUM_SIZE);
		total += fn(p)(500);
	}
	close(fds[0]);
	close(fds[1]);
	printf("syscall: %ld\n", total);
	munmap(p, PAGE);
}

/*
 * The same memory mapped twice: written through one mapping, executed
 * through the other. (Not one of the cases run by default: qemu-riscv64, as
 * a program that runs one program, does not see such writes and goes on
 * running the first code. What is right is known all the same: 560000.)
 */
static void t_twice(void)
{
	int fd = (int)syscall(SYS_memfd_create, "code", 0UL);
	uint8_t *w, *x;
	long total;

	if (fd < 0 || ftruncate(fd, PAGE)) {
		printf("twice: no file in memory (%s)\n", strerror(errno));
		return;
	}
	w = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	x = mmap(NULL, PAGE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
	if (w == MAP_FAILED || x == MAP_FAILED) {
		printf("twice: cannot map it (%s)\n", strerror(errno));
		return;
	}
	emit_sum(w, 4);
	flush(x, SUM_SIZE);
	total = hot(x, 1000);
	for (int k = 5; k < 60; k++) {
		patch_k(w, k);
		flush(x, SUM_SIZE);
		total += fn(x)(300);
	}
	printf("twice: %ld\n", total);
	munmap(w, PAGE);
	munmap(x, PAGE);
	close(fd);
}

/*
 * A function that changes itself: it writes the instruction it is about to
 * execute, in the same straight piece of code.
 *
 *   long f(long x, long insn) { [next instruction] = insn; fence.i; a0 = a0 + ?; return a0; }
 */
static void t_itself(void)
{
	uint8_t *p = map(PAGE, RWX);
	uint32_t code[] = {
		0x00000397,			/* auipc t2, 0 */
		0x00b3a623,			/* sw a1, 12(t2) */
		FENCE_I,
		addi(A0, A0, 1),		/* written over before it runs */
		RET,
	};
	long (*f)(long, long) = (long (*)(long, long))(void *)p;
	long total = 0;

	memcpy(p, code, sizeof(code));
	flush(p, sizeof(code));
	for (int i = 0; i < 2000; i++)
		total += f(i, addi(A0, A0, i % 7 + 1));
	printf("itself: %ld\n", total);
	munmap(p, PAGE);
}

/* Many functions on many pages, all changed at once now and then: more than a translator keeps in its head at once. */
static void t_many(void)
{
	enum { COUNT = 1500, STEP = 96 };
	uint8_t *p = map((COUNT * STEP + PAGE) & ~(PAGE - 1), RWX);
	long total = 0;

	for (int round = 0; round < 4; round++) {
		for (int i = 0; i < COUNT; i++)
			emit_sum(p + i * STEP, (i + round * 7) % 1000 + 1);
		flush(p, COUNT * STEP);
		for (int i = 0; i < COUNT; i++)
			total += fn(p + i * STEP)(260);
		/* (and again without a change in between) */
		for (int i = 0; i < COUNT; i += 3)
			total += fn(p + i * STEP)(60);
	}
	printf("many: %ld\n", total);
	munmap(p, (COUNT * STEP + PAGE) & ~(PAGE - 1));
}

/* fork(): the child changes its code, the parent's stays. */
static void t_fork(void)
{
	uint8_t *p = map(PAGE, RWX);
	long before, after;
	int status = 0;
	pid_t pid;

	emit_sum(p, 3);
	flush(p, SUM_SIZE);
	before = hot(p, 1000);
	pid = fork();
	if (pid == 0) {
		long r = hot(p, 1000);

		patch_k(p, 5);
		flush(p, SUM_SIZE);
		_exit(r == 24000 && hot(p, 1000) == 40000 ? 42 : 1);
	}
	waitpid(pid, &status, 0);
	after = hot(p, 1000);
	printf("fork: %ld %ld child=%d\n", before, after, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	munmap(p, PAGE);
}

/*
 * One thread changes the code while another runs it. The one running may
 * get the old code or the new while the change is made; but once the flush
 * has returned, a call that begins is a call of the new code, and nothing
 * ever goes back to code that was there before.
 */
#define RING	64
static uint8_t *shared_code;
static volatile long calls, results[RING];
static volatile int stop, went_back;

static void *runner(void *arg)
{
	long last = 0;

	(void)arg;
	while (!stop) {
		long r = fn(shared_code)(100);

		if (r < last)
			went_back = 1;
		last = r;
		results[calls % RING] = r;
		__atomic_store_n(&calls, calls + 1, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

static long wait_calls(long until)
{
	while (__atomic_load_n(&calls, __ATOMIC_SEQ_CST) < until)
		sched_yield();
	return until;
}

static void t_threads(void)
{
	pthread_t thread;
	int wrong = 0;

	shared_code = map(PAGE, RWX);
	emit_sum(shared_code, 1);
	flush(shared_code, SUM_SIZE);
	stop = went_back = 0;
	calls = 0;
	if (pthread_create(&thread, NULL, runner, NULL)) {
		printf("threads: cannot make one\n");
		exit(1);
	}
	wait_calls(300);
	for (int k = 2; k <= 250; k++) {
		long seen;

		patch_k(shared_code, k);
		flush(shared_code, SUM_SIZE);
		/* The call under way may be of either; the one after it began after the flush. */
		seen = __atomic_load_n(&calls, __ATOMIC_SEQ_CST);
		wait_calls(seen + (k % 10 ? 3 : 300));
		if (results[(seen + 1) % RING] != 100L * k || results[(seen + 2) % RING] != 100L * k)
			wrong++;
	}
	stop = 1;
	pthread_join(thread, NULL);
	printf("threads: wrong=%d went_back=%d last=%ld mine=%ld\n", wrong, went_back,
	       results[(calls - 1) % RING], fn(shared_code)(100));
	munmap(shared_code, PAGE);
}

/* ... and a thread that makes its own code next to the code another is running, each on the same pages. */
static void *maker(void *arg)
{
	uint8_t *p = arg;
	long total = 0;

	for (int k = 1; k <= 200; k++) {
		emit_sum(p, k);
		flush(p, SUM_SIZE);
		total += fn(p)(150);
	}
	return (void *)total;
}

static void t_makers(void)
{
	enum { THREADS = 4 };
	uint8_t *p = map(PAGE, RWX);
	pthread_t threads[THREADS];
	long total = 0;

	for (int i = 0; i < THREADS; i++)
		pthread_create(&threads[i], NULL, maker, p + i * 512);
	for (int i = 0; i < THREADS; i++) {
		void *r;

		pthread_join(threads[i], &r);
		total += (long)r;
	}
	printf("makers: %ld\n", total);
	munmap(p, PAGE);
}

static const struct {
	const char *name;
	void (*run)(void);
} tests[] = {
	{ "rwx", t_rwx }, { "wx", t_wx }, { "noexec", t_noexec }, { "neighbours", t_neighbours },
	{ "straddle", t_straddle }, { "remap", t_remap }, { "syscall", t_syscall }, { "twice", t_twice },
	{ "itself", t_itself }, { "many", t_many }, { "fork", t_fork }, { "threads", t_threads },
	{ "makers", t_makers },
};

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int wanted = argc < 2 && strcmp(tests[i].name, "twice");

		for (int a = 1; a < argc; a++)
			wanted |= !strcmp(argv[a], tests[i].name);
		if (wanted)
			tests[i].run();
	}
	return 0;
}
