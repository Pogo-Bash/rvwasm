/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Translations kept outside the process (workstream jitcache).
 *
 * A program that starts translates the same code that it, or another program
 * using the same libraries, translated the last time: of the regions that a
 * C and a C++ compile, a shell loop and a Python start made, two thirds had
 * been made before by some process, and those were three fifths of the time
 * spent translating. So the modules are kept, by the host, where every
 * process of the machine finds them: a store of records, each found by a key
 * that this file computes from what the record was made from.
 *
 * What a record is made from, and what decides its key (jit.c,
 * cache_describe()): the region's blocks, where each is (the guest address:
 * the code jumps to absolute addresses, and a library is mostly loaded at the
 * same address in every process that has it; translating relative to where
 * it is loaded would win only a few per cent more, see tests/jitcache.sh),
 * how many instructions and bytes each has, the bytes themselves, and what
 * else changes the code made of them: the translator's switches, -v, the
 * host's fused multiply-add, and the translator's own source (the build puts
 * a hash of every source of rv into RV_JIT_SOURCE). The
 * record is that description followed by the function made of it (jit.c puts
 * it into a module, with others, when it is used). A record is used only if
 * its description is the region's byte for byte, so a key that two regions
 * share by chance does no harm: the second is translated as if there were no
 * record.
 *
 * Where the store is. NEVER anywhere the guest can write: a module from the
 * store is compiled and run as part of the emulator, so a store in the
 * guest's file system would let any program there plant WebAssembly that a
 * later process (root's, say) runs with the emulator's rights over its
 * memory. On Linux/Wasm the store belongs to the page (tools/patch-runtime.py,
 * "the JIT cache"): it lives in memory of the page's own, which no guest and
 * no program on the machine can address, and the page keeps it in the
 * origin's private file system between visits; a process reaches it only
 * through __rv_jit_cache(), which copies records in and out, and which
 * answers only this program: the runtime takes the SHA-256 of every
 * executable it loads, and any other native program on the machine is told
 * there is no store (any program could otherwise fill it with WebAssembly for
 * rv to run). The page knows rv's hash from the server that serves rv itself
 * (tools/stage.sh writes rv.sha256). Natively (for
 * testing, with RV_JIT_FAKE) it is a file named by RV_JIT_CACHE.
 *
 * What is kept, and what the engine keeps. The store keeps the translator's
 * output (a region's function, and the hint for finding it), not the engine's
 * compiled code: a compiled WebAssembly.Module can no longer be kept in
 * IndexedDB, and V8 keeps compiled code between page loads only for a module
 * compiled with compileStreaming() from a fetched URL, of at least 128 KB,
 * once its functions have been optimized; none of that fits modules made as a
 * program runs (this has not been tried with a Service Worker that answers
 * with a Response of bytes it has: tools/v8cache/drive.js on branch jitcache
 * does, and nobody has run it). Making the functions is the part of a start
 * that the store takes away (Konsole's start under node-machine took 20 ms
 * compiling its regions and 1.5 to 6 s making them).
 *
 * Within a page load the engine's work is shared by the page instead
 * (tools/patch-runtime.py, 29): the regions waiting to be batched end a batch
 * by what they are (jit.c, JIT_BATCH_MASK), in the order of their hashes, so a
 * program started again makes mostly the same batches, byte for byte; the
 * host takes a batch's module from the page when another process compiled the
 * same bytes (keyed by their content), and the page keeps the modules (up to
 * ?jitkeep=MB, the least used going first), so the optimized code V8 made for
 * a batch in one process is the one the next process runs.
 *
 * RV_JIT_CACHE=0 turns it off.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rv.h"
#include "jc_source.h"	/* RV_JIT_SOURCE, made by the Makefile */

#ifdef __wasm__
/*
 * The page's store (tools/patch-runtime.py). op 0: the record of `key` (16
 * bytes) into [buf, buf + len), returns its length (which may be more than
 * len: then nothing was copied) or -1 if there is none. op 1: keep [buf, buf
 * + len) as the record of `key`; returns 0, or -1 if it was not kept. -2 for
 * any op: the page has no store.
 */
long __rv_jit_cache(long op, const void *key, void *buf, unsigned long len);
#endif

/* ------------------------------------------------------------------ hashing */

static uint64_t mix(uint64_t h)
{
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;
	return h;
}

/* 128 bits of hash of [data, data + len): two lanes that see the same words differently. */
void rv_jc_key(const void *data, size_t len, uint8_t key[RV_JC_KEY])
{
	const uint8_t *p = data;
	uint64_t a = 0x9e3779b97f4a7c15ULL ^ (uint64_t)RV_JIT_SOURCE ^ len, b = 0x6a09e667f3bcc909ULL + len;
	size_t i = 0;

	for (; i + 8 <= len; i += 8) {
		uint64_t w;

		memcpy(&w, p + i, 8);
		a = (a ^ w) * 0x100000001b3ULL;
		a = (a << 27 | a >> 37) ^ (a >> 29);
		b = (b + w) * 0xc2b2ae3d27d4eb4fULL;
		b = (b << 31 | b >> 33) + 0x165667b19e3779f9ULL;
	}
	if (i < len) {
		uint64_t w = 0;

		memcpy(&w, p + i, len - i);
		a = (a ^ w) * 0x100000001b3ULL;
		b = (b + w) * 0xc2b2ae3d27d4eb4fULL;
	}
	a = mix(a ^ (uint64_t)RV_JIT_SOURCE);
	b = mix(b ^ a);
	memcpy(key, &a, 8);
	memcpy(key + 8, &b, 8);
}

/* ------------------------------------------------------------------- native */

#ifndef __wasm__
/*
 * The file: records one after the other, each a key, a length (4 bytes) and
 * the record. Its keys are read once, when first asked, and a record is read
 * from the file when it is asked for (a store that every block of a g++ made
 * is gigabytes, and every process of a compile has it open); what this
 * process adds is appended (one write() each, with O_APPEND, so processes that
 * add at the same time do not mix their records up) and kept in memory. Only
 * for testing the rest natively.
 */
struct jc_entry {
	uint8_t key[RV_JC_KEY];
	uint32_t len;
	uint8_t *data;		/* what this process added, or NULL: in the file at `at` */
	off_t at;		/* 0: an empty place (no record starts at 0, the first key is there) */
};

static pthread_mutex_t jc_lock = PTHREAD_MUTEX_INITIALIZER;
static struct jc_entry *jc_table;
static uint32_t jc_bits, jc_used;
static int jc_fd = -1, jc_loaded;

static uint32_t jc_slot(const uint8_t key[RV_JC_KEY], uint32_t bits)
{
	uint32_t h;

	memcpy(&h, key, 4);
	return h & ((1u << bits) - 1);
}

/* A record: `data` (copied) if this process made it, else where it is in the file. */
static void jc_insert(const uint8_t key[RV_JC_KEY], const uint8_t *data, uint32_t len, off_t at)
{
	uint32_t i;

	if (2 * (jc_used + 1) > (1u << jc_bits)) {
		uint32_t bits = jc_bits ? jc_bits + 1 : 12;
		struct jc_entry *t = calloc((size_t)1 << bits, sizeof(*t));

		if (!t)
			return;
		for (uint32_t k = 0; jc_table && k < 1u << jc_bits; k++) {
			if (!jc_table[k].at)
				continue;
			for (i = jc_slot(jc_table[k].key, bits); t[i].at; i = (i + 1) & ((1u << bits) - 1))
				;
			t[i] = jc_table[k];
		}
		free(jc_table);
		jc_table = t;
		jc_bits = bits;
	}
	for (i = jc_slot(key, jc_bits); jc_table[i].at; i = (i + 1) & ((1u << jc_bits) - 1))
		if (!memcmp(jc_table[i].key, key, RV_JC_KEY))
			return;
	if (data) {
		if (!(jc_table[i].data = malloc(len ? len : 1)))
			return;
		memcpy(jc_table[i].data, data, len);
	}
	memcpy(jc_table[i].key, key, RV_JC_KEY);
	jc_table[i].len = len;
	jc_table[i].at = data ? -1 : at;
	jc_used++;
}

/* With the lock: the file's keys read, once. 0, or -1 if there is no store. */
static int jc_load(void)
{
	const char *path = getenv("RV_JIT_CACHE");
	uint8_t head[RV_JC_KEY + 4];
	off_t size;

	if (jc_loaded)
		return jc_fd >= 0 ? 0 : -1;
	jc_loaded = 1;
	if (!path || !*path || !strcmp(path, "0"))
		return -1;
	jc_fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (jc_fd < 0)
		return -1;
	/* (Move the descriptor out of the guest's way: it sees descriptors from 0 up as its own.) */
	if (jc_fd < 900) {
		int high = fcntl(jc_fd, F_DUPFD_CLOEXEC, 900);

		if (high >= 0) {
			close(jc_fd);
			jc_fd = high;
		}
	}
	size = lseek(jc_fd, 0, SEEK_END);
	for (off_t at = 0;;) {
		uint32_t len;

		if (pread(jc_fd, head, sizeof(head), at) != (ssize_t)sizeof(head))
			break;
		memcpy(&len, head + RV_JC_KEY, 4);
		/* (A record cut short, by a process that was killed as it wrote, ends what is read.) */
		if (len > (64u << 20) || at + (off_t)sizeof(head) + len > size)
			break;
		jc_insert(head, NULL, len, at + (off_t)sizeof(head));
		at += (off_t)sizeof(head) + len;
	}
	return 0;
}

static long jc_get(const uint8_t key[RV_JC_KEY], void *buf, size_t cap)
{
	long ret = -1;

	pthread_mutex_lock(&jc_lock);
	if (!jc_load() && jc_table) {
		for (uint32_t i = jc_slot(key, jc_bits); jc_table[i].at; i = (i + 1) & ((1u << jc_bits) - 1)) {
			const struct jc_entry *e = &jc_table[i];

			if (!memcmp(e->key, key, RV_JC_KEY)) {
				ret = (long)e->len;
				if (e->len <= cap) {
					if (e->data)
						memcpy(buf, e->data, e->len);
					else if (pread(jc_fd, buf, e->len, e->at) != (ssize_t)e->len)
						ret = -1;
				}
				break;
			}
		}
	}
	pthread_mutex_unlock(&jc_lock);
	return ret;
}

static long jc_put(const uint8_t key[RV_JC_KEY], const void *rec, size_t len)
{
	long ret = -1;

	pthread_mutex_lock(&jc_lock);
	if (!jc_load()) {
		uint8_t *line = malloc(RV_JC_KEY + 4 + len);
		uint32_t len32 = (uint32_t)len;

		if (line) {
			memcpy(line, key, RV_JC_KEY);
			memcpy(line + RV_JC_KEY, &len32, 4);
			memcpy(line + RV_JC_KEY + 4, rec, len);
			if (write(jc_fd, line, RV_JC_KEY + 4 + len) == (ssize_t)(RV_JC_KEY + 4 + len)) {
				jc_insert(key, rec, len32, -1);
				ret = 0;
			}
			free(line);
		}
	}
	pthread_mutex_unlock(&jc_lock);
	return ret;
}
#endif

/* ---------------------------------------------------------------- interface */

/* Is there a store? Asked once per process. */
int rv_jc_on(void)
{
	static int on = -1;

	if (on < 0) {
		const char *v = getenv("RV_JIT_CACHE");

#ifdef __wasm__
		uint8_t key[RV_JC_KEY] = { 0 };

		on = !(v && !strcmp(v, "0")) && __rv_jit_cache(0, key, NULL, 0) != -2;
#else
		on = v && *v && strcmp(v, "0");
#endif
	}
	return on;
}

/* The record of `key` into buf (at most cap bytes): its length, which is more than cap if it did not fit; -1 if none. */
long rv_jc_get(const uint8_t key[RV_JC_KEY], void *buf, size_t cap)
{
#ifdef __wasm__
	long len = __rv_jit_cache(0, key, buf, cap);

	return len < 0 ? -1 : len;
#else
	return jc_get(key, buf, cap);
#endif
}

/* Keep [rec, rec + len) as the record of `key`. 0, or -1 if it was not kept. */
long rv_jc_put(const uint8_t key[RV_JC_KEY], const void *rec, size_t len)
{
#ifdef __wasm__
	return __rv_jit_cache(1, key, (void *)(uintptr_t)rec, len) < 0 ? -1 : 0;
#else
	return jc_put(key, rec, len);
#endif
}
