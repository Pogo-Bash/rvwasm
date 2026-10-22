/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Guest system calls. riscv64 uses the generic Linux syscall numbers:
 * number in a7, arguments in a0..a5, result (or -errno) in a0.
 *
 * Everything goes through libc wrappers rather than syscall(): on Linux/Wasm
 * a system call made with the wrong number of arguments traps in the kernel.
 *
 * errno values are passed through unchanged. The host is always Linux, and
 * every architecture involved (riscv64, arm64, wasm) uses the generic numbers.
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE	/* musl: getdents64() */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/fsuid.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sendfile.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/statfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "rv.h"

enum {
	RV_SYS_getcwd		= 17,
	RV_SYS_flock		= 32,
	RV_SYS_mknodat		= 33,
	RV_SYS_symlinkat	= 36,
	RV_SYS_linkat		= 37,
	RV_SYS_statfs		= 43,
	RV_SYS_fstatfs		= 44,
	RV_SYS_truncate		= 45,
	RV_SYS_ftruncate	= 46,
	RV_SYS_fchdir		= 50,
	RV_SYS_chroot		= 51,
	RV_SYS_fchmod		= 52,
	RV_SYS_fchmodat		= 53,
	RV_SYS_fchownat		= 54,
	RV_SYS_fchown		= 55,
	RV_SYS_getdents64	= 61,
	RV_SYS_sendfile		= 71,
	RV_SYS_pselect6		= 72,
	RV_SYS_ppoll		= 73,
	RV_SYS_sync		= 81,
	RV_SYS_fsync		= 82,
	RV_SYS_fdatasync	= 83,
	RV_SYS_utimensat	= 88,
	RV_SYS_nanosleep	= 101,
	RV_SYS_getitimer	= 102,
	RV_SYS_setitimer	= 103,
	RV_SYS_clock_nanosleep	= 115,
	RV_SYS_sched_getaffinity = 123,
	RV_SYS_getcpu		= 168,
	RV_SYS_timer_create	= 107,
	RV_SYS_timer_gettime	= 108,
	RV_SYS_timer_getoverrun	= 109,
	RV_SYS_timer_settime	= 110,
	RV_SYS_timer_delete	= 111,
	RV_SYS_readahead	= 213,
	RV_SYS_seccomp		= 277,
	RV_SYS_unshare		= 97,
	RV_SYS_setns		= 268,
	RV_SYS_kill		= 129,
	RV_SYS_tkill		= 130,
	RV_SYS_tgkill		= 131,
	RV_SYS_sigaltstack	= 132,
	RV_SYS_rt_sigsuspend	= 133,
	RV_SYS_rt_sigpending	= 136,
	RV_SYS_rt_sigreturn	= 139,
	RV_SYS_setregid		= 143,
	RV_SYS_setgid		= 144,
	RV_SYS_setreuid		= 145,
	RV_SYS_setuid		= 146,
	RV_SYS_setresuid	= 147,
	RV_SYS_getresuid	= 148,
	RV_SYS_setresgid	= 149,
	RV_SYS_getresgid	= 150,
	RV_SYS_times		= 153,
	RV_SYS_setpgid		= 154,
	RV_SYS_getpgid		= 155,
	RV_SYS_getsid		= 156,
	RV_SYS_setsid		= 157,
	RV_SYS_getgroups	= 158,
	RV_SYS_setgroups	= 159,
	RV_SYS_setpriority	= 140,
	RV_SYS_getpriority	= 141,
	RV_SYS_setfsuid		= 151,
	RV_SYS_setfsgid		= 152,
	RV_SYS_getrusage	= 165,
	RV_SYS_umask		= 166,
	RV_SYS_prctl		= 167,
	RV_SYS_gettimeofday	= 169,
	RV_SYS_sysinfo		= 179,
	RV_SYS_clone		= 220,
	RV_SYS_riscv_flush_icache = 259,
	RV_SYS_execve		= 221,
	RV_SYS_wait4		= 260,
	RV_SYS_waitid		= 95,
	RV_SYS_capget		= 90,
	RV_SYS_capset		= 91,
	RV_SYS_close_range	= 436,
	RV_SYS_pidfd_open	= 434,
	RV_SYS_pidfd_send_signal = 424,
	RV_SYS_memfd_create	= 279,
	RV_SYS_fallocate	= 47,
	RV_SYS_ioprio_set	= 30,
	RV_SYS_ioprio_get	= 31,
	RV_SYS_sched_setparam	= 118,
	RV_SYS_sched_setscheduler = 119,
	RV_SYS_sched_getscheduler = 120,
	RV_SYS_sched_getparam	= 121,
	RV_SYS_sched_get_priority_max = 125,
	RV_SYS_sched_get_priority_min = 126,
	RV_SYS_shmget		= 194,
	RV_SYS_shmctl		= 195,
	RV_SYS_shmat		= 196,
	RV_SYS_shmdt		= 197,
	RV_SYS_renameat2	= 276,
	RV_SYS_statx		= 291,
	RV_SYS_dup		= 23,
	RV_SYS_dup3		= 24,
	RV_SYS_fcntl		= 25,
	RV_SYS_ioctl		= 29,
	RV_SYS_mkdirat		= 34,
	RV_SYS_unlinkat		= 35,
	RV_SYS_faccessat	= 48,
	RV_SYS_chdir		= 49,
	RV_SYS_openat		= 56,
	RV_SYS_close		= 57,
	RV_SYS_pipe2		= 59,
	RV_SYS_lseek		= 62,
	RV_SYS_read		= 63,
	RV_SYS_write		= 64,
	RV_SYS_readv		= 65,
	RV_SYS_writev		= 66,
	RV_SYS_pread64		= 67,
	RV_SYS_pwrite64		= 68,
	RV_SYS_readlinkat	= 78,
	RV_SYS_newfstatat	= 79,
	RV_SYS_fstat		= 80,
	RV_SYS_exit		= 93,
	RV_SYS_exit_group	= 94,
	RV_SYS_set_tid_address	= 96,
	RV_SYS_futex		= 98,
	RV_SYS_set_robust_list	= 99,
	RV_SYS_clock_gettime	= 113,
	RV_SYS_clock_getres	= 114,
	RV_SYS_sched_yield	= 124,
	RV_SYS_rt_sigaction	= 134,
	RV_SYS_rt_sigprocmask	= 135,
	RV_SYS_uname		= 160,
	RV_SYS_getpid		= 172,
	RV_SYS_getppid		= 173,
	RV_SYS_getuid		= 174,
	RV_SYS_geteuid		= 175,
	RV_SYS_getgid		= 176,
	RV_SYS_getegid		= 177,
	RV_SYS_gettid		= 178,
	RV_SYS_brk		= 214,
	RV_SYS_munmap		= 215,
	RV_SYS_mremap		= 216,
	RV_SYS_mmap		= 222,
	RV_SYS_mprotect		= 226,
	RV_SYS_eventfd2		= 19,
	RV_SYS_inotify_init1	= 26,
	RV_SYS_inotify_add_watch = 27,
	RV_SYS_inotify_rm_watch	= 28,
	RV_SYS_epoll_create1	= 20,
	RV_SYS_epoll_ctl	= 21,
	RV_SYS_epoll_pwait	= 22,
	RV_SYS_signalfd4	= 74,
	RV_SYS_timerfd_create	= 85,
	RV_SYS_timerfd_settime	= 86,
	RV_SYS_timerfd_gettime	= 87,
	RV_SYS_getxattr		= 8,
	RV_SYS_lgetxattr	= 9,
	RV_SYS_fgetxattr	= 10,
	RV_SYS_listxattr	= 11,
	RV_SYS_llistxattr	= 12,
	RV_SYS_flistxattr	= 13,
	RV_SYS_socket		= 198,
	RV_SYS_socketpair	= 199,
	RV_SYS_bind		= 200,
	RV_SYS_listen		= 201,
	RV_SYS_accept		= 202,
	RV_SYS_connect		= 203,
	RV_SYS_getsockname	= 204,
	RV_SYS_getpeername	= 205,
	RV_SYS_sendto		= 206,
	RV_SYS_recvfrom		= 207,
	RV_SYS_setsockopt	= 208,
	RV_SYS_getsockopt	= 209,
	RV_SYS_shutdown		= 210,
	RV_SYS_sendmsg		= 211,
	RV_SYS_recvmsg		= 212,
	RV_SYS_recvmmsg		= 243,
	RV_SYS_sendmmsg		= 269,
	RV_SYS_fadvise64	= 223,
	RV_SYS_mincore		= 232,
	RV_SYS_accept4		= 242,
	RV_SYS_riscv_hwprobe	= 258,
	RV_SYS_rseq		= 293,
	RV_SYS_clone3		= 435,
	RV_SYS_sync_file_range	= 84,
	RV_SYS_copy_file_range	= 285,
	RV_SYS_msync		= 227,
	RV_SYS_madvise		= 233,
	RV_SYS_prlimit64	= 261,
	RV_SYS_getrandom	= 278,
	RV_SYS_faccessat2	= 439,
};

/* Guest open(2) flags: the asm-generic values, which arm64 does not share. */
#define RV_O_ACCMODE	00000003
#define RV_O_CREAT	00000100
#define RV_O_EXCL	00000200
#define RV_O_NOCTTY	00000400
#define RV_O_TRUNC	00001000
#define RV_O_APPEND	00002000
#define RV_O_NONBLOCK	00004000
#define RV_O_DIRECTORY	00200000
#define RV_O_NOFOLLOW	00400000
#define RV_O_CLOEXEC	02000000

#define RV_F_DUPFD		0
#define RV_F_GETFD		1
#define RV_F_SETFD		2
#define RV_F_GETFL		3
#define RV_F_SETFL		4
#define RV_F_DUPFD_CLOEXEC	1030

#define RV_TCGETS	0x5401
#define RV_TCSETS	0x5402
#define RV_TCSETSW	0x5403
#define RV_TCSETSF	0x5404
#define RV_TIOCGPGRP	0x540f
#define RV_TIOCSPGRP	0x5410
#define RV_TIOCGWINSZ	0x5413
#define RV_TIOCSWINSZ	0x5414
#define RV_FIONREAD	0x541b
#define RV_TERMIOS_SIZE	36	/* kernel struct termios on generic architectures */

#define RV_PR_SET_NAME	15
#define RV_PR_GET_NAME	16

/* Sizes of kernel structures that are identical on every 64-bit generic architecture. */
#define RV_RUSAGE_SIZE	144
#define RV_SYSINFO_SIZE	112
#define RV_STATX_SIZE	256

#define RV_ARG_MAX	1024	/* argv/envp entries accepted by execve */

#define RV_CSIGNAL		0x000000ff
#define RV_CLONE_VM		0x00000100
#define RV_CLONE_VFORK		0x00004000
#define RV_CLONE_PIDFD		0x00001000
#define RV_CLONE_THREAD		0x00010000
#define RV_CLONE_CHILD_CLEARTID	0x00200000
#define RV_CLONE_CHILD_SETTID	0x01000000
#define RV_SIGCHLD		17

#define RV_FUTEX_WAIT		0
#define RV_FUTEX_CMD_MASK	0x7f

#define RV_RLIMIT_STACK	3
#define RV_RLIM_INFINITY (~0ULL)

#define RV_IOV_MAX	1024
#define RV_PATH_MAX	4096

/* The kernel's struct stat on riscv64 (asm-generic/stat.h). */
struct rv_stat {
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t pad1;
	int64_t st_size;
	int32_t st_blksize;
	int32_t pad2;
	int64_t st_blocks;
	int64_t st_atime_sec;
	uint64_t st_atime_nsec;
	int64_t st_mtime_sec;
	uint64_t st_mtime_nsec;
	int64_t st_ctime_sec;
	uint64_t st_ctime_nsec;
	uint32_t unused[2];
};

struct rv_iovec {
	uint64_t base;
	uint64_t len;
};

struct rv_timespec {
	int64_t sec;
	int64_t nsec;
};

/* The kernel's struct statfs on riscv64 (asm-generic/statfs.h). */
struct rv_statfs {
	int64_t f_type;
	int64_t f_bsize;
	uint64_t f_blocks;
	uint64_t f_bfree;
	uint64_t f_bavail;
	uint64_t f_files;
	uint64_t f_ffree;
	int32_t f_fsid[2];
	int64_t f_namelen;
	int64_t f_frsize;
	int64_t f_flags;
	int64_t f_spare[4];
};

static uint64_t neg(int err)
{
	return (uint64_t)-(int64_t)err;
}

/* Result of a host call that returns -1 and sets errno on failure. */
static uint64_t ret_errno(long ret)
{
	return ret < 0 ? neg(errno) : (uint64_t)ret;
}

static int open_flags_to_host(uint64_t flags)
{
	int host = (int)(flags & RV_O_ACCMODE);

	if (flags & RV_O_CREAT)
		host |= O_CREAT;
	if (flags & RV_O_EXCL)
		host |= O_EXCL;
	if (flags & RV_O_NOCTTY)
		host |= O_NOCTTY;
	if (flags & RV_O_TRUNC)
		host |= O_TRUNC;
	if (flags & RV_O_APPEND)
		host |= O_APPEND;
	if (flags & RV_O_NONBLOCK)
		host |= O_NONBLOCK;
	if (flags & RV_O_DIRECTORY)
		host |= O_DIRECTORY;
	if (flags & RV_O_NOFOLLOW)
		host |= O_NOFOLLOW;
	if (flags & RV_O_CLOEXEC)
		host |= O_CLOEXEC;
	return host;
}

static uint64_t open_flags_to_guest(int host)
{
	uint64_t flags = (uint64_t)(host & O_ACCMODE);

	if (host & O_APPEND)
		flags |= RV_O_APPEND;
	if (host & O_NONBLOCK)
		flags |= RV_O_NONBLOCK;
	return flags;
}

/*
 * A NUL-terminated guest path as the host should see it, or NULL if it is not
 * readable or too long.
 *
 * /proc/self/exe is the guest program, not the emulator running it. With a
 * sysroot, an absolute path that exists under it is taken from there, which
 * is how the guest finds riscv64 libraries on a host that has its own /lib.
 */
/* A string of the guest's, where the host can read it; NULL if it cannot be read. */
static const char *user_string(struct rv_cpu *cpu, uint64_t addr)
{
	uint64_t len = rv_user_strlen(cpu, addr);

	return len ? rv_user_ptr(cpu, addr, len, RV_PROT_READ) : NULL;
}

static const char *user_path(struct rv_cpu *cpu, uint64_t addr)
{
	/* Two buffers, used in turn: some calls translate two paths. Static:
	 * they would not fit the small process stack on Linux/Wasm. */
	static __thread char translated[2][RV_SYSROOT_MAX + RV_PATH_MAX];
	static __thread int turn;
	uint64_t len = rv_user_strlen(cpu, addr);
	const char *path;
	char *buf;

	if (!len || len > RV_PATH_MAX)
		return NULL;
	if (!(path = rv_user_ptr(cpu, addr, len, RV_PROT_READ)))
		return NULL;
	/* (After a chroot() only where the new root has a /proc, as for the kernel's link: sys_chroot().) */
	if (cpu->exe_path && !strcmp(path, "/proc/self/exe") &&
	    (cpu->proc->self_fd < 0 || faccessat(AT_FDCWD, "/proc/self", F_OK, 0) == 0))
		return cpu->exe_path;
	/* The emulated GPU's files (virtgpu.c), whatever the root. */
	if (path[0] == '/' && (path[1] == 'd' || path[1] == 's') && rv_virtgpu_enabled()) {
		const char *mine = rv_virtgpu_path(path);

		if (mine)
			return mine;
	}
	if (!cpu->sysroot[0] || path[0] != '/')
		return path;

	buf = translated[turn];
	turn ^= 1;
	snprintf(buf, sizeof(translated[0]), "%s%s", cpu->sysroot, path);
	if (faccessat(AT_FDCWD, buf, F_OK, AT_SYMLINK_NOFOLLOW) == 0)
		return buf;

	/*
	 * Not there. If the directory it would be in is, the path is still the
	 * sysroot's: a file about to be created, or one that does not exist in
	 * the guest's world even if the host has it. (Not for the kernel's own
	 * trees, which a sysroot has as empty directories.)
	 */
	if (strncmp(path, "/proc/", 6) && strncmp(path, "/sys/", 5) && strncmp(path, "/dev/", 5)) {
		char *slash = strrchr(buf, '/');
		int parent_exists;

		*slash = 0;
		parent_exists = slash > buf + strlen(cpu->sysroot) && access(buf, F_OK) == 0;
		*slash = '/';
		if (parent_exists)
			return buf;
	}
	return path;
}

/*
 * read(), write(), pread() and pwrite(): straight from or into the guest's
 * memory, in the pieces it lies in (rv_user_iov()). `positioned`: at `off`
 * and without moving the file's position. More pieces than one call takes
 * are more calls, until one of them comes back short.
 */
static uint64_t transfer(struct rv_cpu *cpu, int fd, uint64_t buf, uint64_t len, int is_write, int positioned, uint64_t off)
{
	/* Static: 16 KiB would not fit the 8 KiB process stack on Linux/Wasm. */
	static __thread struct iovec iov[RV_IOV_MAX];
	uint64_t total = 0;

	if (!len) {
		/* (A length of nothing still asks whether the descriptor can do this.) */
		if (positioned)
			return ret_errno(is_write ? pwrite(fd, cpu->scratch, 0, (off_t)off) : pread(fd, cpu->scratch, 0, (off_t)off));
		return ret_errno(is_write ? write(fd, cpu->scratch, 0) : read(fd, cpu->scratch, 0));
	}
	while (total < len) {
		uint64_t covered;
		ssize_t r;
		int n = rv_user_iov(cpu, buf + total, len - total, is_write ? RV_PROT_READ : RV_PROT_WRITE,
				    iov, RV_IOV_MAX, &covered);

		if (n < 0)
			return total ? total : neg(RV_EFAULT);
		if (n == 1) {
			if (positioned)
				r = is_write ? pwrite(fd, iov[0].iov_base, iov[0].iov_len, (off_t)(off + total))
					     : pread(fd, iov[0].iov_base, iov[0].iov_len, (off_t)(off + total));
			else
				r = is_write ? write(fd, iov[0].iov_base, iov[0].iov_len)
					     : read(fd, iov[0].iov_base, iov[0].iov_len);
		} else if (positioned) {
			r = is_write ? pwritev(fd, iov, n, (off_t)(off + total)) : preadv(fd, iov, n, (off_t)(off + total));
		} else {
			r = is_write ? writev(fd, iov, n) : readv(fd, iov, n);
		}
		if (r < 0)
			return total ? total : neg(errno);
		total += (uint64_t)r;
		if ((uint64_t)r < covered)
			break;
	}
	return total;
}

static uint64_t sys_read(struct rv_cpu *cpu, int fd, uint64_t buf, uint64_t len)
{
	return transfer(cpu, fd, buf, len, 0, 0, 0);
}

static uint64_t sys_write(struct rv_cpu *cpu, int fd, uint64_t buf, uint64_t len)
{
	return transfer(cpu, fd, buf, len, 1, 0, 0);
}

static uint64_t sys_pread(struct rv_cpu *cpu, int fd, uint64_t buf, uint64_t len, uint64_t off)
{
	return transfer(cpu, fd, buf, len, 0, 1, off);
}

static uint64_t sys_pwrite(struct rv_cpu *cpu, int fd, uint64_t buf, uint64_t len, uint64_t off)
{
	uint64_t ret = transfer(cpu, fd, buf, len, 1, 1, off);

	/* (And into the process's copies of the file, where it has it mapped shared: mm.c.) */
	if (ret && ret < (uint64_t)-4095)
		rv_mm_file_written(cpu, fd, ret, off);
	return ret;
}

