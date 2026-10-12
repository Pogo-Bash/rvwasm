/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fork() for hosts that have none.
 *
 * Linux/Wasm cannot duplicate a running process: there is no MMU to share
 * pages copy-on-write, and a WebAssembly call stack cannot be copied. What it
 * can do is start a new program. So a guest fork starts a second emulator,
 * which copies the guest from the first: the cpu state, the page map and the
 * contents of every mapped page. It then resumes after the fork call with a
 * return value of 0. Open files carry over because the new process inherits
 * the descriptors, which is also how a real fork shares them.
 *
 * The child takes the copy straight out of the parent's memory. Without an
 * MMU all processes live in one address space (on Linux/Wasm, one linear
 * memory) with nothing between them, so the parent's pointers are as good in
 * the child as in the parent: the copy is a memcpy(). The parent passes the
 * address of a small description of itself on the command line and waits,
 * touching nothing, until the child reports through a pipe that it is done.
 * (An earlier version sent the pages through the pipe instead, 64 KiB and
 * two task switches at a time: 4 ms for a 2 MiB guest, most of a fork.)
 *
 * One thing has to be undone: the child is started with exec, and exec
 * closes every descriptor that is marked close-on-exec. A fork must not (a
 * program that makes a pipe with O_CLOEXEC and forks expects the child to
 * have it; the mark is about the exec the child may do later). So the mark
 * is taken off those descriptors while the child starts, put back in the
 * parent, and put on in the child once it has them.
 *
 * Where the host does have fork() it is used instead (see sys_clone()); this
 * path can be forced there with RV_FORK=snapshot, for testing. Processes have
 * separate address spaces there, so the same copy is made with
 * process_vm_readv(), which reads another process's memory by address.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef __wasm__
#include <sys/uio.h>
#endif

#include "rv.h"

extern char **environ;

#define SNAPSHOT_MAGIC	0x3430504e53565200ULL	/* "\0RVSNP04" */
#define MAX_CLOEXEC	1024
/* The most of the parent's command line the child shows as its own (identity.c). */
#define MAX_ARGS	65536

/* What the parent tells the child; everything else is found through `cpu`. */
struct snapshot {
	uint64_t magic;
	uint64_t cpu_size;	/* guards against a different emulator build */
	const struct rv_cpu *cpu;
	const struct rv_proc *proc;
	uint64_t child_stack;	/* clone()'s stack argument, or 0 */
	uint64_t set_tid_addr;	/* where CLONE_CHILD_SETTID wants the child's id, or 0 */
	uint64_t clear_tid_addr;	/* CLONE_CHILD_CLEARTID's address, or 0 */
	uint64_t exe_len;	/* length of the guest program path, with NUL */
	uint64_t namespaces;	/* clone()'s CLONE_NEW flags: the child is to be in namespaces of its own */
	int apart;		/* the child is in another namespace of process ids than the parent */
	char comm[16];		/* the name of the thread that forked (/proc/PID/comm) */
	const char *args;	/* the parent's command line as other programs see it (identity.c) */
	uint64_t args_len;
	int ncloexec;		/* descriptors that are close-on-exec in the parent */
	int cloexec[MAX_CLOEXEC];
};

/* Copy len bytes at address src of the parent to dst. */
static int copy_from_parent(void *dst, const void *src, size_t len)
{
#ifdef __wasm__
	memcpy(dst, src, len);
	return 0;
#else
	static pid_t parent;

	if (!parent)
		parent = getppid();
	while (len) {
		struct iovec local = { dst, len }, remote = { (void *)(uintptr_t)src, len };
		ssize_t n = process_vm_readv(parent, &local, 1, &remote, 1, 0);

		if (n <= 0)
			return -1;
		dst = (uint8_t *)dst + n;
		src = (const uint8_t *)src + n;
		len -= (size_t)n;
	}
	return 0;
#endif
}

/* Take the close-on-exec mark off fd if it has it, and note the descriptor. */
static void unmark(struct snapshot *snap, int fd)
{
	int flags = fcntl(fd, F_GETFD);

	if (flags >= 0 && (flags & FD_CLOEXEC) && snap->ncloexec < MAX_CLOEXEC &&
	    fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == 0)
		snap->cloexec[snap->ncloexec++] = fd;
}

