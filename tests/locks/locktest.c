/*
 * What SQLite asks of the system to share a database between processes and
 * threads (its unix VFS): POSIX record locks with F_SETLK and F_GETLK (owned
 * by the process: its threads never conflict with each other, other
 * processes do, and closing any descriptor of the file lets go of them all),
 * open-file-description locks, flock(), and the -shm file of WAL mode: a file
 * grown with ftruncate() and mapped MAP_SHARED in 32 KiB pieces by every
 * process, which must all see each other's stores at once.
 *
 *   locktest DIR      (prints "ok NAME" or "FAIL NAME: why" per check, and "done N failures")
 *
 * Built static for riscv64 (glibc), and compared between rv and qemu-riscv64:
 *   riscv64-linux-gnu-gcc -O2 -static -pthread -o locktest tests/locks/locktest.c
 *   qemu-riscv64 ./locktest /tmp; rv ./locktest /tmp; RV_SHARE_HOST=0 rv ./locktest /tmp
 * RV_SHARE_HOST=0 is the browser's case for a file on a disk (no shared mapping from the host): there
 * "shm-child-pread" fails, as rv/mm.c says it will (a shared, writable mapping is a segment that reaches the
 * file at msync(), munmap() or exit; read() sees the file behind it until then).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static char db[512], shm[512];

static void check(int ok, const char *name, const char *why)
{
	if (ok)
		printf("ok %s\n", name);
	else {
		printf("FAIL %s: %s (errno %d %s)\n", name, why, errno, strerror(errno));
		failures++;
	}
	fflush(stdout);
}

static int setlk(int fd, int cmd, short type, off_t start, off_t len)
{
	struct flock l = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };

	return fcntl(fd, cmd, &l);
}

/* A pipe pair for the parent and child to take turns. */
static int to_child[2], to_parent[2];
static void say(int *p) { char c = 1; if (write(p[1], &c, 1) != 1) _exit(99); }
static void hear(int *p) { char c; if (read(p[0], &c, 1) != 1) _exit(98); }

/* SQLite's lock bytes on a file of the default page size: PENDING 0x40000000, RESERVED +1, SHARED +2 .. +511. */
#define PENDING 0x40000000L

struct thread_arg { int fd; int rc; int err; };
static void *thread_lock(void *p)
{
	struct thread_arg *a = p;

	a->rc = setlk(a->fd, F_SETLK, F_WRLCK, PENDING + 2, 510);
	a->err = errno;
	return NULL;
}