/* readv/writev: translate the guest iovec array, then one host call. */
static uint64_t sys_iov(struct rv_cpu *cpu, int fd, uint64_t iov_addr, uint64_t count, int is_write)
{
	/* Static: 16 KiB would not fit the 8 KiB process stack on Linux/Wasm. */
	static __thread struct iovec host[RV_IOV_MAX];
	struct rv_iovec *guest;

	if (count > RV_IOV_MAX)
		return neg(RV_EINVAL);
	guest = rv_user_ptr(cpu, iov_addr, count * sizeof(*guest), RV_PROT_READ);
	if (!guest)
		return neg(RV_EFAULT);
	for (uint64_t i = 0; i < count; i++) {
		struct rv_iovec v;

		memcpy(&v, &guest[i], sizeof(v));
		host[i].iov_base = rv_user_ptr(cpu, v.base, v.len,
					       is_write ? RV_PROT_READ : RV_PROT_WRITE);
		if (!host[i].iov_base)
			return neg(RV_EFAULT);
		host[i].iov_len = v.len;
	}
	return ret_errno(is_write ? writev(fd, host, (int)count) : readv(fd, host, (int)count));
}

static long memory_file(const char *name, unsigned flags);
static int is_own_maps(const char *path);

/*
 * The kernel's /proc/cpuinfo, just opened as `fd`, gives way to the
 * processor the guest has (identity.c). Any file of that name on a proc
 * file system is that one, however it was reached: /proc/cpuinfo, or
 * "cpuinfo" from a descriptor for /proc (lscpu). The new descriptor, or
 * `fd` itself if it is something else or no file can be made.
 */
#define PROC_SUPER_MAGIC	0x9fa0

static long cpuinfo_instead(long fd, const char *path, uint64_t flags)
{
	const char *base = strrchr(path, '/');
	struct statfs sfs;
	long mine;

	if (strcmp(base ? base + 1 : path, "cpuinfo") || (flags & RV_O_ACCMODE) != 0 ||
	    fstatfs((int)fd, &sfs) || sfs.f_type != PROC_SUPER_MAGIC)
		return fd;
	mine = memory_file("cpuinfo", (flags & RV_O_CLOEXEC) ? 1 : 0);
	if (mine < 0)
		return fd;
	if (rv_cpuinfo((int)mine) || lseek((int)mine, 0, SEEK_SET) < 0) {
		close((int)mine);
		return fd;
	}
	close((int)fd);
	return mine;
}

static uint64_t sys_openat(struct rv_cpu *cpu, int dirfd, uint64_t path_addr, uint64_t flags, uint64_t mode)
{
	const char *path = user_path(cpu, path_addr);

	if (!path)
		return neg(RV_EFAULT);
	/*
	 * The list of the process's mappings is the guest's, not the
	 * emulator's (mm.c): a file made for the occasion.
	 */
	if (path[0] == '/' && path[1] == 'p' && is_own_maps(path) && (flags & RV_O_ACCMODE) == 0) {
		long fd = memory_file("maps", (flags & RV_O_CLOEXEC) ? 1 : 0);

		if (fd < 0)
			return neg(errno);
		if (rv_mm_maps(cpu, (int)fd) || lseek((int)fd, 0, SEEK_SET) < 0) {
			close((int)fd);
			return neg(RV_ENOMEM);
		}
		return (uint64_t)fd;
	}
	{
		long fd = openat(dirfd, path, open_flags_to_host(flags), (mode_t)mode);

		if (fd >= 0)
			fd = cpuinfo_instead(fd, path, flags);
		/* The emulated GPU's device: a connection to its server instead (virtgpu.c). */
		if (fd >= 0 && rv_virtgpu_enabled()) {
			fd = rv_virtgpu_open(fd, (flags & RV_O_CLOEXEC) != 0);
			if (fd < 0)
				return neg((int)-fd);
		}
		return ret_errno(fd);
	}
}

static uint64_t sys_close(struct rv_cpu *cpu, int fd)
{
	/* The guest shares our descriptor table: do not let it close the trace, or our own program (sys_chroot()). */
	if ((cpu->trace && fd == fileno(cpu->trace)) || (fd >= 0 && fd == cpu->proc->self_fd))
		return 0;
	rv_virtgpu_close(fd);
	return ret_errno(close(fd));
}

static uint64_t put_stat(struct rv_cpu *cpu, uint64_t addr, const struct stat *st)
{
	struct rv_stat out;
	void *p = rv_user_ptr(cpu, addr, sizeof(out), RV_PROT_WRITE);

	struct stat fixed;

	if (!p)
		return neg(RV_EFAULT);
	if (rv_virtgpu_enabled()) {
		fixed = *st;
		rv_virtgpu_fix_stat(&fixed);
		st = &fixed;
	}
	memset(&out, 0, sizeof(out));
	out.st_dev = st->st_dev;
	out.st_ino = st->st_ino;
	out.st_mode = st->st_mode;
	out.st_nlink = (uint32_t)st->st_nlink;
	out.st_uid = st->st_uid;
	out.st_gid = st->st_gid;
	out.st_rdev = st->st_rdev;
	out.st_size = st->st_size;
	out.st_blksize = (int32_t)st->st_blksize;
	out.st_blocks = st->st_blocks;
	out.st_atime_sec = st->st_atim.tv_sec;
	out.st_atime_nsec = (uint64_t)st->st_atim.tv_nsec;
	out.st_mtime_sec = st->st_mtim.tv_sec;
	out.st_mtime_nsec = (uint64_t)st->st_mtim.tv_nsec;
	out.st_ctime_sec = st->st_ctim.tv_sec;
	out.st_ctime_nsec = (uint64_t)st->st_ctim.tv_nsec;
	memcpy(p, &out, sizeof(out));
	return 0;
}

static uint64_t sys_fstat(struct rv_cpu *cpu, int fd, uint64_t addr)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return neg(errno);
	/* (A connection to the GPU server that came from another process is the GPU device here too: virtgpu.c) */
	if (S_ISSOCK(st.st_mode) && rv_virtgpu_enabled())
		rv_virtgpu_fd(fd);
	return put_stat(cpu, addr, &st);
}

static uint64_t sys_newfstatat(struct rv_cpu *cpu, int dirfd, uint64_t path_addr, uint64_t addr, int flags)
{
	const char *path = user_path(cpu, path_addr);
	struct stat st;

	if (!path)
		return neg(RV_EFAULT);
	/* AT_* flag values are the same on every architecture. */
	if (fstatat(dirfd, path, &st, flags) < 0)
		return neg(errno);
	return put_stat(cpu, addr, &st);
}

static uint64_t sys_readlinkat(struct rv_cpu *cpu, int dirfd, uint64_t path_addr, uint64_t buf, uint64_t len)
{
	const char *path = user_path(cpu, path_addr);
	void *p = rv_user_ptr(cpu, buf, len, RV_PROT_WRITE);

	if (!path || !p)
		return neg(RV_EFAULT);
	/* user_path() already substituted the guest program for /proc/self/exe:
	 * the link's target is that path itself. */
	if (path == cpu->exe_path) {
		uint64_t n = strlen(cpu->exe_path);

		if (n > len)
			n = len;
		memcpy(p, cpu->exe_path, n);
		return n;
	}
	{
		long n = readlinkat(dirfd, path, p, len), mine;

		/* Another guest process's program, not the emulator that runs it (identity.c). */
		if (n > 0 && (mine = rv_exe_of(cpu->exe_path, cpu->sysroot, dirfd, path, p, len)) > 0)
			n = mine;
		return ret_errno(n);
	}
}

static uint64_t sys_faccessat(struct rv_cpu *cpu, int dirfd, uint64_t path_addr, int mode, int flags)
{
	const char *path = user_path(cpu, path_addr);

	if (!path)
		return neg(RV_EFAULT);
	return ret_errno(faccessat(dirfd, path, mode, flags));
}

static uint64_t sys_getcwd(struct rv_cpu *cpu, uint64_t buf, uint64_t len)
{
	char *p = rv_user_ptr(cpu, buf, len, RV_PROT_WRITE);

	if (!p)
		return neg(RV_EFAULT);
	if (!getcwd(p, len))
		return neg(errno);
	return strlen(p) + 1;
}

/* The kernel's struct flock on a 64-bit machine, and the fcntl commands that take one. */
struct rv_flock {
	int16_t type, whence;
	int32_t pad;
	int64_t start, len;
	int32_t pid, pad2;
};
#define RV_F_GETLK	5
#define RV_F_SETLK	6
#define RV_F_SETLKW	7
#define RV_F_OFD_GETLK	36
#define RV_F_OFD_SETLK	37
#define RV_F_OFD_SETLKW	38

/* Record locks. The lock types (F_RDLCK 0, F_WRLCK 1, F_UNLCK 2) are the same on every Linux this runs on. */
static uint64_t sys_fcntl_lock(struct rv_cpu *cpu, int fd, uint64_t cmd, uint64_t addr)
{
	struct rv_flock *guest = rv_user_ptr(cpu, addr, sizeof(*guest), RV_PROT_READ | RV_PROT_WRITE), g;
	struct flock lock;
	int host_cmd;

	if (!guest)
		return neg(RV_EFAULT);
	memcpy(&g, guest, sizeof(g));
	memset(&lock, 0, sizeof(lock));
	lock.l_type = g.type;
	lock.l_whence = g.whence;
	lock.l_start = g.start;
	lock.l_len = g.len;
	lock.l_pid = g.pid;

	switch (cmd) {
	case RV_F_GETLK:	host_cmd = F_GETLK; break;
	case RV_F_SETLK:	host_cmd = F_SETLK; break;
	case RV_F_SETLKW:	host_cmd = F_SETLKW; break;
	case RV_F_OFD_GETLK:	host_cmd = F_OFD_GETLK; break;
	case RV_F_OFD_SETLK:	host_cmd = F_OFD_SETLK; break;
	default:		host_cmd = F_OFD_SETLKW; break;
	}
	if (fcntl(fd, host_cmd, &lock) < 0)
		return neg(errno);
	if (cmd == RV_F_GETLK || cmd == RV_F_OFD_GETLK) {
		g.type = (int16_t)lock.l_type;
		g.whence = (int16_t)lock.l_whence;
		g.start = lock.l_start;
		g.len = lock.l_len;
		g.pid = lock.l_pid;
		memcpy(guest, &g, sizeof(g));
	}
	return 0;
}

/*
 * The seals of a file in memory (what may no longer be done to it: grow,
 * shrink, write). The host's, if it has such files. Where a plain file with
 * no name stands in for one (memory_file()), the seals are only remembered,
 * in this process and those it forks: they are a promise between programs
 * that share the file, kept by the kernel where there is one to keep it,
 * and a program that asks for them (Firefox, for the memory its processes
 * share) does not go on without.
 */
static uint64_t sys_fcntl_seals(struct rv_cpu *cpu, int fd, int add, unsigned seals)
{
	struct rv_proc *proc = cpu->proc;
	char link[40], name[64];
	struct stat st;
	uint64_t ret;
	ssize_t len;
	uint32_t i;
	long host = rv_host_syscall(SYS_fcntl, fd, add ? 1033 : 1034, (long)seals, 0, 0, 0);

	if (host >= 0 || errno != EINVAL)
		return ret_errno(host);
	/* One of ours? */
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	len = readlink(link, name, sizeof(name) - 1);
	if (len < 0)
		return neg(RV_EINVAL);
	name[len] = '\0';
	if (!strstr(name, "/.rv-memfd-") || fstat(fd, &st))
		return neg(RV_EINVAL);

	rv_lock(cpu);
	for (i = 0; i < proc->nseals; i++)
		if (proc->seals[i].dev == (uint64_t)st.st_dev && proc->seals[i].ino == (uint64_t)st.st_ino)
			break;
	if (!add) {
		ret = i < proc->nseals ? proc->seals[i].seals : 0;
	} else if (i < proc->nseals && (proc->seals[i].seals & 1)) {	/* F_SEAL_SEAL: no more seals */
		ret = neg(RV_EPERM);
	} else {
		if (i == proc->nseals) {
			/* (When the table is full, the oldest goes: its file is most likely gone too.) */
			if (proc->nseals == sizeof(proc->seals) / sizeof(proc->seals[0])) {
				memmove(&proc->seals[0], &proc->seals[1], sizeof(proc->seals) - sizeof(proc->seals[0]));
				i = --proc->nseals;
			}
			proc->seals[i].dev = (uint64_t)st.st_dev;
			proc->seals[i].ino = (uint64_t)st.st_ino;
			proc->seals[i].seals = 0;
			proc->nseals++;
		}
		proc->seals[i].seals |= seals;
		ret = 0;
	}
	rv_unlock(cpu);
	return ret;
}

static uint64_t sys_fcntl(struct rv_cpu *cpu, int fd, uint64_t cmd, uint64_t arg)
{
	switch (cmd) {
	case RV_F_GETLK: case RV_F_SETLK: case RV_F_SETLKW:
	case RV_F_OFD_GETLK: case RV_F_OFD_SETLK: case RV_F_OFD_SETLKW:
		return sys_fcntl_lock(cpu, fd, cmd, arg);
	case RV_F_DUPFD:
		return ret_errno(fcntl(fd, F_DUPFD, (int)arg));
	case RV_F_DUPFD_CLOEXEC:
		return ret_errno(fcntl(fd, F_DUPFD_CLOEXEC, (int)arg));
	case RV_F_GETFD:
		return ret_errno(fcntl(fd, F_GETFD));
	case RV_F_SETFD:
		return ret_errno(fcntl(fd, F_SETFD, (int)arg));	/* FD_CLOEXEC is 1 everywhere */
	case RV_F_GETFL: {
		int flags = fcntl(fd, F_GETFL);

		return flags < 0 ? neg(errno) : open_flags_to_guest(flags);
	}
	case RV_F_SETFL:
		return ret_errno(fcntl(fd, F_SETFL, open_flags_to_host(arg)));
	case 8: case 9:		/* F_SETOWN, F_GETOWN */
	case 10: case 11:	/* F_SETSIG, F_GETSIG (signal numbers are the same) */
	case 1024: case 1025:	/* F_SETLEASE, F_GETLEASE */
	case 1026:		/* F_NOTIFY */
	case 1031: case 1032:	/* F_SETPIPE_SZ, F_GETPIPE_SZ */
		/* A number in, a number out, and the same numbers on every Linux. */
		return ret_errno(rv_host_syscall(SYS_fcntl, fd, (int)cmd, (long)arg, 0, 0, 0));
	case 1033: case 1034:	/* F_ADD_SEALS, F_GET_SEALS */
		return sys_fcntl_seals(cpu, fd, cmd == 1033, (unsigned)arg);
	default:
		return neg(RV_EINVAL);
	}
}

/*
 * Terminal ioctls only. Their structures are kernel ABI and identical on the
 * generic architectures, so the guest buffer is handed to the host as is.
 */
/*
 * ioctl. The request numbers and the layouts of what they point to are the
 * same on every 64-bit Linux this runs on, so a request goes to the host as
 * it is once it is known what its argument is: a number, or the address of
 * so many bytes that the kernel reads or writes.
 *
 * Requests made with _IOR() and _IOW() say that themselves, in their number.
 * Those are passed on for the device types listed in ioctl_types, whose
 * structures hold no pointers. The older requests are plain numbers and
 * need this table.
 */
#define ARG_VALUE	0		/* the argument is a number, or there is none */
#define ARG_IN		RV_PROT_READ	/* the kernel reads what it points to */
#define ARG_OUT		RV_PROT_WRITE	/* the kernel writes it */

static const struct {
	uint32_t req;
	uint16_t size;
	uint8_t arg;
} ioctls[] = {
	/*
	 * Network interfaces, by struct ifreq (40 bytes: a name, then a union
	 * that holds no pointer for these). Chromium names each interface it
	 * finds an address on by SIOCGIFNAME and leaves out those it cannot
	 * name: with none it took itself to be offline. (Not here: SIOCGIFCONF
	 * and SIOCETHTOOL, whose arguments hold pointers.)
	 */
	{ 0x8910 /* SIOCGIFNAME */, 40, ARG_IN | ARG_OUT },
	{ 0x8913 /* SIOCGIFFLAGS */, 40, ARG_IN | ARG_OUT },
	{ 0x8914 /* SIOCSIFFLAGS */, 40, ARG_IN },
	{ 0x8915 /* SIOCGIFADDR */, 40, ARG_IN | ARG_OUT },
	{ 0x8916 /* SIOCSIFADDR */, 40, ARG_IN },
	{ 0x8917 /* SIOCGIFDSTADDR */, 40, ARG_IN | ARG_OUT },
	{ 0x8919 /* SIOCGIFBRDADDR */, 40, ARG_IN | ARG_OUT },
	{ 0x891b /* SIOCGIFNETMASK */, 40, ARG_IN | ARG_OUT },
	{ 0x891c /* SIOCSIFNETMASK */, 40, ARG_IN },
	{ 0x891d /* SIOCGIFMETRIC */, 40, ARG_IN | ARG_OUT },
	{ 0x8921 /* SIOCGIFMTU */, 40, ARG_IN | ARG_OUT },
	{ 0x8922 /* SIOCSIFMTU */, 40, ARG_IN },
	{ 0x8927 /* SIOCGIFHWADDR */, 40, ARG_IN | ARG_OUT },
	{ 0x8933 /* SIOCGIFINDEX */, 40, ARG_IN | ARG_OUT },
	{ 0x8942 /* SIOCGIFTXQLEN */, 40, ARG_IN | ARG_OUT },
	{ 0x8b01 /* SIOCGIWNAME, struct iwreq */, 32, ARG_IN | ARG_OUT },
	/* Terminals. */
	{ RV_TCGETS, RV_TERMIOS_SIZE, ARG_OUT },
	{ RV_TCSETS, RV_TERMIOS_SIZE, ARG_IN },
	{ RV_TCSETSW, RV_TERMIOS_SIZE, ARG_IN },
	{ RV_TCSETSF, RV_TERMIOS_SIZE, ARG_IN },
	{ 0x5409 /* TCSBRK */, 0, ARG_VALUE },
	{ 0x540a /* TCXONC */, 0, ARG_VALUE },
	{ 0x540b /* TCFLSH */, 0, ARG_VALUE },
	{ 0x540c /* TIOCEXCL */, 0, ARG_VALUE },
	{ 0x540d /* TIOCNXCL */, 0, ARG_VALUE },
	{ 0x540e /* TIOCSCTTY */, 0, ARG_VALUE },
	{ RV_TIOCGPGRP, 4, ARG_OUT },
	{ RV_TIOCSPGRP, 4, ARG_IN },
	{ 0x5411 /* TIOCOUTQ */, 4, ARG_OUT },
	{ RV_TIOCGWINSZ, 8, ARG_OUT },
	{ RV_TIOCSWINSZ, 8, ARG_IN },
	{ 0x5415 /* TIOCMGET */, 4, ARG_OUT },
	{ RV_FIONREAD, 4, ARG_OUT },
	{ 0x5421 /* FIONBIO */, 4, ARG_IN },
	{ 0x5422 /* TIOCNOTTY */, 0, ARG_VALUE },
	{ 0x5429 /* TIOCGSID */, 4, ARG_OUT },
	{ 0x5441 /* TIOCGPTPEER */, 0, ARG_VALUE },
	{ 0x5450 /* FIONCLEX */, 0, ARG_VALUE },
	{ 0x5451 /* FIOCLEX */, 0, ARG_VALUE },
	{ 0x5452 /* FIOASYNC */, 4, ARG_IN },
	/* The console: keyboard and display modes (KD*), virtual terminals (VT_*). */
	{ 0x4b2f /* KIOCSOUND */, 0, ARG_VALUE },
	{ 0x4b30 /* KDMKTONE */, 0, ARG_VALUE },
	{ 0x4b31 /* KDGETLED */, 1, ARG_OUT },
	{ 0x4b32 /* KDSETLED */, 0, ARG_VALUE },
	{ 0x4b33 /* KDGKBTYPE */, 1, ARG_OUT },
	{ 0x4b3a /* KDSETMODE */, 0, ARG_VALUE },
	{ 0x4b3b /* KDGETMODE */, 4, ARG_OUT },
	{ 0x4b44 /* KDGKBMODE */, 4, ARG_OUT },
	{ 0x4b45 /* KDSKBMODE */, 0, ARG_VALUE },
	{ 0x4b51 /* KDSKBMUTE */, 0, ARG_VALUE },
	{ 0x4b64 /* KDGKBLED */, 1, ARG_OUT },
	{ 0x4b65 /* KDSKBLED */, 0, ARG_VALUE },
	{ 0x5600 /* VT_OPENQRY */, 4, ARG_OUT },
	{ 0x5601 /* VT_GETMODE */, 8, ARG_OUT },
	{ 0x5602 /* VT_SETMODE */, 8, ARG_IN },
	{ 0x5603 /* VT_GETSTATE */, 6, ARG_OUT },
	{ 0x5605 /* VT_RELDISP */, 0, ARG_VALUE },
	{ 0x5606 /* VT_ACTIVATE */, 0, ARG_VALUE },
	{ 0x5607 /* VT_WAITACTIVE */, 0, ARG_VALUE },
	{ 0x5608 /* VT_DISALLOCATE */, 0, ARG_VALUE },
	/* The frame buffer. (The colour map requests have pointers in their structure: see sys_fb_cmap().) */
	{ 0x4600 /* FBIOGET_VSCREENINFO */, 160, ARG_OUT },
	{ 0x4601 /* FBIOPUT_VSCREENINFO */, 160, ARG_IN | ARG_OUT },
	{ 0x4602 /* FBIOGET_FSCREENINFO */, 80, ARG_OUT },
	{ 0x4606 /* FBIOPAN_DISPLAY */, 160, ARG_IN },
	{ 0x4611 /* FBIOBLANK */, 0, ARG_VALUE },
	/* Input devices: these two say "write" in their numbers but take a number. */
	{ 0x40044590 /* EVIOCGRAB */, 0, ARG_VALUE },
	{ 0x40044591 /* EVIOCREVOKE */, 0, ARG_VALUE },
};

