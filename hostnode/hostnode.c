/*
 * hostnode: run a Node.js program on the JavaScript engine of the browser
 * the machine runs in, as a process of this machine.
 *
 * A task on Linux/Wasm is a Web Worker, and a program is normally a Wasm
 * module instantiated in it. This program is such a module only to get
 * started: it reads the JavaScript that makes up a Node.js (boot.js, next to
 * Node's own library, in HOSTNODE_HOME) and hands it to the host with
 * __run_js (tools/patch-runtime.py, item 11), which runs it as what this task
 * does from then on. The JavaScript makes the system calls itself. This
 * module stays instantiated under it and is its C library: memory for system
 * call arguments, starting other programs, looking up host names.
 * Everything here is exported to the JavaScript (the linker exports all).
 *
 * What runs this way has the whole machine in its reach, as every native
 * program has on a kernel without memory protection: it is not a way to run
 * JavaScript one does not trust.
 *
 * Where the lines are:
 *
 * - The JavaScript (boot.js and what it loads) is given the machine's whole
 *   linear memory and the kernel's system call entries. It can read and
 *   write every byte of every process and of the kernel. So can any native
 *   program on this kernel (no MMU), so nothing is lost; but the program
 *   hostnode runs (a Node.js program) runs in the same JavaScript realm and
 *   has the same reach, through the runtime's own top-level variables if
 *   nothing else. Run under hostnode only what you would run as a native
 *   program of this machine, which is what it is.
 *
 * - Other processes of the guest reach hostnode only the way they reach any
 *   process: files it reads, its environment and arguments, signals, pipes
 *   and sockets. Nothing the guest writes is ever taken for an address.
 *   Every address the JavaScript hands to a system call is one of its own:
 *   its scratch blocks, what it allocated with this C library's malloc, or
 *   what this file set up for it (struct hn_start, the results of
 *   hn_lookup). Data from the guest (a file's bytes, a Buffer of the
 *   program's) is copied into those places, never pointed at.
 *
 * - Node.js bindings that take a position or a length (fs.read into a
 *   Buffer at an offset) take positions within JavaScript objects, which the
 *   engine checks; they never become machine addresses.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* The host's: run JavaScript text as this task (does not return). */
extern void __run_js(const char *text, unsigned long length, void *arg);

/* What the JavaScript is told at its start. */
struct hn_start {
	long argc;
	char **argv;
	char **envp;
	const char *home;	/* where boot.js was found */
};

static struct hn_start start;

/*
 * Signals. The JavaScript does not have handlers for them: it blocks the
 * ones it wants to hear of and reads them from a descriptor (signalfd), like
 * everything else its loop waits for. (A handler would have to be a function
 * of this Wasm program, called by the kernel when a system call returns; and
 * as things are, of several signals that are pending at once only one gets
 * its handler called: probes/multi_signal_probe.c.) What is left to do here
 * is ignoring a signal or not.
 */
long hn_signal(int sig, int ignore)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = ignore ? SIG_IGN : SIG_DFL;
	return sigaction(sig, &sa, NULL) ? -errno : 0;
}

/*
 * Starting a program. The child shares this memory and has the parent wait
 * (CLONE_VM | CLONE_VFORK: there is no fork on this kernel), so it does
 * nothing but arrange its descriptors and exec.
 */
struct hn_spawn {
	const char *file;	/* looked up in the PATH of envp if it has no slash */
	char **argv;
	char **envp;
	const char *cwd;	/* or NULL */
	int *fds;		/* the parent's descriptor for each of the child's 0..nfds-1; -1: /dev/null */
	int nfds;
	int flags;		/* HN_SPAWN_* */
	int uid, gid;		/* with HN_SPAWN_SETUID, HN_SPAWN_SETGID */
	int error;		/* out: why the child could not exec */
};

#define HN_SPAWN_DETACHED 1
#define HN_SPAWN_SETUID 2
#define HN_SPAWN_SETGID 4

