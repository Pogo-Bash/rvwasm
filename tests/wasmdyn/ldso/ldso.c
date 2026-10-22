/*
 * ld-wasm: a dynamic linker for native linux-wasm programs (prototype, see
 * docs/wasm-dynamic-linking.md).
 *
 * It is the C library itself (musl, statically linked into this program, as
 * musl's libc.so is its own ld.so), exporting libc and libc++ to the modules
 * it loads. Run as
 *
 *     ld-wasm PROGRAM [ARGS...]
 *
 * it loads PROGRAM (a `wasm-ld -pie` module without a libc of its own), the
 * shared libraries PROGRAM needs (dylink.0 NEEDED, recursively), binds every
 * import, runs the constructors and calls PROGRAM's main. dlopen/dlsym/dlclose
 * work in the loaded program and its libraries.
 *
 * The host's part is one import, __wasm_dl (wasm_dl.h): it compiles and
 * instantiates a module with the import bindings decided here, and puts its
 * function exports into the table. Every thread (Worker) has its own
 * instances and table, so loads go into a log that every thread replays:
 * a new thread before it runs anything (__dl_clone_hook, called by the
 * runtime), the threads that exist when dlopen loads something through
 * musl's __synccall (a signal to each, whose handler replays the log).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wasm_dl.h"

/* The host (patch-runtime.py item 26). */
long __wasm_dl(long op, void *arg);
/* ldso_asm.S: this module's own __memory_base. */
uint64_t __dl_self_memory_base(void);
/* musl internals (libc.a, hidden there; we are linked into the same module). */
void __synccall(void (*)(void *), void *);
void __dl_seterr(const char *, ...);

#define DL_MAX_LOG 4096

/* ------------------------------------------------------------------------ */
/* Modules */

struct imp {
	char *module, *name;
	uint8_t kind;		/* 0 func, 1 table, 2 memory, 3 global, 4 tag */
	uint32_t type;		/* func/tag: type index; global: valtype | 0x100 if mutable */
	uint8_t weak;		/* dylink.0 IMPORT_INFO: binding weak */
};

struct sym {
	char *name;
	uint8_t kind;		/* export kind */
	uint8_t tls;		/* dylink.0 EXPORT_INFO: a thread-local symbol (value is an offset) */
	uint32_t index;		/* in its index space */
	uint64_t value;		/* globals: the init value (an address relative to __memory_base) */
	uint32_t slot;		/* functions: the table slot it is placed in */
};

struct ftype {
	uint32_t nparams, nresults;
	uint8_t *params, *results;
};

struct dso {
	struct dso *next;	/* in load order */
	char *name;		/* the path it was loaded from */
	int id;			/* the host's instance id (0: ld-wasm itself), -1 until instantiated */
	long log;		/* its index in the log, -1 for ld-wasm itself */
	const uint8_t *bytes;
	size_t len;
	uint64_t mem_base;
	uint32_t mem_size, mem_align, table_size, table_align;
	uint64_t table_base;
	int ntypes, nimp, nsym, nglobal_imports, nfunc_imports, nglobals;
	struct ftype *types;
	struct imp *imp;
	struct sym *sym;
	uint64_t *globals;	/* init values of the module's own globals */
	uint32_t *hash;		/* open addressing over sym (index + 1, 0 = empty) */
	uint32_t hash_mask;
	char **needed;
	int nneeded;
	struct dso **deps;	/* needed, resolved */
	int global;		/* in the global scope */
	int refcount;
	int constructed;
	uint64_t tls_size, tls_align;
	int tablified;		/* tablify.py: function export k is in slot table_base + slot_base + k already */
	int dynsym;		/* tablify.py --dynsym: its symbols are not wasm exports (but for tags) */
	uint32_t slot_base;
	struct dso *all_next;	/* every module opened, loaded or still being loaded (dl_all) */
	uint64_t dev, ino;	/* the file: the same file under two names (libQt6Core.so.6, libQt6Core.so.6.8.2) is one module */
	uint64_t s_ctors, s_relocs, s_tls_init, s_tls_set, s_tls_size, s_tls_align, s_main_argc, s_main_void;
};

struct log_entry {
	struct wasm_dl_desc desc;
	struct dso *dso;	/* NULL for trampolines and placements */
};

static struct log_entry dl_log[DL_MAX_LOG];
static _Atomic size_t dl_log_n;
static struct dso *dl_head, *dl_tail;	/* every module, in load order (dl_head: ld-wasm) */
static struct dso *dl_self, *dl_program;
static uint64_t dl_table_top;
static pthread_mutex_t dl_lock = PTHREAD_MUTEX_INITIALIZER;
static int dl_debug;
static int dl_bind_slots;	/* LD_BIND_SLOTS: bind function imports to table slots even when exported */
static struct timespec dl_t0;

static _Thread_local void **dl_dtv;	/* this thread's TLS block of each logged module */
static _Thread_local char *dl_tls_pool;
static _Thread_local size_t dl_tls_left;

static void dbg(const char *fmt, ...)
{
	if (!dl_debug)
		return;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	double ms = (t.tv_sec - dl_t0.tv_sec) * 1e3 + (t.tv_nsec - dl_t0.tv_nsec) / 1e6;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = snprintf(buf, sizeof buf, "ld-wasm %8.3f ms [%d] ", ms, gettid());
	vsnprintf(buf + n, sizeof buf - n, fmt, ap);
	va_end(ap);
	size_t l = strlen(buf);
	if (l < sizeof buf - 1)
		buf[l++] = '\n';
	write(2, buf, l);
}

static _Noreturn void die(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = snprintf(buf, sizeof buf, "ld-wasm: ");
	vsnprintf(buf + n, sizeof buf - n, fmt, ap);
	va_end(ap);
	size_t l = strlen(buf);
	if (l < sizeof buf - 1)
		buf[l++] = '\n';
	write(2, buf, l);
	_exit(127);
}