/*
 * FBIOGETCMAP and FBIOPUTCMAP: struct fb_cmap is a range of colour numbers
 * and four pointers to as many 16-bit values each (the last may be null).
 */
#define RV_FBIOGETCMAP	0x4604
#define RV_FBIOPUTCMAP	0x4605

static uint64_t sys_fb_cmap(struct rv_cpu *cpu, int fd, uint64_t req, uint64_t addr)
{
	struct {
		uint32_t start, len;
		uint64_t component[4];
	} *guest = rv_user_ptr(cpu, addr, 40, RV_PROT_READ), g;
	struct {
		uint32_t start, len;
		void *component[4];
	} host;

	if (!guest)
		return neg(RV_EFAULT);
	memcpy(&g, guest, sizeof(g));
	host.start = g.start;
	host.len = g.len;
	for (int i = 0; i < 4; i++) {
		host.component[i] = NULL;
		if (g.component[i] && !(host.component[i] = rv_user_ptr(cpu, g.component[i], (uint64_t)g.len * 2,
								     req == RV_FBIOGETCMAP ? RV_PROT_WRITE : RV_PROT_READ)))
			return neg(RV_EFAULT);
	}
	return ret_errno(ioctl(fd, req, &host));
}

/*
 * Sound (ALSA): PCM devices ('A'), their controls ('U') and timers (also
 * 'T', as terminals). Most of their requests are of the plain kind, passed
 * on as their numbers say; these are not:
 *
 * - The frames a program writes or reads: struct snd_xferi holds the
 *   address of the samples, struct snd_xfern of an array of addresses, one
 *   per channel. How many bytes that is depends on the format, which is in
 *   the parameters the program set (SNDRV_PCM_IOCTL_HW_PARAMS): noted here
 *   for every descriptor, as the bits of a frame and the channels.
 * - SNDRV_CTL_IOCTL_ELEM_LIST holds the address of an array of element IDs;
 *   SNDRV_CTL_IOCTL_ELEM_ADD/REPLACE that of an enumeration's names; the TLV
 *   requests have their data after the structure, as long as it says.
 * - PAUSE and LINK say they take an int but take it by value; REWIND and
 *   FORWARD write back what they did into what they read.
 */
#define RV_SND_IOC(dir, type, nr, size)	(((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((type) << 8) | (nr))
#define RV_SNDRV_PCM_IOCTL_HW_PARAMS	RV_SND_IOC(3, 'A', 0x11, 608)
#define RV_SNDRV_PCM_IOCTL_PAUSE	RV_SND_IOC(1, 'A', 0x45, 4)
#define RV_SNDRV_PCM_IOCTL_REWIND	RV_SND_IOC(1, 'A', 0x46, 8)
#define RV_SNDRV_PCM_IOCTL_FORWARD	RV_SND_IOC(1, 'A', 0x49, 8)
#define RV_SNDRV_PCM_IOCTL_WRITEI	RV_SND_IOC(1, 'A', 0x50, 24)
#define RV_SNDRV_PCM_IOCTL_READI	RV_SND_IOC(2, 'A', 0x51, 24)
#define RV_SNDRV_PCM_IOCTL_WRITEN	RV_SND_IOC(1, 'A', 0x52, 24)
#define RV_SNDRV_PCM_IOCTL_READN	RV_SND_IOC(2, 'A', 0x53, 24)
#define RV_SNDRV_PCM_IOCTL_LINK		RV_SND_IOC(1, 'A', 0x60, 4)
#define RV_SNDRV_CTL_IOCTL_ELEM_LIST	RV_SND_IOC(3, 'U', 0x10, 80)
#define RV_SNDRV_CTL_IOCTL_ELEM_ADD	RV_SND_IOC(3, 'U', 0x17, 272)
#define RV_SNDRV_CTL_IOCTL_ELEM_REPLACE	RV_SND_IOC(3, 'U', 0x18, 272)
#define RV_SNDRV_CTL_IOCTL_TLV_READ	RV_SND_IOC(3, 'U', 0x1a, 8)
#define RV_SNDRV_CTL_IOCTL_TLV_WRITE	RV_SND_IOC(3, 'U', 0x1b, 8)
#define RV_SNDRV_CTL_IOCTL_TLV_COMMAND	RV_SND_IOC(3, 'U', 0x1c, 8)

/* Per descriptor, from its last SNDRV_PCM_IOCTL_HW_PARAMS: bits of a frame, channels (0: not set). */
#define SND_FDS	1024
static uint16_t snd_frame_bits[SND_FDS], snd_channels[SND_FDS];

static uint64_t sys_snd_xfer(struct rv_cpu *cpu, int fd, uint64_t req, uint64_t addr)
{
	int write = req == RV_SNDRV_PCM_IOCTL_WRITEI || req == RV_SNDRV_PCM_IOCTL_WRITEN;
	int interleaved = req == RV_SNDRV_PCM_IOCTL_WRITEI || req == RV_SNDRV_PCM_IOCTL_READI;
	unsigned prot = write ? RV_PROT_READ : RV_PROT_WRITE;
	struct { int64_t result; uint64_t buf; uint64_t frames; } *guest, g;
	struct { int64_t result; void *buf; uint64_t frames; } host;
	unsigned bits, channels;
	uint64_t ret;

	if (fd < 0 || fd >= SND_FDS || !(bits = snd_frame_bits[fd]) || !(channels = snd_channels[fd]))
		return neg(RV_EBADFD);
	if (!(guest = rv_user_ptr(cpu, addr, sizeof(*guest), RV_PROT_READ | RV_PROT_WRITE)))
		return neg(RV_EFAULT);
	memcpy(&g, guest, sizeof(g));
	host.result = 0;
	host.frames = g.frames;
	if (interleaved) {
		host.buf = g.frames ? rv_user_ptr(cpu, g.buf, g.frames * bits / 8, prot) : NULL;
		if (g.frames && !host.buf)
			return neg(RV_EFAULT);
	} else {
		/* (One buffer per channel; a null address skips the channel.) */
		uint64_t *bufs = rv_user_ptr(cpu, g.buf, (uint64_t)channels * 8, RV_PROT_READ);
		void *hosts[32];

		if (!bufs || channels > 32)
			return neg(RV_EFAULT);
		for (unsigned c = 0; c < channels; c++) {
			hosts[c] = NULL;
			if (bufs[c] && g.frames && !(hosts[c] = rv_user_ptr(cpu, bufs[c], g.frames * (bits / channels) / 8, prot)))
				return neg(RV_EFAULT);
		}
		host.buf = hosts;
		ret = ret_errno(ioctl(fd, req, &host));
		guest->result = host.result;
		return ret;
	}
	ret = ret_errno(ioctl(fd, req, &host));
	guest->result = host.result;
	return ret;
}

static uint64_t sys_snd_ioctl(struct rv_cpu *cpu, int fd, uint64_t req, uint64_t addr, int *done)
{
	uint8_t *p;
	uint64_t ret;

	*done = 1;
	switch (req) {
	case RV_SNDRV_PCM_IOCTL_HW_PARAMS:
		if (!(p = rv_user_ptr(cpu, addr, 608, RV_PROT_READ | RV_PROT_WRITE)))
			return neg(RV_EFAULT);
		ret = ret_errno(ioctl(fd, req, p));
		if (!ret && fd >= 0 && fd < SND_FDS) {
			uint32_t frame_bits, channels;

			/* intervals[] starts at 260, 12 bytes each: FRAME_BITS is the second, CHANNELS the third (their min). */
			memcpy(&frame_bits, p + 260 + 12, 4);
			memcpy(&channels, p + 260 + 24, 4);
			snd_frame_bits[fd] = (uint16_t)frame_bits;
			snd_channels[fd] = (uint16_t)channels;
		}
		return ret;
	case RV_SNDRV_PCM_IOCTL_PAUSE:
	case RV_SNDRV_PCM_IOCTL_LINK:
		return ret_errno(ioctl(fd, req, (unsigned long)addr));
	case RV_SNDRV_PCM_IOCTL_REWIND:
	case RV_SNDRV_PCM_IOCTL_FORWARD:
		if (!(p = rv_user_ptr(cpu, addr, 8, RV_PROT_READ | RV_PROT_WRITE)))
			return neg(RV_EFAULT);
		return ret_errno(ioctl(fd, req, p));
	case RV_SNDRV_PCM_IOCTL_WRITEI:
	case RV_SNDRV_PCM_IOCTL_READI:
	case RV_SNDRV_PCM_IOCTL_WRITEN:
	case RV_SNDRV_PCM_IOCTL_READN:
		return sys_snd_xfer(cpu, fd, req, addr);
	case RV_SNDRV_CTL_IOCTL_ELEM_LIST: {
		/* offset, space, used, count; the IDs' address; 50 bytes reserved (80 in all). An ID is 64 bytes. */
		uint8_t host[80];
		uint64_t pids;
		uint32_t space;
		void *ids = NULL;

		if (!(p = rv_user_ptr(cpu, addr, 80, RV_PROT_READ | RV_PROT_WRITE)))
			return neg(RV_EFAULT);
		memcpy(host, p, 80);
		memcpy(&space, host + 4, 4);
		memcpy(&pids, host + 16, 8);
		if (space && pids && !(ids = rv_user_ptr(cpu, pids, (uint64_t)space * 64, RV_PROT_WRITE)))
			return neg(RV_EFAULT);
		memcpy(host + 16, &ids, 8);
		ret = ret_errno(ioctl(fd, req, host));
		memcpy(p, host, 16);	/* (used and count) */
		return ret;
	}
	case RV_SNDRV_CTL_IOCTL_ELEM_ADD:
	case RV_SNDRV_CTL_IOCTL_ELEM_REPLACE: {
		/* struct snd_ctl_elem_info: the type at 64; an enumeration's names at 152, as long as the word at 160. */
		uint8_t host[272];
		uint32_t type, length;
		uint64_t names;
		void *n;

		if (!(p = rv_user_ptr(cpu, addr, 272, RV_PROT_READ | RV_PROT_WRITE)))
			return neg(RV_EFAULT);
		memcpy(host, p, 272);
		memcpy(&type, host + 64, 4);
		memcpy(&names, host + 152, 8);
		memcpy(&length, host + 160, 4);
		if (type != 3 /* SNDRV_CTL_ELEM_TYPE_ENUMERATED */ || !names)
			return ret_errno(ioctl(fd, req, p));
		if (!(n = rv_user_ptr(cpu, names, length, RV_PROT_READ)))
			return neg(RV_EFAULT);
		memcpy(host + 152, &n, 8);
		ret = ret_errno(ioctl(fd, req, host));
		memcpy(host + 152, &names, 8);
		memcpy(p, host, 272);
		return ret;
	}
	case RV_SNDRV_CTL_IOCTL_TLV_READ:
	case RV_SNDRV_CTL_IOCTL_TLV_WRITE:
	case RV_SNDRV_CTL_IOCTL_TLV_COMMAND: {
		uint32_t length;

		if (!(p = rv_user_ptr(cpu, addr, 8, RV_PROT_READ)))
			return neg(RV_EFAULT);
		memcpy(&length, p + 4, 4);
		if (!(p = rv_user_ptr(cpu, addr, 8 + (uint64_t)length, RV_PROT_READ | RV_PROT_WRITE)))
			return neg(RV_EFAULT);
		return ret_errno(ioctl(fd, req, p));
	}
	}
	/* Requests without an argument: the PCM's PREPARE, START, DROP, DRAIN ...; a timer's START, STOP ... */
	if (!(req >> 16) && (((req >> 8) & 0xff) == 'A' || ((req >> 8) & 0xff) == 'U' ||
			     (((req >> 8) & 0xff) == 'T' && (req & 0xff) >= 0xa0)))
		return ret_errno(ioctl(fd, req, 0UL));
	*done = 0;
	return 0;
}

/* Device types whose _IOR/_IOW requests are passed on: input devices, terminals and sound timers, sound. */
static const char ioctl_types[] = "ETAU";

static uint64_t sys_ioctl(struct rv_cpu *cpu, int fd, uint64_t req, uint64_t arg)
{
	uint64_t size = 0;
	unsigned prot = ARG_VALUE;
	int known = 0;
	void *p;

	req &= 0xffffffffu;
	/* DRM's requests, on the emulated GPU, and sync_file's on its fences (virtgpu.c). */
	if (((req >> 8) & 0xff) == 'd' || ((req >> 8) & 0xff) == '>') {
		uint64_t ret;

		if (rv_virtgpu_ioctl(cpu, fd, req, arg, &ret))
			return ret;
	}
	if (req == RV_FBIOGETCMAP || req == RV_FBIOPUTCMAP)
		return sys_fb_cmap(cpu, fd, req, arg);
	if (((req >> 8) & 0xff) == 'A' || ((req >> 8) & 0xff) == 'U' || ((req >> 8) & 0xff) == 'T') {
		int done;
		uint64_t ret = sys_snd_ioctl(cpu, fd, req, arg, &done);

		if (done)
			return ret;
	}
	for (unsigned i = 0; i < sizeof(ioctls) / sizeof(ioctls[0]) && !known; i++) {
		if (ioctls[i].req == req) {
			size = ioctls[i].size;
			prot = ioctls[i].arg;
			known = 1;
		}
	}
	if (!known && (req >> 30) && strchr(ioctl_types, (int)((req >> 8) & 0xff)) && ((req >> 8) & 0xff)) {
		/* _IOC(): bit 30 says the kernel reads the argument, bit 31 that it writes it; 14 bits of size. */
		size = (req >> 16) & 0x3fff;
		prot = ((req >> 30) & 1 ? ARG_IN : 0) | ((req >> 30) & 2 ? ARG_OUT : 0);
		known = 1;
	}
	if (!known) {
		if (cpu->verbose)
			fprintf(stderr, "rv: unimplemented ioctl 0x%llx on descriptor %d\n", (unsigned long long)req, fd);
		return neg(RV_ENOTTY);
	}

	if (prot == ARG_VALUE)
		return ret_errno(ioctl(fd, req, (unsigned long)arg));
	p = rv_user_ptr(cpu, arg, size, prot);
	if (!p)
		return neg(RV_EFAULT);
	return ret_errno(ioctl(fd, req, p));
}

static uint64_t sys_uname(struct rv_cpu *cpu, uint64_t addr)
{
	/* struct new_utsname: six fields of 65 bytes. */
	char (*uts)[65] = rv_user_ptr(cpu, addr, 6 * 65, RV_PROT_WRITE);

	if (!uts)
		return neg(RV_EFAULT);
	memset(uts, 0, 6 * 65);
	strcpy(uts[0], "Linux");
	strcpy(uts[1], "rv");
	strcpy(uts[2], "6.6.0");	/* libcs refuse to start on kernels they consider too old */
	strcpy(uts[3], "#1 SMP rv");
	strcpy(uts[4], "riscv64");
	strcpy(uts[5], "(none)");
	return 0;
}

static uint64_t sys_clock_getres(struct rv_cpu *cpu, uint64_t clock, uint64_t addr)
{
	struct rv_timespec out;
	struct timespec ts;

	if (clock_getres((clockid_t)clock, &ts) < 0)
		return neg(errno);
	out.sec = ts.tv_sec;
	out.nsec = ts.tv_nsec;
	if (addr) {
		void *p = rv_user_ptr(cpu, addr, sizeof(out), RV_PROT_WRITE);

		if (!p)
			return neg(RV_EFAULT);
		memcpy(p, &out, sizeof(out));
	}
	return 0;
}

static uint64_t sys_clock_gettime(struct rv_cpu *cpu, uint64_t clock, uint64_t addr)
{
	struct rv_timespec out;
	struct timespec ts;
	void *p = rv_user_ptr(cpu, addr, sizeof(out), RV_PROT_WRITE);

	if (!p)
		return neg(RV_EFAULT);
	/* Clock ids are the same on every architecture. */
	if (clock_gettime((clockid_t)clock, &ts) < 0)
		return neg(errno);
	out.sec = ts.tv_sec;
	out.nsec = ts.tv_nsec;
	memcpy(p, &out, sizeof(out));
	return 0;
}

static uint64_t sys_getrandom(struct rv_cpu *cpu, uint64_t buf, uint64_t len, uint64_t flags)
{
	void *p = rv_user_ptr(cpu, buf, len, RV_PROT_WRITE);

	if (!p)
		return neg(RV_EFAULT);
	return ret_errno(getrandom(p, len, (unsigned)flags));
}

/*
 * Resource limits are the host's (the numbers and the structure are the
 * same), except the one for the stack, which is the guest's own stack and
 * was fixed when the program was loaded.
 */
