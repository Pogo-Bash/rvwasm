/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A heap checker, for hunting memory corruption where no sanitizer runs
 * (Linux/Wasm). Only in a build made with -DRV_HEAPCHECK: then rv.h sends
 * every malloc(), calloc(), realloc() and free() of the emulator here.
 *
 * Every block gets a header in front (where it was allocated, its size,
 * a magic number) and a pattern of bytes behind it, and is on a list. A
 * freed block is filled with another pattern and kept for a while (the
 * quarantine) before it really goes back. rv_hc_check() looks at all of it:
 * a header or a pattern that is not what it was is reported, with which
 * block, where it was allocated, at which offset and what is there now, and
 * the program aborts. It runs every so many allocations and, through
 * rv_hc_syscall_done(), after a system call when a while has passed since
 * the last look: damage first seen after a call that waited was done while
 * the process waited, by someone else.
 */
#ifdef RV_HEAPCHECK
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>

#define HC_MAGIC	0x6b63656863706168ULL	/* "hapcheck" */
#define HC_FREED	0x646565726670616cULL
#define HC_TAIL		32
#define HC_EVERY	512			/* allocations and frees between looks */
#define HC_QUARANTINE	2048
#define HC_QUARANTINE_BYTES (32u << 20)

struct hc_head {
	uint64_t magic;
	struct hc_head *prev, *next;
	size_t size;
	const char *file;
	uint32_t line, seq;
	uint64_t magic2;		/* magic ^ size ^ address: a header copied from elsewhere does not pass */
	uint64_t pad;
};

static pthread_mutex_t hc_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hc_head *hc_list;
static uint32_t hc_seq, hc_ops;
static uint64_t hc_live, hc_live_bytes, hc_checks;
static struct hc_head *hc_quarantine[HC_QUARANTINE];
static uint32_t hc_qnext;
static uint64_t hc_qbytes;
static const char *hc_last_where = "the start";
static uint64_t hc_last_ns;

static void dump(const char *what, const uint8_t *p, size_t n);
static uintptr_t hc_sp_low = ~(uintptr_t)0, hc_sp_first;

/* Where the stack is, the break, and the process's map of its memory: for a report. */
static void where_things_are(void)
{
	char here, buf[4096];
	int fd = open("/proc/self/maps", O_RDONLY);
	ssize_t n;

	fprintf(stderr, "rv: heapcheck:   stack pointer %p (at the first look %p, the lowest seen %p), break %#lx\n",
		(void *)&here, (void *)hc_sp_first, (void *)hc_sp_low, (unsigned long)syscall(SYS_brk, (long)0));
	if (fd < 0)
		return;
	fprintf(stderr, "rv: heapcheck:   /proc/self/maps:\n");
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		fwrite(buf, 1, (size_t)n, stderr);
	close(fd);
}

static void note_sp(void)
{
	char here;

	if (!hc_sp_first)
		hc_sp_first = (uintptr_t)&here;
	if ((uintptr_t)&here < hc_sp_low)
		hc_sp_low = (uintptr_t)&here;
}

#ifdef __wasm__
/*
 * musl's allocator keeps what it knows out of the way of the blocks: a
 * struct meta for every group of blocks, in pages of their own. Its own
 * checks look at a group when it is used; this looks at all of them, as
 * musl 1.2's mallocng (src/malloc/mallocng/meta.h) has them.
 */
struct ng_group {
	struct ng_meta *meta;
	unsigned char active_idx:5;
};

struct ng_meta {
	struct ng_meta *prev, *next;
	struct ng_group *mem;
	volatile int avail_mask, freed_mask;
	uintptr_t last_idx:5;
	uintptr_t freeable:1;
	uintptr_t sizeclass:6;
	uintptr_t maplen:8*sizeof(uintptr_t)-12;
};

struct ng_area {
	uint64_t check;
	struct ng_area *next;
	int nslots;
};

struct ng_context {
	uint64_t secret;
	/* (No pagesize: musl's PAGESIZE is defined, as libc.page_size.) */
	int init_done;
	unsigned mmap_counter;
	struct ng_meta *free_meta_head;
	struct ng_meta *avail_meta;
	size_t avail_meta_count, avail_meta_area_count, meta_alloc_shift;
	struct ng_area *meta_area_head, *meta_area_tail;
	unsigned char *avail_meta_areas;
	struct ng_meta *active[48];
};

