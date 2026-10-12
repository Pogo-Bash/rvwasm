/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Guest address space.
 *
 * The host cannot provide one: Linux/Wasm has no MMU, so its mmap() ignores
 * address hints, rejects MAP_FIXED, has no mprotect(), and gives out memory
 * only in contiguous pieces that are really there. So the guest's address
 * space is made here, in software: a page table with one entry per 4 KiB
 * page of guest addresses (rv_cpu.map), which says what may be done with the
 * page and where in the host's memory it is. The interpreter and translated
 * code look every access up in it (rv_ptr() in rv.h).
 *
 * An entry is the page's protection in its low bits and, in the rest, the
 * distance from the page's guest address to its host address: adding it to
 * a guest address in the page gives the host address. A page can be
 *
 *   not mapped         entry 0: any access faults;
 *   mapped, no memory  it has no host memory yet: an access the mapping
 *                      allows gives it some, zeroed (rv_ptr_slow()). This is
 *                      how address space that is reserved and never used
 *                      (thread stacks, an allocator's arenas, a large buffer
 *                      half filled) costs nothing;
 *   mapped, in memory  the low bits allow what the mapping allows.
 *
 * Host memory comes from the host kernel in two ways: runs of up to
 * POOL_RUN_MAX pages are cut from chunks of 1 MiB (the first few: 256 KiB)
 * shared by all small mappings (the pool), larger ones are a piece of
 * their own. Either goes
 * back to the kernel when none of its pages is in use any more. Nothing
 * ever moves: a host address stays good for as long as the guest's page is
 * mapped.
 *
 * What is next to each other for the guest need not be for the host. For
 * the guest's own accesses that is the page table's business. A system
 * call's buffer is asked for with rv_user_ptr(): if its pages do not lie in
 * one piece, the caller gets a copy instead, and what the host wrote there
 * goes back to the guest's pages when the call is over (rv_user_done()).
 *
 * Files mapped to be read, which is how programs and libraries are loaded,
 * are not copied into every process. The contents of such a file are read
 * once into a shared memory segment that belongs to no process (System V
 * shared memory, whose segments the kernel keeps count of), and every
 * process that maps the file attaches the segment and points its pages
 * there: the code of a library is in memory once, however many programs
 * use it. Without an MMU the segment is at the same host address in all of
 * them, but nothing here depends on that. A page of it that the guest wants
 * to write to after all gets a private copy first. See shared_attach().
 *
 * The guest's address space can become larger: when a mapping no longer
 * fits, the page table is replaced by one twice as long, up to
 * rv_cpu.mem_max of addresses. So a program starts with a small table and
 * takes what it turns out to need.
 *
 * Code. A page that code was translated from (jit.c) is marked, and while it
 * is marked it does not allow writing as it stands, whatever its mapping
 * allows: a write goes the slow way like one to a page without memory,
 * where the translator is told first that its translations of the page are
 * no longer to be trusted (code_writable()). So a program may keep the code
 * it generates on pages it can write to, and what was translated from them
 * is never run stale. Whatever else happens to a marked page (it is
 * unmapped, loses the right to be executed, gets other memory) is told the
 * same way, for just those pages; see struct code_span.
 *
 * With several threads (thread.c): the functions the guest's system calls
 * reach take the lock, so one thread at a time changes the page table; the
 * others go on reading it, an entry changes all at once. The table itself is
 * only replaced while every other thread is parked.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>

#include "rv.h"

/* Lowest address mmap() will pick on its own, like Linux's mmap_min_addr. */
#define RV_MMAP_MIN_ADDR	0x10000ULL

#define POOL_PAGES	256	/* a chunk of the pool: 1 MiB */
#define POOL_PAGES_SMALL 64	/* ... but the first few of a process are this, for the many that need little */
#define POOL_SMALL_CHUNKS 4
#define POOL_RUN_MAX	32	/* runs of up to this many pages come from the pool */
#define LAZY_RUN	16	/* pages given memory at once when one that has none is touched */
#define SHARE_MIN	(32 << 10)	/* files smaller than this are not worth a shared segment */
#define SHARE_MAX	(256ULL << 20)	/* ... and larger ones are not libraries */

#define FLAGS		((uint64_t)RV_PAGE_FLAGS)

/* A piece of host memory that guest pages are in. */
struct rv_chunk {
	uint8_t *host;
	uint32_t npages;
	uint32_t used;		/* pages of it that are a guest's page */
	int pool;		/* handed out a few pages at a time, tracked below */
	uint64_t busy[POOL_PAGES / 64];
	uint64_t dirty[POOL_PAGES / 64];	/* was used before: to be zeroed when handed out again */
	/* A file's contents in a shared segment (see shared_attach()): where it is attached, or NULL. */
	uint8_t *shm_base;
	int shmid;
	int key;
	int file;		/* a file's own memory, mapped shared by the host (see map_host_shared()) */
	/* A segment that is a part of a file, to be written back to it (see live_attach()): */
	int live;
	int fd;			/* our own descriptor of the file */
	uint64_t offset;	/* where in the file */
};

/*
 * A run of the guest's pages that is a mapping of a file, and which part of
 * which file: what /proc/self/maps says about them (rv_mm_maps()). Not what
 * the pages are made of; that is in the page table and the chunks.
 */
struct rv_named {
	uint64_t first, count;	/* pages */
	uint64_t offset;	/* in the file, of the first page */
	uint64_t dev, ino;
	int copy;		/* a shared mapping that is a copy of the file (do_mmap()): pwrite() goes to it too */
	char path[212];
};

/* What a shared segment starts with, in a page of its own before the file's contents. */
struct shared_head {
	uint64_t magic;
	uint64_t dev, ino, size, mtime_sec, mtime_nsec;	/* the file it is of, as it was */
	uint64_t pidns;		/* and which namespace of process ids the list below is in (see pidns()) */
	volatile uint32_t ready;	/* the contents are all there */
	/*
	 * The processes that have it attached, by their ids (0: a free place).
	 * The kernel would know, but without an MMU its count stays at 0, so
	 * the users keep the list themselves; a process that was killed is
	 * found out by its id being nobody's any more.
	 */
	volatile uint32_t pids[];
};
#define SHARED_MAGIC	0x3344524148535652ULL	/* "RVSHARD3" */
#define LIVE_MAGIC	0x3257524148535652ULL	/* "RVSHARW2": for it `size` is the offset in the file, `mtime_sec` the pages */
#define LIVE_MAX_PAGES	((64ULL << 20) >> RV_PAGE_SHIFT)
#define SHARED_PIDS	((RV_PAGE_SIZE - offsetof(struct shared_head, pids)) / sizeof(uint32_t))

/*
 * Which namespace of process ids this process is in, as a number (0 where
 * there are none). The users of a segment know each other by process id,
 * and ask the kernel whether an id is still someone's: that only means
 * something among processes that see the same ids. So a segment belongs to
 * one namespace, and processes in another have one of their own for the
 * same file. (A process that mistook the others for dead would remove the
 * segment from under them.)
 */
static uint64_t pidns_is;
static int pidns_known;

static uint64_t pidns(void)
{
	if (!pidns_known) {
		struct stat st;

		pidns_is = stat("/proc/self/ns/pid", &st) ? 0 : (uint64_t)st.st_ino;
		pidns_known = 1;
	}
	return pidns_is;
}

