/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * What other programs see of a guest process, and of the processor.
 *
 * To the kernel a guest process is the emulator: its command line is
 * `rv PROGRAM ARGS...` (or `/bin/rv PROGRAM ARGS...`, the way binfmt_misc
 * starts it), its /proc/PID/exe is the emulator's file, and /proc/cpuinfo
 * describes whatever the host is. Programs that look at other processes
 * (ps, pgrep, htop, fastfetch, which names the shell after its parent's
 * command line) would see rv everywhere. So:
 *
 * - The name (/proc/PID/comm, and the second field of /proc/PID/stat) is the
 *   guest program's: main.c sets it when the program is loaded, and a child
 *   of a fork takes it from the parent (fork.c).
 *
 * - The command line (/proc/PID/cmdline) is the guest's argv. The kernel
 *   reads it from the strings it gave the emulator, from mm->arg_start to
 *   mm->arg_end, and the guest's arguments are the last of those: the
 *   options and the emulator's own name come first. They are moved to the
 *   front and the rest is cleared (rv_args_show()). The kernel would then
 *   show the cleared bytes as empty arguments at the end; where we may say
 *   where the arguments end (prctl(PR_SET_MM, PR_SET_MM_ARG_END), which
 *   takes CAP_SYS_RESOURCE: root on the machine in the page), it is told,
 *   and they are gone. Where not, ps and the like drop empty arguments at
 *   the end anyway. A child of a fork is a second emulator started as
 *   `rv --resume ...` (fork.c): it copies its parent's command line into
 *   its own, which the parent makes long enough with an argument that is
 *   only there for the room.
 *
 * - /proc/PID/exe of another guest process: the kernel's answer is the
 *   emulator, and only that process knows what it runs. What it shows as
 *   its command line starts with the program's path (the emulator is
 *   always started with it, binfmt_misc too), so that is taken: readlink()
 *   of a /proc/PID/exe that leads to the emulator's own file answers with
 *   where the first argument of that process leads (rv_exe_of()). No state
 *   is kept anywhere, and nothing goes stale; the cost is that a process
 *   that started with a relative path and changed its directory since is
 *   left with the kernel's answer. (Making the kernel itself show the guest
 *   program, prctl(PR_SET_MM, PR_SET_MM_EXE_FILE), would also change what
 *   /proc/self/exe means to the emulator, which starts itself again through
 *   it for every exec and fork.)
 *
 * - /proc/cpuinfo is a file made for the occasion (syscall.c), as a
 *   riscv64 kernel writes it (arch/riscv/kernel/cpu.c, c_show()) for the
 *   version uname() reports, 6.6: per processor `processor`, `hart`, `isa`,
 *   `uarch`, `mvendorid`, `marchid`, `mimpid` and an empty line. A kernel
 *   without an MMU also writes `mmu : none`; that is left out, since the
 *   guest's address spaces are made by the emulator and are not without
 *   one, and a kernel with an MMU would name a paging mode that does not
 *   describe them either. (`hart isa`, the per-hart list, came later than
 *   6.6.) The processors are the ones online, as the kernel lists them, so
 *   that the file, nproc, sysconf() and lscpu agree.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rv.h"

#ifndef PR_SET_MM
#define PR_SET_MM		35
#endif
#ifndef PR_SET_MM_ARG_END
#define PR_SET_MM_ARG_END	9
#endif

#define PATH_SIZE	4096

/* The kernel's copy of our arguments, all of it, and how much of it is shown now. */
static char *area;
static size_t area_size, shown;

void rv_args_init(int argc, char **argv)
{
	char *end;

	if (argc < 1)
		return;
	/* The strings one after another, as exec lays them out; anything else and we leave them alone. */
	end = argv[0];
	for (int k = 0; k < argc; k++) {
		if (argv[k] != end)
			return;
		end += strlen(argv[k]) + 1;
	}
	area = argv[0];
	area_size = (size_t)(end - argv[0]);
	shown = area_size;
}

char *rv_args_area(size_t *size)
{
	*size = area_size;
	return area;
}

const char *rv_args_shown(size_t *len)
{
	*len = shown;
	return area;
}

void rv_args_show(const char *args, size_t len)
{
	if (!area || !len)
		return;
	if (len > area_size)
		len = area_size;
	memmove(area, args, len);
	area[len - 1] = '\0';
	memset(area + len, 0, area_size - len);
	shown = len;
	if (len < area_size)
		prctl(PR_SET_MM, PR_SET_MM_ARG_END, (unsigned long)(area + len), 0UL, 0UL);
}

/* "N-M,K,..." as in /sys/devices/system/cpu/online, into the processor numbers; how many, or 0. */
static int parse_cpus(const char *text, int *cpus, int max)
{
	const char *p = text;
	int n = 0;

	while (*p >= '0' && *p <= '9') {
		char *end;
		long first = strtol(p, &end, 10), last = first;

		p = end;
		if (*p == '-') {
			last = strtol(p + 1, &end, 10);
			p = end;
		}
		for (long c = first; c <= last && n < max; c++)
			cpus[n++] = (int)c;
		if (*p != ',')
			break;
		p++;
	}
	return n;
}

/*
 * The processors online, by their numbers. Where the list cannot be read
 * (no /sys in a chroot), the ones this process may run on, which are
 * online too and have their real numbers (sched_getaffinity()); never
 * 0 to N-1 made up, which other files (/sys/devices/system/cpu/cpuN)
 * would not agree with.
 */
