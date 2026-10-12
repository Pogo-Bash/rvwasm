/*
 * System call filters: errors, the SIGSYS trap with a handler that answers
 * the call, arguments, stacking, threads (with and without TSYNC), fork,
 * exec, killing, and the old strict mode. The expectations are a real
 * kernel's (tools/seccomp-reference.sh runs this on one). riscv64, static.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define LOAD(field)	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, field))
#define IF_NR(nr, ret)	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1), BPF_STMT(BPF_RET | BPF_K, ret)

/*
 * A long filter: mostly loads that come to nothing, then getegid() fails
 * with 66. Eight of them on top of the three below come to 32059 of the
 * 32768 instructions a thread's filters may have together (each counting 4
 * more than its length), so a ninth is refused.
 */
#define BIG_LEN	3999
static struct sock_filter big[BIG_LEN];
static void make_big(void)
{
	for (unsigned i = 0; i < BIG_LEN - 3; i++)
		big[i] = (struct sock_filter)LOAD(nr);
	big[BIG_LEN - 3] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getegid, 0, 1);
	big[BIG_LEN - 2] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 66);
	big[BIG_LEN - 1] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
}

static void check(int ok, const char *what)
{
	printf("%s: %s\n", what, ok ? "ok" : "FAILED");
	fflush(stdout);
	if (!ok)
		exit(1);
}

static int install(struct sock_filter *insns, unsigned len, unsigned flags)
{
	struct sock_fprog prog = { .len = (unsigned short)len, .filter = insns };

	return (int)syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, flags, &prog);
}

static volatile int trap_nr, trap_code, trap_errno;
static volatile unsigned trap_arch;
static volatile unsigned long trap_addr;

static void on_sigsys(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;

	trap_nr = info->si_syscall;
	trap_code = info->si_code;
	trap_errno = info->si_errno;
	trap_arch = info->si_arch;
	trap_addr = (unsigned long)info->si_call_addr;
	/* The call's answer, from us: a0 of the interrupted program. And it goes on after the call. */
	uc->uc_mcontext.__gregs[10] = 1234 + uc->uc_mcontext.__gregs[10];
}

static void *thread_getppid(void *arg)
{
	errno = 0;
	return (void *)(long)(syscall(SYS_getppid) == -1 ? errno : 0);
}

static volatile int go;
static void *thread_waits(void *arg)
{
	while (!go)
		usleep(1000);
	errno = 0;
	return (void *)(long)(syscall(SYS_gettid) == -1 ? errno : 0);
}

