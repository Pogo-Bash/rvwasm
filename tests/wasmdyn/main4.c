/* Children: a vfork-style child (clone with CLONE_VM | CLONE_VFORK, as posix_spawn and Qt's forkfd make
 * them) calls into a library loaded with dlopen before it exits; it runs on its parent's TLS, so the
 * library's thread-local variable it bumps is its parent's. Then posix_spawn of a program. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static int fails;
#define CHECK(c) do { if (c) printf("ok   %s\n", #c); else { printf("FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

static int (*add)(int, int);
static int (*bump)(void);

static int child(void *arg)
{
	(void)arg;
	int r = add(1, 2) + bump();	/* 45 + (parent's 7 + 1) */
	_exit(r);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "libfoo.so";
	void *h = dlopen(path, RTLD_NOW);
	if (!h) {
		printf("FAIL dlopen: %s\n", dlerror());
		return 1;
	}
	add = (int (*)(int, int))dlsym(h, "foo_add");
	bump = (int (*)(void))dlsym(h, "foo_tls_bump");
	static char stack[65536] __attribute__((aligned(16)));
	int pid = clone(child, stack + sizeof stack, CLONE_VM | CLONE_VFORK | SIGCHLD, 0);
	CHECK(pid > 0);
	int status = 0;
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 53);
	CHECK(bump() == 9);		/* the child's bump was on this thread's block */
	pid_t p;
	char *args[] = { "echo", "spawned", 0 };
	CHECK(posix_spawn(&p, "/bin/echo", 0, 0, args, environ) == 0 && waitpid(p, &status, 0) == p && status == 0);
	printf("main4: %s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