static uint64_t sys_prlimit64(struct rv_cpu *cpu, uint64_t resource, uint64_t new_addr, uint64_t old_addr)
{
	uint64_t lim[2], *old = NULL, *new = NULL;
	struct rlimit host;

	if (old_addr && !(old = rv_user_ptr(cpu, old_addr, sizeof(lim), RV_PROT_WRITE)))
		return neg(RV_EFAULT);
	if (new_addr && !(new = rv_user_ptr(cpu, new_addr, sizeof(lim), RV_PROT_READ)))
		return neg(RV_EFAULT);

	if (resource == RV_RLIMIT_STACK) {
		lim[0] = cpu->stack_size;
		lim[1] = RV_RLIM_INFINITY;
	} else {
		if (getrlimit((int)resource, &host) < 0)
			return neg(errno);
		lim[0] = host.rlim_cur == RLIM_INFINITY ? RV_RLIM_INFINITY : host.rlim_cur;
		lim[1] = host.rlim_max == RLIM_INFINITY ? RV_RLIM_INFINITY : host.rlim_max;
	}
	if (new && resource != RV_RLIMIT_STACK) {
		uint64_t want[2];

		memcpy(want, new, sizeof(want));
		host.rlim_cur = want[0] == RV_RLIM_INFINITY ? RLIM_INFINITY : want[0];
		host.rlim_max = want[1] == RV_RLIM_INFINITY ? RLIM_INFINITY : want[1];
		if (setrlimit((int)resource, &host) < 0)
			return neg(errno);
	}
	if (old)
		memcpy(old, lim, sizeof(lim));
	return 0;
}

static uint64_t sys_pipe2(struct rv_cpu *cpu, uint64_t addr, uint64_t flags)
{
	int32_t *p = rv_user_ptr(cpu, addr, 2 * sizeof(int32_t), RV_PROT_WRITE);
	int fds[2];

	if (!p)
		return neg(RV_EFAULT);
	if (pipe2(fds, open_flags_to_host(flags)) < 0)
		return neg(errno);
	p[0] = fds[0];
	p[1] = fds[1];
	return 0;
}

static uint64_t sys_path1(struct rv_cpu *cpu, uint64_t nr, int dirfd, uint64_t path_addr, uint64_t arg)
{
	const char *path = user_path(cpu, path_addr);

	if (!path)
		return neg(RV_EFAULT);
	switch (nr) {
	case RV_SYS_mkdirat:
		return ret_errno(mkdirat(dirfd, path, (mode_t)arg));
	case RV_SYS_unlinkat:
		return ret_errno(unlinkat(dirfd, path, (int)arg));
	default:
		return ret_errno(chdir(path));
	}
}

/*
 * chroot(): the host's, and that is most of it. From then on the kernel
 * resolves every absolute path of this process, the guest's and ours,
 * from the new root; the working directory stays where it was (fs/open.c,
 * chroot(2)). What that changes for the emulator:
 *
 * - The sysroot (-L) put the guest's world under a directory, and the new
 *   root is a directory of that world: from here on the guest's paths are
 *   the kernel's, as they are for any program inside a chroot.
 *
 * - Another emulator (for exec, and for fork on Linux/Wasm, fork.c) is
 *   started by way of /proc/self/exe, which the new root need not have
 *   (sshd's /run/sshd is an empty directory). So a descriptor for our own
 *   program is opened first and kept, out of the way of the guest's
 *   numbers (proc->self_fd), and handed down to the emulators started
 *   from here on, for their own execs and forks. Interpreters of scripts
 *   are left to the kernel: binfmt_misc on the machine in the page has
 *   the F flag, so it finds /bin/rv in any root.
 *
 * - The guest program's path (/proc/self/exe to the guest) is a path from
 *   the old root. Where the program is inside the new root it becomes the
 *   path from there, which is what the kernel would show; otherwise it
 *   stays a path from the old root, as the kernel's would (d_path() of a
 *   file outside the root), though opening it then fails where opening
 *   the kernel's link would not.
 *
 * Whatever else the emulator opens for itself later is under /proc and
 * /sys (the processors for /proc/cpuinfo, other processes' programs), and
 * in the new root it is there or not, the same for us as for the guest.
 */
static uint64_t sys_chroot(struct rv_cpu *cpu, uint64_t path_addr)
{
	static char root[RV_PATH_MAX];
	struct rv_proc *proc = cpu->proc;
	const char *path;
	ssize_t rootlen = -1;
	int opened = 0, err;

	rv_lock(cpu);
	if (!(path = user_path(cpu, path_addr))) {
		rv_unlock(cpu);
		return neg(RV_EFAULT);
	}
	if (proc->self_fd < 0) {
		int fd = open("/proc/self/exe", O_PATH | O_CLOEXEC);

		if (fd >= 0) {
			proc->self_fd = fcntl(fd, F_DUPFD_CLOEXEC, 64);
			opened = proc->self_fd >= 0;
			close(fd);
		}
	}
	/* The new root's name from the old one, links followed, as the program's path is (main.c). */
	{
		char link[40];
		int fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);

		if (fd >= 0) {
			snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
			rootlen = readlink(link, root, sizeof(root) - 1);
			close(fd);
		}
	}
	if (chroot(path) < 0) {
		err = errno;
		if (opened) {
			close(proc->self_fd);
			proc->self_fd = -1;
		}
		rv_unlock(cpu);
		return neg(err);
	}
	for (struct rv_cpu *t = proc->threads; t; t = t->next_thread) {
		t->sysroot[0] = '\0';
		if (rootlen > 1 && root[0] == '/' && t->exe_path &&
		    !strncmp(t->exe_path, root, (size_t)rootlen) && t->exe_path[rootlen] == '/')
			t->exe_path += rootlen;
	}
	rv_unlock(cpu);
	return 0;
}

/* Copy a host value out to guest memory. */
static uint64_t put_user(struct rv_cpu *cpu, uint64_t addr, const void *src, uint64_t size)
{
	void *p = rv_user_ptr(cpu, addr, size, RV_PROT_WRITE);

	if (!p)
		return neg(RV_EFAULT);
	memcpy(p, src, size);
	return 0;
}

/* A guest timespec; the layout matches the host's on every 64-bit Linux. */
static int get_timespec(struct rv_cpu *cpu, uint64_t addr, struct timespec *ts)
{
	struct rv_timespec in;
	void *p = rv_user_ptr(cpu, addr, sizeof(in), RV_PROT_READ);

	if (!p)
		return -1;
	memcpy(&in, p, sizeof(in));
	ts->tv_sec = in.sec;
	ts->tv_nsec = in.nsec;
	return 0;
}

static uint64_t put_timespec(struct rv_cpu *cpu, uint64_t addr, const struct timespec *ts)
{
	struct rv_timespec out = { ts->tv_sec, ts->tv_nsec };

	return put_user(cpu, addr, &out, sizeof(out));
}

/* struct linux_dirent64 is the same everywhere, so the host fills the guest buffer. */
static uint64_t sys_getdents64(struct rv_cpu *cpu, int fd, uint64_t buf, uint64_t len)
{
	void *p = rv_user_ptr(cpu, buf, len, RV_PROT_WRITE);

	if (!p)
		return neg(RV_EFAULT);
	return ret_errno(getdents64(fd, p, len));
}

static uint64_t put_statfs(struct rv_cpu *cpu, uint64_t addr, const struct statfs *st)
{
	struct rv_statfs out;

	memset(&out, 0, sizeof(out));
	out.f_type = (int64_t)st->f_type;
	out.f_bsize = (int64_t)st->f_bsize;
	out.f_blocks = st->f_blocks;
	out.f_bfree = st->f_bfree;
	out.f_bavail = st->f_bavail;
	/*
	 * A file system that lives in memory (ramfs, where Linux/Wasm has its
	 * root) says it has no blocks at all, and programs that check for room
	 * before they write take that at its word (apt: "You don't have enough
	 * free space"). What it has is what memory is free.
	 */
	if (!st->f_blocks) {
		struct sysinfo info;

		if (!sysinfo(&info) && out.f_bsize > 0) {
			uint64_t unit = info.mem_unit ? info.mem_unit : 1;

			out.f_blocks = info.totalram * unit / (uint64_t)out.f_bsize;
			out.f_bfree = out.f_bavail = info.freeram * unit / (uint64_t)out.f_bsize;
			out.f_files = out.f_ffree = out.f_bfree;
		}
	}
	out.f_files = st->f_files;
	out.f_ffree = st->f_ffree;
	memcpy(out.f_fsid, &st->f_fsid, sizeof(out.f_fsid));
	out.f_namelen = (int64_t)st->f_namelen;
	out.f_frsize = (int64_t)st->f_frsize;
	out.f_flags = (int64_t)st->f_flags;
	return put_user(cpu, addr, &out, sizeof(out));
}

static uint64_t sys_statfs(struct rv_cpu *cpu, uint64_t path_addr, uint64_t addr)
{
	const char *path = user_path(cpu, path_addr);
	struct statfs st;

	if (!path)
		return neg(RV_EFAULT);
	if (statfs(path, &st) < 0)
		return neg(errno);
	return put_statfs(cpu, addr, &st);
}

static uint64_t sys_fstatfs(struct rv_cpu *cpu, int fd, uint64_t addr)
{
	struct statfs st;

	if (fstatfs(fd, &st) < 0)
		return neg(errno);
	return put_statfs(cpu, addr, &st);
}

static uint64_t sys_statx(struct rv_cpu *cpu, int dirfd, uint64_t path_addr, int flags,
			  unsigned mask, uint64_t addr)
{
	const char *path = user_path(cpu, path_addr);
	void *p = rv_user_ptr(cpu, addr, RV_STATX_SIZE, RV_PROT_WRITE);

	if (!path || !p)
		return neg(RV_EFAULT);
	{
		long r = statx(dirfd, path, flags, mask, p);

		if (r == 0 && rv_virtgpu_enabled())
			rv_virtgpu_fix_statx(p);
		return ret_errno(r);
	}
}

static uint64_t sys_sysinfo(struct rv_cpu *cpu, uint64_t addr)
{
	struct sysinfo si;

	if (sysinfo(&si) < 0)
		return neg(errno);
	/* libcs may pad their struct, but its start is the kernel's layout. */
	return put_user(cpu, addr, &si, RV_SYSINFO_SIZE);
}

static uint64_t sys_getrusage(struct rv_cpu *cpu, int who, uint64_t addr)
{
	struct rusage ru;

	if (getrusage(who, &ru) < 0)
		return neg(errno);
	return put_user(cpu, addr, &ru, RV_RUSAGE_SIZE);
}

static uint64_t sys_times(struct rv_cpu *cpu, uint64_t addr)
{
	struct tms t;
	clock_t ticks = times(&t);
	int64_t out[4] = { t.tms_utime, t.tms_stime, t.tms_cutime, t.tms_cstime };

	if (ticks == (clock_t)-1)
		return neg(errno);
	if (addr && put_user(cpu, addr, out, sizeof(out)))
		return neg(RV_EFAULT);
	return (uint64_t)ticks;
}

static uint64_t sys_gettimeofday(struct rv_cpu *cpu, uint64_t addr)
{
	struct timeval tv;
	int64_t out[2];

	gettimeofday(&tv, NULL);
	out[0] = tv.tv_sec;
	out[1] = tv.tv_usec;
	return addr ? put_user(cpu, addr, out, sizeof(out)) : 0;
}

static uint64_t sys_sleep(struct rv_cpu *cpu, uint64_t clock, int flags, uint64_t req_addr, uint64_t rem_addr)
{
	struct timespec req, rem = { 0, 0 };
	int err;

	if (get_timespec(cpu, req_addr, &req))
		return neg(RV_EFAULT);
	for (;;) {
		/* clock_nanosleep() returns the error number itself. */
		err = clock_nanosleep((clockid_t)clock, flags, &req, &rem);
		if (err != EINTR || rv_signal_ready(cpu))
			break;
		/* Woken for nothing the guest is to see (a kick, thread.c): park if asked to, and sleep the rest. */
		rv_safepoint(cpu);
		if (!(flags & TIMER_ABSTIME))
			req = rem;
	}
	if (err == EINTR && rem_addr && !flags)
		put_timespec(cpu, rem_addr, &rem);
	return err ? neg(err) : 0;
}

static uint64_t sys_getgroups(struct rv_cpu *cpu, uint64_t size, uint64_t addr)
{
	gid_t *list = NULL;

	if (size) {
		list = rv_user_ptr(cpu, addr, size * sizeof(gid_t), RV_PROT_WRITE);
		if (!list)
			return neg(RV_EFAULT);
	}
	return ret_errno(getgroups((int)size, list));
}

static uint64_t sys_setgroups(struct rv_cpu *cpu, uint64_t size, uint64_t addr)
{
	gid_t *list = rv_user_ptr(cpu, addr, size * sizeof(gid_t), RV_PROT_READ);

	if (size && !list)
		return neg(RV_EFAULT);
	return ret_errno(rv_host_syscall(SYS_setgroups, size, list, 0, 0, 0, 0));	/* (as for setuid, see rv_syscall()) */
}

static uint64_t sys_getres(struct rv_cpu *cpu, int groups, uint64_t r, uint64_t e, uint64_t s)
{
	uint32_t ids[3];
	int ret = groups ? getresgid(&ids[0], &ids[1], &ids[2]) : getresuid(&ids[0], &ids[1], &ids[2]);

	if (ret < 0)
		return neg(errno);
	if (put_user(cpu, r, &ids[0], 4) || put_user(cpu, e, &ids[1], 4) || put_user(cpu, s, &ids[2], 4))
		return neg(RV_EFAULT);
	return 0;
}

static uint64_t sys_sendfile(struct rv_cpu *cpu, int out_fd, int in_fd, uint64_t off_addr, uint64_t count)
{
	off_t off;
	ssize_t n;

	if (!off_addr)
		return ret_errno(sendfile(out_fd, in_fd, NULL, count));
	{
		int64_t *p = rv_user_ptr(cpu, off_addr, 8, RV_PROT_READ | RV_PROT_WRITE);

		if (!p)
			return neg(RV_EFAULT);
		off = (off_t)*p;
		n = sendfile(out_fd, in_fd, &off, count);
		if (n < 0)
			return neg(errno);
		*p = off;
		return (uint64_t)n;
	}
}

/*
 * copy_file_range(), done with reads and writes here: the host has no such call to pass it to. Callers that
 * have their own fallback (cp, dpkg) take ENOSYS as the sign to use it, but KIO, which Dolphin copies with,
 * takes it for an error ("Cannot copy file ... (Errno: 38)": no file could be pasted). As the kernel does
 * it: from the file position, or from *off_in / to *off_out (then the position stays and the offset moves),
 * up to len bytes or the end of the input; the bytes copied, or an error if none were.
 */
static uint64_t sys_copy_file_range(struct rv_cpu *cpu, int fd_in, uint64_t off_in_addr, int fd_out,
				    uint64_t off_out_addr, uint64_t len, unsigned int flags)
{
	int64_t *off_in = NULL, *off_out = NULL;
	static char buf[65536];
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	uint64_t done = 0;
	long err = 0;

	if (flags)
		return neg(RV_EINVAL);
	if (off_in_addr && !(off_in = rv_user_ptr(cpu, off_in_addr, 8, RV_PROT_READ | RV_PROT_WRITE)))
		return neg(RV_EFAULT);
	if (off_out_addr && !(off_out = rv_user_ptr(cpu, off_out_addr, 8, RV_PROT_READ | RV_PROT_WRITE)))
		return neg(RV_EFAULT);
	pthread_mutex_lock(&lock);
	while (done < len) {
		size_t want = len - done < sizeof(buf) ? (size_t)(len - done) : sizeof(buf);
		ssize_t n = off_in ? pread(fd_in, buf, want, (off_t)*off_in) : read(fd_in, buf, want);
		ssize_t w = 0;

		if (n < 0) {
			err = errno;
			break;
		}
		if (n == 0)
			break;
		while (w < n) {
			ssize_t m = off_out ? pwrite(fd_out, buf + w, n - w, (off_t)*off_out + w) : write(fd_out, buf + w, n - w);

			if (m <= 0) {
				err = m < 0 ? errno : RV_EIO;
				break;
			}
			w += m;
		}
		if (off_in)
			*off_in += w;
		else if (w < n)
			lseek(fd_in, (off_t)(w - n), SEEK_CUR);	/* (what was read and not written is read again next time) */
		if (off_out)
			*off_out += w;
		done += w;
		if (err)
			break;
	}
	pthread_mutex_unlock(&lock);
	if (!done && err)
		return neg(err);
	return done;
}

/* A guest signal set (64 bits) as a host sigset_t: never with rv's own signal (signal.c). */
static int get_sigset(struct rv_cpu *cpu, uint64_t addr, uint64_t *mask, sigset_t *set)
{
	uint64_t *p = rv_user_ptr(cpu, addr, 8, RV_PROT_READ);

	if (!p)
		return -1;
	memcpy(mask, p, 8);
	rv_signal_host_set(*mask, set);
	return 0;
}

/* struct pollfd is the same everywhere. */
static uint64_t sys_ppoll(struct rv_cpu *cpu, uint64_t fds_addr, uint64_t nfds, uint64_t ts_addr, uint64_t mask_addr)
{
	static __thread struct pollfd none;
	/* No descriptors at all is how a C library spells pause() or a sleep. */
	struct pollfd *fds = nfds ? rv_user_ptr(cpu, fds_addr, nfds * sizeof(*fds), RV_PROT_READ | RV_PROT_WRITE)
				  : &none;
	struct timespec ts;
	sigset_t set;
	uint64_t mask;
	int ret;

	if (!fds || (ts_addr && get_timespec(cpu, ts_addr, &ts)) ||
	    (mask_addr && get_sigset(cpu, mask_addr, &mask, &set)))
		return neg(RV_EFAULT);

	ret = ppoll(fds, nfds, ts_addr ? &ts : NULL, mask_addr ? &set : NULL);
	if (ret < 0 && errno == EINTR && mask_addr)
		rv_signal_temp_mask(cpu, mask);
	return ret_errno(ret);
}

static uint64_t sys_pselect6(struct rv_cpu *cpu, int n, uint64_t r, uint64_t w, uint64_t e,
			     uint64_t ts_addr, uint64_t sig_addr)
{
	/* The kernel reads and writes whole longs covering n bits. */
	uint64_t bytes = ((uint64_t)(n > 0 ? n : 0) + 63) / 64 * 8;
	fd_set *sets[3] = { NULL, NULL, NULL };
	uint64_t addrs[3] = { r, w, e };
	uint64_t mask = 0, mask_addr = 0;
	struct timespec ts;
	sigset_t set;
	int ret;

	if (n < 0 || n > FD_SETSIZE)
		return neg(RV_EINVAL);
	for (int i = 0; i < 3; i++) {
		if (!addrs[i])
			continue;
		sets[i] = rv_user_ptr(cpu, addrs[i], bytes, RV_PROT_READ | RV_PROT_WRITE);
		if (!sets[i])
			return neg(RV_EFAULT);
	}
	if (ts_addr && get_timespec(cpu, ts_addr, &ts))
		return neg(RV_EFAULT);
	/* The sixth argument points at { sigset pointer, sigset size }. */
	if (sig_addr) {
		uint64_t *p = rv_user_ptr(cpu, sig_addr, 16, RV_PROT_READ);

		if (!p)
			return neg(RV_EFAULT);
		memcpy(&mask_addr, p, 8);
		if (mask_addr && get_sigset(cpu, mask_addr, &mask, &set))
			return neg(RV_EFAULT);
	}
	ret = pselect(n, sets[0], sets[1], sets[2], ts_addr ? &ts : NULL, mask_addr ? &set : NULL);
	if (ret < 0 && errno == EINTR && mask_addr)
		rv_signal_temp_mask(cpu, mask);
	return ret_errno(ret);
}