/* ------------------------------------------------------------------------ */
/* Reading a module */

struct rd {
	const uint8_t *p, *end;
	int bad;
};

static uint64_t leb(struct rd *r)
{
	uint64_t v = 0;
	int shift = 0;
	while (r->p < r->end) {
		uint8_t b = *r->p++;
		v |= (uint64_t)(b & 0x7f) << shift;
		shift += 7;
		if (!(b & 0x80))
			return v;
	}
	r->bad = 1;
	return 0;
}

static int64_t sleb(struct rd *r)
{
	int64_t v = 0;
	int shift = 0;
	uint8_t b = 0x80;
	while (r->p < r->end && (b & 0x80)) {
		b = *r->p++;
		v |= (int64_t)(b & 0x7f) << shift;
		shift += 7;
	}
	if (b & 0x80)
		r->bad = 1;
	else if (shift < 64 && (b & 0x40))
		v |= -((int64_t)1 << shift);
	return v;
}

static uint8_t byte(struct rd *r)
{
	if (r->p >= r->end) {
		r->bad = 1;
		return 0;
	}
	return *r->p++;
}

static char *str(struct rd *r)
{
	uint64_t n = leb(r);
	if (n > (uint64_t)(r->end - r->p)) {
		r->bad = 1;
		return strdup("");
	}
	char *s = strndup((const char *)r->p, n);
	r->p += n;
	return s;
}

static void limits(struct rd *r)
{
	uint8_t flags = byte(r);
	leb(r);
	if (flags & 1)
		leb(r);
}

static uint64_t const_expr(struct rd *r)
{
	uint64_t v = 0;
	for (;;) {
		uint8_t op = byte(r);
		if (r->bad || op == 0x0b)
			return v;
		if (op == 0x41 || op == 0x42)
			v = sleb(r);
		else if (op == 0x23)
			leb(r);	/* global.get: not expected in what we read */
		else {
			r->bad = 1;
			return 0;
		}
	}
}