static int exec_path(struct hn_spawn *s)
{
	const char *path = NULL, *p, *end;
	char buf[4096];
	size_t n, flen = strlen(s->file);
	int i, seen_eacces = 0;

	if (strchr(s->file, '/'))
		return execve(s->file, s->argv, s->envp);
	for (i = 0; s->envp && s->envp[i]; i++)
		if (!strncmp(s->envp[i], "PATH=", 5))
			path = s->envp[i] + 5;
	if (!path)
		path = "/usr/local/bin:/bin:/usr/bin";
	for (p = path; ; p = end + 1) {
		end = strchr(p, ':');
		n = end ? (size_t)(end - p) : strlen(p);
		if (n + flen + 2 <= sizeof(buf)) {
			memcpy(buf, p, n);
			if (n)
				buf[n++] = '/';
			memcpy(buf + n, s->file, flen + 1);
			execve(buf, s->argv, s->envp);
			if (errno == EACCES)
				seen_eacces = 1;
			else if (errno != ENOENT && errno != ENOTDIR)
				return -1;
		}
		if (!end)
			break;
	}
	errno = seen_eacces ? EACCES : ENOENT;
	return -1;
}

static int spawn_child(void *arg)
{
	struct hn_spawn *s = arg;
	sigset_t none;
	int i, fd;

	/*
	 * Signals as a new program expects them: none ignored because the
	 * parent ignores it (a closed pipe), none blocked (further down).
	 */
	for (i = 1; i < 65; i++)
		if (i != SIGKILL && i != SIGSTOP)
			signal(i, SIG_DFL);
	if ((s->flags & HN_SPAWN_DETACHED))
		setsid();

	/*
	 * First move away every descriptor that a later dup2 would overwrite
	 * before it has been used itself.
	 */
	for (i = 0; i < s->nfds; i++) {
		if (s->fds[i] >= 0 && s->fds[i] < i) {
			fd = fcntl(s->fds[i], F_DUPFD_CLOEXEC, s->nfds);
			if (fd < 0)
				goto fail;
			s->fds[i] = fd;
		}
	}
	for (i = 0; i < s->nfds; i++) {
		fd = s->fds[i];
		if (fd < 0) {
			fd = open("/dev/null", i == 0 ? O_RDONLY : O_RDWR);
			if (fd < 0)
				goto fail;
			if (fd != i) {
				if (dup2(fd, i) < 0)
					goto fail;
				close(fd);
			}
		} else if (fd == i) {
			fcntl(fd, F_SETFD, 0);
		} else if (dup2(fd, i) < 0) {
			goto fail;
		}
	}
	if (s->cwd && chdir(s->cwd))
		goto fail;
	if ((s->flags & HN_SPAWN_SETGID) && setgid(s->gid))
		goto fail;
	if ((s->flags & HN_SPAWN_SETUID) && setuid(s->uid))
		goto fail;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	exec_path(s);
fail:
	s->error = errno;
	_exit(127);
}

/* Start a program; its process number, or minus an error number. */
long hn_spawn(struct hn_spawn *s)
{
	sigset_t all, old;
	long pid;

	s->error = 0;
	sigfillset(&all);
	sigprocmask(SIG_SETMASK, &all, &old);
	pid = clone(spawn_child, NULL, CLONE_VM | CLONE_VFORK | SIGCHLD, s);
	if (pid < 0)
		pid = -errno;
	sigprocmask(SIG_SETMASK, &old, NULL);
	if (pid > 0 && s->error) {
		/* It did not get as far as the program: nobody is to see it. */
		int status;

		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
		return -s->error;
	}
	return pid;
}

/*
 * Addresses of a host name: up to max of them into out, 4 or 16 bytes each
 * after one byte that says which (4 or 6). Returns how many, or minus an
 * EAI_ number.
 */
long hn_lookup(const char *name, int family, unsigned char *out, int max)
{
	struct addrinfo hints, *list, *ai;
	int r, n = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	r = getaddrinfo(name, NULL, &hints, &list);
	if (r)
		return r < 0 ? r : -r;
	for (ai = list; ai && n < max; ai = ai->ai_next) {
		if (ai->ai_family == AF_INET) {
			out[0] = 4;
			memcpy(out + 1, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, 4);
		} else if (ai->ai_family == AF_INET6) {
			out[0] = 6;
			memcpy(out + 1, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, 16);
		} else {
			continue;
		}
		out += 17;
		n++;
	}
	freeaddrinfo(list);
	return n;
}