extern struct ng_context __malloc_context;
static struct ng_meta *ng_was[48];
static uint64_t ng_was_word[48];

static int ng_plausible(const void *p)
{
	return p && (uintptr_t)p < (uintptr_t)__builtin_wasm_memory_size(0) * 65536 && !((uintptr_t)p & 7);
}

static void ng_report(const char *where, const char *what, int sc, const void *at)
{
	fprintf(stderr, "rv: heapcheck (pid %d): musl's allocator is damaged, found %s (last look without damage: %s): "
		"%s (size class %d, at %p; memory is %llu MiB)\n", (int)getpid(), where, hc_last_where, what, sc, at,
		(unsigned long long)__builtin_wasm_memory_size(0) / 16);
	if (ng_plausible(at))
		dump("there", (const uint8_t *)at, 48);
	fprintf(stderr, "rv: heapcheck:   musl's records at %p; the lists of size classes %d to %d start at:",
		(void *)&__malloc_context, sc - 2, sc + 2);
	for (int k = sc - 2; k <= sc + 2; k++)
		if (k >= 0 && k < 48)
			fprintf(stderr, " %p", (void *)__malloc_context.active[k]);
	fprintf(stderr, "\n");
	fprintf(stderr, "rv: heapcheck:   at the look before, they started at:");
	for (int k = sc - 2; k <= sc + 2; k++)
		if (k >= 0 && k < 48)
			fprintf(stderr, " %p (%llx)", (void *)ng_was[k], (unsigned long long)ng_was_word[k]);
	fprintf(stderr, "\n");
	where_things_are();
	fflush(stderr);
	abort();
}

static void ng_check(const char *where)
{
	struct ng_context *c = &__malloc_context;
	unsigned n = 0;

	note_sp();

	if (!c->init_done)
		return;
	for (const struct ng_area *a = c->meta_area_head; a; a = a->next) {
		if (!ng_plausible(a) || a->check != c->secret)
			ng_report(where, "a page of struct meta", -1, a);
		if (++n > 1000000)
			ng_report(where, "the list of pages of struct meta goes round", -1, a);
	}
	for (int sc = 0; sc < 48; sc++) {
		const struct ng_meta *first = c->active[sc], *m = first;

		if (!m)
			continue;
		n = 0;
		do {
			uint32_t all;

			if (!ng_plausible(m) || !ng_plausible(m->next) || !ng_plausible(m->prev))
				ng_report(where, "a struct meta, or its neighbours in the list", sc, m);
			if (m->next->prev != m)
				ng_report(where, "the list of groups", sc, m);
			if (m->sizeclass != (uintptr_t)sc)
				ng_report(where, "the size class of a group", sc, m);
			all = (2u << m->last_idx) - 1;
			if ((m->avail_mask & m->freed_mask) || ((uint32_t)(m->avail_mask | m->freed_mask) & ~all))
				ng_report(where, "the masks of a group", sc, m);
			if (!ng_plausible(m->mem))
				ng_report(where, "where a group is", sc, m);
			if (m->mem->meta != m || m->mem->active_idx > m->last_idx)
				ng_report(where, "the header of a group", sc, m->mem);
			if (++n > 1000000)
				ng_report(where, "the list of groups goes round", sc, m);
			m = m->next;
		} while (m != first);
	}
	/* What the heads of the lists were, for the report next time. */
	for (int sc = 0; sc < 48; sc++) {
		ng_was[sc] = c->active[sc];
		ng_was_word[sc] = c->active[sc] ? *(const uint64_t *)((const char *)c->active[sc] + 32) : 0;
	}
}
#else
static void ng_check(const char *where)
{
	(void)where;
	note_sp();
}
#endif

static uint64_t hc_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint8_t tail_byte(const struct hc_head *h, size_t k)
{
	return (uint8_t)(0xa5 ^ (k * 29) ^ ((uintptr_t)h >> 4));
}

static uint64_t magic2(const struct hc_head *h)
{
	return HC_MAGIC ^ h->size ^ (uint64_t)(uintptr_t)h;
}

static void dump(const char *what, const uint8_t *p, size_t n)
{
	fprintf(stderr, "rv: heapcheck:   %s at %p:", what, (const void *)p);
	for (size_t k = 0; k < n; k++)
		fprintf(stderr, "%s%02x", k % 16 ? " " : "\n rv: heapcheck:    ", p[k]);
	fprintf(stderr, "\n");
}