/* All open descriptors: by the kernel's list of them if /proc is there, else by trying the numbers. */
static void unmark_all(struct snapshot *snap)
{
	DIR *dir = opendir("/proc/self/fd");
	struct dirent *entry;

	snap->ncloexec = 0;
	if (!dir) {
		for (int fd = 0; fd < MAX_CLOEXEC; fd++)
			unmark(snap, fd);
		return;
	}
	while ((entry = readdir(dir))) {
		int fd = atoi(entry->d_name);

		if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9' && fd != dirfd(dir))
			unmark(snap, fd);
	}
	closedir(dir);
}

static void mark_again(const struct snapshot *snap)
{
	for (int i = 0; i < snap->ncloexec; i++)
		fcntl(snap->cloexec[i], F_SETFD, FD_CLOEXEC);
}

static int64_t snapshot_spawn(struct rv_cpu *cpu, struct snapshot *out, uint64_t child_stack,
			      uint64_t set_tid_addr, uint64_t clear_tid_addr);

/*
 * Parent side. Returns the child's process id, or a negative Linux errno.
 */
int64_t rv_fork_snapshot(struct rv_cpu *cpu, uint64_t child_stack, uint64_t set_tid_addr,
			 uint64_t clear_tid_addr, uint64_t namespaces, int apart)
{
	static struct snapshot snap;
	int64_t pid;
	int restore;

	/*
	 * With the other threads parked (thread.c): the child copies the guest
	 * as it is between two instructions, the one description of it below
	 * is ours, and no thread makes a descriptor while the marks are off.
	 */
	rv_lock(cpu);
	rv_stop_world(cpu);
	snap.namespaces = namespaces;
	snap.apart = apart;
	if (rv_namespace_children(namespaces, &restore)) {
		pid = -(int64_t)errno;
	} else {
		pid = snapshot_spawn(cpu, &snap, child_stack, set_tid_addr, clear_tid_addr);
		rv_namespace_restore(restore);
	}
	rv_start_world(cpu);
	rv_unlock(cpu);
	return pid;
}

/*
 * A child that is to be in namespaces of its own (clone() with CLONE_NEW
 * flags) is made here like any other, by fork() or by starting an emulator,
 * and neither takes such flags. All the namespaces but one a process can
 * enter by itself, and the child does, first thing (unshare()). The one of
 * process ids is decided when a process is made: the parent says beforehand
 * that its children are to be in a new one (this, which needs the right to:
 * a process without it gets the error clone() would give), and afterwards
 * that they are to be in its own again (rv_namespace_restore()).
 */
int rv_namespace_children(uint64_t namespaces, int *restore)
{
	*restore = -1;
	if (!(namespaces & RV_CLONE_NEWPID))
		return 0;
	*restore = open("/proc/self/ns/pid", O_RDONLY | O_CLOEXEC);
	if (*restore < 0)
		return -1;
	if (unshare((int)RV_CLONE_NEWPID) < 0) {
		int err = errno;

		close(*restore);
		*restore = -1;
		errno = err;
		return -1;
	}
	return 0;
}

void rv_namespace_restore(int restore)
{
	if (restore < 0)
		return;
	setns(restore, (int)RV_CLONE_NEWPID);
	close(restore);
}

/*
 * posix_spawn() of the program open as `fd`, for after a chroot(), when
 * /proc/self/exe may lead nowhere (syscall.c: sys_chroot()). Done the way
 * musl's posix_spawn() does it: a child that shares our memory and does
 * nothing but exec, with every signal held off until it has put the
 * handlers it shares nothing with back to the default; its errno comes
 * back through a pipe, which the exec closes when it succeeds.
 */
struct spawn_args {
	int fd, pipe[2];
	char **argv;
	sigset_t mask;
};

static int spawn_child(void *p)
{
	struct spawn_args *args = p;
	struct sigaction dfl = { .sa_handler = SIG_DFL };
	int err;

	close(args->pipe[0]);
	for (int sig = 1; sig < NSIG; sig++) {
		struct sigaction sa;

		if (sig != SIGKILL && sig != SIGSTOP && !sigaction(sig, NULL, &sa) &&
		    sa.sa_handler != SIG_DFL && sa.sa_handler != SIG_IGN)
			sigaction(sig, &dfl, NULL);
	}
	sigprocmask(SIG_SETMASK, &args->mask, NULL);
	rv_host_syscall(SYS_execveat, args->fd, (uint64_t)(uintptr_t)"", (uint64_t)(uintptr_t)args->argv,
			(uint64_t)(uintptr_t)environ, AT_EMPTY_PATH, 0);
	err = errno;
	while (write(args->pipe[1], &err, sizeof(err)) < 0 && errno == EINTR)
		;
	_exit(127);
}