/* struct itimerval is four longs on every 64-bit Linux. */
static uint64_t sys_itimer(struct rv_cpu *cpu, int set, int which, uint64_t new_addr, uint64_t old_addr)
{
	struct itimerval new_value, old_value;
	int ret;

	if (set && new_addr) {
		void *p = rv_user_ptr(cpu, new_addr, sizeof(new_value), RV_PROT_READ);

		if (!p)
			return neg(RV_EFAULT);
		memcpy(&new_value, p, sizeof(new_value));
	}
	ret = set ? setitimer(which, new_addr ? &new_value : NULL, &old_value)
		  : getitimer(which, &old_value);
	if (ret < 0)
		return neg(errno);
	if (old_addr && put_user(cpu, old_addr, &old_value, sizeof(old_value)))
		return neg(RV_EFAULT);
	return 0;
}

static uint64_t sys_utimensat(struct rv_cpu *cpu, int dirfd, uint64_t path_addr, uint64_t times_addr, int flags)
{
	struct timespec ts[2];
	const char *path = NULL;

	if (times_addr && (get_timespec(cpu, times_addr, &ts[0]) || get_timespec(cpu, times_addr + 16, &ts[1])))
		return neg(RV_EFAULT);
	if (!path_addr)	/* a NULL path means the file descriptor itself */
		return ret_errno(futimens(dirfd, times_addr ? ts : NULL));
	path = user_path(cpu, path_addr);
	if (!path)
		return neg(RV_EFAULT);
	return ret_errno(utimensat(dirfd, path, times_addr ? ts : NULL, flags));
}

/* Calls that take two paths. */
static uint64_t sys_path2(struct rv_cpu *cpu, uint64_t nr, uint64_t a0, uint64_t a1,
			  uint64_t a2, uint64_t a3, uint64_t a4)
{
	const char *p1, *p2;

	switch (nr) {
	case RV_SYS_symlinkat: /* (target, newdirfd, linkpath): the target is stored, not resolved */
		p1 = user_string(cpu, a0);
		p2 = user_path(cpu, a2);
		if (!p1 || !p2)
			return neg(RV_EFAULT);
		return ret_errno(symlinkat(p1, (int)a1, p2));
	case RV_SYS_linkat: /* (olddirfd, old, newdirfd, new, flags) */
		p1 = user_path(cpu, a1);
		p2 = user_path(cpu, a3);
		if (!p1 || !p2)
			return neg(RV_EFAULT);
		return ret_errno(linkat((int)a0, p1, (int)a2, p2, (int)a4));
	default: /* renameat2(olddirfd, old, newdirfd, new, flags) */
		p1 = user_path(cpu, a1);
		p2 = user_path(cpu, a3);
		if (!p1 || !p2)
			return neg(RV_EFAULT);
		if (a4)
			return neg(RV_EINVAL);	/* RENAME_NOREPLACE and friends: not yet */
		return ret_errno(renameat((int)a0, p1, (int)a2, p2));
	}
}

/* More calls that take one path. */
static uint64_t sys_path_misc(struct rv_cpu *cpu, uint64_t nr, uint64_t a0, uint64_t a1,
			      uint64_t a2, uint64_t a3, uint64_t a4)
{
	const char *path = user_path(cpu, nr == RV_SYS_truncate ? a0 : a1);

	if (!path)
		return neg(RV_EFAULT);
	switch (nr) {
	case RV_SYS_truncate:
		return ret_errno(truncate(path, (off_t)a1));
	case RV_SYS_fchmodat:
		return ret_errno(fchmodat((int)a0, path, (mode_t)a2, 0));
	case RV_SYS_fchownat:
		return ret_errno(fchownat((int)a0, path, (uid_t)a2, (gid_t)a3, (int)a4));
	default: /* mknodat */
		return ret_errno(mknodat((int)a0, path, (mode_t)a2, (dev_t)a3));
	}
}

static uint64_t sys_prctl(struct rv_cpu *cpu, uint64_t option, uint64_t arg, uint64_t arg2, uint64_t arg3)
{
	char name[16];
	void *p;

	switch (option) {
	case RV_PR_SET_NAME:
		/* The thread's name is the host thread's: it is what ps shows. */
		if (!(p = rv_user_ptr(cpu, arg, 1, RV_PROT_READ)) || !rv_user_strlen(cpu, arg))
			return neg(RV_EFAULT);
		memset(name, 0, sizeof(name));
		for (unsigned i = 0; i < sizeof(name) - 1; i++) {
			const char *c = rv_user_ptr(cpu, arg + i, 1, RV_PROT_READ);

			if (!c || !*c)
				break;
			name[i] = *c;
		}
		return ret_errno(prctl(PR_SET_NAME, (unsigned long)name, 0UL, 0UL, 0UL));
	case RV_PR_GET_NAME:
		memset(name, 0, sizeof(name));
		if (prctl(PR_GET_NAME, (unsigned long)name, 0UL, 0UL, 0UL) < 0)
			return neg(errno);
		return put_user(cpu, arg, name, sizeof(name));
	case 1:		/* PR_SET_PDEATHSIG (signal numbers are the same) */
	case 4:		/* PR_SET_DUMPABLE */
	case 3:		/* PR_GET_DUMPABLE */
	case 7:		/* PR_GET_KEEPCAPS */
	case 8:		/* PR_SET_KEEPCAPS */
	case 23:	/* PR_CAPBSET_READ */
	case 24:	/* PR_CAPBSET_DROP */
	case 27:	/* PR_GET_SECUREBITS */
	case 28:	/* PR_SET_SECUREBITS */
	case 29:	/* PR_SET_TIMERSLACK */
	case 30:	/* PR_GET_TIMERSLACK */
	case 36:	/* PR_SET_CHILD_SUBREAPER */
	case 38:	/* PR_SET_NO_NEW_PRIVS */
	case 39:	/* PR_GET_NO_NEW_PRIVS */
		/* Numbers in, a number out: the process is the host's, and so are these properties of it. */
		return ret_errno(prctl((int)option, (unsigned long)arg, (unsigned long)arg2, (unsigned long)arg3, 0UL));
	case 47:	/* PR_CAP_AMBIENT */
		return ret_errno(prctl((int)option, (unsigned long)arg, (unsigned long)arg2, 0UL, 0UL));
	case 2:		/* PR_GET_PDEATHSIG */
	case 37: {	/* PR_GET_CHILD_SUBREAPER */
		int value = 0;

		if (prctl((int)option, (unsigned long)&value, 0UL, 0UL, 0UL) < 0)
			return neg(errno);
		return put_user(cpu, arg, &value, sizeof(value));
	}
	case 21:	/* PR_GET_SECCOMP */
		return rv_seccomp_prctl(cpu, 0, 0, 0);
	case 22:	/* PR_SET_SECCOMP: kept and run here, not by the host (seccomp.c) */
		return rv_seccomp_prctl(cpu, 1, arg, arg2);
	default:
		return neg(RV_EINVAL);
	}
}

/* The CPUs the guest's threads can be on: the host's answer, since the threads are the host's (thread.c). */
static uint64_t sys_sched_getaffinity(struct rv_cpu *cpu, uint64_t size, uint64_t addr)
{
	uint64_t mask[16];
	long ret;

	if (size < sizeof(mask[0]))
		return neg(RV_EINVAL);
	if (size > sizeof(mask))
		size = sizeof(mask);
	memset(mask, 0, sizeof(mask));
	ret = rv_host_syscall(SYS_sched_getaffinity, 0, size, mask, 0, 0, 0);
	if (ret <= 0) {
		mask[0] = 1;
		ret = sizeof(mask[0]);
	}
	if (put_user(cpu, addr, mask, (uint64_t)ret))
		return neg(RV_EFAULT);
	return (uint64_t)ret;
}

/*
 * waitid(): the host's, with what it says about the child written the
 * guest's way. (siginfo for a child is the same in every Linux: signal,
 * errno and code, then, at 16, the process, the user and the status.)
 */
static uint64_t sys_waitid(struct rv_cpu *cpu, int idtype, int64_t id, uint64_t info_addr, int options, uint64_t ru_addr)
{
	uint8_t out[128];
	siginfo_t info;
	struct rusage ru;
	int32_t v;
	long ret;

	memset(&info, 0, sizeof(info));
	ret = rv_host_syscall(SYS_waitid, idtype, id, &info, options, ru_addr ? &ru : 0, 0);
	if (ret < 0)
		return neg(errno);
	if (info_addr) {
		memset(out, 0, sizeof(out));
		v = info.si_signo;
		memcpy(out, &v, 4);
		v = info.si_errno;
		memcpy(out + 4, &v, 4);
		v = info.si_code;
		memcpy(out + 8, &v, 4);
		v = (int32_t)info.si_pid;
		memcpy(out + 16, &v, 4);
		v = (int32_t)info.si_uid;
		memcpy(out + 20, &v, 4);
		v = info.si_status;
		memcpy(out + 24, &v, 4);
		if (put_user(cpu, info_addr, out, sizeof(out)))
			return neg(RV_EFAULT);
	}
	if (ru_addr && put_user(cpu, ru_addr, &ru, RV_RUSAGE_SIZE))
		return neg(RV_EFAULT);
	return 0;
}

/* close_range(): the host's, around the descriptors of our own that the guest must not close (the display's, our program's). */
static uint64_t sys_close_range(struct rv_cpu *cpu, uint64_t first, uint64_t last, uint64_t flags)
{
	struct rv_proc *proc = cpu->proc;
	uint64_t keep[2] = {
		proc->fb_len ? (uint64_t)proc->fb_fd : ~0ULL,
		proc->self_fd >= 0 ? (uint64_t)proc->self_fd : ~0ULL,
	};
	long ret = 0;

	if (first > last)
		return neg(RV_EINVAL);
	if (first > 0xffffffffULL)
		return 0;
	if (last > 0xffffffffULL)
		last = 0xffffffffULL;
	if (keep[0] > keep[1]) {
		uint64_t t = keep[0];

		keep[0] = keep[1];
		keep[1] = t;
	}
	/* The stretches between them, lowest first. */
	for (int k = 0; k < 2 && ret >= 0 && first <= last; k++) {
		if (keep[k] < first || keep[k] > last)
			continue;
		if (keep[k] > first)
			ret = rv_host_syscall(SYS_close_range, first, keep[k] - 1, flags, 0, 0, 0);
		first = keep[k] + 1;
	}
	if (ret >= 0 && first <= last && first <= 0xffffffffULL)
		ret = rv_host_syscall(SYS_close_range, first, last, flags, 0, 0, 0);
	return ret_errno(ret);
}

/* The host's memfd_create(); -1 with ENOSYS where it has none, or where RV_MEMFD=0 says to do without it, as on Linux/Wasm, to test the stand-ins elsewhere. */
static long host_memfd(const char *name, unsigned flags)
{
	static int no_memfd = -1;

	if (no_memfd < 0)
		no_memfd = getenv("RV_MEMFD") && !strcmp(getenv("RV_MEMFD"), "0");
	if (no_memfd) {
		errno = ENOSYS;
		return -1;
	}
	return rv_host_syscall(SYS_memfd_create, name, flags, 0, 0, 0, 0);
}

/*
 * memfd_create(): a file that is nowhere but in memory, to be mapped shared
 * (see map_host_shared() in mm.c). The host's, or where it has none
 * (Linux/Wasm: its kernel has no tmpfs proper), a file in a memory file
 * system that has no name any more, which is the same thing but for the
 * seals such a file can be given.
 */
static long memory_file(const char *name, unsigned flags)
{
	static const char *const dirs[] = { "/dev/shm", "/tmp", "/run" };
	char path[64];
	long fd;

	fd = host_memfd(name, flags);
	if (fd >= 0 || errno != ENOSYS)
		return fd;
	for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(path, sizeof(path), "%s/.rv-memfd-XXXXXX", dirs[i]);
		fd = mkostemp(path, (flags & 1) ? O_CLOEXEC : 0);	/* MFD_CLOEXEC */
		if (fd >= 0) {
			unlink(path);
			return fd;
		}
	}
	return -1;
}

/* All of `len` bytes, to a descriptor that may take them in pieces. 0, or -1 (and for a pipe that is full, with O_NONBLOCK on it, instead of waiting). */
static int write_whole(int fd, const void *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		buf = (const char *)buf + n;
		len -= (size_t)n;
	}
	return 0;
}

/*
 * A descriptor for the next emulator to inherit, with `size` bytes behind
 * it (the system call filters across an exec: seccomp.c). A file in memory
 * where the host has them. Where it has not (Linux/Wasm), a pipe: that
 * needs no file system at all (the machine's first file system has no
 * directory to make a file in), only room for everything at once, since
 * nobody reads it before the exec; it is widened first where it is too
 * narrow. Failing that, a file without a name, where a directory will have
 * one. -1 when none can be had: then the filters cannot go along.
 */
static long handover(const void *data, size_t size)
{
	long fd = host_memfd("rv-seccomp", 0);
	int fds[2];

	if (fd < 0 && errno == ENOSYS) {
		if (pipe2(fds, 0) == 0) {
			if (fcntl(fds[1], F_GETPIPE_SZ) < (long)size)
				fcntl(fds[1], F_SETPIPE_SZ, (int)size);
			fcntl(fds[1], F_SETFL, O_NONBLOCK);
			if (!write_whole(fds[1], data, size)) {
				close(fds[1]);
				return fds[0];
			}
			close(fds[0]);
			close(fds[1]);
		}
		fd = memory_file("rv-seccomp", 0);
	}
	if (fd < 0)
		return -1;
	if (write_whole((int)fd, data, size)) {
		close((int)fd);
		return -1;
	}
	return fd;
}

/*
 * fallocate(): the host's. Where the file system has none (ramfs, which is
 * what a memory file is on Linux/Wasm) and the call asks for the plain
 * thing, room up to offset + len, a truncate gives the same: zeros. And on a
 * kernel without an MMU it is the one way to pages that lie together, which
 * a shared mapping of the file needs (fs/ramfs/file-nommu.c gives a file
 * contiguous pages when it is truncated up from nothing, and only then):
 * glibc's posix_fallocate() would otherwise make room by writing a byte into
 * every block, and the file's pages would be anywhere. That is how Wayland
 * clients make the files their windows are drawn in (weston's
 * os_create_anonymous_file(), which Xwayland and libwayland-cursor share).
 */
static uint64_t sys_fallocate(int fd, int mode, uint64_t offset, uint64_t len)
{
	long ret = fallocate(fd, mode, (off_t)offset, (off_t)len);
	struct stat st;

	if (ret < 0 && errno == EOPNOTSUPP && mode == 0 && !fstat(fd, &st) && S_ISREG(st.st_mode)) {
		if ((uint64_t)st.st_size >= offset + len)
			return 0;
		ret = ftruncate(fd, (off_t)(offset + len));
	}
	return ret_errno(ret);
}

/*
 * The flags memfd_create() knows: MFD_CLOEXEC, MFD_ALLOW_SEALING,
 * MFD_HUGETLB, MFD_NOEXEC_SEAL and MFD_EXEC, and with MFD_HUGETLB a page
 * size in bits 26 to 31. Anything else is EINVAL, which programs count on:
 * Chromium asks with every bit set to learn whether there is a
 * memfd_create() at all (EINVAL: yes; ENOSYS: no) and stops if it gets a
 * file. The stand-in below would make one for any flags.
 */
#define MFD_KNOWN	0x1fULL
#define MFD_HUGE_BITS	(0x3fULL << 26)

static uint64_t sys_memfd_create(struct rv_cpu *cpu, uint64_t name_addr, uint64_t flags)
{
	const char *name = user_string(cpu, name_addr);

	flags = (uint32_t)flags;	/* (an unsigned int to the kernel) */
	if (flags & ~(MFD_KNOWN | ((flags & 4) ? MFD_HUGE_BITS : 0)) || (flags & 0x18) == 0x18)
		return neg(RV_EINVAL);
	if (!name)
		return neg(RV_EFAULT);
	if (strlen(name) > 249)
		return neg(RV_EINVAL);
	return ret_errno(memory_file(name, (unsigned)flags));
}

/* Whether `path` is the file in /proc that lists this process's own mappings: /proc/self/maps and its other names. */
static int is_own_maps(const char *path)
{
	char own[24];
	size_t len = strlen(path), n;

	if (len < 12 || strncmp(path, "/proc/", 6) || strcmp(path + len - 5, "/maps"))
		return 0;
	path += 6;
	if (!strncmp(path, "thread-self/", 12))
		return path[12] == 'm';
	snprintf(own, sizeof(own), "%d/", (int)getpid());
	n = !strncmp(path, "self/", 5) ? 5 : !strncmp(path, own, strlen(own)) ? strlen(own) : 0;
	if (!n)
		return 0;
	path += n;
	if (!strcmp(path, "maps"))
		return 1;
	/* task/<a thread of ours>/maps: all threads have the same. */
	if (strncmp(path, "task/", 5))
		return 0;
	path += 5;
	while (*path >= '0' && *path <= '9')
		path++;
	return !strcmp(path, "/maps");
}

/* capset(): the host's, like capget() below. Version 3's data is two sets of three words. */
static uint64_t sys_capset(struct rv_cpu *cpu, uint64_t header_addr, uint64_t data_addr)
{
	uint32_t *header = rv_user_ptr(cpu, header_addr, 8, RV_PROT_READ | RV_PROT_WRITE);
	void *data = rv_user_ptr(cpu, data_addr, 24, RV_PROT_READ);

	if (!header || !data)
		return neg(RV_EFAULT);
	return ret_errno(rv_host_syscall(SYS_capset, header, data, 0, 0, 0, 0));
}

/* capget(): the host's answer (the structures are the same everywhere). */
static uint64_t sys_capget(struct rv_cpu *cpu, uint64_t header_addr, uint64_t data_addr)
{
	uint32_t *header = rv_user_ptr(cpu, header_addr, 8, RV_PROT_READ | RV_PROT_WRITE);
	void *data = data_addr ? rv_user_ptr(cpu, data_addr, 24, RV_PROT_WRITE) : NULL;

	if (!header || (data_addr && !data))
		return neg(RV_EFAULT);
	return ret_errno(rv_host_syscall(SYS_capget, header, data, 0, 0, 0, 0));
}

static int is_riscv_elf(const char *path)
{
	unsigned char hdr[20];
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = read(fd, hdr, sizeof(hdr));
	close(fd);
	return n == (ssize_t)sizeof(hdr) && !memcmp(hdr, "\177ELF", 4) &&
	       hdr[18] == 243 && hdr[19] == 0;	/* e_machine == EM_RISCV */
}

/* Translate a NULL-terminated guest array of string pointers. */
static int get_strings(struct rv_cpu *cpu, uint64_t addr, char **out, int max)
{
	int n = 0;

	if (!addr) {
		out[0] = NULL;
		return 0;
	}
	for (;; n++) {
		uint64_t *slot = rv_user_ptr(cpu, addr + (uint64_t)n * 8, 8, RV_PROT_READ);

		if (!slot || n >= max - 1)
			return -1;
		if (!*slot)
			break;
		if (!(out[n] = (char *)(uintptr_t)user_string(cpu, *slot)))
			return -1;
	}
	out[n] = NULL;
	return n;
}

/*
 * execve() replaces the whole host process, emulator included. A riscv64
 * program is started under a fresh copy of the emulator, so this does not
 * depend on binfmt_misc being set up; anything else is left to the host.
 */