static void report(const char *where, const struct hc_head *h, const char *what, size_t offset)
{
	const uint8_t *user = (const uint8_t *)(h + 1);

	fprintf(stderr, "rv: heapcheck (pid %d): damage found %s (last look without damage: %s, %llu ms before): %s\n",
		(int)getpid(), where, hc_last_where, (unsigned long long)((hc_now() - hc_last_ns) / 1000000), what);
	if (h->magic == HC_MAGIC || h->magic == HC_FREED)
		fprintf(stderr, "rv: heapcheck:   block %p of %zu bytes from %s:%u (number %u), offset %zu\n",
			(const void *)user, h->size, h->file ? h->file : "?", h->line, h->seq, offset);
	dump("header", (const uint8_t *)h, sizeof(*h));
	if (h->magic == HC_MAGIC || h->magic == HC_FREED) {
		size_t from = offset > 32 ? offset - 32 : 0;
		size_t to = offset + 64 < h->size + HC_TAIL ? offset + 64 : h->size + HC_TAIL;

		if (to > from)
			dump("around the damage", user + from, to - from);
	}
	where_things_are();
	fflush(stderr);
	abort();
}

/* With the lock. */
static void check_block(const char *where, const struct hc_head *h, int freed)
{
	const uint8_t *user = (const uint8_t *)(h + 1);

	if (h->magic != (freed ? HC_FREED : HC_MAGIC) || h->magic2 != magic2(h))
		report(where, h, freed ? "the header of a freed block" : "the header of a block", 0);
	for (size_t k = 0; k < HC_TAIL; k++)
		if (user[h->size + k] != tail_byte(h, k))
			report(where, h, "the bytes after a block", h->size + k);
	if (freed)
		for (size_t k = 0; k < h->size; k++)
			if (user[k] != 0xdd)
				report(where, h, "a freed block was written to", k);
}

static void check_all(const char *where)
{
	ng_check(where);
	for (const struct hc_head *h = hc_list; h; h = h->next) {
		check_block(where, h, 0);
		if (h->next && h->next->prev != h)
			report(where, h, "the list of blocks", 0);
	}
	for (uint32_t i = 0; i < HC_QUARANTINE; i++)
		if (hc_quarantine[i])
			check_block(where, hc_quarantine[i], 1);
	/* And musl's own checks, here and not somewhere later: a few sizes, from small to mapped. */
	{
		static const size_t sizes[] = { 24, 200, 1500, 4100, 40000, 200000 };

		for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
			free(malloc(sizes[k]));
	}
	hc_checks++;
	hc_last_where = where;
	hc_last_ns = hc_now();
}

void rv_hc_check(const char *where)
{
	pthread_mutex_lock(&hc_lock);
	check_all(where);
	pthread_mutex_unlock(&hc_lock);
}

/* A look at musl's own records only (cheaper): at every translation and system call. */
void rv_hc_light(const char *where)
{
	pthread_mutex_lock(&hc_lock);
	ng_check(where);
	hc_last_where = where;
	pthread_mutex_unlock(&hc_lock);
}

/* After a system call: all of it if the last full look was a millisecond ago or more. */
void rv_hc_syscall_done(uint64_t nr)
{
	static char where[64];

	pthread_mutex_lock(&hc_lock);
	snprintf(where, sizeof(where), "after system call %llu", (unsigned long long)nr);
	if (hc_now() - hc_last_ns < 1000000) {
		ng_check(where);
		hc_last_where = where;
	} else {
		check_all(where);
	}
	pthread_mutex_unlock(&hc_lock);
}

/* With the lock. */
static void counted(void)
{
	if (++hc_ops % HC_EVERY == 0)
		check_all("at an allocation");
}

void *rv_hc_malloc(size_t size, const char *file, int line)
{
	struct hc_head *h;
	uint8_t *user;

	if (size > SIZE_MAX - sizeof(*h) - HC_TAIL || !(h = malloc(sizeof(*h) + size + HC_TAIL)))
		return NULL;
	user = (uint8_t *)(h + 1);
	h->magic = HC_MAGIC;
	h->size = size;
	h->file = file;
	h->line = (uint32_t)line;
	h->magic2 = magic2(h);
	h->pad = 0;
	for (size_t k = 0; k < HC_TAIL; k++)
		user[size + k] = tail_byte(h, k);
	pthread_mutex_lock(&hc_lock);
	h->seq = ++hc_seq;
	h->prev = NULL;
	h->next = hc_list;
	if (hc_list)
		hc_list->prev = h;
	hc_list = h;
	hc_live++;
	hc_live_bytes += size;
	counted();
	pthread_mutex_unlock(&hc_lock);
	return user;
}