static int spawn_from(int fd, pid_t *pid, char **argv)
{
	/* (Static: the process stack on Linux/Wasm is small. The world is stopped, so it is ours.) */
	static char stack[16384];
	static struct spawn_args args;
	sigset_t all;
	int err = 0;
	pid_t child;

	args.fd = fd;
	args.argv = argv;
	if (pipe2(args.pipe, O_CLOEXEC) < 0)
		return errno;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &args.mask);
	child = clone(spawn_child, stack + sizeof(stack), CLONE_VM | CLONE_VFORK | SIGCHLD, &args);
	if (child < 0)
		err = errno;
	pthread_sigmask(SIG_SETMASK, &args.mask, NULL);
	close(args.pipe[1]);
	if (child > 0 && read(args.pipe[0], &err, sizeof(err)) == sizeof(err))
		waitpid(child, NULL, 0);
	else if (child > 0)
		err = 0;
	close(args.pipe[0]);
	if (!err)
		*pid = child;
	return err;
}

static int64_t snapshot_spawn(struct rv_cpu *cpu, struct snapshot *out, uint64_t child_stack,
			      uint64_t set_tid_addr, uint64_t clear_tid_addr)
{
	char addr[24], done_fd[16], close_fd[16], byte;
	char *argv[] = { (char *)"rv", (char *)"--resume", addr, done_fd, close_fd, NULL, NULL };
	int fds[2], err;
	pid_t pid;
	size_t args_len;
	const char *args = rv_args_shown(&args_len);
	char *room = NULL;

	uint64_t namespaces = out->namespaces;
	int apart = out->apart;

	*out = (struct snapshot){
		.namespaces = namespaces,
		.apart = apart,
		.magic = SNAPSHOT_MAGIC,
		.cpu_size = sizeof(*cpu) + sizeof(*cpu->proc),
		.cpu = cpu,
		.proc = cpu->proc,
		.child_stack = child_stack,
		.set_tid_addr = set_tid_addr,
		.clear_tid_addr = clear_tid_addr,
		.exe_len = cpu->exe_path ? strlen(cpu->exe_path) + 1 : 0,
		.args = args,
		.args_len = args_len < MAX_ARGS ? args_len : MAX_ARGS,
	};
	/*
	 * The child is to show our name and command line (identity.c). The
	 * kernel shows what the child was started with, so that is made long
	 * enough to take ours: an argument that is only there for the room.
	 */
	prctl(PR_GET_NAME, (unsigned long)out->comm, 0UL, 0UL, 0UL);
	if (out->args && out->args_len && (room = malloc(out->args_len + 1))) {
		memset(room, '.', out->args_len);
		room[out->args_len] = '\0';
		argv[5] = room;
	}

	/* The child writes a byte to this pipe when it has its copy. */
	if (pipe(fds) < 0)
		return -(int64_t)errno;
	unmark_all(out);
	snprintf(addr, sizeof(addr), "%llx", (unsigned long long)(uintptr_t)out);
	snprintf(done_fd, sizeof(done_fd), "%d", fds[1]);
	snprintf(close_fd, sizeof(close_fd), "%d", fds[0]);

#ifndef __wasm__
	/* Where reading a parent's memory needs permission (the Yama module), give it. */
	prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif
	if (cpu->proc->self_fd >= 0)
		err = spawn_from(cpu->proc->self_fd, &pid, argv);
	else
		err = posix_spawn(&pid, "/proc/self/exe", NULL, NULL, argv, environ);
	free(room);
	mark_again(out);
	close(fds[1]);
	if (err) {
		close(fds[0]);
		return -(int64_t)err;
	}

	/*
	 * Wait for the byte, or for the end of file that says the child is gone.
	 * A child that could not start (out of memory, most likely) has already
	 * exited. It is still our child: report its id and let the guest collect
	 * the failure with wait(), as it would for a child killed right after fork.
	 */
	while (read(fds[0], &byte, 1) < 0 && errno == EINTR)
		;
	close(fds[0]);
#ifndef __wasm__
	prctl(PR_SET_PTRACER, 0, 0, 0, 0);
#endif
	return pid;
}

/*
 * Child side: `rv --resume ADDR DONE_FD CLOSE_FD`. Fills in *cpu from the
 * parent, whose description of itself is at ADDR, and leaves it ready to run.
 * Returns 0, or -1 if that cannot be done.
 */