static uint32_t hash_name(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

static struct sym *find_sym(struct dso *d, const char *name)
{
	if (!d->hash)
		return NULL;
	for (uint32_t i = hash_name(name) & d->hash_mask;; i = (i + 1) & d->hash_mask) {
		uint32_t e = d->hash[i];
		if (!e)
			return NULL;
		if (!strcmp(d->sym[e - 1].name, name))
			return &d->sym[e - 1];
	}
}

/* Names every module has for the dynamic linker, never bound to another module's. */
static int internal(const char *n)
{
	static const char *const names[] = {
		"__wasm_call_ctors", "__wasm_apply_data_relocs", "__wasm_init_tls", "__wasm_init_memory",
		"__dl_tls_init", "__dl_tls_set", "__dl_tls_size", "__dl_tls_align",
		"__main_argc_argv", "__main_void", "main", "_start", "__dso_handle",
	};
	for (size_t i = 0; i < sizeof names / sizeof *names; i++)
		if (!strcmp(n, names[i]))
			return 1;
	return 0;
}

static void parse_dylink(struct dso *d, struct rd *r)
{
	while (r->p < r->end && !r->bad) {
		uint8_t id = byte(r);
		uint64_t n = leb(r);
		struct rd s = { r->p, r->p + n, 0 };
		r->p += n;
		if (id == 1) {	/* MEM_INFO */
			d->mem_size = leb(&s);
			d->mem_align = leb(&s);
			d->table_size = leb(&s);
			d->table_align = leb(&s);
		} else if (id == 2) {	/* NEEDED */
			d->nneeded = leb(&s);
			d->needed = calloc(d->nneeded, sizeof *d->needed);
			for (int i = 0; i < d->nneeded; i++)
				d->needed[i] = str(&s);
		} else if (id == 0x80) {	/* tablify.py: the function exports have slots */
			d->tablified = 1;
			d->slot_base = leb(&s);
		}
		/* 3 EXPORT_INFO, 4 IMPORT_INFO: read after the import and export sections (parse_info). */
	}
}

static void parse_info(struct dso *d, struct rd *r)
{
	while (r->p < r->end && !r->bad) {
		uint8_t id = byte(r);
		uint64_t n = leb(r);
		struct rd s = { r->p, r->p + n, 0 };
		r->p += n;
		if (id == 3) {	/* EXPORT_INFO: name, flags */
			uint64_t c = leb(&s);
			for (uint64_t i = 0; i < c && !s.bad; i++) {
				char *name = str(&s);
				uint64_t flags = leb(&s);
				struct sym *y = find_sym(d, name);
				if (y && (flags & 0x100))	/* WASM_SYMBOL_TLS */
					y->tls = 1;
				free(name);
			}
		} else if (id == 4) {	/* IMPORT_INFO: module, field, flags */
			uint64_t c = leb(&s);
			for (uint64_t i = 0; i < c && !s.bad; i++) {
				char *m = str(&s), *f = str(&s);
				uint64_t flags = leb(&s);
				for (int k = 0; k < d->nimp; k++)
					if (!strcmp(d->imp[k].module, m) && !strcmp(d->imp[k].name, f) && (flags & 1))
						d->imp[k].weak = 1;
				free(m);
				free(f);
			}
		}
	}
}

/* Parse what the dynamic linker needs of a module: dylink.0, types, imports, globals, exports. */
static int parse(struct dso *d)
{
	struct rd r = { d->bytes, d->bytes + d->len, 0 };
	const uint8_t *dylink = NULL, *dylink_end = NULL, *dynsym = NULL, *dynsym_end = NULL;
	if (d->len < 8 || memcmp(d->bytes, "\0asm\1\0\0\0", 8))
		return -1;
	r.p += 8;
	while (r.p < r.end && !r.bad) {
		uint8_t id = byte(&r);
		uint64_t n = leb(&r);
		if (n > (uint64_t)(r.end - r.p))
			return -1;
		struct rd s = { r.p, r.p + n, 0 };
		r.p += n;
		if (id == 0) {
			char *name = str(&s);
			if (!strcmp(name, "dylink.0")) {
				dylink = s.p;
				dylink_end = s.end;
				parse_dylink(d, &s);
			} else if (!strcmp(name, "linux-wasm.dynsym")) {
				/* tablify.py --dynsym: the exports, moved here (only what the host needs is exported) */
				dynsym = s.p;
				dynsym_end = s.end;
			}
			free(name);
		} else if (id == 1) {	/* types */
			d->ntypes = leb(&s);
			d->types = calloc(d->ntypes, sizeof *d->types);
			for (int i = 0; i < d->ntypes && !s.bad; i++) {
				if (byte(&s) != 0x60)
					return -1;
				struct ftype *t = &d->types[i];
				t->nparams = leb(&s);
				t->params = malloc(t->nparams + 1);
				for (uint32_t k = 0; k < t->nparams; k++)
					t->params[k] = byte(&s);
				t->nresults = leb(&s);
				t->results = malloc(t->nresults + 1);
				for (uint32_t k = 0; k < t->nresults; k++)
					t->results[k] = byte(&s);
			}
		} else if (id == 2) {	/* imports */
			d->nimp = leb(&s);
			d->imp = calloc(d->nimp, sizeof *d->imp);
			for (int i = 0; i < d->nimp && !s.bad; i++) {
				struct imp *m = &d->imp[i];
				m->module = str(&s);
				m->name = str(&s);
				m->kind = byte(&s);
				switch (m->kind) {
				case 0:
					m->type = leb(&s);
					d->nfunc_imports++;
					break;
				case 1:
					byte(&s);
					limits(&s);
					break;
				case 2:
					limits(&s);
					break;
				case 3:
					m->type = byte(&s);
					if (byte(&s))
						m->type |= 0x100;
					d->nglobal_imports++;
					break;
				case 4:
					byte(&s);
					m->type = leb(&s);
					break;
				default:
					return -1;
				}
			}
		} else if (id == 6) {	/* globals */
			d->nglobals = leb(&s);
			d->globals = calloc(d->nglobals + 1, sizeof *d->globals);
			for (int i = 0; i < d->nglobals && !s.bad; i++) {
				byte(&s);
				byte(&s);
				d->globals[i] = const_expr(&s);
			}
		} else if (id == 7 && !dynsym) {	/* exports (unless the symbols were before, in dynsym) */
			d->nsym = leb(&s);
			d->sym = calloc(d->nsym + 1, sizeof *d->sym);
			for (int i = 0; i < d->nsym && !s.bad; i++) {
				d->sym[i].name = str(&s);
				d->sym[i].kind = byte(&s);
				d->sym[i].index = leb(&s);
			}
		}
		if (s.bad)
			return -1;
	}
	if (r.bad || !dylink)
		return -1;
	if (dynsym) {
		struct rd s = { dynsym, dynsym_end, 0 };
		for (int i = 0; i < d->nsym; i++)
			free(d->sym[i].name);
		free(d->sym);
		d->nsym = leb(&s);
		d->sym = calloc(d->nsym + 1, sizeof *d->sym);
		for (int i = 0; i < d->nsym && !s.bad; i++) {
			d->sym[i].name = str(&s);
			d->sym[i].kind = byte(&s);
			d->sym[i].index = leb(&s);
		}
		if (s.bad)
			return -1;
		d->dynsym = 1;
	}
	/* Values of exported globals; the hash of exports. */
	uint32_t size = 16;
	while (size < 2 * (uint32_t)d->nsym)
		size *= 2;
	d->hash = calloc(size, sizeof *d->hash);
	d->hash_mask = size - 1;
	for (int i = 0; i < d->nsym; i++) {
		struct sym *y = &d->sym[i];
		if (y->kind == 3) {
			int k = (int)y->index - d->nglobal_imports;
			if (k >= 0 && k < d->nglobals)
				y->value = d->globals[k];
		}
		uint32_t h = hash_name(y->name) & d->hash_mask;
		while (d->hash[h])
			h = (h + 1) & d->hash_mask;
		d->hash[h] = i + 1;
	}
	struct rd s = { dylink, dylink_end, 0 };
	parse_info(d, &s);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Files */

static const char *dl_path_env;

static int try_open(const char *dir, const char *name, char *out, size_t size)
{
	if (snprintf(out, size, "%s/%s", dir, name) >= (int)size)
		return -1;
	return open(out, O_RDONLY | O_CLOEXEC);
}

/* Find a library as Linux's ld.so would: a name with a slash as it is; else LD_LIBRARY_PATH, the
 * directory of the module that needs it, /lib, /usr/lib. */
static int find_file(const char *name, const struct dso *from, char *out, size_t size)
{
	if (strchr(name, '/')) {
		snprintf(out, size, "%s", name);
		return open(name, O_RDONLY | O_CLOEXEC);
	}
	int fd;
	if (dl_path_env) {
		const char *p = dl_path_env;
		while (*p) {
			size_t l = strcspn(p, ":");
			char dir[PATH_MAX];
			snprintf(dir, sizeof dir, "%.*s", (int)l, p);
			if (l && (fd = try_open(dir, name, out, size)) >= 0)
				return fd;
			p += l;
			if (*p == ':')
				p++;
		}
	}
	if (from && from->name && strchr(from->name, '/')) {
		char dir[PATH_MAX];
		snprintf(dir, sizeof dir, "%s", from->name);
		*strrchr(dir, '/') = 0;
		if ((fd = try_open(*dir ? dir : "/", name, out, size)) >= 0)
			return fd;
	}
	if ((fd = try_open("/lib", name, out, size)) >= 0)
		return fd;
	return try_open("/usr/lib", name, out, size);
}

/* Every module opened, including those a load in progress has opened and not yet put in the lists (a diamond of
 * dependencies reaches the same module twice in one load). */
static struct dso *dl_all;

static struct dso *find_loaded(const struct stat *st)
{
	for (struct dso *d = dl_all; d; d = d->all_next)
		if (d->ino == (uint64_t)st->st_ino && d->dev == (uint64_t)st->st_dev)
			return d;
	return NULL;
}

/* Open and parse one module. The bytes stay mapped for the process's lifetime: threads started later
 * compile them too. */
static struct dso *open_dso(const char *name, const struct dso *from)
{
	char path[PATH_MAX];
	int fd = find_file(name, from, path, sizeof path);
	if (fd < 0) {
		__dl_seterr("Error loading shared library %s: %m", name);
		return NULL;
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < 8) {
		close(fd);
		__dl_seterr("Error loading shared library %s: not a module", path);
		return NULL;
	}
	struct dso *old = find_loaded(&st);
	if (old) {
		close(fd);
		return old;
	}
	void *map = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		__dl_seterr("Error loading shared library %s: %m", path);
		return NULL;
	}
	struct dso *d = calloc(1, sizeof *d);
	d->name = strdup(path);
	d->dev = st.st_dev;
	d->ino = st.st_ino;
	d->bytes = map;
	d->len = st.st_size;
	d->id = -1;
	d->log = -1;
	if (parse(d) < 0) {
		__dl_seterr("Error loading shared library %s: not a WebAssembly module with a dylink.0 section", path);
		munmap(map, st.st_size);
		free(d);
		return NULL;
	}
	d->all_next = dl_all;
	dl_all = d;
	dbg("opened %s: %zu bytes, %d imports, %d exports, memory %u, table %u, %d needed",
	    path, d->len, d->nimp, d->nsym, d->mem_size, d->table_size, d->nneeded);
	return d;
}

/* ------------------------------------------------------------------------ */
/* Binding */

/* The scope a module's imports are looked up in: the global scope (the program, the libraries it
 * needs, those dlopen()ed with RTLD_GLOBAL, in load order; ld-wasm, the C library, last), then the
 * module's own group (itself and what it needs) for RTLD_LOCAL loads. */
struct scope {
	struct dso **v;
	int n;
};

static void scope_add(struct scope *s, struct dso *d)
{
	for (int i = 0; i < s->n; i++)
		if (s->v[i] == d)
			return;
	s->v = realloc(s->v, (s->n + 1) * sizeof *s->v);
	s->v[s->n++] = d;
}

static void scope_group(struct scope *s, struct dso *d)
{
	int start = s->n;
	scope_add(s, d);
	for (int i = start; i < s->n; i++)
		for (int k = 0; k < s->v[i]->nneeded; k++)
			if (s->v[i]->deps && s->v[i]->deps[k])
				scope_add(s, s->v[i]->deps[k]);
}

static struct scope make_scope(struct dso *d)
{
	struct scope s = { 0, 0 };
	for (struct dso *x = dl_head; x; x = x->next)
		if (x->global && x != dl_self)
			scope_add(&s, x);
	scope_add(&s, dl_self);
	if (d)
		scope_group(&s, d);
	return s;
}

static struct sym *lookup(struct scope *s, const char *name, int kind, struct dso **where)
{
	if (internal(name))
		return NULL;
	for (int i = 0; i < s->n; i++) {
		struct sym *y = find_sym(s->v[i], name);
		if (y && y->kind == kind) {
			*where = s->v[i];
			return y;
		}
	}
	return NULL;
}

/* A growable byte buffer, for the trampoline modules. */
struct buf {
	uint8_t *p;
	size_t n, cap;
};

static void put(struct buf *b, const void *p, size_t n)
{
	if (b->n + n > b->cap) {
		b->cap = (b->n + n) * 2 + 64;
		b->p = realloc(b->p, b->cap);
	}
	memcpy(b->p + b->n, p, n);
	b->n += n;
}

static void put8(struct buf *b, uint8_t v)
{
	put(b, &v, 1);
}

static void putleb(struct buf *b, uint64_t v)
{
	do {
		uint8_t c = v & 0x7f;
		v >>= 7;
		put8(b, c | (v ? 0x80 : 0));
	} while (v);
}

static void putsleb(struct buf *b, int64_t v)
{
	for (;;) {
		uint8_t c = v & 0x7f;
		v >>= 7;
		if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40))) {
			put8(b, c);
			return;
		}
		put8(b, c | 0x80);
	}
}