static uint64_t sys_execve(struct rv_cpu *cpu, uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr)
{
	/* Static: these do not fit the small process stack on Linux/Wasm. */
	static char *argv[RV_ARG_MAX + 2], *envp[RV_ARG_MAX];
	const char *path;
	int err, seccomp_fd = -1, envc = 0;

	/*
	 * With the lock, to the end: the arrays are the process's, and they
	 * point into the arena, which must stay where it is. (The other
	 * threads end with the exec; the host sees to that.)
	 */
	rv_lock(cpu);
	path = user_path(cpu, path_addr);
	if (!path || get_strings(cpu, argv_addr, argv + 2, RV_ARG_MAX) < 0 ||
	    get_strings(cpu, envp_addr, envp, RV_ARG_MAX) < 0) {
		rv_unlock(cpu);
		return neg(RV_EFAULT);
	}
	if (access(path, X_OK) < 0) {
		err = errno;
		rv_unlock(cpu);
		return neg(err);
	}

	if (cpu->trace)
		fflush(cpu->trace);
	/*
	 * RV_SECCOMP in the new program's environment is ours: it says where
	 * this thread's system call filters are for the next emulator to take
	 * up (seccomp.c). Whatever the guest put there under that name goes,
	 * and so does RV_SELF, which is ours in the same way (below).
	 */
	{
		static char filters[32];
		void *image;
		size_t size;

		for (int k = 0; envp[k]; k++)
			if (strncmp(envp[k], "RV_SECCOMP=", 11) && strncmp(envp[k], "RV_SELF=", 8))
				envp[envc++] = envp[k];
		envp[envc] = NULL;
		image = rv_seccomp_save(cpu, &size);
		if (size) {
			if (image) {
				seccomp_fd = (int)handover(image, size);
				free(image);
			}
			if (seccomp_fd < 0 || envc >= RV_ARG_MAX - 1 || !is_riscv_elf(path)) {
				/*
				 * They cannot go along (or the program is not one an
				 * emulator runs): then neither does the exec. A program
				 * without its filters is not an option.
				 */
				if (seccomp_fd >= 0)
					close(seccomp_fd);
				fprintf(stderr, "rv: not executing %s: its system call filters cannot go along\n", path);
				rv_unlock(cpu);
				return neg(RV_EACCES);
			}
			snprintf(filters, sizeof(filters), "RV_SECCOMP=%d", seccomp_fd);
			envp[envc++] = filters;
			envp[envc] = NULL;
		}
	}
	if (is_riscv_elf(path)) {
		/* rv [-L SYSROOT] PROGRAM ARGS...: the guest's argv[0] gives way to the path. */
		static char *self[RV_ARG_MAX + 6];
		int n = 0;

		self[n++] = (char *)"rv";
		if (cpu->sysroot[0]) {
			self[n++] = (char *)"-L";
			self[n++] = cpu->sysroot;
		}
		self[n++] = (char *)path;
		for (int k = 3; argv[k - 1] && argv[k]; k++)
			self[n++] = argv[k];
		self[n] = NULL;
		if (cpu->proc->self_fd >= 0 && envc < RV_ARG_MAX - 1) {
			/*
			 * After a chroot(): by the descriptor we hold for our own
			 * program, which the next emulator also gets, named to it
			 * as RV_SELF (main.c), for the execs and forks it does.
			 */
			static char held[32];
			int fd = cpu->proc->self_fd;

			snprintf(held, sizeof(held), "RV_SELF=%d", fd);
			envp[envc++] = held;
			envp[envc] = NULL;
			fcntl(fd, F_SETFD, 0);
			rv_host_syscall(SYS_execveat, fd, "", self, envp, AT_EMPTY_PATH, 0);
			err = errno;
			fcntl(fd, F_SETFD, FD_CLOEXEC);
			envp[--envc] = NULL;
			errno = err;
		} else {
			execve("/proc/self/exe", self, envp);
		}
	} else {
		execve(path, argv + 2, envp);
	}
	err = errno;
	if (seccomp_fd >= 0)
		close(seccomp_fd);
	rv_unlock(cpu);
	return neg(err);
}

/*
 * fork() and vfork(), as clone(flags, stack, ptid, tls, ctid). vfork() is
 * treated like fork(): the child gets a copy instead of borrowing the
 * parent's memory, which is allowed. Threads are a separate matter
 * (thread.c).
 *
 * Where the host has fork() it duplicates this whole process, guest arena
 * included, which is precisely a fork of the guest. Linux/Wasm has none, so
 * there a second emulator is started from a snapshot (fork.c).
 */
/*
 * In the parent, after a fork: with CLONE_PIDFD the caller also gets a
 * descriptor for the child (where the parent's thread id would go), to wait
 * for it with and to signal it by. The guest's child is a process of the
 * host's, so that is the host's descriptor for it.
 */
static uint64_t child_started(struct rv_cpu *cpu, int64_t pid, uint64_t flags, uint64_t ptid)
{
	if (flags & RV_CLONE_PIDFD) {
		int32_t fd = (int32_t)rv_host_syscall(SYS_pidfd_open, pid, 0, 0, 0, 0, 0);

		if (fd < 0 || put_user(cpu, ptid, &fd, sizeof(fd))) {
			int err = fd < 0 ? errno : EFAULT;

			if (fd >= 0)
				close(fd);
			/* The caller will not know about the child: there must not be one. */
			kill((pid_t)pid, SIGKILL);
			waitpid((pid_t)pid, NULL, 0);
			return neg(err);
		}
	}
	return (uint64_t)pid;
}

static uint64_t sys_clone(struct rv_cpu *cpu, uint64_t flags, uint64_t stack, uint64_t ptid,
			  uint64_t tls, uint64_t ctid)
{
	uint64_t allowed = RV_CSIGNAL | RV_CLONE_VM | RV_CLONE_VFORK | RV_CLONE_PIDFD |
			   RV_CLONE_CHILD_CLEARTID | RV_CLONE_CHILD_SETTID | RV_CLONE_NEW;
	uint64_t namespaces = flags & RV_CLONE_NEW;
	int apart = (namespaces & RV_CLONE_NEWPID) || cpu->proc->children_apart;
	uint64_t set_tid_addr = (flags & RV_CLONE_CHILD_SETTID) ? ctid : 0;
	uint64_t clear_tid_addr = (flags & RV_CLONE_CHILD_CLEARTID) ? ctid : 0;
	int64_t pid;

	/* A thread is a different matter altogether: see thread.c. */
	if (flags & RV_CLONE_THREAD)
		return rv_thread_clone(cpu, flags, stack, ptid, tls, ctid);

	if ((flags & ~allowed) || (flags & RV_CSIGNAL) != RV_SIGCHLD)
		return neg(RV_ENOSYS);
	/* CLONE_VM without CLONE_VFORK or CLONE_THREAD: a process sharing our memory. */
	if ((flags & RV_CLONE_VM) && !(flags & RV_CLONE_VFORK))
		return neg(RV_ENOSYS);

	if (cpu->trace)
		fflush(cpu->trace);
	fflush(stderr);

#ifndef __wasm__
	if (!cpu->fork_snapshot) {
		/* With the other threads parked (thread.c): the child gets the guest as it is between two instructions. */
		int restore;

		rv_lock(cpu);
		rv_stop_world(cpu);
		/* (Namespaces for the child: see rv_namespace_children().) */
		if (rv_namespace_children(namespaces, &restore)) {
			pid = -1;
		} else {
			pid = fork();
			if (pid != 0) {
				int err = errno;

				rv_namespace_restore(restore);
				errno = err;
			}
		}
		if (pid != 0) {
			int err = errno;

			rv_start_world(cpu);
			rv_unlock(cpu);
			return pid < 0 ? neg(err) : child_started(cpu, pid, flags, ptid);
		}
		if (pid == 0) {
			int32_t tid = (int32_t)getpid();

			if (stack)
				cpu->x[2] = stack;
			if (set_tid_addr)
				put_user(cpu, set_tid_addr, &tid, sizeof(tid));
			/* The instruction trace belongs to the parent, as do its pending signals
			 * and all threads but the one that forked. */
			cpu->trace = NULL;
			rv_thread_after_fork(cpu);
			rv_jit_after_fork();
			rv_mm_after_fork(cpu, apart);
			cpu->proc->children_apart = 0;
			if ((namespaces & ~RV_CLONE_NEWPID) && unshare((int)(namespaces & ~RV_CLONE_NEWPID)) < 0)
				_exit(127);
			cpu->clear_child_tid = clear_tid_addr;
			if (cpu->proc->fb_len) {	/* the display stays the parent's */
				close(cpu->proc->fb_fd);
				cpu->proc->fb_len = 0;
			}
			return 0;
		}
	}
#endif
	pid = rv_fork_snapshot(cpu, stack, set_tid_addr, clear_tid_addr, namespaces, apart);
	return pid < 0 ? neg((int)-pid) : child_started(cpu, pid, flags, ptid);
}

static uint64_t sys_wait4(struct rv_cpu *cpu, int64_t pid, uint64_t status_addr, int options, uint64_t ru_addr)
{
	struct rusage ru;
	int status = 0;
	pid_t ret;

	ret = wait4((pid_t)pid, &status, options, ru_addr ? &ru : NULL);
	if (ret < 0)
		return neg(errno);
	/* The status encoding and the W* option bits are the same on the host. */
	if (ret > 0 && status_addr && put_user(cpu, status_addr, &status, sizeof(int32_t)))
		return neg(RV_EFAULT);
	if (ret > 0 && ru_addr && put_user(cpu, ru_addr, &ru, RV_RUSAGE_SIZE))
		return neg(RV_EFAULT);
	return (uint64_t)ret;
}

/*
 * Sockets: the calls go to the host as they are. Addresses and options are
 * bytes the emulator does not interpret (their layout is the same on every
 * Linux), so a socket does whatever the host's sockets do; where the host
 * has none (Linux/Wasm in a browser) the guest is told so.
 */
#define RV_SOCKADDR_MAX	128	/* sizeof(struct sockaddr_storage) */
#define RV_ENOTSOCK	88
#define RV_EAFNOSUPPORT	97

/*
 * The result of a socket call on descriptor fd (-1: a call that makes one).
 * A host built without networking answers ENOSYS to all of them. Programs do
 * not expect that from Linux: bash asks getpeername() about its standard
 * input to see whether it was started by a remote shell daemon, and anything
 * but "not a socket" means yes to it. So the guest is told what a Linux with
 * networking would say when there happen to be no sockets.
 */
static uint64_t socket_result(long ret, int fd)
{
	if (ret >= 0 || errno != ENOSYS)
		return ret_errno(ret);
	if (fd < 0)
		return neg(RV_EAFNOSUPPORT);
	return neg(fcntl(fd, F_GETFD) < 0 ? RV_EBADF : RV_ENOTSOCK);
}

/* A socket address the guest passes in: its bytes, or NULL if they are not readable. */
static void *sockaddr_in(struct rv_cpu *cpu, uint64_t addr, uint64_t len)
{
	return len <= RV_SOCKADDR_MAX ? rv_user_ptr(cpu, addr, len, RV_PROT_READ) : NULL;
}

/* getsockname, getpeername, accept: an address comes back into a buffer whose size the guest also passes. */
static uint64_t sys_sockaddr_out(struct rv_cpu *cpu, uint64_t nr, int fd, uint64_t addr, uint64_t len_addr, int flags)
{
	static __thread uint8_t buf[RV_SOCKADDR_MAX];
	socklen_t len = sizeof(buf);
	uint32_t *guest_len = addr ? rv_user_ptr(cpu, len_addr, 4, RV_PROT_READ | RV_PROT_WRITE) : NULL;
	uint32_t room = 0;
	void *guest_addr = NULL;
	int ret;

	if (addr && !guest_len)
		return neg(RV_EFAULT);
	if (guest_len)
		memcpy(&room, guest_len, 4);
	if (addr && !(guest_addr = rv_user_ptr(cpu, addr, room, RV_PROT_WRITE)))
		return neg(RV_EFAULT);

	switch (nr) {
	case RV_SYS_getsockname:	ret = getsockname(fd, (struct sockaddr *)buf, &len); break;
	case RV_SYS_getpeername:	ret = getpeername(fd, (struct sockaddr *)buf, &len); break;
	default:			ret = accept4(fd, addr ? (struct sockaddr *)buf : NULL, addr ? &len : NULL, flags);
	}
	if (ret < 0)
		return socket_result(ret, fd);
	if (addr) {
		memcpy(guest_addr, buf, len < room ? len : room);
		memcpy(guest_len, &len, 4);
	}
	return (uint64_t)ret;
}

static uint64_t sys_socketpair(struct rv_cpu *cpu, int domain, int type, int protocol, uint64_t addr)
{
	int32_t *p = rv_user_ptr(cpu, addr, 2 * sizeof(int32_t), RV_PROT_WRITE);
	int fds[2];

	if (!p)
		return neg(RV_EFAULT);
	if (socketpair(domain, type, protocol, fds) < 0)
		return socket_result(-1, -1);
	memcpy(p, fds, sizeof(fds));
	return 0;
}

static uint64_t sys_sendto(struct rv_cpu *cpu, int fd, uint64_t buf, uint64_t len, int flags,
			   uint64_t addr, uint64_t addr_len)
{
	void *p = rv_user_ptr(cpu, buf, len, RV_PROT_READ);
	void *to = addr ? sockaddr_in(cpu, addr, addr_len) : NULL;

	if (!p || (addr && !to))
		return neg(RV_EFAULT);
	return socket_result(sendto(fd, p, len, flags, to, to ? (socklen_t)addr_len : 0), fd);
}

static uint64_t sys_recvfrom(struct rv_cpu *cpu, int fd, uint64_t buf, uint64_t len, int flags,
			     uint64_t addr, uint64_t len_addr)
{
	static __thread uint8_t from[RV_SOCKADDR_MAX];
	socklen_t from_len = sizeof(from);
	void *p = rv_user_ptr(cpu, buf, len, RV_PROT_WRITE);
	uint32_t *guest_len = addr ? rv_user_ptr(cpu, len_addr, 4, RV_PROT_READ | RV_PROT_WRITE) : NULL;
	uint32_t room = 0;
	void *guest_addr = NULL;
	ssize_t ret;

	if (!p || (addr && !guest_len))
		return neg(RV_EFAULT);
	if (guest_len)
		memcpy(&room, guest_len, 4);
	if (addr && !(guest_addr = rv_user_ptr(cpu, addr, room, RV_PROT_WRITE)))
		return neg(RV_EFAULT);
	ret = recvfrom(fd, p, len, flags, addr ? (struct sockaddr *)from : NULL, addr ? &from_len : NULL);
	if (ret < 0)
		return socket_result(ret, fd);
	if (addr) {
		memcpy(guest_addr, from, from_len < room ? from_len : room);
		memcpy(guest_len, &from_len, 4);
	}
	return (uint64_t)ret;
}

static uint64_t sys_sockopt(struct rv_cpu *cpu, int get, int fd, int level, int name, uint64_t val, uint64_t len_arg)
{
	uint32_t len = (uint32_t)len_arg, *guest_len = NULL;
	socklen_t host_len;
	void *p;

	if (get) {
		guest_len = rv_user_ptr(cpu, len_arg, 4, RV_PROT_READ | RV_PROT_WRITE);
		if (!guest_len)
			return neg(RV_EFAULT);
		memcpy(&len, guest_len, 4);
	}
	p = rv_user_ptr(cpu, val, len, get ? RV_PROT_WRITE : RV_PROT_READ);
	if (!p)
		return neg(RV_EFAULT);
	host_len = len;
	if (get ? getsockopt(fd, level, name, p, &host_len) : setsockopt(fd, level, name, p, host_len))
		return socket_result(-1, fd);
	if (get) {
		len = host_len;
		memcpy(guest_len, &len, 4);
	}
	return 0;
}

/*
 * epoll. The guest's struct epoll_event is 16 bytes with its 64 bits of data
 * aligned, as on every 64-bit Linux except x86-64, where the host's is packed
 * into 12: there (the native build on a PC, for testing) the events are
 * translated through a copy.
 */
struct rv_epoll_event {
	uint32_t events;
	uint32_t pad;
	uint64_t data;
};
#define EPOLL_SAME_LAYOUT (sizeof(struct epoll_event) == sizeof(struct rv_epoll_event))
#define EPOLL_MAX_EVENTS 256

static uint64_t sys_epoll_ctl(struct rv_cpu *cpu, int epfd, int op, int fd, uint64_t addr)
{
	struct epoll_event host, *event = NULL;

	if (op != EPOLL_CTL_DEL) {
		const struct rv_epoll_event *g = rv_user_ptr(cpu, addr, sizeof(*g), RV_PROT_READ);

		if (!g)
			return neg(RV_EFAULT);
		memset(&host, 0, sizeof(host));
		host.events = g->events;
		host.data.u64 = g->data;
		event = &host;
	}
	return ret_errno(epoll_ctl(epfd, op, fd, event));
}

static uint64_t sys_epoll_pwait(struct rv_cpu *cpu, int epfd, uint64_t addr, int max, int timeout, uint64_t mask_addr)
{
	struct rv_epoll_event *events;
	struct epoll_event host[EPOLL_SAME_LAYOUT ? 1 : EPOLL_MAX_EVENTS], *out;
	sigset_t set;
	uint64_t mask;
	int ret;

	if (max <= 0)
		return neg(RV_EINVAL);
	if (!EPOLL_SAME_LAYOUT && max > EPOLL_MAX_EVENTS)
		max = EPOLL_MAX_EVENTS;
	events = rv_user_ptr(cpu, addr, (uint64_t)max * sizeof(*events), RV_PROT_WRITE);
	if (!events || (mask_addr && get_sigset(cpu, mask_addr, &mask, &set)))
		return neg(RV_EFAULT);

	out = EPOLL_SAME_LAYOUT ? (struct epoll_event *)(void *)events : host;
	ret = epoll_pwait(epfd, out, max, timeout, mask_addr ? &set : NULL);
	if (ret < 0 && errno == EINTR && mask_addr)
		rv_signal_temp_mask(cpu, mask);
	if (!EPOLL_SAME_LAYOUT) {
		for (int i = 0; i < ret; i++) {
			events[i].events = host[i].events;
			events[i].pad = 0;
			events[i].data = host[i].data.u64;
		}
	}
	return ret_errno(ret);
}

static uint64_t sys_timerfd_settime(struct rv_cpu *cpu, int fd, int flags, uint64_t new_addr, uint64_t old_addr)
{
	/* struct itimerspec: two of the kernel's timespecs. */
	struct itimerspec *new = rv_user_ptr(cpu, new_addr, sizeof(*new), RV_PROT_READ);
	struct itimerspec *old = old_addr ? rv_user_ptr(cpu, old_addr, sizeof(*old), RV_PROT_WRITE) : NULL;

	if (!new || (old_addr && !old))
		return neg(RV_EFAULT);
	return ret_errno(timerfd_settime(fd, flags, new, old));
}

static uint64_t sys_timerfd_gettime(struct rv_cpu *cpu, int fd, uint64_t addr)
{
	struct itimerspec *value = rv_user_ptr(cpu, addr, sizeof(*value), RV_PROT_WRITE);

	return value ? ret_errno(timerfd_gettime(fd, value)) : neg(RV_EFAULT);
}

/* The kernel's struct msghdr on a 64-bit machine. */
struct rv_msghdr {
	uint64_t name;
	uint32_t namelen, pad0;
	uint64_t iov, iovlen;
	uint64_t control, controllen;
	uint32_t flags, pad1;
};