static void *thread_open_lock(void *p)
{
	struct thread_arg *a = p;
	int fd = open(db, O_RDWR);

	a->rc = setlk(fd, F_SETLK, F_RDLCK, PENDING + 2, 510);
	a->err = errno;
	close(fd);	/* (and that lets go of every lock of the process on the file) */
	return NULL;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "/tmp";
	struct flock l;
	pthread_t t;
	struct thread_arg a;
	int fd, fd2, status;
	pid_t child;

	snprintf(db, sizeof(db), "%s/locktest.db", dir);
	snprintf(shm, sizeof(shm), "%s/locktest.db-shm", dir);
	unlink(db);
	unlink(shm);
	fd = open(db, O_RDWR | O_CREAT, 0644);
	check(fd >= 0, "open", "open db");
	check(write(fd, "SQLite format 3", 16) == 16, "write", "write header");

	/* 1. Record locks in one process: F_SETLK and F_GETLK. */
	check(setlk(fd, F_SETLK, F_RDLCK, PENDING, 1) == 0, "setlk-rd-pending", "F_SETLK F_RDLCK");
	check(setlk(fd, F_SETLK, F_RDLCK, PENDING + 2, 510) == 0, "setlk-rd-shared", "F_SETLK F_RDLCK shared range");
	check(setlk(fd, F_SETLK, F_UNLCK, PENDING, 1) == 0, "setlk-unlock", "F_SETLK F_UNLCK");
	memset(&l, 0, sizeof(l));
	l.l_type = F_WRLCK; l.l_whence = SEEK_SET; l.l_start = PENDING + 2; l.l_len = 510;
	check(fcntl(fd, F_GETLK, &l) == 0 && l.l_type == F_UNLCK, "getlk-own", "F_GETLK sees the process's own lock");

	/* 2. Threads of the process do not conflict with each other. */
	a.fd = fd;
	pthread_create(&t, NULL, thread_lock, &a);
	pthread_join(t, NULL);
	errno = a.err;
	check(a.rc == 0, "thread-same-owner", "a second thread's F_WRLCK over the process's F_RDLCK");
	check(setlk(fd, F_SETLK, F_RDLCK, PENDING + 2, 510) == 0, "back-to-rd", "back to a read lock");

	/* 3. Another process conflicts. */
	pipe(to_child);
	pipe(to_parent);
	child = fork();
	if (child == 0) {
		int cfd = open(db, O_RDWR), r;
		struct flock g = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = PENDING + 2, .l_len = 510 };

		r = setlk(cfd, F_SETLK, F_WRLCK, PENDING + 2, 510);
		printf("%s child-conflict: F_SETLK F_WRLCK over the parent's read lock gave %d errno %d\n",
		       r < 0 && (errno == EAGAIN || errno == EACCES) ? "ok" : "FAIL", r, errno);
		r = fcntl(cfd, F_GETLK, &g);
		printf("%s child-getlk: rc %d type %d (F_RDLCK %d) pid %d (parent %d)\n",
		       r == 0 && g.l_type == F_RDLCK && g.l_pid == getppid() ? "ok" : "FAIL", r, g.l_type, F_RDLCK,
		       g.l_pid, getppid());
		r = setlk(cfd, F_SETLK, F_RDLCK, PENDING + 2, 510);
		printf("%s child-shared: F_RDLCK beside the parent's: %d errno %d\n", r == 0 ? "ok" : "FAIL", r, errno);
		fflush(stdout);
		say(to_parent);
		hear(to_child);		/* the parent let go */
		r = setlk(cfd, F_SETLK, F_WRLCK, 0, 0);
		printf("%s child-after-close: F_WRLCK on the whole file after the parent closed a descriptor: %d errno %d\n",
		       r == 0 ? "ok" : "FAIL", r, errno);
		fflush(stdout);
		say(to_parent);
		hear(to_child);
		/* F_SETLKW waits for the parent's lock */
		r = setlk(cfd, F_SETLK, F_UNLCK, 0, 0);
		say(to_parent);
		hear(to_child);		/* the parent has byte 0 */
		r = setlk(cfd, F_SETLKW, F_WRLCK, 0, 1);
		printf("%s child-setlkw: waited for the parent's lock: %d errno %d\n", r == 0 ? "ok" : "FAIL", r, errno);
		fflush(stdout);
		_exit(0);
	}
	hear(to_parent);
	/* Closing ANY descriptor of the file lets go of the process's locks (another one, opened by a thread). */
	pthread_create(&t, NULL, thread_open_lock, &a);
	pthread_join(t, NULL);
	errno = a.err;
	check(a.rc == 0, "thread-open-lock", "a lock through a thread's own descriptor");
	say(to_child);
	hear(to_parent);
	check(setlk(fd, F_SETLK, F_RDLCK, 0, 1) < 0 && (errno == EAGAIN || errno == EACCES), "parent-conflict",
	      "the child's whole-file write lock must stop a read lock");
	say(to_child);
	hear(to_parent);	/* the child let go */
	check(setlk(fd, F_SETLK, F_WRLCK, 0, 1) == 0, "parent-take", "write lock on byte 0");
	say(to_child);
	usleep(200000);
	setlk(fd, F_SETLK, F_UNLCK, 0, 1);
	waitpid(child, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child-exit", "child");

	/* 4. Open-file-description locks: two descriptions in one process conflict. */
	fd2 = open(db, O_RDWR);
	{
		struct flock o = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 100, .l_len = 1 };
		int r = fcntl(fd, F_OFD_SETLK, &o);

		check(r == 0, "ofd-setlk", "F_OFD_SETLK");
		o.l_type = F_WRLCK; o.l_pid = 0;
		r = fcntl(fd2, F_OFD_SETLK, &o);
		check(r < 0 && (errno == EAGAIN || errno == EACCES), "ofd-conflict", "a second description's F_OFD_SETLK");
		o.l_type = F_WRLCK; o.l_pid = 0;
		r = fcntl(fd2, F_OFD_GETLK, &o);
		check(r == 0 && o.l_type == F_WRLCK && o.l_pid == -1, "ofd-getlk", "F_OFD_GETLK (type F_WRLCK, pid -1)");
		o.l_type = F_UNLCK; o.l_pid = 0;
		fcntl(fd, F_OFD_SETLK, &o);
	}

	/* 5. flock(). */
	check(flock(fd, LOCK_EX | LOCK_NB) == 0, "flock-ex", "flock LOCK_EX");
	check(flock(fd2, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK, "flock-conflict", "a second description's flock");
	check(flock(fd, LOCK_UN) == 0 && flock(fd2, LOCK_SH | LOCK_NB) == 0, "flock-un", "flock after LOCK_UN");
	flock(fd2, LOCK_UN);
	close(fd2);

	/* 6. The -shm file: grown with ftruncate, mapped MAP_SHARED in 32 KiB pieces in two processes. */
	{
		int sfd = open(shm, O_RDWR | O_CREAT, 0644);
		volatile unsigned *p0, *p1;
		struct stat st;

		check(sfd >= 0 && ftruncate(sfd, 32768) == 0, "shm-truncate", "open and ftruncate -shm");
		p0 = mmap(0, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
		check(p0 != MAP_FAILED, "shm-map0", "mmap MAP_SHARED piece 0");
		p0[0] = 0x11111111;
		/* SQLite grows the file by writing a byte at the end of each new page, then maps the next piece. */
		check(pwrite(sfd, "", 1, 65535) == 1 && fstat(sfd, &st) == 0 && st.st_size == 65536, "shm-grow",
		      "pwrite at the end grows the file");
		p1 = mmap(0, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 32768);
		check(p1 != MAP_FAILED, "shm-map1", "mmap MAP_SHARED piece 1 at offset 32768");
		if (p1 != MAP_FAILED)
			p1[0] = 0x22222222;
		child = fork();
		if (child == 0) {
			int cfd = open(shm, O_RDWR);
			volatile unsigned *c0 = mmap(0, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
			volatile unsigned *c1 = mmap(0, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 32768);
			unsigned v;
			int i;

			if (c0 == MAP_FAILED || c1 == MAP_FAILED) {
				printf("FAIL shm-child-map: errno %d\n", errno);
				_exit(1);
			}
			printf("%s shm-child-sees: %08x %08x\n", c0[0] == 0x11111111 && c1[0] == 0x22222222 ? "ok" : "FAIL",
			       c0[0], c1[0]);
			c0[1] = 0x33333333;
			c1[1] = 0x44444444;
			fflush(stdout);
			say(to_parent);
			/* now wait (spinning on the mapping, as SQLite's readers look at the WAL index) for the parent */
			for (i = 0; i < 2000000 && (v = c0[2]) != 0x55555555; i++)
				usleep(1);
			printf("%s shm-child-sees-later: %08x\n", c0[2] == 0x55555555 ? "ok" : "FAIL", c0[2]);
			{
				unsigned w;
				pread(cfd, &w, 4, 8);
				printf("%s shm-child-pread: %08x\n", w == 0x55555555 ? "ok" : "FAIL", w);
			}
			fflush(stdout);
			_exit(0);
		}
		hear(to_parent);
		check(p0[1] == 0x33333333, "shm-parent-sees0", "the child's store in piece 0");
		check(p1 != MAP_FAILED && p1[1] == 0x44444444, "shm-parent-sees1", "the child's store in piece 1");
		p0[2] = 0x55555555;
		waitpid(child, &status, 0);
		check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "shm-child-exit", "child");
		/* What was stored in the mapping is in the file. */
		{
			unsigned w[2];

			check(pread(sfd, w, 8, 32768) == 8 && w[0] == 0x22222222 && w[1] == 0x44444444, "shm-file",
			      "the stores are in the file");
		}
		/* A write() to the file shows in the mapping. */
		{
			unsigned w = 0x66666666;

			pwrite(sfd, &w, 4, 12);
			check(p0[3] == 0x66666666, "shm-write-shows", "pwrite shows in the mapping");
		}
		munmap((void *)p0, 32768);
		if (p1 != MAP_FAILED)
			munmap((void *)p1, 32768);
		close(sfd);
	}
	/*
	 * 7. The database itself, mapped to be read (SQLite's PRAGMA mmap_size): it writes pages with pwrite()
	 * and reads them back through the mapping, which must show what was written. Or the mapping is refused
	 * (ENODEV, as a kernel without an MMU says), and SQLite reads the file instead.
	 */
	{
		char page[4096];
		volatile unsigned char *m;

		memset(page, 'A', sizeof(page));
		pwrite(fd, page, sizeof(page), 0);
		pwrite(fd, page, sizeof(page), 4096);
		m = mmap(0, 8192, PROT_READ, MAP_SHARED, fd, 0);
		if (m == MAP_FAILED) {
			check(errno == ENODEV, "db-map", "a refused mapping says ENODEV");
		} else {
			memset(page, 'B', sizeof(page));
			check(pwrite(fd, page, sizeof(page), 4096) == 4096, "db-pwrite", "pwrite");
			check(m[0] == 'A' && m[4096] == 'B' && m[8191] == 'B', "db-map-shows-pwrite",
			      "what pwrite wrote shows in the read-only shared mapping");
			munmap((void *)m, 8192);
		}
	}
	close(fd);
	unlink(db);
	unlink(shm);
	printf("done %d failures\n", failures);
	return failures != 0;
}