static void putsection(struct buf *b, uint8_t id, struct buf *body)
{
	put8(b, id);
	putleb(b, body->n);
	put(b, body->p, body->n);
	body->n = 0;
}

struct tramp {
	struct ftype *type;
	uint64_t slot;
};

/* A module of functions that call through the table: an import whose definition is in a module not
 * instantiated yet (the importing module itself: wasm-ld imports a shared library's own weak
 * definitions; or a later one in a cycle) is bound to one of these, which calls the slot that
 * module's function will be placed in. */
static uint8_t *make_trampolines(struct tramp *t, int n, size_t *len)
{
	struct buf b = { 0 }, s = { 0 };
	put(&b, "\0asm\1\0\0\0", 8);
	putleb(&s, n);
	for (int i = 0; i < n; i++) {
		put8(&s, 0x60);
		putleb(&s, t[i].type->nparams);
		put(&s, t[i].type->params, t[i].type->nparams);
		putleb(&s, t[i].type->nresults);
		put(&s, t[i].type->results, t[i].type->nresults);
	}
	putsection(&b, 1, &s);
	putleb(&s, 1);
	putleb(&s, 3);
	put(&s, "env", 3);
	putleb(&s, 25);
	put(&s, "__indirect_function_table", 25);
	put8(&s, 1);	/* table */
	put8(&s, 0x70);	/* funcref */
	put8(&s, 0);	/* limits: min 0, no max */
	putleb(&s, 0);
	putsection(&b, 2, &s);
	putleb(&s, n);
	for (int i = 0; i < n; i++)
		putleb(&s, i);
	putsection(&b, 3, &s);
	putleb(&s, n);
	for (int i = 0; i < n; i++) {
		char name[16];
		int l = snprintf(name, sizeof name, "t%d", i);
		putleb(&s, l);
		put(&s, name, l);
		put8(&s, 0);
		putleb(&s, i);
	}
	putsection(&b, 7, &s);
	putleb(&s, n);
	for (int i = 0; i < n; i++) {
		struct buf f = { 0 };
		putleb(&f, 0);	/* no locals */
		for (uint32_t k = 0; k < t[i].type->nparams; k++) {
			put8(&f, 0x20);
			putleb(&f, k);
		}
		put8(&f, 0x41);	/* i32.const slot (a 32-bit table with this LLVM) */
		putsleb(&f, (int64_t)t[i].slot);
		put8(&f, 0x11);	/* call_indirect type, table 0 */
		putleb(&f, i);
		put8(&f, 0);
		put8(&f, 0x0b);
		putleb(&s, f.n);
		put(&s, f.p, f.n);
		free(f.p);
	}
	putsection(&b, 10, &s);
	free(s.p);
	*len = b.n;
	return b.p;
}