/* sendmsg and recvmsg: the vector's and the other buffers' addresses are translated, their contents are not looked at. */
static uint64_t sys_msg(struct rv_cpu *cpu, int fd, uint64_t msg_addr, int flags, int sending)
{
	/* Static: these do not fit the small process stack on Linux/Wasm. */
	static __thread struct iovec host[RV_IOV_MAX];
	static __thread uint8_t name[RV_SOCKADDR_MAX];
	unsigned prot = sending ? RV_PROT_READ : RV_PROT_WRITE;
	struct rv_msghdr *guest = rv_user_ptr(cpu, msg_addr, sizeof(*guest), RV_PROT_READ | RV_PROT_WRITE), m;
	void *guest_name = NULL;
	struct rv_iovec *vec;
	struct msghdr h;
	ssize_t ret;

	if (!guest)
		return neg(RV_EFAULT);
	memcpy(&m, guest, sizeof(m));
	if (m.iovlen > RV_IOV_MAX)
		return neg(RV_EINVAL);
	vec = rv_user_ptr(cpu, m.iov, m.iovlen * sizeof(*vec), RV_PROT_READ);
	if (!vec || (m.namelen > RV_SOCKADDR_MAX && sending))
		return neg(RV_EFAULT);
	for (uint64_t i = 0; i < m.iovlen; i++) {
		struct rv_iovec v;

		memcpy(&v, &vec[i], sizeof(v));
		host[i].iov_base = rv_user_ptr(cpu, v.base, v.len, prot);
		if (!host[i].iov_base)
			return neg(RV_EFAULT);
		host[i].iov_len = v.len;
	}

	memset(&h, 0, sizeof(h));
	h.msg_iov = host;
	h.msg_iovlen = m.iovlen;
	if (m.name) {
		uint32_t len = m.namelen < RV_SOCKADDR_MAX ? m.namelen : RV_SOCKADDR_MAX;

		if (!(guest_name = rv_user_ptr(cpu, m.name, len, prot)))
			return neg(RV_EFAULT);
		if (sending)
			memcpy(name, guest_name, len);
		h.msg_name = name;
		h.msg_namelen = len;
	}
	if (m.control) {
		h.msg_control = rv_user_ptr(cpu, m.control, m.controllen, prot);
		if (!h.msg_control)
			return neg(RV_EFAULT);
		h.msg_controllen = m.controllen;
	}

	ret = sending ? sendmsg(fd, &h, flags) : recvmsg(fd, &h, flags);
	if (ret < 0)
		return socket_result(ret, fd);
	if (!sending) {
		if (m.name)
			memcpy(guest_name, name, h.msg_namelen < m.namelen ? h.msg_namelen : m.namelen);
		m.namelen = m.name ? (uint32_t)h.msg_namelen : 0;
		m.controllen = h.msg_controllen;
		m.flags = (uint32_t)h.msg_flags;
		memcpy(guest, &m, sizeof(m));
	}
	return (uint64_t)ret;
}

/*
 * sendmmsg and recvmmsg: several messages in one call, each a struct msghdr
 * followed by the number of bytes that went through. One at a time here.
 * (glibc's resolver sends its two queries, for IPv4 and IPv6 addresses, with
 * sendmmsg and has no other way if that fails.)
 */
static uint64_t sys_mmsg(struct rv_cpu *cpu, int fd, uint64_t vec_addr, uint64_t count, int flags, int sending)
{
	uint64_t done = 0, ret = 0;

	if (count > RV_IOV_MAX)
		count = RV_IOV_MAX;
	for (; done < count; done++) {
		uint64_t entry = vec_addr + done * (sizeof(struct rv_msghdr) + 8);
		uint32_t *len = rv_user_ptr(cpu, entry + sizeof(struct rv_msghdr), 4, RV_PROT_WRITE);
		uint32_t n;

		if (!len) {
			ret = neg(RV_EFAULT);
			break;
		}
		ret = sys_msg(cpu, fd, entry, flags, sending);
		if (ret >= (uint64_t)-4095)
			break;
		n = (uint32_t)ret;
		memcpy(len, &n, 4);
		/* After the first message only take what is there already. */
		if (!sending)
			flags |= MSG_DONTWAIT;
	}
	return done ? done : ret;
}

/* The extended-attribute calls that read: `kind` picks get or list, by path, by link or by descriptor. */
static uint64_t sys_xattr(struct rv_cpu *cpu, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int by_fd = nr == RV_SYS_fgetxattr || nr == RV_SYS_flistxattr;
	int list = nr >= RV_SYS_listxattr;
	const char *path = by_fd ? NULL : user_path(cpu, a0);
	uint64_t buf_addr = list ? a1 : a2, size = list ? a2 : a3;
	const char *name = list ? NULL : user_string(cpu, a1);
	void *buf = rv_user_ptr(cpu, buf_addr, size, RV_PROT_WRITE);
	ssize_t ret;

	if ((!by_fd && !path) || (!list && !name) || (size && !buf))
		return neg(RV_EFAULT);
	switch (nr) {
	case RV_SYS_getxattr:	ret = getxattr(path, name, buf, size); break;
	case RV_SYS_lgetxattr:	ret = lgetxattr(path, name, buf, size); break;
	case RV_SYS_fgetxattr:	ret = fgetxattr((int)a0, name, buf, size); break;
	case RV_SYS_listxattr:	ret = listxattr(path, buf, size); break;
	case RV_SYS_llistxattr:	ret = llistxattr(path, buf, size); break;
	default:		ret = flistxattr((int)a0, buf, size); break;
	}
	return ret_errno(ret);
}

/* mincore: everything that is mapped is in core. */
static uint64_t sys_mincore(struct rv_cpu *cpu, uint64_t addr, uint64_t len, uint64_t vec_addr)
{
	uint64_t pages = RV_PAGE_ALIGN(len) >> RV_PAGE_SHIFT;
	uint8_t *vec = rv_user_ptr(cpu, vec_addr, pages, RV_PROT_WRITE);

	if (addr & (RV_PAGE_SIZE - 1))
		return neg(RV_EINVAL);
	if (!vec)
		return neg(RV_EFAULT);
	for (uint64_t i = 0; i < pages; i++) {
		uint64_t page = (addr >> RV_PAGE_SHIFT) + i;

		if (page >= cpu->npages || !(cpu->map[page] & RV_PAGE_MAPPED))
			return neg(RV_ENOMEM);
		vec[i] = 1;
	}
	return 0;
}

/*
 * riscv_hwprobe: what the processor can do, as key and value pairs. C
 * libraries ask at start-up, mainly to pick a memcpy for processors on which
 * unaligned accesses are fast, which they are here; JavaScript engines, to
 * know which extensions they may generate code for.
 */
static uint64_t sys_riscv_hwprobe(struct rv_cpu *cpu, uint64_t pairs_addr, uint64_t count, uint64_t flags)
{
	struct { int64_t key; uint64_t value; } pair;
	uint8_t *pairs = rv_user_ptr(cpu, pairs_addr, count * sizeof(pair), RV_PROT_READ | RV_PROT_WRITE);

	if (flags)
		return neg(RV_EINVAL);
	if (!pairs)
		return neg(RV_EFAULT);
	for (uint64_t i = 0; i < count; i++) {
		memcpy(&pair, pairs + i * sizeof(pair), sizeof(pair));
		switch (pair.key) {
		case 0:	pair.value = RV_MVENDORID; break;	/* vendor, architecture, implementation ids (rv.h) */
		case 1:	pair.value = RV_MARCHID; break;
		case 2:	pair.value = RV_MIMPID; break;
		case 3:	pair.value = 1; break;			/* base behaviour: IMA */
		case 4:	/* extensions: FD (bit 0), C (1), and Zba, Zbb, Zbs (3, 4, 5; bitmanip.c) */
			pair.value = 1 | 2 | (rv_zb_advertised() ? 1 << 3 | 1 << 4 | 1 << 5 : 0);
			break;
		case 5:	pair.value = 3; break;			/* misaligned accesses: fast */
		default: pair.key = -1; pair.value = 0;		/* not a key we know */
		}
		memcpy(pairs + i * sizeof(pair), &pair, sizeof(pair));
	}
	return 0;
}

static void do_syscall(struct rv_cpu *cpu);

/* For the log of system calls: ` "text"` in out[64] if `addr` is that of a string of four or more printable characters, else nothing. */
static void strace_text(struct rv_cpu *cpu, uint64_t addr, char *out)
{
	unsigned n = 0;

	out[0] = '\0';
	if (addr < RV_PAGE_SIZE)
		return;
	out[n++] = ' ';
	out[n++] = '"';
	while (n < 58) {
		const char *c = rv_user_ptr(cpu, addr + n - 2, 1, RV_PROT_READ);

		if (!c || !*c)
			break;
		if (*c < 32 || *c > 126) {
			out[0] = '\0';
			return;
		}
		out[n++] = *c;
	}
	if (n < 6) {
		out[0] = '\0';
		return;
	}
	out[n++] = '"';
	out[n] = '\0';
}

void rv_syscall(struct rv_cpu *cpu)
{
	rv_stack_note("a system call");
	do_syscall(cpu);
	/* What the host wrote into copies of the guest's buffers goes to the guest's pages (mm.c). */
	if (__builtin_expect(cpu->nbounces, 0))
		rv_user_done(cpu);
}