int main(int argc, char **argv)
{
	struct sock_filter first[] = {
		LOAD(arch),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_RISCV64, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
		LOAD(nr),
		IF_NR(SYS_getppid, SECCOMP_RET_ERRNO | 42),
		IF_NR(SYS_uname, SECCOMP_RET_TRAP | 7),
		IF_NR(SYS_sync, SECCOMP_RET_KILL_PROCESS),
		IF_NR(SYS_getpgid, SECCOMP_RET_KILL_THREAD),
		/* write(fd 9, ...): by its first argument */
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 0, 3),
		LOAD(args[0]),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 9, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 77),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_filter second[] = {
		LOAD(nr),
		IF_NR(SYS_getpid, SECCOMP_RET_ERRNO | EPERM),
		IF_NR(SYS_getppid, SECCOMP_RET_ALLOW),	/* the first one's answer weighs more */
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_filter third[] = {
		LOAD(nr),
		IF_NR(SYS_gettid, SECCOMP_RET_ERRNO | 5),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_filter bad[] = { BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 2), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW) };
	struct sigaction sa = { .sa_sigaction = on_sigsys, .sa_flags = SA_SIGINFO };
	pthread_t t, waiting;
	void *value;
	unsigned action;
	int status;
	pid_t pid;

	if (argc > 1 && !strcmp(argv[1], "after-exec")) {
		/* The program this one was executed by had filters: they are ours now. */
		errno = 0;
		return syscall(SYS_getppid) == -1 && errno == 42 && syscall(SYS_getpid) == -1 && errno == EPERM &&
		       prctl(PR_GET_SECCOMP) == 2 ? 0 : 3;
	}
	if (argc > 1 && !strcmp(argv[1], "after-exec-long")) {
		/* The same, with the chain filled to the limit: all of it is here (a long filter more is still one too many, a short one fits). */
		make_big();
		errno = 0;
		if (!(syscall(SYS_getppid) == -1 && errno == 42 && syscall(SYS_getegid) == -1 && errno == 66))
			return 3;
		errno = 0;
		if (!(install(big, BIG_LEN, 0) == -1 && errno == ENOMEM))
			return 8;
		return install(third, sizeof(third) / sizeof(third[0]), 0) == 0 ? 0 : 9;
	}

	check(prctl(PR_GET_SECCOMP) == 0, "no filter to begin with");
	check(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, NULL) == -1 && errno == EFAULT, "no program: a bad address");
	check(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, NULL) == -1 && errno == EFAULT, "the same by prctl");
	if (geteuid() != 0)
		check(install(third, sizeof(third) / sizeof(third[0]), 0) == -1 && errno == EACCES, "not before the promise of no new privileges");
	check(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0, "the promise");
	check(install(bad, 2, 0) == -1 && errno == EINVAL, "a program that reads half a word is refused");
	action = SECCOMP_RET_TRAP;
	check(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) == 0, "trap is on offer");
	action = 0x12340000;
	check(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) == -1 && errno == EOPNOTSUPP, "an unknown answer is not");

	sigaction(SIGSYS, &sa, NULL);
	check(pthread_create(&waiting, NULL, thread_waits, NULL) == 0, "a thread, before any filter");
	check(install(first, sizeof(first) / sizeof(first[0]), 0) == 0, "the first filter");
	check(prctl(PR_GET_SECCOMP) == 2, "mode 2");
	errno = 0;
	check(syscall(SYS_getppid) == -1 && errno == 42, "an error of the filter's choosing");
	check(syscall(SYS_write, 9, "x", 1) == -1 && errno == 77, "by an argument");
	check(syscall(SYS_write, 1, "", 0) == 0, "and not otherwise");
	check(syscall(SYS_uname, 100) == 1334, "a trap, answered by the handler");
	check(trap_nr == SYS_uname && trap_code == 1 /* SYS_SECCOMP */ && trap_errno == 7 && trap_arch == AUDIT_ARCH_RISCV64 && trap_addr,
	      "what the handler was told");
	check(syscall(SYS_getuid) == (long)getuid(), "other calls as before");

	check(install(second, sizeof(second) / sizeof(second[0]), 0) == 0, "a second filter");
	errno = 0;
	check(syscall(SYS_getpid) == -1 && errno == EPERM, "its error");
	check(syscall(SYS_getppid) == -1 && errno == 42, "the first one still counts");

	check(pthread_create(&t, NULL, thread_getppid, NULL) == 0 && pthread_join(t, &value) == 0 && (long)value == 42, "a new thread has them");

	/* Without TSYNC a filter is the calling thread's; with it, every thread's. */
	check(install(third, sizeof(third) / sizeof(third[0]), SECCOMP_FILTER_FLAG_TSYNC) == 0, "a third, for all threads");
	go = 1;
	check(pthread_join(waiting, &value) == 0 && (long)value == 5, "the thread from before has it");

	pid = fork();
	if (pid == 0)
		_exit(syscall(SYS_getppid) == -1 && errno == 42 ? 0 : 2);
	check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "a child has them");

	pid = fork();
	if (pid == 0) {
		execl("/proc/self/exe", argv[0], "after-exec", (char *)NULL);
		_exit(4);
	}
	check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "and the program a child executes");

	/* A chain as long as they get, in a child: the ninth long filter is too many, and all of it goes along to the program it executes. */
	pid = fork();
	if (pid == 0) {
		make_big();
		for (int i = 0; i < 8; i++)
			if (install(big, BIG_LEN, 0))
				_exit(11);
		errno = 0;
		if (!(install(big, BIG_LEN, 0) == -1 && errno == ENOMEM))
			_exit(12);
		errno = 0;
		if (!(syscall(SYS_getegid) == -1 && errno == 66))
			_exit(13);
		execl("/proc/self/exe", argv[0], "after-exec-long", (char *)NULL);
		_exit(4);
	}
	check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "a chain filled to the limit goes along whole");

	/*
	 * With no descriptor to spare, the filters may have no way to travel.
	 * Then the exec must not happen at all (an emulator refuses it with
	 * EACCES); a kernel needs no descriptor, and the program runs with
	 * them. Never the program without them.
	 */
	pid = fork();
	if (pid == 0) {
		struct rlimit few = { 3, 3 };

		setrlimit(RLIMIT_NOFILE, &few);
		execl("/proc/self/exe", argv[0], "after-exec", (char *)NULL);
		_exit(errno == EACCES ? 10 : 4);
	}
	check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 10),
	      "starved of descriptors: the filters come along, or the program does not run");

	pid = fork();
	if (pid == 0) {
		syscall(SYS_sync);
		_exit(5);
	}
	check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS, "killed by the filter");

	pid = fork();
	if (pid == 0) {
		signal(SIGSYS, SIG_DFL);
		syscall(SYS_uname, 0);
		_exit(6);
	}
	check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS, "a trap nobody handles kills");

	pid = fork();
	if (pid == 0) {
		/* One thread of a process with one thread: the process. */
		syscall(SYS_getpgid, 0);
		_exit(7);
	}
	check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS, "kill the thread");

	puts("seccomp: all ok");
	return 0;
}