static int online_cpus(int *cpus, int max)
{
	char text[256];
	int fd = open("/sys/devices/system/cpu/online", O_RDONLY | O_CLOEXEC), n = 0;
	ssize_t len = -1;
	cpu_set_t set;

	if (fd >= 0) {
		len = read(fd, text, sizeof(text) - 1);
		close(fd);
	}
	if (len > 0) {
		text[len] = '\0';
		if ((n = parse_cpus(text, cpus, max)))
			return n;
	}
	if (sched_getaffinity(0, sizeof(set), &set) == 0)
		for (int c = 0; c < CPU_SETSIZE && n < max; c++)
			if (CPU_ISSET(c, &set))
				cpus[n++] = c;
	if (!n)
		cpus[n++] = 0;
	return n;
}

/* As the kernel prints it: the isa line (what all harts have) and, last, this hart's own (the same here). */
static int write_cpuinfo(int fd, const int *cpus, int n, const char *isa)
{
	static __thread char text[384];

	for (int k = 0; k < n; k++) {
		int len = snprintf(text, sizeof(text),
				   "processor\t: %d\n"
				   "hart\t\t: %d\n"
				   "isa\t\t: %s\n"
				   "uarch\t\t: %s\n"
				   "mvendorid\t: 0x%x\n"
				   "marchid\t\t: 0x%x\n"
				   "mimpid\t\t: 0x%x\n"
				   "hart isa\t: %s\n"
				   "\n",
				   cpus[k], cpus[k], isa, RV_UARCH, RV_MVENDORID, RV_MARCHID, RV_MIMPID, isa);

		if (write(fd, text, (size_t)len) != len)
			return -1;
	}
	return 0;
}

int rv_cpuinfo(int fd)
{
	static __thread int cpus[1024];

	return write_cpuinfo(fd, cpus, online_cpus(cpus, 1024), rv_zb_advertised() ? RV_ISA : RV_ISA_NO_ZB);
}

/* The same for a list of the processors online given as text (tests/cpuinfo). */
int rv_cpuinfo_for(int fd, const char *online)
{
	static __thread int cpus[1024];

	return write_cpuinfo(fd, cpus, parse_cpus(online, cpus, 1024), RV_ISA);
}

/* /proc/PID/... as a process id: this process for self and thread-self; 0 if it is not such a path. */
static pid_t proc_pid(const char *path, const char **rest)
{
	pid_t pid = 0;

	if (strncmp(path, "/proc/", 6))
		return 0;
	path += 6;
	if (!strncmp(path, "self/", 5)) {
		*rest = path + 5;
		return getpid();
	}
	if (!strncmp(path, "thread-self/", 12)) {
		*rest = path + 12;
		return getpid();
	}
	while (*path >= '0' && *path <= '9')
		pid = pid * 10 + (*path++ - '0');
	if (*path != '/')
		return 0;
	*rest = path + 1;
	return pid;
}

/* Where `path` leads, links followed, into `out`: the kernel's name for the file once it is open. Its length, or -1. */
static ssize_t resolve(const char *path, char *out, size_t size)
{
	char link[40];
	int fd = open(path, O_PATH | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	n = readlink(link, out, size);
	close(fd);
	return n > 0 && out[0] == '/' ? n : -1;
}

long rv_exe_of(const char *exe_path, const char *sysroot, int dirfd, const char *path, char *out, size_t size)
{
	static __thread char full[PATH_SIZE + 64], args[PATH_SIZE];
	static struct stat self;
	static int self_known;
	const char *rest, *base = strrchr(path, '/');
	struct stat st;
	ssize_t n;
	pid_t pid;
	int fd;

	/* Only for a link named exe that leads to the emulator's own file. */
	if (strcmp(base ? base + 1 : path, "exe"))
		return -1;
	if (!self_known) {
		if (stat("/proc/self/exe", &self))
			return -1;
		self_known = 1;
	}
	if (fstatat(dirfd, path, &st, 0) || st.st_dev != self.st_dev || st.st_ino != self.st_ino)
		return -1;

	/* Whose: the path in full, where it was relative to a directory. */
	if (path[0] == '/') {
		snprintf(full, sizeof(full), "%s", path);
	} else {
		char link[40];

		if (dirfd == AT_FDCWD) {
			if (!getcwd(full, PATH_SIZE))
				return -1;
			n = (ssize_t)strlen(full);
		} else {
			snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);
			if ((n = readlink(link, full, PATH_SIZE - 1)) <= 0)
				return -1;
		}
		snprintf(full + n, sizeof(full) - (size_t)n, "/%s", path);
	}
	/* (With a sysroot that has the kernel's /proc in it, the path may be the sysroot's.) */
	n = (ssize_t)strlen(sysroot);
	if (n && !strncmp(full, sysroot, (size_t)n) && full[n] == '/')
		memmove(full, full + n, strlen(full + n) + 1);
	pid = proc_pid(full, &rest);
	if (!pid)
		return -1;
	if (strcmp(rest, "exe") && (strncmp(rest, "task/", 5) || !strchr(rest + 5, '/') ||
				    strcmp(strchr(rest + 5, '/'), "/exe")))
		return -1;
	if (pid == getpid()) {
		if (!exe_path)
			return -1;
		n = (ssize_t)strlen(exe_path);
		memcpy(out, exe_path, (size_t)n < size ? (size_t)n : size);
		return (size_t)n < size ? n : (ssize_t)size;
	}

	/* The first argument of the other process is its program. */
	snprintf(full, sizeof(full), "/proc/%d/cmdline", (int)pid);
	fd = open(full, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, args, sizeof(args) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	args[n] = '\0';
	if (args[0] == '/')
		snprintf(full, sizeof(full), "%s", args);
	else
		snprintf(full, sizeof(full), "/proc/%d/cwd/%s", (int)pid, args);
	return resolve(full, out, size);
}