static long host_load(struct log_entry *e)
{
	size_t i = atomic_load_explicit(&dl_log_n, memory_order_relaxed);
	if (i >= DL_MAX_LOG)
		die("too many modules");
	dl_log[i] = *e;
	atomic_store_explicit(&dl_log_n, i + 1, memory_order_release);
	long id = __wasm_dl(WASM_DL_LOAD, &dl_log[i].desc);
	if (id != (long)i + 1)
		die("the host did not load log entry %zu (%ld)", i, id);
	dbg("  %s: compile %.3f ms, instantiate %.3f ms, place %.3f ms", e->dso ? e->dso->name : "(trampolines)",
	    dl_log[i].desc.us_compile / 1e3, dl_log[i].desc.us_instantiate / 1e3, dl_log[i].desc.us_place / 1e3);
	if (e->dso) {
		e->dso->id = id;
		e->dso->log = i;
	}
	return id;
}

static uint64_t slot_of(struct dso *d, const char *name)
{
	struct sym *y = find_sym(d, name);
	return y && y->kind == 0 ? y->slot : 0;
}

/* Bind this thread's TLS block of a module (its __tls_base): the block this thread has already (a
 * vfork child runs on its parent's TLS, as for the C library's own), else a new one, initialised. */
static void bind_tls(struct dso *d)
{
	if (!d->tls_size)
		return;
	if (!dl_dtv) {
		dl_dtv = mmap(0, DL_MAX_LOG * sizeof *dl_dtv, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (dl_dtv == MAP_FAILED)
			die("no memory for TLS");
	}
	void *block = dl_dtv[d->log];
	if (block) {
		((void (*)(void *))(uintptr_t)d->s_tls_set)(block);
		return;
	}
	/* mmap, not malloc: this runs in signal handlers (__synccall). Blocks of a thread that ends are
	 * not given back (yet). */
	uint64_t align = d->tls_align > 16 ? d->tls_align : 16;
	uint64_t need = d->tls_size + align;
	if (need > dl_tls_left) {
		size_t chunk = need > 65536 ? (need + 4095) & ~4095ul : 65536;
		dl_tls_pool = mmap(0, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (dl_tls_pool == MAP_FAILED)
			die("no memory for TLS");
		dl_tls_left = chunk;
	}
	uintptr_t p = ((uintptr_t)dl_tls_pool + align - 1) & ~(uintptr_t)(align - 1);
	dl_tls_left -= p + d->tls_size - (uintptr_t)dl_tls_pool;
	dl_tls_pool = (char *)(p + d->tls_size);
	dl_dtv[d->log] = (void *)p;
	((void (*)(void *))(uintptr_t)d->s_tls_init)((void *)p);
}

/* Instantiate one module in this thread (which logs it for every other thread). */
static int instantiate(struct dso *d, struct scope *s)
{
	struct wasm_dl_import *plan = calloc(d->nimp + 1, sizeof *plan);
	struct tramp *tr = calloc(d->nimp + 1, sizeof *tr);
	int *tri = calloc(d->nimp + 1, sizeof *tri);
	int ntr = 0;
	for (int i = 0; i < d->nimp; i++) {
		struct imp *m = &d->imp[i];
		struct wasm_dl_import *p = &plan[i];
		struct dso *w = NULL;
		struct sym *y;
		if (!strcmp(m->module, "env")) {
			if (m->kind == 1 || m->kind == 2 || !strcmp(m->name, "__stack_pointer") ||
			    !strncmp(m->name, "__wasm_syscall_", 15) || !strcmp(m->name, "__wasm_abort") ||
			    !strcmp(m->name, "__wasm_dl") || !strcmp(m->name, "__rv_jit_install") || !strcmp(m->name, "__run_js")) {
				p->kind = WASM_DL_HOST;
				p->a = (uintptr_t)m->name;
			} else if (m->kind == 3) {
				p->kind = WASM_DL_GLOBAL;
				p->type = m->type;
				if (!strcmp(m->name, "__memory_base"))
					p->a = d->mem_base;
				else if (!strcmp(m->name, "__table_base") || !strcmp(m->name, "__table_base32"))
					p->a = d->table_base;
				else {
					__dl_seterr("%s: cannot bind global import %s", d->name, m->name);
					return -1;
				}
			} else if (m->kind == 4) {
				y = lookup(s, m->name, 4, &w);
				if (!y || w->id < 0) {
					__dl_seterr("%s: no tag %s", d->name, m->name);
					return -1;
				}
				p->kind = WASM_DL_EXPORT;
				p->a = w->id;
				p->b = (uintptr_t)y->name;
			} else if ((y = lookup(s, m->name, 0, &w))) {
				if (w->id >= 0 && (w->dynsym || (dl_bind_slots && w->tablified))) {
					/* The function in its table slot: no export needed. */
					p->kind = WASM_DL_SLOT;
					p->a = y->slot;
				} else if (w->id >= 0) {
					p->kind = WASM_DL_EXPORT;
					p->a = w->id;
					p->b = (uintptr_t)y->name;
				} else {
					/* Not instantiated yet (itself, or later in this load): through the table. */
					if (m->type >= (uint32_t)d->ntypes)
						return -1;
					tr[ntr].type = &d->types[m->type];
					tr[ntr].slot = y->slot;
					tri[i] = ++ntr;
				}
			} else {
				if (!m->weak)
					dbg("%s: undefined function %s (an error if it is called)", d->name, m->name);
				p->kind = WASM_DL_MISSING;
				p->a = (uintptr_t)m->name;
			}
		} else if (!strcmp(m->module, "GOT.mem") || !strcmp(m->module, "GOT.func")) {
			int func = m->module[4] == 'f';
			p->kind = WASM_DL_GLOBAL;
			p->type = m->type;
			y = lookup(s, m->name, func ? 0 : 3, &w);
			if (y && func)
				p->a = y->slot;
			else if (y && !y->tls)
				p->a = w->mem_base + y->value;
			else if (y) {
				__dl_seterr("%s: %s is a thread-local variable of %s (not supported across modules)",
					    d->name, m->name, w->name);
				return -1;
			} else if (!m->weak) {
				__dl_seterr("%s: undefined symbol %s", d->name, m->name);
				return -1;
			}
		} else {
			__dl_seterr("%s: import from unknown module %s", d->name, m->module);
			return -1;
		}
	}
	if (ntr) {
		size_t len;
		uint8_t *bytes = make_trampolines(tr, ntr, &len);
		struct wasm_dl_import *tp = calloc(1, sizeof *tp);
		tp->kind = WASM_DL_HOST;
		tp->a = (uintptr_t)"__indirect_function_table";
		struct log_entry e = { { (uintptr_t)bytes, len, 1, (uintptr_t)tp, dl_table_top, 0, 0 }, NULL };
		long id = host_load(&e);
		static int tcount;
		tcount += ntr;
		dbg("%s: %d imports through trampolines (instance %ld; %d in all)", d->name, ntr, id, tcount);
		for (int i = 0; i < d->nimp; i++)
			if (tri[i]) {
				char *name = malloc(16);
				snprintf(name, 16, "t%d", tri[i] - 1);
				plan[i].kind = WASM_DL_EXPORT;
				plan[i].a = id;
				plan[i].b = (uintptr_t)name;
			}
	}
	uint64_t place = 0;
	for (int i = 0; i < d->nsym && !d->tablified; i++)
		if (d->sym[i].kind == 0) {
			place = d->sym[i].slot;
			break;
		}
	struct log_entry e = { { (uintptr_t)d->bytes, d->len, d->nimp, (uintptr_t)plan, dl_table_top, place, 0 }, d };
	host_load(&e);
	free(tr);
	free(tri);
	d->s_ctors = slot_of(d, "__wasm_call_ctors");
	d->s_relocs = slot_of(d, "__wasm_apply_data_relocs");
	d->s_tls_init = slot_of(d, "__dl_tls_init");
	d->s_tls_set = slot_of(d, "__dl_tls_set");
	d->s_tls_size = slot_of(d, "__dl_tls_size");
	d->s_tls_align = slot_of(d, "__dl_tls_align");
	d->s_main_argc = slot_of(d, "__main_argc_argv");
	d->s_main_void = slot_of(d, "__main_void");
	if (d->s_tls_size && d->s_tls_init && d->s_tls_set) {
		d->tls_size = ((uint64_t (*)(void))(uintptr_t)d->s_tls_size)();
		d->tls_align = ((uint64_t (*)(void))(uintptr_t)d->s_tls_align)();
	}
	bind_tls(d);
	/* Pointers in its data to other modules' symbols: once, the memory is everyone's. */
	if (d->s_relocs)
		((void (*)(void))(uintptr_t)d->s_relocs)();
	dbg("instantiated %s: instance %d, memory %#llx+%u, table %llu+%u, TLS %llu", d->name, d->id,
	    (unsigned long long)d->mem_base, d->mem_size, (unsigned long long)d->table_base, d->table_size,
	    (unsigned long long)d->tls_size);
	return 0;
}

/* Memory and table slots for a module, before anything is bound (so that others can refer to them). */
static int place(struct dso *d)
{
	if (d->mem_size) {
		uint64_t align = (uint64_t)1 << d->mem_align;
		size_t size = d->mem_size + (align > 4096 ? align : 0);
		char *m = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m == MAP_FAILED) {
			__dl_seterr("%s: no memory for its data (%u bytes)", d->name, d->mem_size);
			return -1;
		}
		d->mem_base = ((uintptr_t)m + align - 1) & ~(uintptr_t)(align - 1);
	}
	uint64_t talign = (uint64_t)1 << d->table_align;
	dl_table_top = (dl_table_top + talign - 1) & ~(talign - 1);
	d->table_base = dl_table_top;
	dl_table_top += d->table_size;
	uint64_t next = d->table_base + d->slot_base;
	for (int i = 0; i < d->nsym; i++)
		if (d->sym[i].kind == 0)
			d->sym[i].slot = d->tablified ? next++ : dl_table_top++;
	return 0;
}

/* Load a module and what it needs (breadth first); returns the module, with every new one placed,
 * bound and instantiated (dependencies first) but not constructed. */
static struct dso *load(const char *name, const struct dso *from, int global, struct dso ***newv, int *newn)
{
	struct dso *root = open_dso(name, from);
	if (!root)
		return NULL;
	if (root->id >= 0 || root == dl_self) {
		if (global)
			root->global = 1;
		return root;
	}
	struct dso **v = malloc(sizeof *v);
	int n = 0;
	v[n++] = root;
	for (int i = 0; i < n; i++) {
		struct dso *d = v[i];
		d->deps = calloc(d->nneeded + 1, sizeof *d->deps);
		for (int k = 0; k < d->nneeded; k++) {
			struct dso *x = open_dso(d->needed[k], d);
			if (!x)
				return NULL;
			d->deps[k] = x;
			int seen = x->id >= 0 || x == dl_self;
			for (int j = 0; j < n && !seen; j++)
				seen = v[j] == x;
			if (!seen) {
				v = realloc(v, (n + 1) * sizeof *v);
				v[n++] = x;
			}
		}
	}
	for (int i = 0; i < n; i++) {
		if (place(v[i]) < 0)
			return NULL;
		v[i]->global = global;
		v[i]->refcount = 0;
		if (dl_tail)
			dl_tail->next = v[i];
		else
			dl_head = v[i];
		dl_tail = v[i];
	}
	/* Dependencies first: in reverse breadth-first order. */
	for (int i = n - 1; i >= 0; i--) {
		struct scope s = make_scope(v[i]);
		int r = instantiate(v[i], &s);
		free(s.v);
		if (r < 0)
			return NULL;
	}
	*newv = v;
	*newn = n;
	return root;
}

static void construct(struct dso **v, int n)
{
	for (int i = n - 1; i >= 0; i--) {
		if (v[i]->constructed)
			continue;
		v[i]->constructed = 1;
		if (v[i]->s_ctors) {
			dbg("constructors of %s", v[i]->name);
			((void (*)(void))(uintptr_t)v[i]->s_ctors)();
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Threads */

/* Load in this thread what other threads have loaded and it has not: every thread runs this before it
 * runs anything else (__dl_clone_hook), and when dlopen() has loaded something (__synccall). */
void __dl_sync(void)
{
	for (;;) {
		size_t have = (size_t)__wasm_dl(WASM_DL_COUNT, 0);
		size_t n = atomic_load_explicit(&dl_log_n, memory_order_acquire);
		if (have >= n)
			return;
		struct wasm_dl_desc desc = dl_log[have].desc;	/* (a copy: the host writes its timings into it) */
		long id = __wasm_dl(WASM_DL_LOAD, &desc);
		if (id != (long)have + 1)
			die("thread %d could not load log entry %zu (%ld)", gettid(), have, id);
		dbg("  %s: compile %.3f ms, instantiate %.3f ms, place %.3f ms",
		    dl_log[have].dso ? dl_log[have].dso->name : "(trampolines)", desc.us_compile / 1e3,
		    desc.us_instantiate / 1e3, desc.us_place / 1e3);
		if (dl_log[have].dso)
			bind_tls(dl_log[have].dso);
	}
}

/* Called by the runtime in a new task (thread, vfork child) before the task's own code. */
__attribute__((export_name("__dl_clone_hook"))) void __dl_clone_hook(void)
{
	sigset_t all, old;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &old);
	size_t before = (size_t)__wasm_dl(WASM_DL_COUNT, 0);
	struct timespec a, b;
	if (dl_debug)
		clock_gettime(CLOCK_MONOTONIC, &a);
	__dl_sync();
	if (dl_debug) {
		clock_gettime(CLOCK_MONOTONIC, &b);
		dbg("new thread: loaded %zu modules in %.3f ms", (size_t)__wasm_dl(WASM_DL_COUNT, 0) - before,
		    (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
	}
	pthread_sigmask(SIG_SETMASK, &old, 0);
}

static void sync_one(void *unused)
{
	(void)unused;
	__dl_sync();
}

/* ------------------------------------------------------------------------ */
/* dlfcn */

static struct dso *valid(void *h)
{
	for (struct dso *d = dl_head; d; d = d->next)
		if (d == h)
			return d;
	return NULL;
}

void *dlopen(const char *file, int mode)
{
	if (!file)
		return dl_program ? dl_program : dl_self;
	pthread_mutex_lock(&dl_lock);
	int cs;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
	__dl_sync();
	struct dso **v = NULL;
	int n = 0;
	size_t before = atomic_load(&dl_log_n);
	struct dso *d = load(file, dl_program, mode & RTLD_GLOBAL, &v, &n);
	if (d) {
		if (atomic_load(&dl_log_n) != before) {
			struct timespec a, b;
			clock_gettime(CLOCK_MONOTONIC, &a);
			__synccall(sync_one, 0);
			clock_gettime(CLOCK_MONOTONIC, &b);
			dbg("dlopen %s: every thread has it (%.3f ms)", file,
			    (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6);
		}
		construct(v, n);
		d->refcount++;
	}
	free(v);
	pthread_setcancelstate(cs, 0);
	pthread_mutex_unlock(&dl_lock);
	return d;
}

/* musl's dlclose: never unloads (and neither does this one: other threads' instances would have to
 * go at the same time); it only checks the handle. */
int __dl_invalid_handle(void *h)
{
	if (valid(h))
		return 0;
	__dl_seterr("Invalid library handle %p", h);
	return 1;
}

void *__dlsym(void *restrict h, const char *restrict name, void *restrict ra)
{
	(void)ra;
	struct scope s = { 0, 0 };
	if (h == RTLD_DEFAULT) {
		s = make_scope(NULL);
	} else if (h == RTLD_NEXT) {
		__dl_seterr("RTLD_NEXT is not supported");
		return 0;
	} else {
		struct dso *d = valid(h);
		if (!d) {
			__dl_seterr("Invalid library handle %p", h);
			return 0;
		}
		scope_group(&s, d);
	}
	void *r = 0;
	struct dso *w;
	struct sym *y;
	if ((y = lookup(&s, name, 0, &w)))
		r = (void *)(uintptr_t)y->slot;
	else if ((y = lookup(&s, name, 3, &w)) && !y->tls)
		r = (void *)(uintptr_t)(w->mem_base + y->value);
	else
		__dl_seterr("Symbol not found: %s", name);
	free(s.v);
	return r;
}

/* This musl's dlfcn.h redirects dlsym to __dlsym_time64 (_REDIR_TIME64); neither libc.a nor anything else has it. */
void *__dlsym_time64(void *restrict h, const char *restrict name)
{
	return __dlsym(h, name, 0);
}

int dladdr(const void *addr, Dl_info *info)
{
	uintptr_t a = (uintptr_t)addr;
	for (struct dso *d = dl_head; d; d = d->next) {
		int hit = d->mem_size && a >= d->mem_base && a < d->mem_base + d->mem_size;
		for (int i = 0; i < d->nsym && !hit; i++)
			if (d->sym[i].kind == 0 && d->sym[i].slot == a) {
				info->dli_sname = d->sym[i].name;
				info->dli_saddr = (void *)a;
				hit = 2;
			}
		if (hit) {
			info->dli_fname = d->name;
			info->dli_fbase = (void *)(uintptr_t)d->mem_base;
			if (hit == 1) {
				info->dli_sname = 0;
				info->dli_saddr = 0;
			}
			return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Starting a program */

/* ld-wasm itself: its exports are libc's, libc++'s and the dynamic linker's. Its bytes are where the
 * kernel mapped the file it ran (the start and end of code in /proc/self/stat). */
static void init_self(const char *argv0)
{
	struct dso *d = calloc(1, sizeof *d);
	d->name = strdup(argv0);
	d->id = 0;
	d->log = -1;
	d->global = 1;
	char buf[1024];
	int fd = open("/proc/self/stat", O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
	if (fd >= 0)
		close(fd);
	if (n <= 0)
		die("cannot read /proc/self/stat");
	buf[n] = 0;
	char *p = strrchr(buf, ')');
	unsigned long long start = 0, end = 0;
	for (int field = 2; p && *p; field++) {
		p = strchr(p, ' ');
		if (!p)
			break;
		p++;
		if (field + 1 == 26)
			start = strtoull(p, 0, 10);
		if (field + 1 == 27)
			end = strtoull(p, 0, 10);
	}
	if (!start || end <= start)
		die("cannot find my own module in memory");
	d->bytes = (const uint8_t *)(uintptr_t)start;
	d->len = end - start;
	if (parse(d) < 0)
		die("cannot parse my own module");
	d->mem_base = __dl_self_memory_base();
	dl_table_top = (uint64_t)__wasm_dl(WASM_DL_TABLE, 0);
	int nfunc = 0;
	uint64_t first;
	if (d->tablified) {
		/* Our own element segment has put them in the table (the runtime gives us __table_base 0). */
		first = d->slot_base;
		for (int i = 0; i < d->nsym; i++)
			if (d->sym[i].kind == 0)
				d->sym[i].slot = first + nfunc++;
		dl_head = dl_tail = dl_self = d;
	} else {
		/* Put them there: a placement in the log, which every thread replays. */
		first = dl_table_top;
		for (int i = 0; i < d->nsym; i++)
			if (d->sym[i].kind == 0)
				d->sym[i].slot = dl_table_top + nfunc++;
		struct log_entry e = { { 0, 0, 0, 0, dl_table_top + nfunc, dl_table_top, 0 }, NULL };
		dl_table_top += nfunc;
		dl_head = dl_tail = dl_self = d;
		host_load(&e);
	}
	dbg("ld-wasm: %zu bytes, %d exports (%d functions, in slots from %llu%s)", d->len, d->nsym, nfunc,
	    (unsigned long long)first, d->tablified ? ", by its own element segment" : "");
}

int main(int argc, char **argv)
{
	clock_gettime(CLOCK_MONOTONIC, &dl_t0);
	dl_debug = getenv("LD_DEBUG") != NULL;
	dl_path_env = getenv("LD_LIBRARY_PATH");
	dl_bind_slots = getenv("LD_BIND_SLOTS") != NULL;
	if (argc < 2) {
		fprintf(stderr, "usage: %s PROGRAM [ARGS...]\n", argv[0]);
		return 127;
	}
	init_self(argv[0]);
	struct dso **v = NULL;
	int n = 0;
	dl_program = load(argv[1], NULL, 1, &v, &n);
	if (!dl_program)
		die("%s", dlerror());
	construct(v, n);
	dbg("starting %s", argv[1]);
	if (dl_program->s_main_argc)
		exit(((int (*)(int, char **))(uintptr_t)dl_program->s_main_argc)(argc - 1, argv + 1));
	if (dl_program->s_main_void)
		exit(((int (*)(void))(uintptr_t)dl_program->s_main_void)());
	die("%s has no main", argv[1]);
}