int rv_fork_resume(struct rv_cpu *cpu, struct rv_proc *proc, const char *addr, int done_fd, int close_fd)
{
	static char exe_path[4096];
	static struct snapshot snap;	/* (static: 4 KiB is too much for the small process stack on Linux/Wasm) */
	const uint64_t *parent_map;
	const char *parent_exe;
	int32_t tid = (int32_t)getpid();
	uint64_t table;

	/* The parent's end of the pipe, which we inherited. */
	close(close_fd);

	if (copy_from_parent(&snap, (const void *)(uintptr_t)strtoull(addr, NULL, 16), sizeof(snap)) ||
	    snap.magic != SNAPSHOT_MAGIC || snap.cpu_size != sizeof(*cpu) + sizeof(*proc) ||
	    snap.exe_len > sizeof(exe_path) || copy_from_parent(cpu, snap.cpu, sizeof(*cpu)) ||
	    copy_from_parent(proc, snap.proc, sizeof(*proc)))
		return -1;
	/* Of the parent's threads only the one that forked exists here; the locks are new ones. */
	cpu->proc = proc;
	rv_thread_after_fork(cpu);
	/* Namespaces of our own, if that is what the fork asked for (the one of process ids we are in already). */
	if ((snap.namespaces & ~RV_CLONE_NEWPID) && unshare((int)(snap.namespaces & ~RV_CLONE_NEWPID)) < 0)
		return -1;

	mark_again(&snap);

	/* Everything in *cpu that points into the parent is replaced. */
	parent_map = cpu->map;
	parent_exe = cpu->exe_path;
	cpu->trace = NULL;
	cpu->exe_path = snap.exe_len ? exe_path : NULL;
	cpu->bounces = NULL;
	cpu->nbounces = cpu->bounces_cap = 0;
	table = (cpu->npages + 1) * sizeof(*cpu->map);
	if (!(cpu->map = malloc(table)))
		return -1;
	if ((snap.exe_len && copy_from_parent(exe_path, parent_exe, snap.exe_len)) ||
	    copy_from_parent(cpu->map, parent_map, table))
		return -1;

	/*
	 * The guest's memory: the page table is the parent's, and every page
	 * that has memory there gets memory here, with the parent's contents.
	 * Pages that have none yet cost nothing here either.
	 */
	proc->children_apart = 0;
	if (rv_mm_fork_child(cpu, copy_from_parent, snap.apart) ||
	    rv_seccomp_fork_child(cpu, copy_from_parent))
		return -1;

	/* The parent's name and command line are ours, while the parent waits and they stay as they are. */
	if (snap.args && snap.args_len) {
		size_t room;
		char *mine = rv_args_area(&room);
		size_t len = snap.args_len < room ? snap.args_len : room;

		if (mine && len && !copy_from_parent(mine, snap.args, len))
			rv_args_show(mine, len);
	}
	snap.comm[sizeof(snap.comm) - 1] = '\0';
	if (snap.comm[0])
		prctl(PR_SET_NAME, (unsigned long)snap.comm, 0UL, 0UL, 0UL);

	/* The parent may go on. */
	if (write(done_fd, "", 1) != 1)
		return -1;
	close(done_fd);

	/* Translations are per process. */
	cpu->jit_used = NULL;
	if (cpu->jit) {
		cpu->jit = NULL;
		if (rv_jit_init(cpu))
			return -1;
	}
	cpu->undo_on = 0;

	/*
	 * The copy was made inside the clone() system call: return 0 from
	 * it and continue with the instruction after the ecall.
	 */
	cpu->pc += 4;
	cpu->x[10] = 0;
	if (snap.child_stack)
		cpu->x[2] = snap.child_stack;
	if (snap.set_tid_addr && rv_user_ptr(cpu, snap.set_tid_addr, sizeof(tid), RV_PROT_WRITE))
		rv_mm_fill(cpu, snap.set_tid_addr, sizeof(tid), &tid, -1, 0);
	rv_user_done(cpu);
	cpu->trap = RV_TRAP_NONE;
	cpu->pc_set = 0;
	cpu->sys_interrupted = 0;

	/* The display stays the parent's: what the child has of it is a copy of the pixels. */
	if (cpu->proc->fb_len) {
		close(cpu->proc->fb_fd);
		cpu->proc->fb_len = 0;
	}

	cpu->clear_child_tid = snap.clear_tid_addr;

	/* A child starts with no pending signals (rv_thread_after_fork()); exec made the host forget our handlers. */
	rv_signal_init();
	rv_signal_reinstall(cpu);
	return 0;
}