static void do_syscall(struct rv_cpu *cpu)
{
	uint64_t *x = cpu->x;
	uint64_t nr = x[17];
	uint64_t a0 = x[10], a1 = x[11], a2 = x[12], a3 = x[13], a4 = x[14], a5 = x[15];
	uint64_t ret = 0;

	/*
	 * Not with something waiting to be looked at: a signal to deliver, or
	 * the request to park (thread.c). The call might wait, and what would
	 * have got the thread out of it may have come and gone already. The
	 * run loop deals with it and comes back here.
	 */
	if (__builtin_expect(cpu->sig_pending != 0, 0)) {
		cpu->pc_set = 1;
		return;
	}

	cpu->sys_pc = cpu->pc;
	cpu->sys_a0 = a0;
	cpu->sys_interrupted = 0;

	/* The program's own filters first, if it has given itself any (seccomp.c). */
	if (__builtin_expect(cpu->seccomp != NULL || cpu->seccomp_strict, 0)) {
		uint32_t verdict = rv_seccomp_check(cpu, nr, &x[10]);

		if (cpu->strace && (verdict & 0xffff0000U) != RV_SECCOMP_ALLOW)
			fprintf(stderr, "rv[%d]: syscall %llu: the filter says 0x%x\n", cpu->tid, (unsigned long long)nr, verdict);
		switch (verdict & 0xffff0000U) {
		case RV_SECCOMP_ALLOW:
		case RV_SECCOMP_LOG:
			break;
		case RV_SECCOMP_ERRNO:
			x[10] = neg((verdict & 0xffff) < 4095 ? (int)(verdict & 0xffff) : 4095);
			return;
		case RV_SECCOMP_TRAP:
			rv_signal_sys(cpu, nr, verdict & 0xffff);
			return;
		case RV_SECCOMP_TRACE:		/* nobody is tracing */
		case RV_SECCOMP_USER_NOTIF:	/* nobody is listening */
			x[10] = neg(RV_ENOSYS);
			return;
		case RV_SECCOMP_KILL_THREAD:
			if (rv_thread_exit(cpu))
				return;
			rv_signal_die(31);	/* SIGSYS */
			return;
		default:
			rv_signal_die((verdict & 0xffff0000U) == RV_SECCOMP_KILL_STRICT ? 9 : 31);
			return;
		}
	}

	switch (nr) {
	case RV_SYS_exit:
		/* One thread of several ends; the last one ends the process. */
		if (rv_thread_exit(cpu)) {
			if (cpu->strace)
				fprintf(stderr, "rv: syscall 93(%lld) ends a thread\n", (long long)a0);
			return;
		}
		/* fall through */
	case RV_SYS_exit_group:
		cpu->trap = RV_TRAP_EXIT;
		cpu->exit_code = (int)(a0 & 0xff);
		if (cpu->strace)
			fprintf(stderr, "rv: syscall %llu(%lld) exits\n",
				(unsigned long long)nr, (long long)a0);
		return;

	/* Memory. */
	case RV_SYS_brk:	ret = rv_mm_brk(cpu, a0); break;
	case RV_SYS_mmap: {
		int fd = (int)a4;
		uint64_t off = a5;

		/* A buffer of the emulated GPU: its backing file (virtgpu.c). */
		if (!(a3 & RV_MAP_ANONYMOUS) && fd >= 0 && rv_virtgpu_mmap(&fd, &off) && fd < 0) {
			ret = neg(RV_EINVAL);
			break;
		}
		ret = rv_mm_mmap(cpu, a0, a1, (unsigned)a2, a3, fd, off);
		break;
	}
	case RV_SYS_munmap:	ret = rv_mm_munmap(cpu, a0, a1); break;
	case RV_SYS_mremap:	ret = rv_mm_mremap(cpu, a0, a1, a2, a3); break;
	case RV_SYS_mprotect:	ret = rv_mm_mprotect(cpu, a0, a1, (unsigned)a2); break;
	case RV_SYS_madvise:	ret = rv_mm_madvise(cpu, a0, a1, (int)a2); break;
	/* A file mapping here is a private copy that cannot be written through (mm.c): nothing to write back. */
	case RV_SYS_msync:	ret = rv_mm_msync(cpu, a0, a1); break;
	case RV_SYS_mincore:	ret = sys_mincore(cpu, a0, a1, a2); break;
	case RV_SYS_riscv_flush_icache:	ret = rv_mm_flush_icache(cpu, a0, a1); break;

	/* Files. */
	case RV_SYS_read:
		ret = sys_read(cpu, (int)a0, a1, a2);
		break;
	case RV_SYS_write:
		ret = sys_write(cpu, (int)a0, a1, a2);
		break;
	case RV_SYS_readv:
		ret = sys_iov(cpu, (int)a0, a1, a2, 0);
		break;
	case RV_SYS_writev:
		ret = sys_iov(cpu, (int)a0, a1, a2, 1);
		break;
	case RV_SYS_pread64:	ret = sys_pread(cpu, (int)a0, a1, a2, a3); break;
	case RV_SYS_pwrite64:	ret = sys_pwrite(cpu, (int)a0, a1, a2, a3); break;
	case RV_SYS_lseek:	ret = ret_errno(lseek((int)a0, (off_t)a1, (int)a2)); break;
	case RV_SYS_openat:	ret = sys_openat(cpu, (int)a0, a1, a2, a3); break;
	case RV_SYS_close:	ret = sys_close(cpu, (int)a0); break;
	case RV_SYS_dup:	ret = ret_errno(dup((int)a0)); break;
	case RV_SYS_dup3:	ret = ret_errno(dup3((int)a0, (int)a1, open_flags_to_host(a2))); break;
	case RV_SYS_pipe2:	ret = sys_pipe2(cpu, a0, a1); break;
	case RV_SYS_fcntl:	ret = sys_fcntl(cpu, (int)a0, a1, a2); break;
	case RV_SYS_ioctl:	ret = sys_ioctl(cpu, (int)a0, a1, a2); break;
	case RV_SYS_fstat:	ret = sys_fstat(cpu, (int)a0, a1); break;
	case RV_SYS_newfstatat:	ret = sys_newfstatat(cpu, (int)a0, a1, a2, (int)a3); break;
	case RV_SYS_readlinkat:	ret = sys_readlinkat(cpu, (int)a0, a1, a2, a3); break;
	case RV_SYS_faccessat:	ret = sys_faccessat(cpu, (int)a0, a1, (int)a2, 0); break;
	case RV_SYS_faccessat2:	ret = sys_faccessat(cpu, (int)a0, a1, (int)a2, (int)a3); break;
	case RV_SYS_getcwd:	ret = sys_getcwd(cpu, a0, a1); break;
	case RV_SYS_chdir:	ret = sys_path1(cpu, nr, 0, a0, 0); break;
	case RV_SYS_mkdirat:
	case RV_SYS_unlinkat:	ret = sys_path1(cpu, nr, (int)a0, a1, a2); break;
	case RV_SYS_getdents64:	ret = sys_getdents64(cpu, (int)a0, a1, a2); break;
	case RV_SYS_statfs:	ret = sys_statfs(cpu, a0, a1); break;
	case RV_SYS_fstatfs:	ret = sys_fstatfs(cpu, (int)a0, a1); break;
	case RV_SYS_statx:	ret = sys_statx(cpu, (int)a0, a1, (int)a2, (unsigned)a3, a4); break;
	case RV_SYS_sendfile:	ret = sys_sendfile(cpu, (int)a0, (int)a1, a2, a3); break;
	case RV_SYS_ppoll:	ret = sys_ppoll(cpu, a0, a1, a2, a3); break;
	case RV_SYS_pselect6:	ret = sys_pselect6(cpu, (int)a0, a1, a2, a3, a4, a5); break;
	case RV_SYS_utimensat:	ret = sys_utimensat(cpu, (int)a0, a1, a2, (int)a3); break;
	case RV_SYS_symlinkat:
	case RV_SYS_linkat:
	case RV_SYS_renameat2:	ret = sys_path2(cpu, nr, a0, a1, a2, a3, a4); break;
	case RV_SYS_truncate:
	case RV_SYS_fchmodat:
	case RV_SYS_fchownat:
	case RV_SYS_mknodat:	ret = sys_path_misc(cpu, nr, a0, a1, a2, a3, a4); break;
	case RV_SYS_ftruncate:	ret = ret_errno(ftruncate((int)a0, (off_t)a1)); break;
	case RV_SYS_fchmod:	ret = ret_errno(fchmod((int)a0, (mode_t)a1)); break;
	case RV_SYS_fchown:	ret = ret_errno(fchown((int)a0, (uid_t)a1, (gid_t)a2)); break;
	case RV_SYS_fchdir:	ret = ret_errno(fchdir((int)a0)); break;
	case RV_SYS_chroot:	ret = sys_chroot(cpu, a0); break;
	case RV_SYS_flock:	ret = ret_errno(flock((int)a0, (int)a1)); break;
	case RV_SYS_fsync:	ret = ret_errno(fsync((int)a0)); break;
	case RV_SYS_fdatasync:	ret = ret_errno(fdatasync((int)a0)); break;
	case RV_SYS_sync:	sync(); ret = 0; break;
	case RV_SYS_umask:	ret = umask((mode_t)a0); break;

	/* Process, identity, time. */
	case RV_SYS_getpid:	ret = (uint64_t)getpid(); break;
	case RV_SYS_set_tid_address:
		cpu->clear_child_tid = a0;
		/* fall through */
	case RV_SYS_gettid:	ret = (uint64_t)cpu->tid; break;
	case RV_SYS_getppid:	ret = (uint64_t)getppid(); break;
	case RV_SYS_getuid:	ret = getuid(); break;
	case RV_SYS_geteuid:	ret = geteuid(); break;
	case RV_SYS_getgid:	ret = getgid(); break;
	case RV_SYS_getegid:	ret = getegid(); break;
	case RV_SYS_uname:	ret = sys_uname(cpu, a0); break;
	case RV_SYS_clock_gettime:	ret = sys_clock_gettime(cpu, a0, a1); break;
	case RV_SYS_clock_getres:	ret = sys_clock_getres(cpu, a0, a1); break;
	case RV_SYS_getrandom:	ret = sys_getrandom(cpu, a0, a1, a2); break;
	case RV_SYS_prlimit64:	ret = sys_prlimit64(cpu, a1, a2, a3); break;
	case RV_SYS_sched_yield:	sched_yield(); ret = 0; break;
	case RV_SYS_set_robust_list:	ret = 0; break;
	case RV_SYS_getitimer:	ret = sys_itimer(cpu, 0, (int)a0, 0, a1); break;
	case RV_SYS_setitimer:	ret = sys_itimer(cpu, 1, (int)a0, a1, a2); break;
	case RV_SYS_gettimeofday:	ret = sys_gettimeofday(cpu, a0); break;
	case RV_SYS_nanosleep:	ret = sys_sleep(cpu, CLOCK_REALTIME, 0, a0, a1); break;
	case RV_SYS_clock_nanosleep:	ret = sys_sleep(cpu, a0, (int)a1, a2, a3); break;
	case RV_SYS_times:	ret = sys_times(cpu, a0); break;
	case RV_SYS_getrusage:	ret = sys_getrusage(cpu, (int)a0, a1); break;
	case RV_SYS_sysinfo:	ret = sys_sysinfo(cpu, a0); break;
	case RV_SYS_sched_getaffinity:	ret = sys_sched_getaffinity(cpu, a1, a2); break;
	/*
	 * Timers of the process (timer_create() and so on): the host's. What
	 * one does when it runs out is a signal, to the process or to one
	 * thread; the signal's number and the thread's id are the host's as
	 * they are the guest's, and so are the structures, on any 64-bit Linux.
	 * (The C library's "run a function in a thread" is made of these too.)
	 */
	case RV_SYS_timer_create: {
		void *event = a1 ? rv_user_ptr(cpu, a1, 64, RV_PROT_READ) : NULL;
		int id = 0;

		if (a1 && !event) {
			ret = neg(RV_EFAULT);
			break;
		}
		ret = ret_errno(rv_host_syscall(SYS_timer_create, (int)a0, event, &id, 0, 0, 0));
		if (!ret && put_user(cpu, a2, &id, sizeof(id))) {
			rv_host_syscall(SYS_timer_delete, id, 0, 0, 0, 0, 0);
			ret = neg(RV_EFAULT);
		}
		break;
	}
	case RV_SYS_timer_settime: {
		void *new_value = rv_user_ptr(cpu, a2, 32, RV_PROT_READ);
		void *old_value = a3 ? rv_user_ptr(cpu, a3, 32, RV_PROT_WRITE) : NULL;

		if (!new_value || (a3 && !old_value))
			ret = neg(RV_EFAULT);
		else
			ret = ret_errno(rv_host_syscall(SYS_timer_settime, (int)a0, (int)a1, new_value, old_value, 0, 0));
		break;
	}
	case RV_SYS_timer_gettime: {
		void *value = rv_user_ptr(cpu, a1, 32, RV_PROT_WRITE);

		ret = value ? ret_errno(rv_host_syscall(SYS_timer_gettime, (int)a0, value, 0, 0, 0, 0)) : neg(RV_EFAULT);
		break;
	}
	case RV_SYS_timer_getoverrun:	ret = ret_errno(rv_host_syscall(SYS_timer_getoverrun, (int)a0, 0, 0, 0, 0, 0)); break;
	case RV_SYS_timer_delete:	ret = ret_errno(rv_host_syscall(SYS_timer_delete, (int)a0, 0, 0, 0, 0, 0)); break;
	case RV_SYS_readahead:	ret = 0; break;	/* advice */
	case RV_SYS_seccomp:	ret = rv_seccomp(cpu, a0, a1, a2); break;
	/* Namespaces are the host's: the process is. (One of process ids is for the children it makes from then on.) */
	case RV_SYS_unshare:
		ret = ret_errno(unshare((int)a0));
		if (!ret && (a0 & RV_CLONE_NEWPID))
			cpu->proc->children_apart = 1;
		break;
	case RV_SYS_setns:
		ret = ret_errno(setns((int)a0, (int)a1));
		if (!ret && (!a1 || (a1 & RV_CLONE_NEWPID)))
			cpu->proc->children_apart = 1;	/* (it may have been that one) */
		break;
	case RV_SYS_getcpu: {	/* the host's: the thread is the host's */
		unsigned where[2] = { 0, 0 };

		if (rv_host_syscall(SYS_getcpu, &where[0], &where[1], 0, 0, 0, 0) < 0)
			where[0] = where[1] = 0;
		ret = 0;
		if ((a0 && put_user(cpu, a0, &where[0], sizeof(where[0]))) || (a1 && put_user(cpu, a1, &where[1], sizeof(where[1]))))
			ret = neg(RV_EFAULT);
		break;
	}
	case RV_SYS_prctl:	ret = sys_prctl(cpu, a0, a1, a2, a3); break;

	/* Signals: see signal.c. One for a thread of ours takes its way through thread.c. */
	case RV_SYS_kill:	ret = rv_sys_kill((int64_t)a0, (int)a1); break;
	case RV_SYS_tkill:
		if (!rv_thread_kill(cpu, (int64_t)a0, (int)a1, &ret))
			ret = rv_sys_kill((int64_t)a0, (int)a1);
		break;
	case RV_SYS_tgkill:
		if (!rv_thread_kill(cpu, (int64_t)a1, (int)a2, &ret))
			ret = rv_sys_kill((int64_t)a1, (int)a2);
		break;
	case RV_SYS_rt_sigaction:	ret = rv_sys_rt_sigaction(cpu, a0, a1, a2, a3); break;
	case RV_SYS_rt_sigprocmask:	ret = rv_sys_rt_sigprocmask(cpu, a0, a1, a2, a3); break;
	case RV_SYS_rt_sigpending:	ret = rv_sys_rt_sigpending(cpu, a0, a1); break;
	case RV_SYS_rt_sigsuspend:	ret = rv_sys_rt_sigsuspend(cpu, a0, a1); break;
	case RV_SYS_sigaltstack:	ret = rv_sys_sigaltstack(cpu, a0, a1); break;
	case RV_SYS_rt_sigreturn:
		/* Restores every register, a0 included: nothing to return. */
		rv_sys_rt_sigreturn(cpu);
		if (cpu->strace)
			fprintf(stderr, "rv: syscall 139() returns to 0x%llx\n", (unsigned long long)cpu->pc);
		return;
	case RV_SYS_execve:	ret = sys_execve(cpu, a0, a1, a2); break;
	case RV_SYS_clone:	ret = sys_clone(cpu, a0, a1, a2, a3, a4); break;
	case RV_SYS_wait4:	ret = sys_wait4(cpu, (int64_t)a0, a1, (int)a2, a3); break;
	case RV_SYS_waitid:	ret = sys_waitid(cpu, (int)a0, (int64_t)a1, a2, (int)a3, a4); break;
	case RV_SYS_capget:	ret = sys_capget(cpu, a0, a1); break;
	case RV_SYS_capset:	ret = sys_capset(cpu, a0, a1); break;
	case RV_SYS_memfd_create:	ret = sys_memfd_create(cpu, a0, a1); break;
	case RV_SYS_fallocate:	ret = sys_fallocate((int)a0, (int)a1, a2, a3); break;
	/* Priorities: for input and output there are none to set here; for the processor, the host's. */
	case RV_SYS_ioprio_set:
	case RV_SYS_ioprio_get:	ret = 0; break;
	case RV_SYS_sched_getscheduler:
		ret = ret_errno(rv_host_syscall(SYS_sched_getscheduler, (pid_t)a0, 0, 0, 0, 0, 0));
		break;
	case RV_SYS_sched_get_priority_max:
		ret = ret_errno(rv_host_syscall(SYS_sched_get_priority_max, (int)a0, 0, 0, 0, 0, 0));
		break;
	case RV_SYS_sched_get_priority_min:
		ret = ret_errno(rv_host_syscall(SYS_sched_get_priority_min, (int)a0, 0, 0, 0, 0, 0));
		break;
	case RV_SYS_sched_setscheduler: {
		int32_t *param = rv_user_ptr(cpu, a2, 4, RV_PROT_READ);

		ret = param ? ret_errno(rv_host_syscall(SYS_sched_setscheduler, (pid_t)a0, (int)a1, param, 0, 0, 0))
			    : neg(RV_EFAULT);
		break;
	}
	case RV_SYS_sched_setparam:
	case RV_SYS_sched_getparam: {
		int32_t *param = rv_user_ptr(cpu, a1, 4, nr == RV_SYS_sched_getparam ? RV_PROT_WRITE : RV_PROT_READ);

		ret = param ? ret_errno(rv_host_syscall(nr == RV_SYS_sched_getparam ? SYS_sched_getparam : SYS_sched_setparam,
							 (pid_t)a0, param, 0, 0, 0, 0))
			    : neg(RV_EFAULT);
		break;
	}
	case RV_SYS_close_range:	ret = sys_close_range(cpu, a0, a1, a2); break;

	/* Credentials and sessions: the guest is the host process. */
	/*
	 * The system calls themselves, which change the calling thread only,
	 * not the C library's functions, which change all threads: the guest's
	 * C library does that part itself, by making the call in each of its
	 * threads.
	 */
	case RV_SYS_setuid:	ret = ret_errno(rv_host_syscall(SYS_setuid, (uid_t)a0, 0, 0, 0, 0, 0)); break;
	case RV_SYS_setgid:	ret = ret_errno(rv_host_syscall(SYS_setgid, (gid_t)a0, 0, 0, 0, 0, 0)); break;
	case RV_SYS_setreuid:	ret = ret_errno(rv_host_syscall(SYS_setreuid, (uid_t)a0, (uid_t)a1, 0, 0, 0, 0)); break;
	case RV_SYS_setregid:	ret = ret_errno(rv_host_syscall(SYS_setregid, (gid_t)a0, (gid_t)a1, 0, 0, 0, 0)); break;
	case RV_SYS_setresuid:	ret = ret_errno(rv_host_syscall(SYS_setresuid, (uid_t)a0, (uid_t)a1, (uid_t)a2, 0, 0, 0)); break;
	case RV_SYS_setresgid:	ret = ret_errno(rv_host_syscall(SYS_setresgid, (gid_t)a0, (gid_t)a1, (gid_t)a2, 0, 0, 0)); break;
	case RV_SYS_getresuid:	ret = sys_getres(cpu, 0, a0, a1, a2); break;
	case RV_SYS_getresgid:	ret = sys_getres(cpu, 1, a0, a1, a2); break;
	case RV_SYS_getgroups:	ret = sys_getgroups(cpu, a0, a1); break;
	case RV_SYS_setgroups:	ret = sys_setgroups(cpu, a0, a1); break;
	/* These two return the previous id and never fail. */
	case RV_SYS_setfsuid:	ret = (uint64_t)setfsuid((uid_t)a0); break;
	case RV_SYS_setfsgid:	ret = (uint64_t)setfsgid((gid_t)a0); break;
	case RV_SYS_setpriority:	ret = ret_errno(setpriority((int)a0, (id_t)a1, (int)a2)); break;
	case RV_SYS_getpriority: {
		/* The system call returns 20 minus the nice value, so that it is never negative. */
		int nice;

		errno = 0;
		nice = getpriority((int)a0, (id_t)a1);
		ret = nice == -1 && errno ? neg(errno) : (uint64_t)(20 - nice);
		break;
	}
	case RV_SYS_setpgid:	ret = ret_errno(setpgid((pid_t)a0, (pid_t)a1)); break;
	case RV_SYS_getpgid:	ret = ret_errno(getpgid((pid_t)a0)); break;
	case RV_SYS_getsid:	ret = ret_errno(getsid((pid_t)a0)); break;
	case RV_SYS_setsid:	ret = ret_errno(setsid()); break;
	case RV_SYS_futex:	ret = rv_thread_futex(cpu, a0, a1, a2, a3, a4, a5); break;
	case RV_SYS_fadvise64:	ret = 0; break;
	case RV_SYS_sync_file_range:	ret = 0; break;	/* advice about when to write; dpkg gives it a lot */
	case RV_SYS_copy_file_range:	ret = sys_copy_file_range(cpu, (int)a0, a1, (int)a2, a3, a4, (unsigned int)a5); break;
	case RV_SYS_getxattr: case RV_SYS_lgetxattr: case RV_SYS_fgetxattr:
	case RV_SYS_listxattr: case RV_SYS_llistxattr: case RV_SYS_flistxattr:
		ret = sys_xattr(cpu, nr, a0, a1, a2, a3);
		break;
	case RV_SYS_riscv_hwprobe:	ret = sys_riscv_hwprobe(cpu, a0, a1, a4); break;

	/* Waiting for many things at once, and descriptors that are events and timers. */
	case RV_SYS_epoll_create1:	ret = ret_errno(epoll_create1((int)a0)); break;
	case RV_SYS_epoll_ctl:	ret = sys_epoll_ctl(cpu, (int)a0, (int)a1, (int)a2, a3); break;
	case RV_SYS_epoll_pwait:	ret = sys_epoll_pwait(cpu, (int)a0, a1, (int)a2, (int)a3, a4); break;
	case RV_SYS_eventfd2:	ret = ret_errno(eventfd((unsigned)a0, (int)a1)); break;
	/* Watching files. (What is read from the descriptor has the same layout everywhere.) */
	case RV_SYS_inotify_init1:	ret = ret_errno(inotify_init1((int)a0)); break;
	case RV_SYS_inotify_add_watch: {
		const char *path = user_path(cpu, a1);

		ret = path ? ret_errno(inotify_add_watch((int)a0, path, (uint32_t)a2)) : neg(RV_EFAULT);
		break;
	}
	case RV_SYS_inotify_rm_watch:	ret = ret_errno(inotify_rm_watch((int)a0, (int)a1)); break;
	case RV_SYS_signalfd4: {
		/*
		 * Signals read from a descriptor. They are ones the guest keeps
		 * blocked, and what the guest blocks the host blocks (signal.c), so
		 * they wait in the host kernel, where the host's signalfd finds them.
		 */
		sigset_t set;
		uint64_t mask;

		if (a2 != 8)
			ret = neg(RV_EINVAL);
		else if (get_sigset(cpu, a1, &mask, &set))
			ret = neg(RV_EFAULT);
		else
			ret = ret_errno(signalfd((int)a0, &set, (int)a3));
		break;
	}
	case RV_SYS_timerfd_create:	ret = ret_errno(timerfd_create((int)a0, (int)a1)); break;
	case RV_SYS_timerfd_settime:	ret = sys_timerfd_settime(cpu, (int)a0, (int)a1, a2, a3); break;
	case RV_SYS_timerfd_gettime:	ret = sys_timerfd_gettime(cpu, (int)a0, a1); break;

	/* Sockets. */
	case RV_SYS_socket:	ret = socket_result(socket((int)a0, (int)a1, (int)a2), -1); break;
	case RV_SYS_socketpair:	ret = sys_socketpair(cpu, (int)a0, (int)a1, (int)a2, a3); break;
	case RV_SYS_bind:
	case RV_SYS_connect: {
		void *addr = sockaddr_in(cpu, a1, a2);

		if (!addr)
			ret = neg(RV_EFAULT);
		else if (nr == RV_SYS_bind)
			ret = socket_result(bind((int)a0, addr, (socklen_t)a2), (int)a0);
		else
			ret = socket_result(connect((int)a0, addr, (socklen_t)a2), (int)a0);
		break;
	}
	case RV_SYS_listen:	ret = socket_result(listen((int)a0, (int)a1), (int)a0); break;
	case RV_SYS_shutdown:	ret = socket_result(shutdown((int)a0, (int)a1), (int)a0); break;
	case RV_SYS_accept:
	case RV_SYS_accept4:
		ret = sys_sockaddr_out(cpu, nr, (int)a0, a1, a2, nr == RV_SYS_accept4 ? (int)a3 : 0);
		break;
	case RV_SYS_getsockname:
	case RV_SYS_getpeername:	ret = sys_sockaddr_out(cpu, nr, (int)a0, a1, a2, 0); break;
	case RV_SYS_sendto:
		ret = sys_sendto(cpu, (int)a0, a1, a2, (int)a3, a4, a5);
		break;
	case RV_SYS_recvfrom:
		ret = sys_recvfrom(cpu, (int)a0, a1, a2, (int)a3, a4, a5);
		break;
	case RV_SYS_sendmsg:
		ret = sys_msg(cpu, (int)a0, a1, (int)a2, 1);
		break;
	case RV_SYS_recvmsg:
		ret = sys_msg(cpu, (int)a0, a1, (int)a2, 0);
		break;
	case RV_SYS_sendmmsg:
		ret = sys_mmsg(cpu, (int)a0, a1, a2, (int)a3, 1);
		break;
	case RV_SYS_recvmmsg:
		ret = sys_mmsg(cpu, (int)a0, a1, a2, (int)a3, 0);
		break;
	case RV_SYS_setsockopt:	ret = sys_sockopt(cpu, 0, (int)a0, (int)a1, (int)a2, a3, a4); break;
	case RV_SYS_getsockopt:	ret = sys_sockopt(cpu, 1, (int)a0, (int)a1, (int)a2, a3, a4); break;

	/* Known and deliberately absent: C libraries try these and fall back. */
	case RV_SYS_pidfd_open:
		ret = ret_errno(rv_host_syscall(SYS_pidfd_open, (pid_t)a0, (unsigned)a1, 0, 0, 0, 0));
		break;
	case RV_SYS_pidfd_send_signal:
		/* (Without the description of the signal that may come with it: it is the host's layout.) */
		ret = a2 ? neg(RV_EINVAL) : ret_errno(rv_host_syscall(SYS_pidfd_send_signal, (int)a0, (int)a1, 0, (unsigned)a3, 0, 0));
		break;
	case RV_SYS_rseq:
	case RV_SYS_clone3:
	case RV_SYS_shmget: case RV_SYS_shmctl: case RV_SYS_shmat: case RV_SYS_shmdt:
		/* (shared memory with the X server, for pictures: they are sent through the connection instead) */
		ret = neg(RV_ENOSYS);
		break;

	default:
		if (cpu->verbose)
			fprintf(stderr, "rv: unimplemented syscall %llu at pc 0x%llx\n",
				(unsigned long long)nr, (unsigned long long)cpu->pc);
		ret = neg(RV_ENOSYS);
	}

	/*
	 * The host's call was interrupted, and by nothing the guest is to
	 * see: a kick (thread.c), or a signal that has been dealt with. As far
	 * as the guest can tell the call has not been made yet: pc stays on
	 * the ecall, a0 is still the argument, and the run loop, which parks
	 * the thread first if that is what the kick was for, makes it again.
	 * (rt_sigsuspend is its own loop, and its EINTR is its result.)
	 */
	if (ret == neg(RV_EINTR) && nr != RV_SYS_rt_sigsuspend && !rv_signal_ready(cpu)) {
		cpu->pc_set = 1;
		return;
	}

	/* Calls the kernel restarts after a handler that was set up with SA_RESTART. */
	if (ret == neg(RV_EINTR)) {
		switch (nr) {
		case RV_SYS_read: case RV_SYS_write: case RV_SYS_readv: case RV_SYS_writev:
		case RV_SYS_pread64: case RV_SYS_pwrite64: case RV_SYS_wait4: case RV_SYS_waitid:
		case RV_SYS_openat: case RV_SYS_ioctl: case RV_SYS_flock:
			cpu->sys_interrupted = 1;
		}
	}

	if (cpu->strace) {
		char text[3][64];

		/* With what an argument points at, where that reads like a name. */
		strace_text(cpu, a0, text[0]);
		strace_text(cpu, a1, text[1]);
		strace_text(cpu, a2, text[2]);
		fprintf(stderr, "rv[%d]: syscall %llu(0x%llx%s, 0x%llx%s, 0x%llx%s, 0x%llx) = 0x%llx\n", cpu->tid,
			(unsigned long long)nr, (unsigned long long)a0, text[0], (unsigned long long)a1, text[1],
			(unsigned long long)a2, text[2], (unsigned long long)a3, (unsigned long long)ret);
		rv_user_done(cpu);
	}
	x[10] = ret;
#ifdef RV_HEAPCHECK
	rv_hc_syscall_done(nr);
#endif
}