/* From the C library, for the JavaScript: named here so that they are linked in. */
void *const hn_libc[] = { malloc, free, realloc, calloc, gai_strerror, strerror };

static char *read_file(const char *path, long *size)
{
	struct stat st;
	char *text;
	long got = 0, n;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || !(text = malloc(st.st_size + 1))) {
		close(fd);
		return NULL;
	}
	while (got < st.st_size && (n = read(fd, text + got, st.st_size - got)) > 0)
		got += n;
	close(fd);
	text[got] = 0;
	*size = got;
	return text;
}

#ifndef PR_SET_MM
#define PR_SET_MM		35
#endif
#ifndef PR_SET_MM_ARG_END
#define PR_SET_MM_ARG_END	9
#endif

/*
 * A program of its own (HOSTNODE_HOME=/run/wlserver: the window server) is
 * named after its directory, for whoever looks at the processes: ps, and
 * fastfetch, which names the Wayland compositor after the first word of
 * the command line of the process at the other end of its socket. The
 * name goes where the kernel keeps it (/proc/PID/comm) and in place of the
 * first argument in the strings the kernel shows as the command line
 * (/proc/PID/cmdline): those are moved, and what is left over is cleared
 * and, where we may say so (root), cut off. The JavaScript is given a copy
 * of the arguments as they were. Node.js programs (boot.js in
 * /usr/lib/hostnode or /run/hostnode) stay hostnode.
 */
static char **name_process(int argc, char **argv, const char *home)
{
	const char *name = strrchr(home, '/') ? strrchr(home, '/') + 1 : home;
	char **copy, *area = argv[0], *end = argv[0], *p;
	size_t size, len;
	int i;

	if (!*name || !strcmp(name, "hostnode"))
		return argv;
	/* The strings one after another, as exec lays them out; anything else and they are left alone. */
	for (i = 0; i < argc; i++) {
		if (argv[i] != end)
			return argv;
		end += strlen(argv[i]) + 1;
	}
	if (!(copy = calloc((size_t)argc + 1, sizeof(*copy))))
		return argv;
	for (i = 0; i < argc; i++)
		if (!(copy[i] = strdup(argv[i])))
			return argv;
	prctl(PR_SET_NAME, (unsigned long)name, 0UL, 0UL, 0UL);

	size = (size_t)(end - area);
	p = area;
	for (i = 0; i < argc; i++) {
		const char *arg = i ? copy[i] : name;

		len = strlen(arg) + 1;
		if (len > size - (size_t)(p - area))
			break;
		memcpy(p, arg, len);
		p += len;
	}
	if (p == area) {
		/* (Not even the name fits.) */
		len = size - 1;
		memcpy(p, name, len);
		p[len] = '\0';
		p += size;
	}
	memset(p, 0, (size_t)(end - p));
	if (p < end)
		prctl(PR_SET_MM, PR_SET_MM_ARG_END, (unsigned long)p, 0UL, 0UL);
	return copy;
}

int main(int argc, char **argv)
{
	static const char *const homes[] = { NULL, "/usr/lib/hostnode", "/run/hostnode", NULL };
	char path[4096];
	char *text = NULL;
	long size = 0;
	int i;

	for (i = 0; i < 3 && !text; i++) {
		const char *home = i == 0 ? getenv("HOSTNODE_HOME") : homes[i];

		if (!home || strlen(home) > sizeof(path) - 16)
			continue;
		snprintf(path, sizeof(path), "%s/boot.js", home);
		text = read_file(path, &size);
		if (text)
			start.home = strdup(home);
	}
	if (!text) {
		fprintf(stderr, "%s: no boot.js in $HOSTNODE_HOME, /usr/lib/hostnode or /run/hostnode\n", argv[0]);
		return 127;
	}
	start.argc = argc;
	start.argv = name_process(argc, argv, start.home);
	start.envp = environ;
	__run_js(text, size, &start);
	return 127;
}