void *rv_hc_calloc(size_t n, size_t size, const char *file, int line)
{
	void *p;

	if (size && n > SIZE_MAX / size)
		return NULL;
	if ((p = rv_hc_malloc(n * size, file, line)))
		memset(p, 0, n * size);
	return p;
}

void rv_hc_free(void *p, const char *file, int line)
{
	struct hc_head *h, *old;

	(void)file;
	(void)line;
	if (!p)
		return;
	h = (struct hc_head *)p - 1;
	pthread_mutex_lock(&hc_lock);
	check_block("at a free", h, 0);
	if (h->prev)
		h->prev->next = h->next;
	else
		hc_list = h->next;
	if (h->next)
		h->next->prev = h->prev;
	hc_live--;
	hc_live_bytes -= h->size;
	h->magic = HC_FREED;
	memset(p, 0xdd, h->size);
	/* Into the quarantine, unless it is large; the oldest there goes back for real (after a look). */
	if (h->size <= HC_QUARANTINE_BYTES / 16) {
		old = hc_quarantine[hc_qnext];
		hc_quarantine[hc_qnext] = h;
		hc_qnext = (hc_qnext + 1) % HC_QUARANTINE;
		hc_qbytes += h->size;
		while (old || hc_qbytes > HC_QUARANTINE_BYTES) {
			if (!old) {
				/* Too many bytes kept: the next oldest goes too. */
				old = hc_quarantine[hc_qnext];
				hc_quarantine[hc_qnext] = NULL;
				if (!old)
					break;
			}
			check_block("at a free (leaving the quarantine)", old, 1);
			hc_qbytes -= old->size;
			free(old);
			old = NULL;
		}
	} else {
		free(h);
	}
	counted();
	pthread_mutex_unlock(&hc_lock);
}

void *rv_hc_realloc(void *p, size_t size, const char *file, int line)
{
	struct hc_head *h;
	void *q;

	if (!p)
		return rv_hc_malloc(size, file, line);
	h = (struct hc_head *)p - 1;
	if (!(q = rv_hc_malloc(size, file, line)))
		return NULL;
	memcpy(q, p, h->size < size ? h->size : size);
	rv_hc_free(p, file, line);
	return q;
}

void rv_hc_report(void)
{
	struct site { const char *file; uint32_t line; uint64_t n, bytes; } sites[64];
	unsigned nsites = 0;

	fprintf(stderr, "rv: heapcheck: %llu blocks (%llu KiB) still allocated, %llu looks without damage; "
		"the break at %#lx, the stack pointer as low as %p\n",
		(unsigned long long)hc_live, (unsigned long long)(hc_live_bytes >> 10), (unsigned long long)hc_checks,
		(unsigned long)syscall(SYS_brk, (long)0), (void *)hc_sp_low);
	/* Where the memory still allocated was allocated, the most first. */
	pthread_mutex_lock(&hc_lock);
	for (const struct hc_head *h = hc_list; h; h = h->next) {
		unsigned k;

		for (k = 0; k < nsites && (sites[k].file != h->file || sites[k].line != h->line); k++)
			;
		if (k == nsites) {
			if (nsites == 64)
				continue;
			sites[nsites++] = (struct site){ h->file, h->line, 0, 0 };
		}
		sites[k].n++;
		sites[k].bytes += h->size;
	}
	pthread_mutex_unlock(&hc_lock);
	for (unsigned shown = 0; shown < 6 && nsites; shown++) {
		unsigned best = 0;

		for (unsigned k = 1; k < nsites; k++)
			if (sites[k].bytes > sites[best].bytes)
				best = k;
		fprintf(stderr, "rv: heapcheck:   %s:%u: %llu blocks, %llu KiB\n", sites[best].file, sites[best].line,
			(unsigned long long)sites[best].n, (unsigned long long)(sites[best].bytes >> 10));
		sites[best] = sites[--nsites];
	}
}
#else
typedef int rv_heapcheck_unused;
#endif