/* This process has the segment attached. (If there is no place left it goes unlisted, and is then never the last.) */
static void shared_join(struct shared_head *head)
{
	uint32_t pid = (uint32_t)getpid();

	for (uint32_t i = 0; i < SHARED_PIDS; i++) {
		uint32_t expected = 0;

		if (head->pids[i] == pid ||
		    __atomic_compare_exchange_n(&head->pids[i], &expected, pid, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return;
	}
}

/* This process lets go of the segment. Returns whether no process that is alive is left on its list. */
static int shared_leave(struct shared_head *head)
{
	uint32_t pid = (uint32_t)getpid();
	int alive = 0;

	for (uint32_t i = 0; i < SHARED_PIDS; i++) {
		uint32_t other = head->pids[i];

		if (!other)
			continue;
		if (other == pid || (kill((pid_t)other, 0) < 0 && errno == ESRCH))
			__atomic_compare_exchange_n(&head->pids[i], &other, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		else
			alive = 1;
	}
	return !alive;
}

/* RV_SHARE=0: every process reads the files it maps into memory of its own, as if there were no shared segments. */
static int share_off = -1;
static int share_host = -1;
static struct rv_proc *dying;	/* the process, for rv_mm_die(): once it has memory */

static uint64_t neg(int err)
{
	return (uint64_t)-(int64_t)err;
}

/* ------------------------------------------------------------- host memory */

/* The chunk an address of host memory is in, or NULL. (They are kept sorted by address.) */
static struct rv_chunk *chunk_of(struct rv_proc *proc, const uint8_t *host)
{
	uint32_t lo = 0, hi = proc->nchunks;

	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;

		if (proc->chunks[mid]->host <= host)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo || host >= proc->chunks[lo - 1]->host + ((uint64_t)proc->chunks[lo - 1]->npages << RV_PAGE_SHIFT))
		return NULL;
	return proc->chunks[lo - 1];
}

/* Room for one more chunk in the list. */
static int chunk_room(struct rv_proc *proc)
{
	if (proc->nchunks == proc->chunks_cap) {
		uint32_t cap = proc->chunks_cap ? proc->chunks_cap * 2 : 64;
		struct rv_chunk **chunks = realloc(proc->chunks, cap * sizeof(*chunks));

		if (!chunks)
			return -1;
		proc->chunks = chunks;
		proc->chunks_cap = cap;
	}
	return 0;
}

/* Into the list, which chunk_room() has made room in. */
static void chunk_add(struct rv_proc *proc, struct rv_chunk *c)
{
	uint32_t at;

	for (at = proc->nchunks; at > 0 && proc->chunks[at - 1]->host > c->host; at--)
		proc->chunks[at] = proc->chunks[at - 1];
	proc->chunks[at] = c;
	proc->nchunks++;
	dying = proc;
	if (c->shm_base || c->file)
		proc->shared_pages += c->npages;
	else
		proc->host_pages += c->npages;
}

static struct rv_chunk *chunk_new(struct rv_proc *proc, uint32_t npages, int pool)
{
	struct rv_chunk *c;

	if (chunk_room(proc) || !(c = calloc(1, sizeof(*c))))
		return NULL;
	/* (Zeroed by the kernel.) */
	c->host = mmap(NULL, (size_t)npages << RV_PAGE_SHIFT, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (c->host == MAP_FAILED) {
		free(c);
		return NULL;
	}
	c->npages = npages;
	c->pool = pool;
	chunk_add(proc, c);
	return c;
}

/*
 * Whether a live segment's descriptor is still of its file. It is a number
 * like the guest's own, which the guest may have closed without knowing (a
 * program that closes everything from 3 up), and which may be another file's
 * by now.
 */
static int live_fd(const struct rv_chunk *c)
{
	const struct shared_head *head = (const struct shared_head *)(const void *)c->shm_base;
	struct stat st;

	return c->fd >= 0 && !fstat(c->fd, &st) && (uint64_t)st.st_dev == head->dev && (uint64_t)st.st_ino == head->ino;
}

/* Write a live segment to its place in the file, as far as the file goes. */
static void live_sync(struct rv_chunk *c)
{
	uint64_t len = (uint64_t)c->npages << RV_PAGE_SHIFT, done = 0;
	struct stat st;

	if (!live_fd(c) || fstat(c->fd, &st) || (uint64_t)st.st_size <= c->offset)
		return;
	if (len > (uint64_t)st.st_size - c->offset)
		len = (uint64_t)st.st_size - c->offset;
	while (done < len) {
		ssize_t r = pwrite(c->fd, c->host + done, len - done, (off_t)(c->offset + done));

		if (r <= 0)
			break;
		done += (uint64_t)r;
	}
}

static void chunk_free(struct rv_proc *proc, struct rv_chunk *c)
{
	uint32_t at = 0;

	while (proc->chunks[at] != c)
		at++;
	memmove(&proc->chunks[at], &proc->chunks[at + 1], (proc->nchunks - at - 1) * sizeof(*proc->chunks));
	proc->nchunks--;
	if (c->shm_base) {
		/* A file's segment: let go of it, and if nobody else has it, it has served. */
		int last;

		if (c->live) {
			live_sync(c);
			if (live_fd(c))
				close(c->fd);
		}
		last = shared_leave((struct shared_head *)(void *)c->shm_base);

		proc->shared_pages -= c->npages;
		shmdt(c->shm_base);
		if (last)
			shmctl(c->shmid, IPC_RMID, NULL);
	} else {
		if (c->file)
			proc->shared_pages -= c->npages;
		else
			proc->host_pages -= c->npages;
		munmap(c->host, (size_t)c->npages << RV_PAGE_SHIFT);
	}
	free(c);
}

/*
 * The contents of the file open as `fd` (which fstat() described as *st) in
 * a shared segment, attached here: the chunk for it, or NULL if that cannot
 * be had (and the caller reads the file into memory of its own).
 *
 * The segment is found by a key made from what identifies the file's
 * contents: device, inode, size, time of last change. Whoever asks first
 * creates it, reads the file into it and marks it ready; the others attach
 * it and wait for the mark. Its first page says which file it is of, in
 * full, since two files can make the same key, and which processes have it
 * attached; the last one to let go removes it (chunk_free()).
 */
static struct rv_chunk *segment_attach(struct rv_proc *proc, int fd, const struct shared_head *wanted, uint64_t pages);

static struct rv_chunk *shared_attach(struct rv_proc *proc, int fd, const struct stat *st)
{
	struct shared_head want = {
		.pidns = pidns(),
		.magic = SHARED_MAGIC, .dev = (uint64_t)st->st_dev, .ino = (uint64_t)st->st_ino,
		.size = (uint64_t)st->st_size, .mtime_sec = (uint64_t)st->st_mtim.tv_sec,
		.mtime_nsec = (uint64_t)st->st_mtim.tv_nsec,
	};

	return segment_attach(proc, fd, &want, RV_PAGE_ALIGN((uint64_t)st->st_size) >> RV_PAGE_SHIFT);
}

/*
 * `count` pages of the file open as `fd`, from `offset`, in a segment that
 * every process which maps that part of the file shared and to be written
 * has attached: what one writes the others see, as a shared mapping
 * promises. The chunk for it, or NULL.
 *
 * A kernel with an MMU does this with the file's pages themselves. Here the
 * segment is a copy that takes the file's place for as long as it is mapped,
 * and is written back to the file when a process asks (msync()), unmaps it
 * or ends. In between, the file as read() sees it is behind; and a process
 * that maps another part of the file, or the same part not to write to,
 * does not see these pages. Enough for what it is mostly used for: a small
 * file that processes share as memory (SQLite's index of its write-ahead
 * log, a lock file with counters in it).
 */
static struct rv_chunk *live_attach(struct rv_proc *proc, int fd, const struct stat *st, uint64_t offset, uint64_t count)
{
	struct shared_head want = {
		.pidns = pidns(),
		.magic = LIVE_MAGIC, .dev = (uint64_t)st->st_dev, .ino = (uint64_t)st->st_ino,
		.size = offset, .mtime_sec = count,
	};
	struct rv_chunk *c = segment_attach(proc, fd, &want, count);

	if (c && !c->live) {
		c->live = 1;
		c->offset = offset;
		c->fd = fcntl(fd, F_DUPFD_CLOEXEC, 64);
	}
	return c;
}

/* The segment that starts with `wanted`, of `pages` pages after that, attached: made and filled from `fd` if there is none. */
static struct rv_chunk *segment_attach(struct rv_proc *proc, int fd, const struct shared_head *wanted, uint64_t pages)
{
	uint64_t h = wanted->dev * 0x9e3779b97f4a7c15ULL;
	struct shared_head want = *wanted;
	int live = want.magic == LIVE_MAGIC;
	struct shared_head *head;
	struct rv_chunk *c;
	uint8_t *base;
	int key, id, created;

	h = (h ^ want.ino) * 0x9e3779b97f4a7c15ULL;
	h = (h ^ want.size) * 0x9e3779b97f4a7c15ULL;
	h = (h ^ want.mtime_sec ^ want.mtime_nsec << 32) * 0x9e3779b97f4a7c15ULL;
	h = (h ^ want.pidns) * 0x9e3779b97f4a7c15ULL;
	key = (int)((h >> 33) | 0x40000000) ^ live;

	/* Attached already? (A library is mapped in several pieces.) */
	for (uint32_t i = 0; i < proc->nchunks; i++) {
		c = proc->chunks[i];
		if (c->shm_base && c->key == key && !memcmp(c->shm_base, &want, offsetof(struct shared_head, ready)))
			return c;
	}
	if (chunk_room(proc) || !(c = calloc(1, sizeof(*c))))
		return NULL;

	for (int attempt = 0; attempt < 2; attempt++) {
		id = shmget(key, (size_t)(pages + 1) << RV_PAGE_SHIFT, IPC_CREAT | IPC_EXCL | 0600);
		created = id >= 0;
		if (!created && (errno != EEXIST || (id = shmget(key, 0, 0)) < 0))
			break;
		base = shmat(id, NULL, 0);
		if (base == (uint8_t *)-1) {
			if (created)
				shmctl(id, IPC_RMID, NULL);
			break;
		}
		head = (struct shared_head *)(void *)base;

		if (created) {
			/* The whole file; or, live, the part of it, which may reach beyond its end (zeros there). */
			uint64_t from = live ? want.size : 0, all = live ? pages << RV_PAGE_SHIFT : want.size, got = 0;

			while (got < all) {
				ssize_t r = pread(fd, base + RV_PAGE_SIZE + got, all - got, (off_t)(from + got));

				if (r <= 0)
					break;
				got += (uint64_t)r;
			}
			if (got < all && !live) {
				shmdt(base);
				shmctl(id, IPC_RMID, NULL);
				break;
			}
			memcpy(head, &want, offsetof(struct shared_head, ready));
			__atomic_store_n(&head->ready, 1, __ATOMIC_SEQ_CST);
		} else {
			/*
			 * Someone else's: wait until it is filled. If its maker is
			 * gone and it never will be, it goes, and we make it.
			 */
			struct shmid_ds ds;
			int waited = 0, stale = 0;

			while (!__atomic_load_n(&head->ready, __ATOMIC_SEQ_CST)) {
				if (++waited % 50 == 0 && (shmctl(id, IPC_STAT, &ds) ||
							   (kill(ds.shm_cpid, 0) < 0 && errno == ESRCH))) {
					stale = 1;
					break;
				}
				usleep(2000);
			}
			if (stale || memcmp(head, &want, offsetof(struct shared_head, ready))) {
				shmdt(base);
				if (!stale)
					break;	/* another file with the same key: no sharing for this one */
				shmctl(id, IPC_RMID, NULL);
				continue;
			}
		}

		shared_join(head);
		c->host = base + RV_PAGE_SIZE;
		c->npages = (uint32_t)pages;
		c->shm_base = base;
		c->shmid = id;
		c->key = key;
		chunk_add(proc, c);
		return c;
	}
	free(c);
	return NULL;
}

/* A run of n free pages in a chunk of the pool: its first page, or -1. */
static int pool_find(const struct rv_chunk *c, uint32_t n)
{
	uint32_t run = 0;

	if (c->npages - c->used < n)
		return -1;
	for (uint32_t i = 0; i < c->npages; i++) {
		if (c->busy[i / 64] >> (i % 64) & 1)
			run = 0;
		else if (++run == n)
			return (int)(i + 1 - n);
	}
	return -1;
}

/* n pages of host memory in one piece, zeroed; NULL if there is none to be had. */
static uint8_t *pages_alloc(struct rv_proc *proc, uint32_t n)
{
	struct rv_chunk *c = NULL;
	int first = -1;

	if (n > POOL_RUN_MAX) {
		if (!(c = chunk_new(proc, n, 0)))
			return NULL;
		c->used = n;
		return c->host;
	}
	/* Where the last one was found first: chunks fill up in turn. */
	if (proc->pool_hint < proc->nchunks && proc->chunks[proc->pool_hint]->pool)
		first = pool_find(c = proc->chunks[proc->pool_hint], n);
	for (uint32_t i = 0; first < 0 && i < proc->nchunks; i++) {
		if (proc->chunks[i]->pool && (first = pool_find(c = proc->chunks[i], n)) >= 0)
			proc->pool_hint = i;
	}
	if (first < 0) {
		if (!(c = chunk_new(proc, proc->pool_chunks < POOL_SMALL_CHUNKS ? POOL_PAGES_SMALL : POOL_PAGES, 1)))
			return NULL;
		proc->pool_chunks++;
		first = 0;
	}
	for (uint32_t i = (uint32_t)first; i < (uint32_t)first + n; i++) {
		c->busy[i / 64] |= 1ULL << (i % 64);
		if (c->dirty[i / 64] >> (i % 64) & 1) {
			memset(c->host + ((uint64_t)i << RV_PAGE_SHIFT), 0, RV_PAGE_SIZE);
			c->dirty[i / 64] &= ~(1ULL << (i % 64));
		}
	}
	c->used += n;
	return c->host + ((uint64_t)first << RV_PAGE_SHIFT);
}

/* A page of host memory is no guest page any more. */
static void page_free(struct rv_proc *proc, uint8_t *host)
{
	struct rv_chunk *c = chunk_of(proc, host);

	if (!c)
		return;
	if (c->pool) {
		uint32_t i = (uint32_t)((host - c->host) >> RV_PAGE_SHIFT);

		c->busy[i / 64] &= ~(1ULL << (i % 64));
		c->dirty[i / 64] |= 1ULL << (i % 64);
	}
	if (--c->used == 0)
		chunk_free(proc, c);
}

/* -------------------------------------------------------------- page table */

static unsigned want_of(uint64_t e)
{
	return (unsigned)(e >> RV_PAGE_WANT_SHIFT) & RV_PROT_MASK;
}

static uint8_t *host_of(uint64_t e, uint64_t page)
{
	return (uint8_t *)(uintptr_t)((page << RV_PAGE_SHIFT) + (e & ~FLAGS));
}

/* The entry of a mapped page: allowed `want`, at `host` if it has memory. */
static uint64_t entry(uint64_t page, unsigned want, const uint8_t *host)
{
	uint64_t e = RV_PAGE_MAPPED | (uint64_t)want << RV_PAGE_WANT_SHIFT;

	if (host)
		e |= (((uint64_t)(uintptr_t)host - (page << RV_PAGE_SHIFT)) & ~FLAGS) | RV_PAGE_BACKED | want;
	return e;
}

/* ... at `host` in a file's shared segment: as it stands it is never to be written (see page_private()). */
static uint64_t shared_entry(uint64_t page, unsigned want, const uint8_t *host)
{
	return (entry(page, want, host) & ~(uint64_t)RV_PROT_WRITE) | RV_PAGE_SHARED;
}

/* The entry for the memory that the page with entry `old` has, if any, as page `page` allowed `want`. */
static uint64_t entry_as(uint64_t old, uint64_t old_page, uint64_t page, unsigned want)
{
	if (!(old & RV_PAGE_BACKED))
		return entry(page, want, NULL);
	if (old & RV_PAGE_LIVE)
		return entry(page, want, host_of(old, old_page)) | RV_PAGE_LIVE;
	return old & RV_PAGE_SHARED ? shared_entry(page, want, host_of(old, old_page))
				    : entry(page, want, host_of(old, old_page));
}

/*
 * The pages, of those a call changes, that code was translated from: from
 * the first of them to the last. (Mostly none, or a few next to each other.)
 */
struct code_span {
	uint64_t first, end;
};

/*
 * Give a page a new entry. If code was translated from it, in whichever
 * thread, that code is no longer to be run as it is (jit.c): the new entry,
 * which has no mark, is in place first, so a thread that translates from the
 * page from here on sees it, and one that did before has left its mark,
 * which is noted in *code for code_changed(). Returns the old entry.
 */
static uint64_t set_page(struct rv_cpu *cpu, uint64_t i, uint64_t e, struct code_span *code)
{
	uint64_t old = __atomic_exchange_n(&cpu->map[i], e, __ATOMIC_SEQ_CST);

	if (old & RV_PAGE_CODE) {
		if (!code->end)
			code->first = i;
		code->end = i + 1;
	}
	return old;
}

/* ... and when all entries are in place: tell the translator, if there is anything to tell. */
static void code_changed(struct rv_cpu *cpu, const struct code_span *code, enum rv_code_cause cause)
{
	if (code->end)
		rv_jit_code_changed(cpu, code->first, code->end - code->first, cause);
}

int rv_mm_init(struct rv_cpu *cpu, uint64_t size)
{
	cpu->npages = size >> RV_PAGE_SHIFT;
	/* One more than there are pages: an entry that allows nothing, for the JIT's checks. */
	cpu->map = calloc(cpu->npages + 1, sizeof(*cpu->map));
	return cpu->map ? 0 : -1;
}

/*
 * The display.
 *
 * A program draws by mapping the frame buffer device and writing pixels to
 * the mapping, which has to be the device's memory for that to show. Here a
 * mapping is host memory of ours. On Linux/Wasm that is no obstacle: all of
 * the machine is one address space, the host that shows the display sees
 * all of it, and the frame buffer driver has a request for exactly this
 * (WASMFB_SET_PIXELS: show the caller's memory at this address instead). So
 * the guest's mapping is one piece of our memory, and it is what is on the
 * screen: nothing is copied. The display goes back to the device's own
 * memory when the mapping goes or the process ends (the kernel sees to the
 * latter).
 */
#define FB_MAJOR		29
#define WASMFB_SET_PIXELS	_IOW('F', 0xa0, unsigned long)

static int is_display(int fd)
{
	struct stat st;

	return !fstat(fd, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == FB_MAJOR;
}

/* The guest's mapping of the display is going away, in part or whole. */
static void display_release(struct rv_cpu *cpu, uint64_t first, uint64_t count)
{
	struct rv_proc *proc = cpu->proc;

	if (!proc->fb_len || (first << RV_PAGE_SHIFT) >= proc->fb_addr + proc->fb_len ||
	    ((first + count) << RV_PAGE_SHIFT) <= proc->fb_addr)
		return;
	ioctl(proc->fb_fd, WASMFB_SET_PIXELS, 0UL);
	close(proc->fb_fd);
	proc->fb_len = 0;
}

static uint64_t va_size(const struct rv_cpu *cpu)
{
	return cpu->npages << RV_PAGE_SHIFT;
}

static int range_ok(struct rv_cpu *cpu, uint64_t addr, uint64_t len)
{
	return !(addr & (RV_PAGE_SIZE - 1)) && len && addr <= va_size(cpu) && len <= va_size(cpu) - addr;
}

static int range_free(struct rv_cpu *cpu, uint64_t first, uint64_t count)
{
	for (uint64_t i = 0; i < count; i++)
		if (cpu->map[first + i] & RV_PAGE_MAPPED)
			return 0;
	return 1;
}

/* The pages [first, first + count) are no file's any more. */
static void named_cut(struct rv_proc *proc, uint64_t first, uint64_t count)
{
	uint64_t end = first + count;

	for (uint32_t i = 0; i < proc->nnamed; i++) {
		struct rv_named *n = &proc->named[i];
		uint64_t n_end = n->first + n->count;

		if (n_end <= first || n->first >= end)
			continue;
		if (n->first < first && n_end > end) {
			/* Out of its middle: two of it. (If there is no room for the second, the part above goes unnamed.) */
			if (proc->nnamed < proc->named_cap) {
				struct rv_named *above = &proc->named[proc->nnamed++];

				*above = *n;
				above->first = end;
				above->count = n_end - end;
				above->offset += (end - n->first) << RV_PAGE_SHIFT;
			}
			n->count = first - n->first;
		} else if (n->first < first) {
			n->count = first - n->first;
		} else if (n_end > end) {
			n->offset += (end - n->first) << RV_PAGE_SHIFT;
			n->count = n_end - end;
			n->first = end;
		} else {
			*n = proc->named[--proc->nnamed];
			i--;
		}
	}
}

/* The pages [first, first + count) are a mapping of the file `fd` from `offset`. With the lock. */
static void named_add(struct rv_proc *proc, uint64_t first, uint64_t count, int fd, uint64_t offset, int copy)
{
	struct rv_named *n;
	struct stat st;
	char link[40];
	ssize_t len;

	if (proc->nnamed + 2 > proc->named_cap) {	/* (one to spare: named_cut() may need it) */
		uint32_t cap = proc->named_cap ? proc->named_cap * 2 : 64;

		if (!(n = realloc(proc->named, cap * sizeof(*n))))
			return;
		proc->named = n;
		proc->named_cap = cap;
	}
	n = &proc->named[proc->nnamed];
	memset(n, 0, sizeof(*n));
	n->first = first;
	n->count = count;
	n->offset = offset;
	n->copy = copy;
	if (copy)
		proc->file_copies = 1;
	if (!fstat(fd, &st)) {
		n->dev = (uint64_t)st.st_dev;
		n->ino = (uint64_t)st.st_ino;
	}
	/* The file's name as the kernel has it: what an open file is called. */
	snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	len = readlink(link, n->path, sizeof(n->path) - 1);
	if (len < 0)
		len = 0;
	n->path[len] = '\0';
	proc->nnamed++;
}

/* For the loader, which fills the program's segments itself (rv_mm_fill()). */
void rv_mm_name(struct rv_cpu *cpu, uint64_t addr, uint64_t len, int fd, uint64_t offset)
{
	uint64_t first = addr >> RV_PAGE_SHIFT, in_page = addr & (RV_PAGE_SIZE - 1);

	if (!len)
		return;
	rv_lock(cpu);
	named_add(cpu->proc, first, (RV_PAGE_ALIGN(addr + len) >> RV_PAGE_SHIFT) - first, fd, offset - in_page, 0);
	rv_unlock(cpu);
}

/*
 * Which file the code at `addr` is of, and where in it: for knowing a piece
 * of a program again in another process (jit.c). *fixed: the page is the
 * file's own, shared and never written, so the bytes are the file's for as
 * long as it is mapped. Returns 0 if `addr` is in no file's mapping.
 */
int rv_mm_file_at(struct rv_cpu *cpu, uint64_t addr, uint64_t *dev, uint64_t *ino, uint64_t *offset, int *fixed)
{
	struct rv_proc *proc = cpu->proc;
	uint64_t page = addr >> RV_PAGE_SHIFT;
	int found = 0;

	rv_lock(cpu);
	for (uint32_t k = 0; k < proc->nnamed; k++) {
		const struct rv_named *n = &proc->named[k];

		if (page >= n->first && page < n->first + n->count) {
			*dev = n->dev;
			*ino = n->ino;
			*offset = n->offset + (addr - (n->first << RV_PAGE_SHIFT));
			*fixed = page < cpu->npages && (cpu->map[page] & RV_PAGE_SHARED) != 0;
			found = 1;
			break;
		}
	}
	rv_unlock(cpu);
	return found;
}

/*
 * /proc/self/maps for the guest, written to `fd`: its mappings as its page
 * table has them, a line for each run of pages of one kind. (The host's
 * file of that name describes the emulator.) Programs read it to find their
 * own stack (the C library does, for the first thread), their libraries, and
 * what they may touch. 0, or -1.
 */
int rv_mm_maps(struct rv_cpu *cpu, int fd)
{
	struct rv_proc *proc = cpu->proc;
	uint64_t heap_first = RV_PAGE_ALIGN(cpu->brk_start) >> RV_PAGE_SHIFT, heap_end = RV_PAGE_ALIGN(proc->brk) >> RV_PAGE_SHIFT;
	uint64_t stack_first = cpu->stack_low >> RV_PAGE_SHIFT, i = 0;
	size_t cap = 16384, used = 0;
	char *text = malloc(cap);
	int ret = 0;

	if (!text)
		return -1;
	rv_lock(cpu);
	while (i < cpu->npages) {
		uint64_t e = cpu->map[i], run = 1, key;
		const struct rv_named *named = NULL;
		const char *label = "";
		unsigned want;
		int n;

		if (!(e & RV_PAGE_MAPPED)) {
			i++;
			continue;
		}
		for (uint32_t k = 0; k < proc->nnamed; k++)
			if (i >= proc->named[k].first && i < proc->named[k].first + proc->named[k].count)
				named = &proc->named[k];
		want = want_of(e);
		key = (e & RV_PAGE_LIVE) | want;
		/* As far as the pages are of the same kind: the same file's or nobody's, heap, stack or neither. */
		while (i + run < cpu->npages && (cpu->map[i + run] & RV_PAGE_MAPPED) &&
		       ((cpu->map[i + run] & RV_PAGE_LIVE) | want_of(cpu->map[i + run])) == key &&
		       (named ? i + run < named->first + named->count
			      : (i + run != heap_first && i + run != heap_end && i + run != stack_first))) {
			int other = 0;

			if (!named)
				for (uint32_t k = 0; k < proc->nnamed && !other; k++)
					other = i + run == proc->named[k].first;
			if (other)
				break;
			run++;
		}
		if (named)
			label = named->path;
		else if (i >= heap_first && i < heap_end)
			label = "[heap]";
		else if (stack_first && i <= stack_first && stack_first < i + run)
			label = "[stack]";

		if (cap - used < 400) {
			char *more = realloc(text, cap * 2);

			if (!more) {
				ret = -1;
				break;
			}
			text = more;
			cap *= 2;
		}
		n = snprintf(text + used, cap - used, "%08llx-%08llx %c%c%c%c %08llx %02x:%02x %llu",
			     (unsigned long long)(i << RV_PAGE_SHIFT), (unsigned long long)((i + run) << RV_PAGE_SHIFT),
			     want & RV_PROT_READ ? 'r' : '-', want & RV_PROT_WRITE ? 'w' : '-',
			     want & RV_PROT_EXEC ? 'x' : '-', e & RV_PAGE_LIVE ? 's' : 'p',
			     (unsigned long long)(named ? named->offset + ((i - named->first) << RV_PAGE_SHIFT) : 0),
			     named ? (unsigned)(named->dev >> 8) & 0xfff : 0, named ? (unsigned)(named->dev & 0xff) : 0,
			     (unsigned long long)(named ? named->ino : 0));
		used += (size_t)n;
		if (*label) {
			/* (The kernel puts the name in column 74.) */
			while (n++ < 73)
				text[used++] = ' ';
			used += (size_t)snprintf(text + used, cap - used, "%s", label);
		}
		text[used++] = '\n';
		i += run;
	}
	/* The reservations, each as what nearly all of it is. */
	for (uint32_t k = 0; k < proc->nreserves && !ret; k++) {
		const struct rv_reserve *r = &proc->reserves[k];

		if (cap - used < 200) {
			char *more = realloc(text, cap * 2);

			if (!more) {
				ret = -1;
				break;
			}
			text = more;
			cap *= 2;
		}
		used += (size_t)snprintf(text + used, cap - used, "%08llx-%08llx ---p 00000000 00:00 0\n",
					 (unsigned long long)r->base,
					 (unsigned long long)(r->base + (r->count << RV_PAGE_SHIFT)));
	}
	rv_unlock(cpu);
	for (size_t done = 0; !ret && done < used;) {
		ssize_t w = write(fd, text + done, used - done);

		if (w <= 0)
			ret = -1;
		else
			done += (size_t)w;
	}
	free(text);
	return ret;
}

static void unmap_pages(struct rv_cpu *cpu, uint64_t first, uint64_t count)
{
	struct code_span code = { 0, 0 };

	display_release(cpu, first, count);
	if (cpu->proc->nnamed)
		named_cut(cpu->proc, first, count);
	for (uint64_t i = first; i < first + count; i++) {
		uint64_t old;

		if (!(cpu->map[i] & RV_PAGE_MAPPED))
			continue;
		old = set_page(cpu, i, 0, &code);
		if (old & RV_PAGE_BACKED)
			page_free(cpu->proc, host_of(old, i));
	}
	code_changed(cpu, &code, RV_CODE_UNMAP);
}

/* Map pages, replacing whatever was there. They have no memory yet, and read as zero when they get some. */
static void map_pages(struct rv_cpu *cpu, uint64_t first, uint64_t count, unsigned prot)
{
	unmap_pages(cpu, first, count);
	for (uint64_t i = first; i < first + count; i++)
		__atomic_store_n(&cpu->map[i], entry(i, prot & RV_PROT_MASK, NULL), __ATOMIC_SEQ_CST);
}

/*
 * Give memory to those of the pages [first, first + count) that are mapped
 * and have none, a run of them at a time, each run in one piece if that can
 * be had. -1 if the host has no memory left.
 */
static int back_pages(struct rv_cpu *cpu, uint64_t first, uint64_t count)
{
	uint64_t i = first, end = first + count;

	while (i < end) {
		uint64_t run = 0;
		uint8_t *host;

		if ((cpu->map[i] & (RV_PAGE_MAPPED | RV_PAGE_BACKED)) != RV_PAGE_MAPPED) {
			i++;
			continue;
		}
		while (i + run < end && (cpu->map[i + run] & (RV_PAGE_MAPPED | RV_PAGE_BACKED)) == RV_PAGE_MAPPED)
			run++;
		/* In one piece; failing that (the host's memory is in small pieces by now), in halves. */
		while (!(host = pages_alloc(cpu->proc, (uint32_t)run))) {
			if (run == 1)
				return -1;
			run = (run + 1) / 2;
		}
		for (uint64_t k = 0; k < run; k++) {
			uint64_t old = cpu->map[i + k];

			/* (A page without memory has no mark of translated code: nothing can have been translated from it.) */
			__atomic_store_n(&cpu->map[i + k], entry(i + k, want_of(old), host + (k << RV_PAGE_SHIFT)),
					 __ATOMIC_SEQ_CST);
		}
		i += run;
	}
	return 0;
}

static int page_private(struct rv_cpu *cpu, uint64_t page);
static void code_writable(struct rv_cpu *cpu, uint64_t page);

/* The page kept free below the initial stack, so that running off the stack faults. */
static uint64_t guard_page(const struct rv_cpu *cpu)
{
	return cpu->mmap_top >> RV_PAGE_SHIFT;
}

/*
 * Find room for a new mapping: the highest free run, like Linux's top-down
 * layout, which keeps mappings away from the heap growing upwards. Returns
 * the first page of the run, or 0.
 */
static uint64_t find_run(struct rv_cpu *cpu, uint64_t count)
{
	uint64_t floor = RV_MMAP_MIN_ADDR >> RV_PAGE_SHIFT;
	uint64_t guard = guard_page(cpu);
	uint64_t run = 0;

	for (uint64_t i = cpu->npages; i > floor; i--) {
		if ((cpu->map[i - 1] & RV_PAGE_MAPPED) || i - 1 == guard) {
			run = 0;
			continue;
		}
		if (++run == count)
			return i - 1;
	}
	return 0;
}

/*
 * Make the guest's address space larger by at least `need` bytes: a longer
 * page table. The guest sees an address space that has become larger at its
 * top end, above the initial stack, and nothing else. Every thread has the
 * table's address (translated code reads it from there whenever a region is
 * entered), so the others are parked, outside translated code, for the
 * moment it changes (thread.c).
 */
static int grow(struct rv_cpu *cpu, uint64_t need)
{
	uint64_t old_pages = cpu->npages, pages = old_pages;
	uint64_t max_pages = cpu->mem_max >> RV_PAGE_SHIFT;
	uint64_t *map, *old = cpu->map;

	need = RV_PAGE_ALIGN(need) >> RV_PAGE_SHIFT;
	while (pages < old_pages + need && pages < max_pages)
		pages *= 2;
	if (pages > max_pages)
		pages = max_pages;
	if (pages < old_pages + need)
		return -1;
	if (!(map = calloc(pages + 1, sizeof(*map))))
		return -1;

	rv_stop_world(cpu);
	memcpy(map, old, old_pages * sizeof(*map));
	for (struct rv_cpu *t = cpu->proc->threads; t; t = t->next_thread) {
		t->map = map;
		t->npages = pages;
	}
	rv_jit_code_changed(cpu, 0, 0, RV_CODE_MOVED);
	rv_start_world(cpu);
#ifdef RV_HEAPCHECK
	rv_hc_light("with the page table moved");
#endif
	free(old);
#ifdef RV_HEAPCHECK
	rv_hc_light("with the old page table freed");
#endif
	if (cpu->verbose)
		fprintf(stderr, "rv: the guest's address space grew from %llu to %llu MiB\n",
			(unsigned long long)(old_pages >> (20 - RV_PAGE_SHIFT)),
			(unsigned long long)(pages >> (20 - RV_PAGE_SHIFT)));
	return 0;
}

/* find_run(), making the address space larger if that is what it takes. */
static uint64_t find_free(struct rv_cpu *cpu, uint64_t count)
{
	uint64_t first = find_run(cpu, count);

	/* (A page more than asked for: the run must not end up against the stack's guard page only.) */
	if (!first && !grow(cpu, (count + 1) << RV_PAGE_SHIFT))
		first = find_run(cpu, count);
	return first;
}

/*
 * Make the address space span at least `bytes`, for the loader: a program
 * may reach above what the arena starts with (Chromium is 250 MiB of code
 * and data), and the stack has to go above the program. 0, or -1 if it
 * cannot grow that far.
 */
int rv_mm_grow_to(struct rv_cpu *cpu, uint64_t bytes)
{
	uint64_t have = cpu->npages << RV_PAGE_SHIFT;

	return bytes <= have ? 0 : grow(cpu, bytes - have);
}

/* A free, page-aligned range of `len` bytes, or 0. (For the loader: before there is a second thread.) */
uint64_t rv_mm_find_free(struct rv_cpu *cpu, uint64_t len)
{
	return find_free(cpu, RV_PAGE_ALIGN(len) >> RV_PAGE_SHIFT) << RV_PAGE_SHIFT;
}

/*
 * Make sure a range is mapped and add the given protection, keeping what is
 * there. For the ELF loader, where two segments may share a page, and for
 * the initial stack. (Before there is a second thread: no lock.)
 */
int rv_mm_reserve(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot)
{
	uint64_t start = addr & ~(RV_PAGE_SIZE - 1);
	uint64_t end = RV_PAGE_ALIGN(addr + len);

	if (end < addr || end > va_size(cpu) || start < RV_PAGE_SIZE)
		return -1;
	for (uint64_t i = start >> RV_PAGE_SHIFT; i < end >> RV_PAGE_SHIFT; i++) {
		uint64_t e = cpu->map[i];
		unsigned want = (e & RV_PAGE_MAPPED ? want_of(e) : 0) | (prot & RV_PROT_MASK);

		cpu->map[i] = entry_as(e, i, i, want);
	}
	return 0;
}

/*
 * Put bytes into the guest's memory whatever its protection, giving the
 * pages memory first: `len` bytes at `addr`, from `src`, or if that is NULL
 * from the file `fd` at `offset` (where the file ends short, what is there
 * stays: zeros, in fresh pages). For the loader and for file mappings.
 * Returns 0, or -1 if a page is not mapped, there is no memory, or the file
 * cannot be read.
 */
int rv_mm_fill(struct rv_cpu *cpu, uint64_t addr, uint64_t len, const void *src, int fd, uint64_t offset)
{
	uint64_t first = addr >> RV_PAGE_SHIFT, last, done = 0;

	if (!len)
		return 0;
	last = (addr + len - 1) >> RV_PAGE_SHIFT;
	if (addr + len < addr || last >= cpu->npages)
		return -1;
	for (uint64_t i = first; i <= last; i++)
		if (!(cpu->map[i] & RV_PAGE_MAPPED))
			return -1;
	if (back_pages(cpu, first, last - first + 1)) {
		errno = ENOMEM;
		return -1;
	}
	for (uint64_t i = first; i <= last; i++) {
		if (page_private(cpu, i)) {
			errno = ENOMEM;
			return -1;
		}
		code_writable(cpu, i);
	}

	while (done < len) {
		uint64_t a = addr + done, page = a >> RV_PAGE_SHIFT;
		uint64_t e = cpu->map[page], n = RV_PAGE_SIZE - (a & (RV_PAGE_SIZE - 1));
		uint8_t *host = (uint8_t *)(uintptr_t)(a + (e & ~FLAGS));

		/* As far as the pages lie in one piece. */
		while (n < len - done && page + 1 <= last && (cpu->map[page + 1] & ~FLAGS) == (e & ~FLAGS)) {
			n += RV_PAGE_SIZE;
			page++;
		}
		if (n > len - done)
			n = len - done;
		if (src) {
			memcpy(host, (const uint8_t *)src + done, n);
		} else {
			for (uint64_t got = 0; got < n;) {
				ssize_t r = pread(fd, host + got, n - got, (off_t)(offset + done + got));

				if (r < 0)
					return -1;
				if (r == 0)
					return 0;	/* past the end of the file */
				got += (uint64_t)r;
			}
		}
		done += n;
	}
	return 0;
}

/*
 * A shared mapping of a file, as the guest means it: the file's own memory,
 * the same for every process that maps it, so that what one writes the
 * others see. That is the host's to give, and it does for files that live in
 * memory (what memfd_create() and shm_open() make: how a program and the X
 * server share a window's pixels); the guest's pages are then that memory.
 * Without an MMU it does not for a file on a disk. Returns whether it did.
 * (A fork's child that started as a second emulator gets copies of such
 * pages for now, not the shared memory.)
 */
static int map_host_shared(struct rv_cpu *cpu, uint64_t first, uint64_t count, unsigned prot, int fd, uint64_t offset)
{
	struct rv_proc *proc = cpu->proc;
	size_t len = (size_t)count << RV_PAGE_SHIFT;
	struct rv_chunk *c;
	uint8_t *host;

	/* Writable if the file is open for that, whatever the guest asks for now: it may ask for more later. */
	host = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);
	if (host == MAP_FAILED) {
		if (prot & RV_PROT_WRITE)
			return 0;
		host = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, (off_t)offset);
		if (host == MAP_FAILED)
			return 0;
	}
	if (chunk_room(proc) || !(c = calloc(1, sizeof(*c)))) {
		munmap(host, len);
		return 0;
	}
	c->host = host;
	c->npages = (uint32_t)count;
	c->used = (uint32_t)count;
	c->file = 1;
	chunk_add(proc, c);
	map_pages(cpu, first, count, prot);
	for (uint64_t k = 0; k < count; k++)
		__atomic_store_n(&cpu->map[first + k], entry(first + k, prot & RV_PROT_MASK, host + (k << RV_PAGE_SHIFT)),
				 __ATOMIC_SEQ_CST);
	return 1;
}

/* ------------------------------------------------------------ reservations */

/*
 * A program may ask for far more addresses than it will ever use, to have
 * them in one piece: 16 GiB for every sandbox Firefox runs a library in (the
 * library's memory, which may grow to 4 GiB, and as much again that nothing
 * is ever in, so that a wild pointer lands there). Nothing may be accessed
 * in a range like that until the program says so, a part at a time.
 *
 * The page table has an entry for every page of the addresses it spans, and
 * cannot span such ranges (16 GiB of addresses are 32 MiB of entries, of
 * real memory here). So a mapping of nothing (no access, no file) that is
 * too large for the table becomes a reservation instead: a range of
 * addresses above the table's, of which only the part that the program has
 * made something of has pages, and those are pages of the table like any
 * others, somewhere in it, under addresses the program never sees. The
 * calls that take an address (mmap() at a fixed place, mprotect(),
 * munmap(), madvise()) and every access by address go by way of
 * reserve_flat() to those. Translated code does not know about any of this:
 * to it the address is outside the table, and the instruction is left to
 * the interpreter (rv_ptr_slow()). Slower, for what little runs there.
 */
#define RESERVE_MIN	(1ULL << 30)
#define RESERVE_ALIGN	(4ULL << 30)
#define RESERVE_END	(1ULL << 46)

static struct rv_reserve *reserve_of(struct rv_proc *proc, uint64_t addr)
{
	for (uint32_t i = 0; i < proc->nreserves; i++) {
		struct rv_reserve *r = &proc->reserves[i];

		if (addr >= r->base && addr - r->base < r->count << RV_PAGE_SHIFT)
			return r;
	}
	return NULL;
}

/*
 * Where in the page table's addresses [addr, addr + len) is, if it is in a
 * reservation and in the part of it that has pages; 0 if not. *inside:
 * whether it began in a reservation at all.
 */
static uint64_t reserve_flat(struct rv_proc *proc, uint64_t addr, uint64_t len, int *inside)
{
	struct rv_reserve *r = reserve_of(proc, addr);
	uint64_t at, lo, hi;

	*inside = r != NULL;
	if (!r)
		return 0;
	at = addr - r->base;
	lo = at >> RV_PAGE_SHIFT;
	hi = (at + (len ? len - 1 : 0)) >> RV_PAGE_SHIFT;
	if (lo < r->off || hi >= r->off + r->n)
		return 0;
	return ((r->flat - r->off) << RV_PAGE_SHIFT) + at;
}

/* What has changed where in the table a reservation's pages are (or whether they are anywhere): see rv_reserve_xlat(). */
static void reserve_moved(struct rv_proc *proc)
{
	__atomic_add_fetch(&proc->reserve_gen, 1, __ATOMIC_SEQ_CST);
}

/* Where in the table's addresses the page of `addr` in a reservation is, by way of what this thread remembers; 0 if nowhere. *gen: the count it is good for. */
static uint64_t reserve_seen_flat(struct rv_cpu *cpu, uint64_t addr, uint64_t *gen)
{
	struct rv_proc *proc = cpu->proc;
	uint64_t page = addr >> RV_PAGE_SHIFT, flat;
	struct rv_reserve_seen *seen = &cpu->reserve_seen[page & (sizeof(cpu->reserve_seen) / sizeof(cpu->reserve_seen[0]) - 1)];
	int inside;

	*gen = __atomic_load_n(&proc->reserve_gen, __ATOMIC_ACQUIRE);
	if (cpu->verbose)
		proc->reserve_lookups++;	/* (counts for RV_VERBOSE; threads may lose some of each other's) */
	if (seen->page == page && seen->gen == *gen)
		return seen->flat << RV_PAGE_SHIFT;
	if (cpu->verbose)
		proc->reserve_misses++;
	rv_lock(cpu);
	*gen = proc->reserve_gen;
	flat = reserve_flat(proc, addr & ~(RV_PAGE_SIZE - 1), 1, &inside);
	rv_unlock(cpu);
	if (flat) {
		seen->page = page;
		seen->flat = flat >> RV_PAGE_SHIFT;
		seen->gen = *gen;
	}
	return flat;
}

/*
 * The host address of [addr, addr + len) in a reservation, if that is in one
 * page that allows `prot` as it stands; 0 if not (no such page, not allowed,
 * across two pages, or no memory yet: the slow way, rv_ptr_slow(), sees to
 * those).
 *
 * Programs that reserve make their heaps there: all of Chromium's memory is
 * in reservations (PartitionAlloc's pools, V8's and Blink's heaps), and every
 * access took the lock to find the page. So each thread remembers which page
 * of the table stands for which page of a reservation, in reserve_seen, with
 * the count reserve_gen had when it looked; what it remembers is good while
 * the count is the same. Whatever moves a reservation's pages in the table,
 * or frees them, counts up afterwards (reserve_moved()). A thread that
 * remembers the old place while that happens finds there either the same
 * memory (entries move, the memory stays) or an entry that allows nothing,
 * and takes the slow way; the count is looked at again after the entry, so
 * that pages given to something else in the meantime are not used.
 */
uint64_t rv_reserve_xlat(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot)
{
	uint64_t page = addr >> RV_PAGE_SHIFT, flat, gen, e;

	if (((addr + len - 1) >> RV_PAGE_SHIFT) != page || page < cpu->npages || !cpu->proc->nreserves)
		return 0;
	flat = reserve_seen_flat(cpu, addr, &gen) >> RV_PAGE_SHIFT;
	if (!flat || flat >= cpu->npages)
		return 0;
	e = __atomic_load_n(&cpu->map[flat], __ATOMIC_ACQUIRE);
	if ((e & prot) != prot || __atomic_load_n(&cpu->proc->reserve_gen, __ATOMIC_ACQUIRE) != gen)
		return 0;
	return (flat << RV_PAGE_SHIFT) + (addr & (RV_PAGE_SIZE - 1)) + (e & ~(uint64_t)RV_PAGE_FLAGS);
}

/* For translated code (jit.c: check_access_as()): rv_reserve_xlat() with the size, prot and whether the access must be aligned in one number. */
uint64_t rv_jit_reserve(struct rv_cpu *cpu, uint64_t addr, uint64_t how)
{
	uint64_t size = how & 0xff;

	if ((how & (1 << 16)) && (addr & (size - 1)))
		return 0;
	return rv_reserve_xlat(cpu, addr, size, (unsigned)(how >> 8) & 0xff);
}

/*
 * Give the reservation's pages [lo, hi) pages of the table, if they have
 * none: more of them next to the ones it has, or all of them somewhere else
 * (the entries move; the memory stays where it is). What is new allows
 * nothing, as all of a reservation does at first. 0, or -1.
 */
static int reserve_cover(struct rv_cpu *cpu, struct rv_reserve *r, uint64_t lo, uint64_t hi)
{
	uint64_t off = r->n && r->off < lo ? r->off : lo;
	uint64_t end = r->n && r->off + r->n > hi ? r->off + r->n : hi;
	uint64_t need, first, more;
	struct code_span code = { 0, 0 };

	if (r->n && off == r->off && end == r->off + r->n)
		return 0;
	/* Some to spare: it mostly grows, a little at a time. */
	more = (end - off) / 4 > 64 ? (end - off) / 4 : 64;
	end = end + more < r->count ? end + more : r->count;
	need = end - off;
	if (r->n && off == r->off && r->flat + need <= cpu->npages &&
	    !(r->flat + r->n <= guard_page(cpu) && guard_page(cpu) < r->flat + need) &&
	    range_free(cpu, r->flat + r->n, need - r->n)) {
		map_pages(cpu, r->flat + r->n, need - r->n, 0);
		r->n = need;
		return 0;
	}
	first = find_free(cpu, need);
	if (!first)
		return -1;
	map_pages(cpu, first, need, 0);
	for (uint64_t k = 0; k < r->n; k++) {
		uint64_t old = set_page(cpu, r->flat + k, 0, &code), to = first + (r->off - off) + k;

		__atomic_store_n(&cpu->map[to], entry_as(old, r->flat + k, to, want_of(old)), __ATOMIC_SEQ_CST);
	}
	code_changed(cpu, &code, RV_CODE_UNMAP);
	r->flat = first;
	r->off = off;
	r->n = need;
	reserve_moved(cpu->proc);
	return 0;
}

/*
 * One run of the table's pages covers whatever part of a reservation the
 * program has made something of, gaps included. That cannot be far too
 * much: Chromium's allocator keeps two 16 GiB pools in one 32 GiB
 * reservation and uses each from its start, and a run across both would be
 * 16 GiB of the table, more than the arena may span. So where what is
 * asked for lies RESERVE_GAP or more from the pages a reservation has, the
 * reservation is split into two in between, and the far part is a
 * reservation of its own with a run of its own. The split is at an address
 * that is a multiple of RESERVE_GAP, which nothing is likely to straddle.
 */
#define RESERVE_GAP	(256ULL << 20)

/* Make pages [at, count) of `r` a reservation of their own; the run goes with the half it is in. The new one, or NULL. */
static struct rv_reserve *reserve_split(struct rv_proc *proc, struct rv_reserve *r, uint64_t at)
{
	struct rv_reserve *s;

	if (proc->nreserves == sizeof(proc->reserves) / sizeof(proc->reserves[0]))
		return NULL;
	s = &proc->reserves[proc->nreserves++];
	memset(s, 0, sizeof(*s));
	s->base = r->base + (at << RV_PAGE_SHIFT);
	s->count = r->count - at;
	r->count = at;
	if (r->n && r->off >= at) {
		s->flat = r->flat;
		s->off = r->off - at;
		s->n = r->n;
		r->flat = r->off = r->n = 0;
	}
	reserve_moved(proc);
	return s;
}

/*
 * reserve_cover() for the pages [*lo, *hi) of **rp, after splitting the
 * reservation where those lie far from the pages it has. On return *rp,
 * *lo and *hi name the same addresses, in the reservation they are in now.
 * 0, or -1.
 */
static int reserve_pages(struct rv_cpu *cpu, struct rv_reserve **rp, uint64_t *lo, uint64_t *hi)
{
	struct rv_reserve *r = *rp, *s;
	uint64_t gap = RESERVE_GAP >> RV_PAGE_SHIFT, first = r->base >> RV_PAGE_SHIFT, at;

	if (r->n && *lo >= r->off + r->n + gap) {
		/* Far above the pages: the split is at the aligned address just below, which is still above them. */
		at = *lo - (first + *lo) % gap;
		if (at > r->off + r->n && (s = reserve_split(cpu->proc, r, at))) {
			*rp = r = s;
			*lo -= at;
			*hi -= at;
		}
	} else if (r->n && *hi + gap <= r->off) {
		/* Far below them: the pages go to the new reservation, which begins at the aligned address just above *hi. */
		at = *hi + (gap - (first + *hi) % gap) % gap;
		if (at <= r->off)
			reserve_split(cpu->proc, r, at);
	}
	return reserve_cover(cpu, r, *lo, *hi);
}

/* A new reservation of `len` bytes: its address, or an error. */
static uint64_t reserve_new(struct rv_cpu *cpu, uint64_t len)
{
	struct rv_proc *proc = cpu->proc;
	struct rv_reserve *r;
	uint64_t base;

	if (proc->nreserves == sizeof(proc->reserves) / sizeof(proc->reserves[0]))
		return neg(RV_ENOMEM);
	/* Above everything the page table can come to span, at a multiple of 4 GiB (what such programs want anyway). */
	if (!proc->reserve_next)
		proc->reserve_next = (cpu->mem_max + 2 * RESERVE_ALIGN - 1) & ~(RESERVE_ALIGN - 1);
	base = proc->reserve_next;
	if (base + len > RESERVE_END)
		return neg(RV_ENOMEM);
	proc->reserve_next = (base + len + RESERVE_ALIGN - 1) & ~(RESERVE_ALIGN - 1);
	r = &proc->reserves[proc->nreserves++];
	memset(r, 0, sizeof(*r));
	r->base = base;
	r->count = len >> RV_PAGE_SHIFT;
	return base;
}

/* munmap() of a part of a reservation: from its beginning or its end it gets shorter, and with all of it gone it is gone. */
static void reserve_unmap(struct rv_cpu *cpu, struct rv_reserve *r, uint64_t addr, uint64_t len)
{
	struct rv_proc *proc = cpu->proc;
	uint64_t lo = (addr - r->base) >> RV_PAGE_SHIFT, hi = lo + (RV_PAGE_ALIGN(len) >> RV_PAGE_SHIFT);
	uint64_t s_lo, s_hi;

	reserve_moved(proc);	/* (first: what is freed here can be given out again as soon as the lock is let go) */
	if (hi > r->count)
		hi = r->count;
	/* The pages of the table for that part. */
	s_lo = lo > r->off ? lo : r->off;
	s_hi = hi < r->off + r->n ? hi : r->off + r->n;
	if (lo == 0 || hi == r->count) {
		if (s_lo < s_hi) {
			unmap_pages(cpu, r->flat + (s_lo - r->off), s_hi - s_lo);
			if (lo == 0)
				r->flat += s_hi - r->off;
			r->n -= s_hi - s_lo;
		}
		if (lo == 0) {
			r->off = r->n ? (r->off > hi ? r->off - hi : 0) : 0;
			r->base += hi << RV_PAGE_SHIFT;
			r->count -= hi;
		} else {
			r->count = lo;
		}
		if (!r->n)
			r->off = 0;
		if (!r->count) {
			if (r->n)
				unmap_pages(cpu, r->flat, r->n);
			*r = proc->reserves[--proc->nreserves];
		}
	} else if (s_lo < s_hi) {
		/* Out of its middle: no hole in the table's pages, which something else could move into; pages that allow nothing. */
		map_pages(cpu, r->flat + (s_lo - r->off), s_hi - s_lo, 0);
	}
}

static uint64_t do_mmap(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot,
			uint64_t flags, int fd, uint64_t offset, int *copy)
{
	struct rv_proc *proc = cpu->proc;
	struct rv_chunk *c = NULL;
	uint64_t count, first;
	struct stat st;

	if (!len)
		return neg(RV_EINVAL);
	/* (More than half of what the page table may ever span: not by growing the table to its limit and failing then.) */
	if (len > cpu->mem_max / 2 && !(flags & (RV_MAP_FIXED | RV_MAP_FIXED_NOREPLACE)))
		return neg(RV_ENOMEM);
	if (len > cpu->mem_max)
		return neg(RV_ENOMEM);
	if (!(flags & (RV_MAP_PRIVATE | RV_MAP_SHARED)))
		return neg(RV_EINVAL);
	if (!(flags & RV_MAP_ANONYMOUS) && (offset & (RV_PAGE_SIZE - 1)))
		return neg(RV_EINVAL);
	len = RV_PAGE_ALIGN(len);
	count = len >> RV_PAGE_SHIFT;

	if (flags & (RV_MAP_FIXED | RV_MAP_FIXED_NOREPLACE)) {
		if (!range_ok(cpu, addr, len) || addr < RV_PAGE_SIZE)
			return neg((addr & (RV_PAGE_SIZE - 1)) ? RV_EINVAL : RV_ENOMEM);
		first = addr >> RV_PAGE_SHIFT;
		if ((flags & RV_MAP_FIXED_NOREPLACE) && !range_free(cpu, first, count))
			return neg(RV_EEXIST);
	} else {
		/* A hint is honoured when the whole range is free. */
		uint64_t hint = addr & ~(RV_PAGE_SIZE - 1);

		if (hint >= RV_MMAP_MIN_ADDR && range_ok(cpu, hint, len) &&
		    range_free(cpu, hint >> RV_PAGE_SHIFT, count) &&
		    !(hint >> RV_PAGE_SHIFT <= guard_page(cpu) && guard_page(cpu) < (hint >> RV_PAGE_SHIFT) + count)) {
			first = hint >> RV_PAGE_SHIFT;
		} else {
			first = find_free(cpu, count);
			if (!first)
				return neg(RV_ENOMEM);
		}
	}

	if (flags & RV_MAP_ANONYMOUS) {
		map_pages(cpu, first, count, prot);
		return first << RV_PAGE_SHIFT;
	}

	/*
	 * Shared: the file's own memory, where the host can map that (map_host_shared()).
	 * (RV_SHARE_HOST=0: as if it could not, to try what follows on a host with an MMU.)
	 */
	if (share_host < 0)
		share_host = !(getenv("RV_SHARE_HOST") && !strcmp(getenv("RV_SHARE_HOST"), "0"));
	if ((flags & RV_MAP_SHARED) && fd >= 0 && !is_display(fd) && share_host &&
	    map_host_shared(cpu, first, count, prot, fd, offset))
		return first << RV_PAGE_SHIFT;

	/*
	 * Otherwise a file mapping is a copy read in up front, or the file's
	 * shared segment below. That is what a no-MMU kernel does too. Shared
	 * writable mappings of files that are not in memory would need writing
	 * back and are not supported; the display is a matter of its own.
	 */
	if ((flags & RV_MAP_SHARED) && (prot & RV_PROT_WRITE) && fd >= 0 && !is_display(fd) &&
	    !fstat(fd, &st) && S_ISREG(st.st_mode)) {
		/* A file on a disk, to be written through the mapping: its live segment. */
		if ((fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR)
			return neg(RV_EACCES);
		if (count > LIVE_MAX_PAGES || !(c = live_attach(proc, fd, &st, offset, count)))
			return neg(RV_ENODEV);
		map_pages(cpu, first, count, prot);
		for (uint64_t k = 0; k < count; k++)
			__atomic_store_n(&cpu->map[first + k],
					 entry(first + k, prot & RV_PROT_MASK, c->host + (k << RV_PAGE_SHIFT)) | RV_PAGE_LIVE,
					 __ATOMIC_SEQ_CST);
		c->used += (uint32_t)count;
		/* (What pwrite() writes to the file goes into the segment as well: rv_mm_file_written().) */
		*copy = 1;
		return first << RV_PAGE_SHIFT;
	}
	if ((flags & RV_MAP_SHARED) && (prot & RV_PROT_WRITE)) {
		uint8_t *host;

		if (fd < 0 || !is_display(fd) || proc->fb_len || count <= POOL_RUN_MAX)
			return neg(RV_ENODEV);
		map_pages(cpu, first, count, prot);
		/* One piece of memory, which the display then shows; it starts out with what is on the screen now. */
		if (!(host = pages_alloc(proc, (uint32_t)count))) {
			unmap_pages(cpu, first, count);
			return neg(RV_ENOMEM);
		}
		for (uint64_t k = 0; k < count; k++)
			__atomic_store_n(&cpu->map[first + k],
					 entry(first + k, prot & RV_PROT_MASK, host + (k << RV_PAGE_SHIFT)), __ATOMIC_SEQ_CST);
		if (pread(fd, host, len, (off_t)offset) < 0)
			memset(host, 0, len);
		proc->fb_fd = fcntl(fd, F_DUPFD_CLOEXEC, 64);
		proc->fb_addr = first << RV_PAGE_SHIFT;
		proc->fb_len = len;
		if (proc->fb_fd < 0 || ioctl(proc->fb_fd, WASMFB_SET_PIXELS, (unsigned long)(uintptr_t)host) < 0) {
			/* Not a host whose display can do this. */
			if (proc->fb_fd >= 0)
				close(proc->fb_fd);
			proc->fb_len = 0;
			unmap_pages(cpu, first, count);
			return neg(RV_ENODEV);
		}
		return first << RV_PAGE_SHIFT;
	}
	if (fd < 0)
		return neg(RV_EBADF);

	map_pages(cpu, first, count, prot);
	/*
	 * Shared, from here on, is a copy of the file as it is now, which
	 * write() does not change, where with an MMU the mapping is the file
	 * and shows every write at once. A program that writes the file and
	 * reads it back through its mapping would read what was there before:
	 * SQLite does just that with its databases (PRAGMA mmap_size; Chromium
	 * has it on for every one), read pages from before its own last
	 * transaction, found tables missing, and Chromium threw the databases
	 * away ("Something went wrong when opening your profile", once for
	 * each). So pwrite() puts what it writes into this process's copies of
	 * the file as well (rv_mm_file_written()). Another process's copy still
	 * falls behind. (Refusing the mapping, ENODEV, as a kernel without an
	 * MMU does, is not an answer: dconf, in every GTK program, asserts that
	 * its one-byte mapping is there.)
	 */
	*copy = (flags & RV_MAP_SHARED) != 0;
	if (share_off < 0)
		share_off = getenv("RV_SHARE") && !strcmp(getenv("RV_SHARE"), "0");
	/* Only to be read (a program's or a library's code and constants): the file's shared segment, if it gets one. */
	if (!(prot & RV_PROT_WRITE) && !fstat(fd, &st) && S_ISREG(st.st_mode) &&
	    st.st_size >= SHARE_MIN && (uint64_t)st.st_size <= SHARE_MAX && !share_off &&
	    (offset >> RV_PAGE_SHIFT) < RV_PAGE_ALIGN((uint64_t)st.st_size) >> RV_PAGE_SHIFT &&
	    (c = shared_attach(proc, fd, &st))) {
		uint64_t from = offset >> RV_PAGE_SHIFT, n = c->npages - from;

		/* (Pages of the mapping beyond the file's end stay as they are: without memory, and zero if touched.) */
		if (n > count)
			n = count;
		for (uint64_t k = 0; k < n; k++)
			__atomic_store_n(&cpu->map[first + k],
					 shared_entry(first + k, prot & RV_PROT_MASK, c->host + ((from + k) << RV_PAGE_SHIFT)),
					 __ATOMIC_SEQ_CST);
		c->used += (uint32_t)n;
		return first << RV_PAGE_SHIFT;
	}
	if (rv_mm_fill(cpu, first << RV_PAGE_SHIFT, len, NULL, fd, offset)) {
		int err = errno;

		unmap_pages(cpu, first, count);
		return neg(err == ENOMEM ? RV_ENOMEM : RV_EBADF);
	}
	return first << RV_PAGE_SHIFT;
}

/* (rv_mm_mmap() below notes which file a mapping is of.) */
uint64_t rv_mm_mmap(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot,
		    uint64_t flags, int fd, uint64_t offset)
{
	uint64_t ret;

	struct rv_reserve *r;
	uint64_t at = addr;
	int copy = 0;

	rv_lock(cpu);
	/* At a fixed place in a reservation: in the pages of the table that stand for that part of it. */
	if ((flags & (RV_MAP_FIXED | RV_MAP_FIXED_NOREPLACE)) && cpu->proc->nreserves && (r = reserve_of(cpu->proc, addr))) {
		uint64_t lo = (addr - r->base) >> RV_PAGE_SHIFT, hi = lo + (RV_PAGE_ALIGN(len) >> RV_PAGE_SHIFT);

		if ((addr & (RV_PAGE_SIZE - 1)) || !len || hi > r->count || reserve_pages(cpu, &r, &lo, &hi)) {
			rv_unlock(cpu);
			return neg(RV_ENOMEM);
		}
		at = ((r->flat - r->off) << RV_PAGE_SHIFT) + (addr - r->base);
		flags = (flags & ~(uint64_t)RV_MAP_FIXED_NOREPLACE) | RV_MAP_FIXED;
	}
	ret = do_mmap(cpu, at, len, prot, flags, fd, offset, &copy);
	if (ret < (uint64_t)-4095 && !(flags & RV_MAP_ANONYMOUS) && fd >= 0)
		named_add(cpu->proc, ret >> RV_PAGE_SHIFT, RV_PAGE_ALIGN(len) >> RV_PAGE_SHIFT, fd, offset, copy);
	if (at != addr && ret < (uint64_t)-4095)
		ret = addr;
	/* Nothing, and too much of it for the page table: a reservation. */
	if (ret == neg(RV_ENOMEM) && (flags & RV_MAP_ANONYMOUS) && !(prot & RV_PROT_MASK) &&
	    !(flags & (RV_MAP_FIXED | RV_MAP_FIXED_NOREPLACE)) && len >= RESERVE_MIN && len < RESERVE_END)
		ret = reserve_new(cpu, RV_PAGE_ALIGN(len));
	rv_unlock(cpu);
	return ret;
}

uint64_t rv_mm_munmap(struct rv_cpu *cpu, uint64_t addr, uint64_t len)
{
	struct rv_reserve *r;

	rv_lock(cpu);
	if (cpu->proc->nreserves && (r = reserve_of(cpu->proc, addr))) {
		uint64_t end = addr + RV_PAGE_ALIGN(len);

		if ((addr & (RV_PAGE_SIZE - 1)) || !len) {
			rv_unlock(cpu);
			return neg(RV_EINVAL);
		}
		/* One reservation at a time: what was one may be several by now (reserve_split()). */
		while (addr < end && (r = reserve_of(cpu->proc, addr))) {
			uint64_t r_end = r->base + (r->count << RV_PAGE_SHIFT);
			uint64_t part = (r_end < end ? r_end : end) - addr;

			reserve_unmap(cpu, r, addr, part);
			addr += part;
		}
		rv_unlock(cpu);
		return 0;
	}
	if (!range_ok(cpu, addr, len)) {
		rv_unlock(cpu);
		return neg(RV_EINVAL);
	}
	unmap_pages(cpu, addr >> RV_PAGE_SHIFT, RV_PAGE_ALIGN(len) >> RV_PAGE_SHIFT);
	rv_unlock(cpu);
	return 0;
}

static uint64_t do_mprotect(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot)
{
	uint64_t first, count;
	struct code_span code = { 0, 0 };

	if (addr & (RV_PAGE_SIZE - 1))
		return neg(RV_EINVAL);
	if (!len)
		return 0;
	if (!range_ok(cpu, addr, len))
		return neg(RV_ENOMEM);

	first = addr >> RV_PAGE_SHIFT;
	count = RV_PAGE_ALIGN(len) >> RV_PAGE_SHIFT;
	for (uint64_t i = first; i < first + count; i++)
		if (!(cpu->map[i] & RV_PAGE_MAPPED))
			return neg(RV_ENOMEM);
	for (uint64_t i = first; i < first + count; i++) {
		uint64_t e = __atomic_load_n(&cpu->map[i], __ATOMIC_SEQ_CST);

		/*
		 * A page that code was translated from and that stays executable:
		 * its bytes are what they were, so the translations stay, and so
		 * does the mark that keeps it from being written as it stands,
		 * whatever the mapping now allows. (A translator may be marking the
		 * page at this moment: the entry is replaced only as it was read.)
		 */
		while ((e & RV_PAGE_CODE) && (prot & RV_PROT_EXEC)) {
			uint64_t to = (entry_as(e, i, i, prot & RV_PROT_MASK) & ~(uint64_t)RV_PROT_WRITE) | RV_PAGE_CODE;

			if (__atomic_compare_exchange_n(&cpu->map[i], &e, to, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
				break;
		}
		if ((e & RV_PAGE_CODE) && (prot & RV_PROT_EXEC))
			continue;
		set_page(cpu, i, entry_as(e, i, i, prot & RV_PROT_MASK), &code);
	}
	code_changed(cpu, &code, RV_CODE_PROTECT);
	return 0;
}

uint64_t rv_mm_mprotect(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot)
{
	struct rv_reserve *r;
	uint64_t ret;

	rv_lock(cpu);
	if (cpu->proc->nreserves && (r = reserve_of(cpu->proc, addr))) {
		uint64_t lo = (addr - r->base) >> RV_PAGE_SHIFT, hi = lo + (RV_PAGE_ALIGN(len) >> RV_PAGE_SHIFT);

		if ((addr & (RV_PAGE_SIZE - 1)) || hi > r->count) {
			ret = neg((addr & (RV_PAGE_SIZE - 1)) ? RV_EINVAL : RV_ENOMEM);
		} else if (!(prot & RV_PROT_MASK)) {
			/* To nothing again: only where there are pages; elsewhere it is nothing already. */
			uint64_t s_lo = lo > r->off ? lo : r->off, s_hi = hi < r->off + r->n ? hi : r->off + r->n;

			ret = s_lo < s_hi ? do_mprotect(cpu, (r->flat + (s_lo - r->off)) << RV_PAGE_SHIFT,
							(s_hi - s_lo) << RV_PAGE_SHIFT, prot) : 0;
		} else if (lo == hi) {
			ret = 0;
		} else if (reserve_pages(cpu, &r, &lo, &hi)) {
			ret = neg(RV_ENOMEM);
		} else {
			ret = do_mprotect(cpu, (r->flat + (lo - r->off)) << RV_PAGE_SHIFT, (hi - lo) << RV_PAGE_SHIFT, prot);
		}
		rv_unlock(cpu);
		return ret;
	}
	ret = do_mprotect(cpu, addr, len, prot);
	rv_unlock(cpu);
	return ret;
}

static uint64_t do_mremap(struct rv_cpu *cpu, uint64_t old_addr, uint64_t old_len,
			  uint64_t new_len, uint64_t flags)
{
	uint64_t old_first, old_count, new_count, first;
	unsigned prot;
	struct code_span code = { 0, 0 };

	if (!range_ok(cpu, old_addr, old_len) || !new_len)
		return neg(RV_EINVAL);
	if (new_len > cpu->mem_max)
		return neg(RV_ENOMEM);
	if (flags & ~(uint64_t)RV_MREMAP_MAYMOVE)
		return neg(RV_EINVAL);	/* MREMAP_FIXED and friends: not yet */

	old_first = old_addr >> RV_PAGE_SHIFT;
	old_count = RV_PAGE_ALIGN(old_len) >> RV_PAGE_SHIFT;
	new_count = RV_PAGE_ALIGN(new_len) >> RV_PAGE_SHIFT;

	for (uint64_t i = old_first; i < old_first + old_count; i++)
		if (!(cpu->map[i] & RV_PAGE_MAPPED))
			return neg(RV_EFAULT);
	prot = want_of(cpu->map[old_first]);

	if (new_count <= old_count) {
		unmap_pages(cpu, old_first + new_count, old_count - new_count);
		return old_addr;
	}

	/*
	 * A mapping of a file that grows would show more of the file, from
	 * where the mapping ends; what follows makes the new pages anonymous,
	 * zero. SQLite maps its databases to read them (Chromium's every one,
	 * PRAGMA mmap_size) and grows the mapping with mremap() as the file
	 * grows: it read zeros for every page past the first mapping's end, and
	 * found the database malformed. A file's mapping cannot grow, then:
	 * ENOMEM, which callers take as "map it again" (SQLite does).
	 */
	for (uint32_t k = 0; k < cpu->proc->nnamed; k++) {
		const struct rv_named *n = &cpu->proc->named[k];

		if (old_first < n->first + n->count && n->first < old_first + old_count)
			return neg(RV_ENOMEM);
	}

	/* Grow in place when the pages behind the mapping are free. */
	if (old_first + new_count <= cpu->npages &&
	    !(old_first <= guard_page(cpu) && guard_page(cpu) < old_first + new_count) &&
	    range_free(cpu, old_first + old_count, new_count - old_count)) {
		map_pages(cpu, old_first + old_count, new_count - old_count, prot);
		return old_addr;
	}

	if (!(flags & RV_MREMAP_MAYMOVE))
		return neg(RV_ENOMEM);
	first = find_free(cpu, new_count);
	if (!first)
		return neg(RV_ENOMEM);
	/* The pages get new addresses and stay where they are: nothing is copied. */
	for (uint64_t k = 0; k < old_count; k++) {
		uint64_t old = set_page(cpu, old_first + k, 0, &code);

		__atomic_store_n(&cpu->map[first + k], entry_as(old, old_first + k, first + k, want_of(old)),
				 __ATOMIC_SEQ_CST);
	}
	map_pages(cpu, first + old_count, new_count - old_count, prot);
	code_changed(cpu, &code, RV_CODE_UNMAP);
	return first << RV_PAGE_SHIFT;
}

uint64_t rv_mm_mremap(struct rv_cpu *cpu, uint64_t old_addr, uint64_t old_len,
		      uint64_t new_len, uint64_t flags)
{
	uint64_t ret;

	rv_lock(cpu);
	ret = do_mremap(cpu, old_addr, old_len, new_len, flags);
	rv_unlock(cpu);
	return ret;
}

/*
 * `len` bytes were written to the file `fd` at `offset` (pwrite()). Where
 * this process has that part of the file mapped shared as a copy
 * (do_mmap()), the copy gets them too, read back from the file, as the
 * mapping would show them with an MMU.
 */
void rv_mm_file_written(struct rv_cpu *cpu, int fd, uint64_t len, uint64_t offset)
{
	struct rv_proc *proc = cpu->proc;
	struct stat st;

	if (!proc->file_copies || !len || fstat(fd, &st) || !S_ISREG(st.st_mode))
		return;
	rv_lock(cpu);
	for (uint32_t k = 0; k < proc->nnamed; k++) {
		const struct rv_named *n = &proc->named[k];
		uint64_t lo = offset, hi = offset + len;

		if (!n->copy || n->dev != (uint64_t)st.st_dev || n->ino != (uint64_t)st.st_ino)
			continue;
		if (lo < n->offset)
			lo = n->offset;
		if (hi > n->offset + (n->count << RV_PAGE_SHIFT))
			hi = n->offset + (n->count << RV_PAGE_SHIFT);
		if (lo < hi)
			rv_mm_fill(cpu, (n->first << RV_PAGE_SHIFT) + (lo - n->offset), hi - lo, NULL, fd, lo);
	}
	rv_unlock(cpu);
}

/*
 * brk(): returns the new break on success and the current one on failure,
 * which is also how brk(0) queries it.
 */
static uint64_t do_brk(struct rv_cpu *cpu, uint64_t addr)
{
	struct rv_proc *proc = cpu->proc;
	uint64_t old_end = RV_PAGE_ALIGN(proc->brk) >> RV_PAGE_SHIFT;
	uint64_t new_end;

	if (addr < cpu->brk_start || addr > cpu->mmap_top)
		return proc->brk;
	new_end = RV_PAGE_ALIGN(addr) >> RV_PAGE_SHIFT;

	if (new_end > old_end) {
		if (!range_free(cpu, old_end, new_end - old_end))
			return proc->brk;
		map_pages(cpu, old_end, new_end - old_end, RV_PROT_READ | RV_PROT_WRITE);
	} else {
		unmap_pages(cpu, new_end, old_end - new_end);
	}
	proc->brk = addr;
	return proc->brk;
}

uint64_t rv_mm_brk(struct rv_cpu *cpu, uint64_t addr)
{
	uint64_t ret;

	rv_lock(cpu);
	ret = do_brk(cpu, addr);
	rv_unlock(cpu);
	return ret;
}

/* ------------------------------------------------------------ the accesses */

/* Record a fault at addr; hands back scratch space so that the caller can finish its instruction. */
uint8_t *rv_fault(struct rv_cpu *cpu, uint64_t addr)
{
	if (cpu->trap == RV_TRAP_NONE) {
		cpu->trap = RV_TRAP_SEGV;
		cpu->trap_addr = addr;
	}
	return cpu->scratch;
}

/* May the page be accessed like that, by its mapping? */
static int allowed(const struct rv_cpu *cpu, uint64_t page, unsigned prot)
{
	uint64_t e = cpu->map[page];

	return (e & RV_PAGE_MAPPED) && (want_of(e) & prot) == prot;
}

/*
 * A page in a file's shared segment is to be written to (its mapping came to
 * allow that later, or the loader has something to put there): it gets
 * memory of its own, with the same contents. With the lock. -1: no memory.
 */
static int page_private(struct rv_cpu *cpu, uint64_t page)
{
	uint64_t e = cpu->map[page];
	uint8_t *old = host_of(e, page), *host;
	struct code_span code = { 0, 0 };

	if (!(e & RV_PAGE_SHARED))
		return 0;
	if (!(host = pages_alloc(cpu->proc, 1)))
		return -1;
	memcpy(host, old, RV_PAGE_SIZE);
	set_page(cpu, page, entry(page, want_of(e), host), &code);
	page_free(cpu->proc, old);
	/* (It is about to be written: that is all the translator needs to know.) */
	code_changed(cpu, &code, RV_CODE_WRITE);
	return 0;
}

/*
 * A page that code was translated from is to be written to, which its
 * mapping allows and its mark does not: the mark goes, the page allows what
 * its mapping does again, and the translator is told, in that order, before
 * anything is written. With the lock. (A thread that translates from the
 * page again after this marks it again, and compares what it has from before
 * with what the page holds then.)
 */
static void code_writable(struct rv_cpu *cpu, uint64_t page)
{
	struct code_span code = { 0, 0 };
	uint64_t e = cpu->map[page];

	if (!(e & RV_PAGE_CODE))
		return;
	set_page(cpu, page, entry_as(e, page, page, want_of(e)), &code);
	code_changed(cpu, &code, RV_CODE_WRITE);
}

/*
 * Make the pages [first, last] ready for an access their mappings allow:
 * memory for those that have none, and for a write, memory of their own for
 * those that are in a file's shared segment, and the translator told for
 * those that code was translated from. Takes the lock for it.
 * `around`: a page touched on its own, not a buffer handed to a system
 * call; its neighbours are likely next, so they come along. -1: there is no
 * memory.
 */
static int touch(struct rv_cpu *cpu, uint64_t first, uint64_t last, unsigned prot, int around)
{
	uint64_t end = last + 1;
	int ret = 0, locked = 0;

	for (uint64_t i = first; i <= last && !ret; i++) {
		uint64_t e = cpu->map[i];

		if ((e & RV_PAGE_BACKED) && !((prot & RV_PROT_WRITE) && (e & (RV_PAGE_SHARED | RV_PAGE_CODE))))
			continue;
		if (!locked) {
			rv_lock(cpu);
			locked = 1;
			e = cpu->map[i];
		}
		if ((prot & RV_PROT_WRITE) && (e & RV_PAGE_CODE) && !(e & RV_PAGE_SHARED)) {
			code_writable(cpu, i);
		} else if (!(e & RV_PAGE_BACKED)) {
			uint64_t from = i;

			/*
			 * The whole block of LAZY_RUN pages the page is in, as far as it
			 * is mapped and without memory: the same block whether the guest
			 * works its way up (a heap) or down (a stack), so that what it
			 * touches next to each other mostly is next to each other.
			 */
			if (around) {
				uint64_t low = i & ~(uint64_t)(LAZY_RUN - 1), high = low + LAZY_RUN;

				while (from > low && (cpu->map[from - 1] & (RV_PAGE_MAPPED | RV_PAGE_BACKED)) == RV_PAGE_MAPPED)
					from--;
				while (end < high && end < cpu->npages &&
				       (cpu->map[end] & (RV_PAGE_MAPPED | RV_PAGE_BACKED)) == RV_PAGE_MAPPED)
					end++;
			}
			ret = back_pages(cpu, from, end - from);
		} else if ((prot & RV_PROT_WRITE) && (e & RV_PAGE_SHARED)) {
			ret = page_private(cpu, i);
		}
	}
	if (locked)
		rv_unlock(cpu);
	return ret;
}

/* Copy between the guest's pages, which have memory, and a buffer of ours, page by page. */
static void copy_guest(struct rv_cpu *cpu, uint64_t addr, uint8_t *buf, uint64_t len, int to_guest)
{
	if (__builtin_expect((addr >> RV_PAGE_SHIFT) >= cpu->npages, 0)) {
		int inside;

		/* (In a reservation, or the caller would not have come this far.) */
		rv_lock(cpu);
		addr = reserve_flat(cpu->proc, addr, len, &inside);
		rv_unlock(cpu);
		if (!addr)
			return;
	}
	while (len) {
		uint64_t n = RV_PAGE_SIZE - (addr & (RV_PAGE_SIZE - 1));
		uint8_t *host = (uint8_t *)(uintptr_t)(addr + (cpu->map[addr >> RV_PAGE_SHIFT] & ~FLAGS));

		if (n > len)
			n = len;
		if (to_guest)
			memcpy(host, buf, n);
		else
			memcpy(buf, host, n);
		addr += n;
		buf += n;
		len -= n;
	}
}

/*
 * What rv_ptr() (rv.h) does not do inline: an access that its page does not
 * allow as it stands. Either the page has no memory yet and gets some, or
 * the access is in two pages, or it is a fault.
 *
 * Two pages that are not next to each other for the host: a load gets the
 * bytes collected in scratch space; a store is made to scratch space and
 * taken to the pages by rv_store_split(), which the caller calls when it
 * finds that scratch space is what it got.
 */
uint8_t *rv_ptr_slow(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot)
{
	uint64_t first = addr >> RV_PAGE_SHIFT, last = (addr + len - 1) >> RV_PAGE_SHIFT;

	if (__builtin_expect(first >= cpu->npages && cpu->proc->nreserves, 0)) {
		uint64_t flat;
		uint8_t *p;
		int inside;

		/* In a reservation: as the access to the page of the table that stands for the page there, if one does. */
		if (first == last) {
			uint64_t gen;

			flat = reserve_seen_flat(cpu, addr, &gen);
			flat = flat ? flat + (addr & (RV_PAGE_SIZE - 1)) : 0;
		} else {
			rv_lock(cpu);
			flat = reserve_flat(cpu->proc, addr, len, &inside);
			rv_unlock(cpu);
		}
		if (!flat)
			return rv_fault(cpu, addr);
		for (int again = 0;; again++) {
			int trapped = cpu->trap != RV_TRAP_NONE;
			uint64_t now;

			p = rv_ptr(cpu, flat, len, prot);
			if (trapped || cpu->trap != RV_TRAP_SEGV || cpu->trap_addr != flat)
				return p;
			/*
			 * A fault, unless another thread was moving the reservation's
			 * entries in the table just then (reserve_cover()), which it
			 * does under the lock, emptying the old ones as it goes: where
			 * the page is now, with the lock taken, says which.
			 */
			rv_lock(cpu);
			now = reserve_flat(cpu->proc, addr, len, &inside);
			rv_unlock(cpu);
			if (!now || now == flat || again == 3) {
				cpu->trap_addr = addr;	/* what the program knows the place as */
				return p;
			}
			cpu->trap = RV_TRAP_NONE;
			flat = now;
		}
	}
	if (last >= cpu->npages || first > last || !allowed(cpu, first, prot) || !allowed(cpu, last, prot))
		return rv_fault(cpu, addr);
	if (touch(cpu, first, last, prot, 1)) {
		fprintf(stderr, "rv: out of memory (the guest has %llu MiB and touched a page at 0x%llx that had none yet)\n",
			(unsigned long long)(cpu->proc->host_pages >> (20 - RV_PAGE_SHIFT)), (unsigned long long)addr);
		return rv_fault(cpu, addr);
	}
	if ((prot & RV_PROT_WRITE) && cpu->undo_on)
		rv_undo_note(cpu, addr, len);
	if (first == last || (cpu->map[first] & ~FLAGS) == (cpu->map[last] & ~FLAGS))
		return (uint8_t *)(uintptr_t)(addr + (cpu->map[first] & ~FLAGS));
	if (!(prot & RV_PROT_WRITE))
		copy_guest(cpu, addr, cpu->scratch, len, 0);
	return cpu->scratch;
}

/* The second half of a store that rv_ptr() answered with scratch space (see rv_ptr_slow()). */
void rv_store_split(struct rv_cpu *cpu, uint64_t addr, uint64_t len)
{
	if (cpu->trap == RV_TRAP_NONE)
		copy_guest(cpu, addr, cpu->scratch, len, 1);
}

/* The host address of a guest address in a page that has memory. */
uint8_t *rv_host_addr(struct rv_cpu *cpu, uint64_t addr)
{
	return (uint8_t *)(uintptr_t)(addr + (cpu->map[addr >> RV_PAGE_SHIFT] & ~FLAGS));
}

/* Are all pages of the range mapped and in memory? */
static int in_memory(struct rv_cpu *cpu, uint64_t addr, uint64_t len)
{
	uint64_t last = (addr + len - 1) >> RV_PAGE_SHIFT;

	if (last >= cpu->npages)
		return 0;
	for (uint64_t i = addr >> RV_PAGE_SHIFT; i <= last; i++)
		if (!(cpu->map[i] & RV_PAGE_BACKED))
			return 0;
	return 1;
}

/*
 * A system call's buffer: `len` bytes of the guest's at `addr`, to be read
 * or written by the host as `prot` says. NULL when a page lacks the access.
 * Otherwise an address the host can use. That is the guest's memory itself
 * if the pages lie in one piece. If they do not, it is a copy, and
 * rv_user_done() takes what the host wrote in it back to the guest's pages;
 * every path that may have called this ends there (rv_syscall() does it for
 * system calls).
 */
void *rv_user_ptr(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot)
{
	uint64_t first, last, delta;
	struct rv_bounce *b;

	if (!len)
		return cpu->scratch;
	if (__builtin_expect((addr >> RV_PAGE_SHIFT) >= cpu->npages && cpu->proc->nreserves, 0)) {
		int inside;

		rv_lock(cpu);
		addr = reserve_flat(cpu->proc, addr, len, &inside);
		rv_unlock(cpu);
		if (!addr)
			return NULL;
	}
	first = addr >> RV_PAGE_SHIFT;
	last = (addr + len - 1) >> RV_PAGE_SHIFT;
	if (addr + len < addr || last >= cpu->npages)
		return NULL;
	for (uint64_t i = first; i <= last; i++)
		if (!allowed(cpu, i, prot))
			return NULL;
	if (touch(cpu, first, last, prot, 0) || !in_memory(cpu, addr, len))
		return NULL;

	delta = cpu->map[first] & ~FLAGS;
	for (uint64_t i = first + 1; i <= last; i++)
		if ((cpu->map[i] & ~FLAGS) != delta)
			goto copy;
	return (uint8_t *)(uintptr_t)(addr + delta);

copy:
	if (cpu->nbounces == cpu->bounces_cap) {
		int cap = cpu->bounces_cap ? cpu->bounces_cap * 2 : 8;

		if (!(b = realloc(cpu->bounces, (size_t)cap * sizeof(*b))))
			return NULL;
		cpu->bounces = b;
		cpu->bounces_cap = cap;
	}
	b = &cpu->bounces[cpu->nbounces];
	if (!(b->buf = malloc(len)))
		return NULL;
	b->addr = addr;
	b->len = len;
	b->write = (prot & RV_PROT_WRITE) != 0;
	/* What is there goes in whatever the access: the host may write only part of it. */
	copy_guest(cpu, addr, b->buf, len, 0);
	cpu->nbounces++;
	return b->buf;
}

/*
 * The same buffer as the pieces it lies in, for a system call that takes a
 * list of them (readv() and the like): nothing is copied, however the
 * guest's pages are spread over the host's memory, and no memory of the
 * buffer's size is needed in one piece (which a host without an MMU may not
 * have when the buffer is large). Fills `iov` with at most `max` pieces
 * that cover the buffer from its start, and says in *covered how many bytes
 * those are: all of it, unless the pieces ran out. Returns the number of
 * pieces, or -1 when a page lacks the access.
 */
int rv_user_iov(struct rv_cpu *cpu, uint64_t addr, uint64_t len, unsigned prot,
		struct iovec *iov, int max, uint64_t *covered)
{
	uint64_t first, last, at, end;
	int n = 0;

	*covered = 0;
	if (!len)
		return 0;
	if (__builtin_expect((addr >> RV_PAGE_SHIFT) >= cpu->npages && cpu->proc->nreserves, 0)) {
		int inside;

		rv_lock(cpu);
		addr = reserve_flat(cpu->proc, addr, len, &inside);
		rv_unlock(cpu);
		if (!addr)
			return -1;
	}
	at = addr;
	end = addr + len;
	first = addr >> RV_PAGE_SHIFT;
	last = (end - 1) >> RV_PAGE_SHIFT;
	if (end < addr || last >= cpu->npages)
		return -1;
	for (uint64_t i = first; i <= last; i++)
		if (!allowed(cpu, i, prot))
			return -1;
	if (touch(cpu, first, last, prot, 0) || !in_memory(cpu, addr, len))
		return -1;
	while (at < end && n < max) {
		uint64_t page = at >> RV_PAGE_SHIFT, delta = cpu->map[page] & ~FLAGS, next = (page + 1) << RV_PAGE_SHIFT;

		while (next < end && (cpu->map[next >> RV_PAGE_SHIFT] & ~FLAGS) == delta)
			next += RV_PAGE_SIZE;
		if (next > end)
			next = end;
		iov[n].iov_base = (void *)(uintptr_t)(at + delta);
		iov[n].iov_len = (size_t)(next - at);
		n++;
		at = next;
	}
	*covered = at - addr;
	return n;
}

/* The host is done with what rv_user_ptr() gave out. */
void rv_user_done(struct rv_cpu *cpu)
{
	for (int i = 0; i < cpu->nbounces; i++) {
		struct rv_bounce *b = &cpu->bounces[i];

		/* (Unless another thread unmapped it under the system call: then there is nowhere to put it.) */
		if (b->write && in_memory(cpu, b->addr, b->len))
			copy_guest(cpu, b->addr, b->buf, b->len, 1);
		free(b->buf);
	}
	cpu->nbounces = 0;
}

/* Length of a NUL-terminated guest string including the NUL, or 0 if it runs into memory that cannot be read. */
uint64_t rv_user_strlen(struct rv_cpu *cpu, uint64_t addr)
{
	uint64_t a = addr;

	for (;;) {
		uint64_t page = a >> RV_PAGE_SHIFT, n = RV_PAGE_SIZE - (a & (RV_PAGE_SIZE - 1));
		const uint8_t *p, *end;

		if (__builtin_expect(page >= cpu->npages && cpu->proc->nreserves, 0)) {
			uint64_t flat;
			int inside;

			rv_lock(cpu);
			flat = reserve_flat(cpu->proc, a, 1, &inside);
			rv_unlock(cpu);
			if (!flat || !allowed(cpu, flat >> RV_PAGE_SHIFT, RV_PROT_READ) ||
			    touch(cpu, flat >> RV_PAGE_SHIFT, flat >> RV_PAGE_SHIFT, RV_PROT_READ, 0) ||
			    !(cpu->map[flat >> RV_PAGE_SHIFT] & RV_PAGE_BACKED))
				return 0;
			p = rv_host_addr(cpu, flat);
			if ((end = memchr(p, 0, n)))
				return a + (uint64_t)(end - p) - addr + 1;
			a += n;
			continue;
		}
		if (page >= cpu->npages || !allowed(cpu, page, RV_PROT_READ) || touch(cpu, page, page, RV_PROT_READ, 0) ||
		    !(cpu->map[page] & RV_PAGE_BACKED))
			return 0;
		p = rv_host_addr(cpu, a);
		if ((end = memchr(p, 0, n)))
			return a + (uint64_t)(end - p) - addr + 1;
		a += n;
	}
}

/*
 * The process is ending: let go of the files' shared segments, so that those
 * nobody else has are removed. (A process that is killed does not get here.
 * What it had attached stays, and is still good for the next process that
 * maps the file; that one, or any other user that ends properly, notices
 * that the dead one is gone.)
 */
void rv_mm_exit(struct rv_cpu *cpu)
{
	struct rv_proc *proc = cpu->proc;

	dying = NULL;
	for (uint32_t i = proc->nchunks; i > 0; i--)
		if (i <= proc->nchunks && proc->chunks[i - 1]->shm_base)
			chunk_free(proc, proc->chunks[i - 1]);
}

/*
 * madvise(). Most advice is about speed and can be left alone. One piece is
 * a promise: after MADV_DONTNEED, memory that is the process's own and no
 * file's reads as zeros again. Allocators give such memory out as zeroed
 * without looking (Firefox's does, and Go's), so the pages have to lose what
 * they held: they go back to having no memory, like pages never touched.
 * (Pages of a file's mapping would show the file again; those are left as
 * they are, as are shared ones, which keep their contents by the rule.)
 */
uint64_t rv_mm_madvise(struct rv_cpu *cpu, uint64_t addr, uint64_t len, int advice)
{
	struct rv_proc *proc = cpu->proc;
	uint64_t i, end, named_from = 0, named_end = 0, clear_until = 0;
	struct code_span code = { 0, 0 };

	if (addr & (RV_PAGE_SIZE - 1))
		return neg(RV_EINVAL);
	if (advice != 4 || !len)	/* MADV_DONTNEED */
		return 0;
	rv_lock(cpu);
	if (proc->nreserves) {
		struct rv_reserve *r = reserve_of(proc, addr);

		if (r) {
			/* Of a reservation, the part that has pages. */
			uint64_t lo = (addr - r->base) >> RV_PAGE_SHIFT, hi = lo + (RV_PAGE_ALIGN(len) >> RV_PAGE_SHIFT);
			uint64_t s_lo = lo > r->off ? lo : r->off, s_hi = hi < r->off + r->n ? hi : r->off + r->n;

			if (s_lo >= s_hi) {
				rv_unlock(cpu);
				return 0;
			}
			addr = (r->flat + (s_lo - r->off)) << RV_PAGE_SHIFT;
			len = (s_hi - s_lo) << RV_PAGE_SHIFT;
		}
	}
	if (!range_ok(cpu, addr, len)) {
		rv_unlock(cpu);
		return neg(RV_ENOMEM);
	}
	end = (addr + RV_PAGE_ALIGN(len)) >> RV_PAGE_SHIFT;
	for (i = addr >> RV_PAGE_SHIFT; i < end; i++) {
		uint64_t e = cpu->map[i], old;
		struct rv_chunk *c;

		if ((e & (RV_PAGE_MAPPED | RV_PAGE_BACKED)) != (RV_PAGE_MAPPED | RV_PAGE_BACKED) ||
		    (e & (RV_PAGE_SHARED | RV_PAGE_LIVE)))
			continue;
		/* In a file's mapping? (Looked up once per stretch of pages, not per page.) */
		if (i >= clear_until && !(i >= named_from && i < named_end)) {
			named_from = named_end = 0;
			clear_until = end;
			for (uint32_t k = 0; k < proc->nnamed; k++) {
				const struct rv_named *n = &proc->named[k];

				if (i >= n->first && i < n->first + n->count) {
					named_from = n->first;
					named_end = n->first + n->count;
					clear_until = 0;
					break;
				}
				if (n->first > i && n->first < clear_until)
					clear_until = n->first;
			}
		}
		if (i >= named_from && i < named_end)
			continue;
		if (proc->fb_len && (i << RV_PAGE_SHIFT) >= proc->fb_addr && (i << RV_PAGE_SHIFT) < proc->fb_addr + proc->fb_len)
			continue;
		c = chunk_of(proc, host_of(e, i));
		if (c && (c->file || c->shm_base))
			continue;
		old = set_page(cpu, i, entry(i, want_of(e), NULL), &code);
		page_free(proc, host_of(old, i));
	}
	code_changed(cpu, &code, RV_CODE_UNMAP);
	rv_unlock(cpu);
	return 0;
}

/*
 * riscv_flush_icache(): the program says that it has changed the code in
 * [start, end) and that every thread is to run the new code from now on.
 * Mostly there is nothing left to do: its writes were seen as they were made
 * (code_writable()). What is left is what could not be seen: a write whose
 * address a thread already had when another marked the page, one made by the
 * host for a system call that was under way, one made through a second
 * mapping of the same memory. So the pages of the range that code was
 * translated from lose their marks, and the translator is told, which makes
 * every thread compare what it translated with what is there now before it
 * runs it again. (The kernel flushes for the whole address space whatever the
 * range; here the range counts, unless it is empty, which means everything.)
 */
uint64_t rv_mm_flush_icache(struct rv_cpu *cpu, uint64_t start, uint64_t end)
{
	struct code_span code = { 0, 0 };
	uint64_t first = 0, last;
	int all = end <= start;

	/*
	 * A compiler flushes after everything it emits, and mostly none of the
	 * pages is marked (the write took the mark off): a look without the
	 * lock settles that. (Addresses beyond the table are a reservation's,
	 * or nobody's.)
	 */
	if (!all && (end - 1) >> RV_PAGE_SHIFT < cpu->npages) {
		int marked = 0;

		for (uint64_t i = start >> RV_PAGE_SHIFT; i <= (end - 1) >> RV_PAGE_SHIFT && !marked; i++)
			marked = (__atomic_load_n(&cpu->map[i], __ATOMIC_SEQ_CST) & RV_PAGE_CODE) != 0;
		if (!marked)
			return 0;
	}
	rv_lock(cpu);
	last = cpu->npages - 1;
	if (!all) {
		int inside = 0;
		uint64_t flat = cpu->proc->nreserves ? reserve_flat(cpu->proc, start, end - start, &inside) : 0;

		/* (In a reservation: its pages of the table, if it has those. Nothing is translated at its own addresses.) */
		if (inside) {
			end = flat + (end - start);
			start = flat;
		}
		first = start >> RV_PAGE_SHIFT;
		if ((end - 1) >> RV_PAGE_SHIFT < last)
			last = (end - 1) >> RV_PAGE_SHIFT;
		if (inside && !flat)
			first = last + 1;
	}
	for (uint64_t i = first; i <= last; i++) {
		uint64_t e = cpu->map[i];

		if (e & RV_PAGE_CODE)
			set_page(cpu, i, entry_as(e, i, i, want_of(e)), &code);
	}
	code_changed(cpu, &code, RV_CODE_FLUSH);
	rv_unlock(cpu);
	return 0;
}

/*
 * msync(): the live segments that have pages in the range go to their files.
 * (All of each: there is no telling which of its pages were written.)
 */
uint64_t rv_mm_msync(struct rv_cpu *cpu, uint64_t addr, uint64_t len)
{
	struct rv_chunk *done = NULL;

	if (addr & (RV_PAGE_SIZE - 1))
		return neg(RV_EINVAL);
	rv_lock(cpu);
	if (!range_ok(cpu, addr, len)) {
		rv_unlock(cpu);
		return len ? neg(RV_ENOMEM) : 0;
	}
	for (uint64_t i = addr >> RV_PAGE_SHIFT; i < (addr + RV_PAGE_ALIGN(len)) >> RV_PAGE_SHIFT; i++) {
		uint64_t e = cpu->map[i];
		struct rv_chunk *c;

		if (!(e & RV_PAGE_LIVE) || !(c = chunk_of(cpu->proc, host_of(e, i))) || c == done)
			continue;
		live_sync(c);
		done = c;
	}
	rv_unlock(cpu);
	return 0;
}

/*
 * In the child of a fork that the host did (it has the parent's memory, and
 * the segments attached as the parent had): it is one more process that has
 * them.
 */
void rv_mm_after_fork(struct rv_cpu *cpu, int apart)
{
	struct rv_proc *proc = cpu->proc;

	/* `apart`: in a namespace of process ids of its own, where the lists mean nothing. It keeps what it has attached and is on no list. */
	if (apart) {
		pidns_known = 0;
		for (uint32_t i = 0; i < proc->nchunks; i++) {
			struct rv_chunk *c = proc->chunks[i];

			if (!c->shm_base)
				continue;
			/* (Ours from now on like memory of our own: not to be let go of as a segment.) */
			if (c->live && live_fd(c))
				close(c->fd);
			c->host = c->shm_base + RV_PAGE_SIZE;
			c->shm_base = NULL;
			c->live = 0;
			c->file = 1;
		}
		return;
	}
	for (uint32_t i = 0; i < proc->nchunks; i++)
		if (proc->chunks[i]->shm_base)
			shared_join((struct shared_head *)(void *)proc->chunks[i]->shm_base);
}

/*
 * The same when the process is about to die of a signal, in whichever
 * thread, the others still running: what must not be left behind is seen
 * to, and nothing is taken apart. A live segment goes to its file; a
 * segment nobody else has is removed (the kernel keeps it until the process
 * is gone, or frees it now: either way nobody will look at it again).
 */
void rv_mm_die(void)
{
	struct rv_proc *proc = dying;

	if (!proc)
		return;
	for (uint32_t i = 0; i < proc->nchunks; i++) {
		struct rv_chunk *c = proc->chunks[i];

		if (!c->shm_base)
			continue;
		if (c->live)
			live_sync(c);
		if (shared_leave((struct shared_head *)(void *)c->shm_base))
			shmctl(c->shmid, IPC_RMID, NULL);
	}
}

/* ------------------------------------------------------------------- fork */

/*
 * In the child of a fork that started a second emulator (fork.c): cpu->map
 * is a copy of the parent's page table, entries and all. Every page that has
 * memory there gets memory of the child's own with the same contents, which
 * `copy` fetches from the parent by its host address; pages without stay
 * without. Returns 0, or -1.
 */
int rv_mm_fork_child(struct rv_cpu *cpu, int (*copy)(void *dst, const void *src, size_t len), int apart)
{
	struct rv_proc *proc = cpu->proc;
	struct rv_chunk **theirs = proc->chunks, *shared = NULL, *last = NULL;
	uint32_t ntheirs = proc->nchunks, nshared = 0;
	uint64_t i = 0;

	/* The list of which mappings are of which files: a copy of the parent's. */
	{
		struct rv_named *theirs_named = proc->named;
		uint32_t n = proc->nnamed;

		proc->named = NULL;
		proc->nnamed = proc->named_cap = 0;
		if (n && (proc->named = malloc((n + 2) * sizeof(*proc->named)))) {
			if (copy(proc->named, theirs_named, n * sizeof(*proc->named))) {
				free(proc->named);
				proc->named = NULL;
			} else {
				proc->nnamed = n;
				proc->named_cap = n + 2;
				for (uint32_t k = 0; k < n; k++)
					proc->file_copies |= proc->named[k].copy;
			}
		}
	}

	/* The parent's host memory is not ours. */
	proc->chunks = NULL;
	proc->nchunks = proc->chunks_cap = proc->pool_hint = proc->pool_chunks = 0;
	proc->host_pages = proc->shared_pages = 0;

	/*
	 * Except for the files' shared segments it has attached: those we
	 * attach too. `shared` has the parent's description of each, with ours
	 * (where we have it attached) in shm_base.
	 */
	/* (`apart`: not for a child in a namespace of process ids of its own, see pidns(). It gets copies.) */
	if (!apart && ntheirs && (shared = malloc(ntheirs * sizeof(*shared)))) {
		struct rv_chunk **list = malloc(ntheirs * sizeof(*list));

		if (list && !copy(list, theirs, ntheirs * sizeof(*list))) {
			for (uint32_t k = 0; k < ntheirs; k++) {
				struct rv_chunk *c = &shared[nshared], *mine;
				uint8_t *base;

				if (copy(c, list[k], sizeof(*c)) || !c->shm_base)
					continue;
				base = shmat(c->shmid, NULL, 0);
				if (base == (uint8_t *)-1 || chunk_room(proc) || !(mine = calloc(1, sizeof(*mine)))) {
					if (base != (uint8_t *)-1)
						shmdt(base);
					continue;	/* (its pages are copied, like any others) */
				}
				shared_join((struct shared_head *)(void *)base);
				mine->host = base + RV_PAGE_SIZE;
				mine->npages = c->npages;
				mine->shm_base = base;
				mine->shmid = c->shmid;
				mine->key = c->key;
				mine->live = c->live;
				mine->fd = c->fd;	/* (inherited, under the same number) */
				mine->offset = c->offset;
				chunk_add(proc, mine);
				c->shm_base = (uint8_t *)mine;	/* ours, for the pages below */
				nshared++;
			}
		}
		free(list);
	}

	while (i < cpu->npages) {
		uint64_t e = cpu->map[i], run = 0;
		uint8_t *host;

		if (!(e & RV_PAGE_BACKED)) {
			cpu->map[i] &= ~(uint64_t)RV_PAGE_CODE;
			i++;
			continue;
		}
		if (e & (RV_PAGE_SHARED | RV_PAGE_LIVE)) {
			uint8_t *at = host_of(e, i);

			/* In which segment? (Mostly the one the page before was in.) */
			if (!last || at < last->host || at >= last->host + ((uint64_t)last->npages << RV_PAGE_SHIFT)) {
				last = NULL;
				for (uint32_t k = 0; k < nshared; k++)
					if (at >= shared[k].host && at < shared[k].host + ((uint64_t)shared[k].npages << RV_PAGE_SHIFT))
						last = &shared[k];
			}
			if (last) {
				struct rv_chunk *mine = (struct rv_chunk *)(void *)last->shm_base;

				cpu->map[i] = e & RV_PAGE_LIVE
					? entry(i, want_of(e), mine->host + (at - last->host)) | RV_PAGE_LIVE
					: shared_entry(i, want_of(e), mine->host + (at - last->host));
				mine->used++;
				i++;
				continue;
			}
			/* A segment we could not attach: a copy of the page, of our own. */
		}
		/* As long a run as lies in one piece in the parent. */
		while (i + run < cpu->npages && (cpu->map[i + run] & RV_PAGE_BACKED) &&
		       (cpu->map[i + run] & ~FLAGS) == (e & ~FLAGS) &&
		       !((cpu->map[i + run] ^ e) & (RV_PAGE_SHARED | RV_PAGE_LIVE)))
			run++;
		while (!(host = pages_alloc(proc, (uint32_t)run))) {
			if (run == 1) {
				free(shared);
				return -1;
			}
			run = (run + 1) / 2;
		}
		if (copy(host, host_of(e, i), run << RV_PAGE_SHIFT)) {
			free(shared);
			return -1;
		}
		for (uint64_t k = 0; k < run; k++)
			cpu->map[i + k] = entry(i + k, want_of(cpu->map[i + k]), host + (k << RV_PAGE_SHIFT));
		i += run;
	}
	/* Segments none of whose pages the guest still had mapped. */
	for (uint32_t k = 0; k < nshared; k++) {
		struct rv_chunk *mine = (struct rv_chunk *)(void *)shared[k].shm_base;

		if (!mine->used)
			chunk_free(proc, mine);
	}
	free(shared);
	cpu->map[cpu->npages] = 0;
	return 0;
}
