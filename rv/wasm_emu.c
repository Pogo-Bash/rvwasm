/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * An interpreter for the WebAssembly the JIT makes, for the native build:
 * RV_JIT_EMU=1 (jit.c). Where the browser compiles a region's module and puts
 * its function into the program's table, this parses the module and keeps it,
 * and where translated code would be called, this runs it, instruction by
 * instruction. Memory instructions take host addresses, as they do on
 * Linux/Wasm, where the emulator and its translated code share one memory:
 * natively that is the process's own memory, so the translated code acts on
 * the very same *cpu and guest pages as the interpreter does.
 *
 * It knows the instructions jit.c emits (and a few more), not all of
 * WebAssembly: a module that uses something else fails to install, and the
 * JIT goes on without it. Of the vector instructions it knows only the three
 * of the fused multiply-add (fma_probe() in jit.c), and those as if a vector
 * had one lane: the code splats scalars and extracts lane 0, so that is exact.
 *
 * With RV_JIT_CHECK=1 on top, every block of translated code is compared
 * with the interpreter (checked_call() in jit.c): a native test of the code
 * generator, with no browser. Speed is not the point; correctness is, so
 * this stays simple: a stack machine over the instruction bytes, with the
 * matching `end` of every block found once per function and remembered.
 */
#ifndef __wasm__
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rv.h"
#include "wasm_emu.h"

#define EMU_MAX_TYPES	8
#define EMU_MAX_IMPORTS	32
#define EMU_STACK	1024
#define EMU_LABELS	1024
#define EMU_FIRST_SLOT	16

struct emu_type {
	uint32_t nparams, nresults;
};

struct emu_func {
	uint32_t type;
	const uint8_t *locals;	/* the local declarations, then the code */
	const uint8_t *code;
	const uint8_t *end;	/* one past the function's last byte (its final `end`) */
	uint32_t nlocals;	/* parameters included */
	uint32_t *jump;		/* by offset from `code`: for block/loop/if the offset past the matching `end`; at the type byte of an `if`, the offset past its `else` (0: none) */
};

struct emu_module {
	uint8_t *bytes;
	size_t len;
	struct emu_type types[EMU_MAX_TYPES];
	uint32_t ntypes;
	struct {
		uint32_t type;
		void *fn;
	} imports[EMU_MAX_IMPORTS];
	uint32_t nimports;
	struct emu_func *funcs;
	uint32_t nfuncs;
	uint32_t export_r;	/* function index (imports counted), or ~0u */
	uint32_t refs;		/* table slots that name it */
};

struct emu_slot {
	struct emu_module *m;
	uint32_t func;		/* ~0u: empty */
};

/*
 * The table is the thread's, as in the browser, where every thread of the
 * guest is a Worker with an instance and a function table of its own: slots
 * mean something only to the thread whose map names them (jit.c), and no
 * thread ever installs into, empties or calls another's.
 */
static __thread struct emu_slot *emu_table;
static __thread uint32_t emu_table_len;
static uint64_t emu_stat_installed, emu_stat_calls, emu_stat_tails;	/* all threads: counted atomically */
#define COUNT(stat) __atomic_add_fetch(&(stat), 1, __ATOMIC_RELAXED)

/* The functions a module may import, by name. */
struct emu_helper {
	const char *name;
	void *fn;
};

uint64_t rv_op_mulh(uint64_t a, uint64_t b);
uint64_t rv_op_mulhsu(uint64_t a, uint64_t b);
uint64_t rv_op_mulhu(uint64_t a, uint64_t b);
uint64_t rv_op_div(uint64_t a, uint64_t b);
uint64_t rv_op_divu(uint64_t a, uint64_t b);
uint64_t rv_op_rem(uint64_t a, uint64_t b);
uint64_t rv_op_remu(uint64_t a, uint64_t b);
uint64_t rv_op_divw(uint64_t a, uint64_t b);
uint64_t rv_op_divuw(uint64_t a, uint64_t b);
uint64_t rv_op_remw(uint64_t a, uint64_t b);
uint64_t rv_op_remuw(uint64_t a, uint64_t b);

static const struct emu_helper emu_helpers[] = {
	{ "rv_op_mulh", (void *)rv_op_mulh }, { "rv_op_mulhsu", (void *)rv_op_mulhsu },
	{ "rv_op_mulhu", (void *)rv_op_mulhu }, { "rv_op_div", (void *)rv_op_div },
	{ "rv_op_divu", (void *)rv_op_divu }, { "rv_op_rem", (void *)rv_op_rem },
	{ "rv_op_remu", (void *)rv_op_remu }, { "rv_op_divw", (void *)rv_op_divw },
	{ "rv_op_divuw", (void *)rv_op_divuw }, { "rv_op_remw", (void *)rv_op_remw },
	{ "rv_op_remuw", (void *)rv_op_remuw },
	{ "rv_jit_fp", (void *)rv_jit_fp }, { "rv_jit_csr", (void *)rv_jit_csr },
	{ "rv_jit_fused64", (void *)rv_jit_fused64 }, { "rv_jit_reserve", (void *)rv_jit_reserve },
	{ NULL, NULL },
};

/* ------------------------------------------------------------------ reading */

struct rd {
	const uint8_t *p, *end;
	int bad;
};

static uint64_t rd_uleb(struct rd *r)
{
	uint64_t v = 0;
	unsigned shift = 0;

	for (;;) {
		uint8_t b;

		if (r->p >= r->end || shift > 63) {
			r->bad = 1;
			return 0;
		}
		b = *r->p++;
		v |= (uint64_t)(b & 0x7f) << shift;
		if (!(b & 0x80))
			return v;
		shift += 7;
	}
}

static int64_t rd_sleb(struct rd *r)
{
	int64_t v = 0;
	unsigned shift = 0;
	uint8_t b;

	do {
		if (r->p >= r->end || shift > 63) {
			r->bad = 1;
			return 0;
		}
		b = *r->p++;
		v |= (int64_t)(b & 0x7f) << shift;
		shift += 7;
	} while (b & 0x80);
	if (shift < 64 && (b & 0x40))
		v |= -((int64_t)1 << shift);
	return v;
}

static uint8_t rd_byte(struct rd *r)
{
	if (r->p >= r->end) {
		r->bad = 1;
		return 0;
	}
	return *r->p++;
}

static const uint8_t *rd_bytes(struct rd *r, uint64_t n)
{
	const uint8_t *p = r->p;

	if (n > (uint64_t)(r->end - r->p)) {
		r->bad = 1;
		return r->end;
	}
	r->p += n;
	return p;
}

/* ------------------------------------------------------------------ parsing */

static void module_free(struct emu_module *m)
{
	if (!m)
		return;
	for (uint32_t i = 0; m->funcs && i < m->nfuncs; i++)
		free(m->funcs[i].jump);
	free(m->funcs);
	free(m->bytes);
	free(m);
}

/* Skip the immediates of one instruction (the pre-pass that matches blocks with their ends). */
static int skip_insn(struct rd *r, uint8_t op)
{
	switch (op) {
	case 0x00: case 0x01: case 0x05: case 0x0b: case 0x0f: case 0x1a: case 0x1b:
		return 0;
	case 0x02: case 0x03: case 0x04:
		rd_byte(r);	/* (block types used here are single bytes) */
		return 0;
	case 0x0c: case 0x0d: case 0x10: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
		rd_uleb(r);
		return 0;
	case 0x0e: {
		uint64_t n = rd_uleb(r);

		for (uint64_t i = 0; i <= n && !r->bad; i++)
			rd_uleb(r);
		return 0;
	}
	case 0x11: case 0x13:
		rd_uleb(r);
		rd_uleb(r);
		return 0;
	case 0x41:
		rd_sleb(r);
		return 0;
	case 0x42:
		rd_sleb(r);
		return 0;
	case 0x43:
		rd_bytes(r, 4);
		return 0;
	case 0x44:
		rd_bytes(r, 8);
		return 0;
	case 0xfc:
		rd_uleb(r);
		return 0;
	case 0xfd: {
		uint64_t sub = rd_uleb(r);

		if (sub == 0x1d)
			rd_byte(r);
		return 0;
	}
	case 0xfe:
		rd_uleb(r);
		rd_uleb(r);
		rd_uleb(r);
		return 0;
	default:
		if (op >= 0x28 && op <= 0x3e) {	/* loads and stores: align, offset */
			rd_uleb(r);
			rd_uleb(r);
			return 0;
		}
		if (op >= 0x45 && op <= 0xc4)	/* numeric, no immediates */
			return 0;
		return -1;
	}
}

/* Find the matching `end` (and `else`) of every block, loop and if in f. 0, or -1 for a malformed body. */
static int func_jumps(struct emu_func *f)
{
	struct rd r = { f->code, f->end, 0 };
	size_t len = (size_t)(f->end - f->code);
	uint32_t *jump = calloc(len + 1, sizeof(*jump));
	uint32_t open[EMU_LABELS];
	int depth = 0;

	if (!jump)
		return -1;
	while (r.p < r.end && !r.bad) {
		uint32_t at = (uint32_t)(r.p - f->code);
		uint8_t op = rd_byte(&r);

		switch (op) {
		case 0x02: case 0x03: case 0x04:
			if (depth == EMU_LABELS)
				goto bad;
			open[depth++] = at;
			rd_byte(&r);
			break;
		case 0x05:
			if (!depth)
				goto bad;
			jump[open[depth - 1] + 1] = (uint32_t)(r.p - f->code);
			break;
		case 0x0b:
			if (!depth) {
				/* The function's own end. */
				if (r.p != r.end)
					goto bad;
				break;
			}
			jump[open[--depth]] = (uint32_t)(r.p - f->code);
			break;
		default:
			if (skip_insn(&r, op))
				goto bad;
		}
	}
	if (r.bad || depth)
		goto bad;
	f->jump = jump;
	return 0;
bad:
	free(jump);
	return -1;
}

static struct emu_module *module_parse(const uint8_t *data, size_t len)
{
	struct emu_module *m = calloc(1, sizeof(*m));
	struct rd r;
	uint32_t ncode = 0;
	int have_funcs = 0;

	if (!m)
		return NULL;
	if (!(m->bytes = malloc(len))) {
		free(m);
		return NULL;
	}
	memcpy(m->bytes, data, len);
	m->len = len;
	m->export_r = ~0u;
	r = (struct rd){ m->bytes, m->bytes + len, 0 };
	if (len < 8 || memcmp(m->bytes, "\0asm\1\0\0\0", 8))
		goto bad;
	r.p += 8;

	while (r.p < r.end && !r.bad) {
		uint8_t id = rd_byte(&r);
		uint64_t size = rd_uleb(&r);
		struct rd s = { r.p, rd_bytes(&r, size) + size, 0 };

		if (r.bad)
			goto bad;
		switch (id) {
		case 1: {	/* types */
			uint64_t n = rd_uleb(&s);

			if (n > EMU_MAX_TYPES)
				goto bad;
			for (uint64_t i = 0; i < n; i++) {
				uint64_t np, nr;

				if (rd_byte(&s) != 0x60)
					goto bad;
				np = rd_uleb(&s);
				rd_bytes(&s, np);
				nr = rd_uleb(&s);
				rd_bytes(&s, nr);
				if (np > 8 || nr > 1)
					goto bad;
				m->types[i].nparams = (uint32_t)np;
				m->types[i].nresults = (uint32_t)nr;
			}
			m->ntypes = (uint32_t)n;
			break;
		}
		case 2: {	/* imports */
			uint64_t n = rd_uleb(&s);

			for (uint64_t i = 0; i < n && !s.bad; i++) {
				uint64_t mlen = rd_uleb(&s);
				const uint8_t *mod = rd_bytes(&s, mlen);
				uint64_t nlen = rd_uleb(&s);
				const uint8_t *name = rd_bytes(&s, nlen);
				uint8_t kind = rd_byte(&s);

				(void)mod;
				switch (kind) {
				case 0x00: {	/* a function: of the emulator's, by name */
					uint64_t type = rd_uleb(&s);
					const struct emu_helper *h;

					if (m->nimports == EMU_MAX_IMPORTS || type >= m->ntypes)
						goto bad;
					for (h = emu_helpers; h->name; h++)
						if (strlen(h->name) == nlen && !memcmp(h->name, name, nlen))
							break;
					if (!h->name)
						goto bad;
					m->imports[m->nimports].type = (uint32_t)type;
					m->imports[m->nimports].fn = h->fn;
					m->nimports++;
					break;
				}
				case 0x01:	/* a table: reftype, limits */
					rd_byte(&s);
					if (rd_byte(&s) & 1) {
						rd_uleb(&s);
						rd_uleb(&s);
					} else {
						rd_uleb(&s);
					}
					break;
				case 0x02:	/* a memory: limits (flags say whether there is a maximum) */
					if (rd_byte(&s) & 1) {
						rd_uleb(&s);
						rd_uleb(&s);
					} else {
						rd_uleb(&s);
					}
					break;
				default:
					goto bad;
				}
			}
			break;
		}
		case 3: {	/* functions: their types */
			uint64_t n = rd_uleb(&s);

			if (n > 100000 || !(m->funcs = calloc(n ? n : 1, sizeof(*m->funcs))))
				goto bad;
			m->nfuncs = (uint32_t)n;
			for (uint64_t i = 0; i < n; i++) {
				uint64_t t = rd_uleb(&s);

				if (t >= m->ntypes)
					goto bad;
				m->funcs[i].type = (uint32_t)t;
			}
			have_funcs = 1;
			break;
		}
		case 7: {	/* exports: "r" */
			uint64_t n = rd_uleb(&s);

			for (uint64_t i = 0; i < n && !s.bad; i++) {
				uint64_t nlen = rd_uleb(&s);
				const uint8_t *name = rd_bytes(&s, nlen);
				uint8_t kind = rd_byte(&s);
				uint64_t index = rd_uleb(&s);

				if (kind == 0 && nlen == 1 && name[0] == 'r')
					m->export_r = (uint32_t)index;
			}
			break;
		}
		case 9: {	/* elements: active segments into table 0 with i32.const offsets (batch_make()) */
			uint64_t n = rd_uleb(&s);

			for (uint64_t i = 0; i < n && !s.bad; i++) {
				uint64_t flags = rd_uleb(&s), count;
				int64_t at;

				if (flags != 0 || rd_byte(&s) != 0x41)
					goto bad;
				at = rd_sleb(&s);
				if (rd_byte(&s) != 0x0b)
					goto bad;
				count = rd_uleb(&s);
				for (uint64_t k = 0; k < count && !s.bad; k++) {
					uint64_t fn = rd_uleb(&s);

					if (at < EMU_FIRST_SLOT || (uint64_t)at >= emu_table_len || fn < m->nimports ||
					    fn - m->nimports >= m->nfuncs)
						goto bad;
					/* (Put into the table once the module is parsed: emu_install() does it from the segments again.) */
					at++;
				}
			}
			break;
		}
		case 10: {	/* code */
			uint64_t n = rd_uleb(&s);

			if (!have_funcs || n != m->nfuncs)
				goto bad;
			for (uint64_t i = 0; i < n && !s.bad; i++) {
				uint64_t bsize = rd_uleb(&s);
				struct rd body = { s.p, rd_bytes(&s, bsize) + bsize, 0 };
				struct emu_func *f = &m->funcs[i];
				uint64_t groups, nlocals = m->types[f->type].nparams;

				if (s.bad)
					goto bad;
				f->locals = body.p;
				f->end = body.end;
				groups = rd_uleb(&body);
				for (uint64_t g = 0; g < groups && !body.bad; g++) {
					uint64_t count = rd_uleb(&body);

					rd_byte(&body);
					nlocals += count;
					if (nlocals > 50000)
						goto bad;
				}
				if (body.bad)
					goto bad;
				f->code = body.p;
				f->nlocals = (uint32_t)nlocals;
				if (func_jumps(f))
					goto bad;
			}
			ncode = (uint32_t)n;
			break;
		}
		default:	/* custom sections, and the others: of no interest */
			break;
		}
		if (s.bad)
			goto bad;
		r.p = s.end;
	}
	if (r.bad || ncode != m->nfuncs)
		goto bad;
	return m;
bad:
	module_free(m);
	return NULL;
}

/* The element segments again, this time putting the functions into the table. */
static void module_elements(struct emu_module *m)
{
	struct rd r = { m->bytes + 8, m->bytes + m->len, 0 };

	while (r.p < r.end && !r.bad) {
		uint8_t id = rd_byte(&r);
		uint64_t size = rd_uleb(&r);
		struct rd s = { r.p, rd_bytes(&r, size) + size, 0 };

		if (id == 9) {
			uint64_t n = rd_uleb(&s);

			for (uint64_t i = 0; i < n && !s.bad; i++) {
				int64_t at;
				uint64_t count;

				rd_uleb(&s);
				rd_byte(&s);
				at = rd_sleb(&s);
				rd_byte(&s);
				count = rd_uleb(&s);
				for (uint64_t k = 0; k < count && !s.bad; k++, at++) {
					uint64_t fn = rd_uleb(&s);
					struct emu_slot *slot = &emu_table[at];

					if (slot->m && slot->m != m && --slot->m->refs == 0)
						module_free(slot->m);
					slot->m = m;
					slot->func = (uint32_t)fn;
					m->refs++;
				}
			}
		}
		r.p = s.end;
	}
}

/* ---------------------------------------------------------------- the table */

static int table_room(uint32_t slot)
{
	uint32_t len = emu_table_len ? emu_table_len : 256;
	struct emu_slot *t;

	if (slot < emu_table_len)
		return 0;
	while (len <= slot)
		len *= 2;
	if (!(t = realloc(emu_table, len * sizeof(*t))))
		return -1;
	for (uint32_t i = emu_table_len; i < len; i++) {
		t[i].m = NULL;
		t[i].func = ~0u;
	}
	emu_table = t;
	emu_table_len = len;
	return 0;
}

static void slot_clear(uint32_t slot)
{
	struct emu_slot *s;

	if (slot >= emu_table_len)
		return;
	s = &emu_table[slot];
	if (s->m && --s->m->refs == 0)
		module_free(s->m);
	s->m = NULL;
	s->func = ~0u;
}

/*
 * What __rv_jit_install() is on Linux/Wasm (tools/patch-runtime.py): a
 * module's "r" into table slot `slot`, or into a new one if it is negative;
 * with a length of 0, empty the slot (-2: the budget query, which this host
 * has no answer to: 0); with slot -16 - n, a batch of n regions whose
 * element segments say where its functions go. Returns the slot, or -1.
 */
long rv_emu_install(const void *code, unsigned long len, long slot)
{
	struct emu_module *m;

	if (!len) {
		if (slot == -2)
			return 0;
		if (slot >= 0)
			slot_clear((uint32_t)slot);
		return 0;
	}
	if (table_room(EMU_FIRST_SLOT))
		return -1;
	if (!(m = module_parse(code, len)))
		return -1;
	COUNT(emu_stat_installed);
	if (slot <= -16) {
		module_elements(m);
		if (!m->refs) {
			module_free(m);
			return -1;
		}
		return 0;
	}
	if (m->export_r == ~0u || m->export_r < m->nimports || m->export_r - m->nimports >= m->nfuncs) {
		module_free(m);
		return -1;
	}
	if (slot < 0) {
		uint32_t at = EMU_FIRST_SLOT;

		while (at < emu_table_len && emu_table[at].m)
			at++;
		if (table_room(at)) {
			module_free(m);
			return -1;
		}
		slot = at;
	} else if (table_room((uint32_t)slot)) {
		module_free(m);
		return -1;
	}
	slot_clear((uint32_t)slot);
	emu_table[slot].m = m;
	emu_table[slot].func = m->export_r;
	m->refs = 1;
	return slot;
}

/* ---------------------------------------------------------------- execution */

struct label {
	const uint8_t *target;	/* where `br` to it goes */
	uint32_t height;	/* operand stack height at entry */
	uint8_t arity;		/* values a `br` carries: 1 for a block with an i64 result */
	uint8_t is_loop;
};

/* The result of exec(): the function returned, or asked for a tail call. */
struct tail {
	int wanted;
	uint32_t slot;
	uint64_t args[8];
	uint32_t nargs;
};

static uint64_t f64_bits(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }
static double bits_f64(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static uint32_t f32_bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static float bits_f32(uint64_t v) { float f; uint32_t w = (uint32_t)v; memcpy(&f, &w, 4); return f; }

static int64_t sat_s(double d, int64_t lo, int64_t hi)
{
	if (d != d)
		return 0;
	if (d <= (double)lo)
		return lo;
	if (d >= (double)hi)
		return hi;
	return (int64_t)d;
}

static uint64_t sat_u(double d, uint64_t hi)
{
	if (d != d || d <= 0)
		return 0;
	if (d >= (double)hi)
		return hi;
	return (uint64_t)d;
}

/* Floating-point results: the host's NaN bits are fine (translated code never keeps a NaN as a result). */
#define F64OP(expr) do { double y_ = bits_f64(POP()), x_ = bits_f64(POP()); PUSH(f64_bits(expr)); } while (0)
#define F32OP(expr) do { float y_ = bits_f32(POP()), x_ = bits_f32(POP()); PUSH(f32_bits(expr)); } while (0)
#define F64CMP(expr) do { double y_ = bits_f64(POP()), x_ = bits_f64(POP()); PUSH((expr) ? 1 : 0); } while (0)
#define F32CMP(expr) do { float y_ = bits_f32(POP()), x_ = bits_f32(POP()); PUSH((expr) ? 1 : 0); } while (0)

static int trap(const char *what, const struct emu_func *f, const uint8_t *pc)
{
	fprintf(stderr, "rv: jit emu: %s at offset %u of the function\n", what, (unsigned)(pc - f->code));
	return -1;
}

/*
 * Run function `index` of m with `args`. Returns 0 (the result, if any, in
 * *result), 1 for a tail call (in *tail), -1 for a trap.
 */
static int exec(struct emu_module *m, uint32_t index, const uint64_t *args, uint64_t *result, struct tail *tail, int depth)
{
	const struct emu_func *f;
	const struct emu_type *type;
	uint64_t stack[EMU_STACK], *sp = stack;
	uint64_t *locals;
	struct label labels[EMU_LABELS];
	int nlabels = 0;
	const uint8_t *pc;
	struct rd r;

	if (index < m->nimports) {
		const struct emu_type *t = &m->types[m->imports[index].type];

		if (t->nparams == 2)
			*result = ((uint64_t (*)(uint64_t, uint64_t))m->imports[index].fn)(args[0], args[1]);
		else
			*result = ((uint64_t (*)(uint64_t, uint64_t, uint64_t))m->imports[index].fn)(args[0], args[1], args[2]);
		return 0;
	}
	if (depth > 64)
		return -1;
	f = &m->funcs[index - m->nimports];
	type = &m->types[f->type];
	locals = calloc(f->nlocals ? f->nlocals : 1, sizeof(*locals));
	if (!locals)
		return -1;
	memcpy(locals, args, type->nparams * sizeof(*locals));

#define PUSH(v) do { uint64_t pv_ = (v); if (sp == stack + EMU_STACK) goto overflow; *sp++ = pv_; } while (0)
#define POP() (*--sp)
#define TOP() (sp[-1])
#define LABEL_PUSH(tgt, ar, loop) do { \
		if (nlabels == EMU_LABELS) goto overflow; \
		labels[nlabels].target = (tgt); labels[nlabels].height = (uint32_t)(sp - stack); \
		labels[nlabels].arity = (ar); labels[nlabels].is_loop = (loop); nlabels++; } while (0)

	r = (struct rd){ f->code, f->end, 0 };
	for (;;) {
		uint8_t op;
		uint64_t brn;	/* the label a branch goes to */

		pc = r.p;
		if (r.p >= r.end || r.bad) {
			free(locals);
			return trap("ran off the end", f, pc);
		}
		op = *r.p++;
		switch (op) {
		case 0x00:	/* unreachable */
			free(locals);
			return trap("unreachable", f, pc);
		case 0x01:	/* nop */
			break;
		case 0x02: case 0x03: {	/* block, loop */
			uint8_t bt = rd_byte(&r);
			uint32_t at = (uint32_t)(pc - f->code);

			LABEL_PUSH(op == 0x03 ? r.p : f->code + f->jump[at], bt == 0x40 ? 0 : 1, op == 0x03);
			if (op == 0x03)
				labels[nlabels - 1].arity = 0;	/* (a br to a loop carries its parameters: none here) */
			break;
		}
		case 0x04: {	/* if */
			uint8_t bt = rd_byte(&r);
			uint32_t at = (uint32_t)(pc - f->code);
			uint32_t els = f->jump[at + 1];
			uint64_t cond = POP();

			if (cond) {
				LABEL_PUSH(f->code + f->jump[at], bt == 0x40 ? 0 : 1, 0);
			} else if (els) {
				LABEL_PUSH(f->code + f->jump[at], bt == 0x40 ? 0 : 1, 0);
				r.p = f->code + els;
			} else {
				r.p = f->code + f->jump[at];
			}
			break;
		}
		case 0x05:	/* else: the end of the then-branch */
			if (!nlabels)
				goto malformed;
			r.p = labels[--nlabels].target;
			break;
		case 0x0b:	/* end */
			if (!nlabels) {
				/* The function's. */
				if (type->nresults)
					*result = POP();
				free(locals);
				return 0;
			}
			nlabels--;
			break;
		case 0x0c: case 0x0d:	/* br, br_if */
			brn = rd_uleb(&r);
			if (op == 0x0d && !POP())
				break;
		branch:
			if (brn >= (uint64_t)nlabels)
				goto malformed;
			{
				struct label *l = &labels[nlabels - 1 - brn];
				uint64_t carry = l->arity ? TOP() : 0;

				sp = stack + l->height;
				if (l->arity)
					PUSH(carry);
				r.p = l->target;
				nlabels -= (int)brn + (l->is_loop ? 0 : 1);
			}
			break;
		case 0x0e: {	/* br_table: count targets, then the default; the index on the stack */
			uint64_t count = rd_uleb(&r), i = (uint32_t)POP();

			brn = 0;
			for (uint64_t k = 0; k <= count; k++) {
				uint64_t t = rd_uleb(&r);

				if (k == i || (k == count && i >= count))
					brn = t;
			}
			goto branch;
		}
		case 0x0f:	/* return */
			if (type->nresults)
				*result = POP();
			free(locals);
			return 0;
		case 0x10: {	/* call */
			uint64_t fn = rd_uleb(&r), a[8], res = 0;
			const struct emu_type *t;
			int ret;

			if (fn >= m->nimports + m->nfuncs)
				goto malformed;
			t = fn < m->nimports ? &m->types[m->imports[fn].type] : &m->types[m->funcs[fn - m->nimports].type];
			if ((uint64_t)(sp - stack) < t->nparams)
				goto malformed;
			sp -= t->nparams;
			memcpy(a, sp, t->nparams * sizeof(*a));
			if (fn >= m->nimports)
				COUNT(emu_stat_calls);
			ret = exec(m, (uint32_t)fn, a, &res, tail, depth + 1);
			if (ret) {
				free(locals);
				return ret;
			}
			if (t->nresults)
				PUSH(res);
			break;
		}
		case 0x13: {	/* return_call_indirect: type, table; the index on top of the stack, the arguments under it */
			uint64_t t = rd_uleb(&r), tbl = rd_uleb(&r), slot;

			(void)tbl;
			if (t >= m->ntypes || !tail)
				goto malformed;
			slot = (uint32_t)POP();
			if ((uint64_t)(sp - stack) < m->types[t].nparams)
				goto malformed;
			sp -= m->types[t].nparams;
			tail->wanted = 1;
			tail->slot = (uint32_t)slot;
			tail->nargs = m->types[t].nparams;
			memcpy(tail->args, sp, tail->nargs * sizeof(*sp));
			free(locals);
			return 1;
		}
		case 0x1a:	/* drop */
			sp--;
			break;
		case 0x1b: {	/* select */
			uint64_t c = POP(), b = POP(), a = POP();

			PUSH(c ? a : b);
			break;
		}
		case 0x20: {
			uint64_t i = rd_uleb(&r);

			if (i >= f->nlocals)
				goto malformed;
			PUSH(locals[i]);
			break;
		}
		case 0x21: case 0x22: {
			uint64_t i = rd_uleb(&r);

			if (i >= f->nlocals)
				goto malformed;
			locals[i] = op == 0x21 ? POP() : TOP();
			break;
		}

		/* Memory: the address is a host address; the offset is added. */
		case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: {
			uint64_t offset;
			uintptr_t a;
			uint64_t v = 0;

			rd_uleb(&r);
			offset = rd_uleb(&r);
			a = (uintptr_t)(POP() + offset);
			switch (op) {
			case 0x28: { uint32_t x; memcpy(&x, (void *)a, 4); v = x; break; }		/* i32.load */
			case 0x29: memcpy(&v, (void *)a, 8); break;					/* i64.load */
			case 0x2a: { uint32_t x; memcpy(&x, (void *)a, 4); v = x; break; }		/* f32.load */
			case 0x2b: memcpy(&v, (void *)a, 8); break;					/* f64.load */
			case 0x2c: v = (uint32_t)(int32_t)*(int8_t *)a; break;				/* i32.load8_s */
			case 0x2d: v = *(uint8_t *)a; break;						/* i32.load8_u */
			case 0x2e: { int16_t x; memcpy(&x, (void *)a, 2); v = (uint32_t)(int32_t)x; break; }
			case 0x2f: { uint16_t x; memcpy(&x, (void *)a, 2); v = x; break; }
			case 0x30: v = (uint64_t)(int64_t)*(int8_t *)a; break;				/* i64.load8_s */
			case 0x31: v = *(uint8_t *)a; break;
			case 0x32: { int16_t x; memcpy(&x, (void *)a, 2); v = (uint64_t)(int64_t)x; break; }
			case 0x33: { uint16_t x; memcpy(&x, (void *)a, 2); v = x; break; }
			case 0x34: { int32_t x; memcpy(&x, (void *)a, 4); v = (uint64_t)(int64_t)x; break; }
			case 0x35: { uint32_t x; memcpy(&x, (void *)a, 4); v = x; break; }
			}
			PUSH(v);
			break;
		}
		case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: {
			uint64_t offset, v;
			uintptr_t a;

			rd_uleb(&r);
			offset = rd_uleb(&r);
			v = POP();
			a = (uintptr_t)(POP() + offset);
			switch (op) {
			case 0x36: case 0x38: case 0x3e: { uint32_t x = (uint32_t)v; memcpy((void *)a, &x, 4); break; }
			case 0x37: case 0x39: memcpy((void *)a, &v, 8); break;
			case 0x3a: case 0x3c: *(uint8_t *)a = (uint8_t)v; break;
			case 0x3b: case 0x3d: { uint16_t x = (uint16_t)v; memcpy((void *)a, &x, 2); break; }
			}
			break;
		}

		case 0x41: PUSH((uint32_t)rd_sleb(&r)); break;
		case 0x42: PUSH((uint64_t)rd_sleb(&r)); break;
		case 0x43: { uint32_t x; memcpy(&x, rd_bytes(&r, 4), 4); PUSH(x); break; }
		case 0x44: { uint64_t x; memcpy(&x, rd_bytes(&r, 8), 8); PUSH(x); break; }

		/* i32 comparisons */
		case 0x45: PUSH((uint32_t)POP() == 0); break;
		case 0x46: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a == b); break; }
		case 0x47: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a != b); break; }
		case 0x48: { int32_t b = (int32_t)POP(), a = (int32_t)POP(); PUSH(a < b); break; }
		case 0x49: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a < b); break; }
		case 0x4a: { int32_t b = (int32_t)POP(), a = (int32_t)POP(); PUSH(a > b); break; }
		case 0x4b: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a > b); break; }
		case 0x4c: { int32_t b = (int32_t)POP(), a = (int32_t)POP(); PUSH(a <= b); break; }
		case 0x4d: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a <= b); break; }
		case 0x4e: { int32_t b = (int32_t)POP(), a = (int32_t)POP(); PUSH(a >= b); break; }
		case 0x4f: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a >= b); break; }
		/* i64 comparisons */
		case 0x50: PUSH(POP() == 0); break;
		case 0x51: { uint64_t b = POP(), a = POP(); PUSH(a == b); break; }
		case 0x52: { uint64_t b = POP(), a = POP(); PUSH(a != b); break; }
		case 0x53: { int64_t b = (int64_t)POP(), a = (int64_t)POP(); PUSH(a < b); break; }
		case 0x54: { uint64_t b = POP(), a = POP(); PUSH(a < b); break; }
		case 0x55: { int64_t b = (int64_t)POP(), a = (int64_t)POP(); PUSH(a > b); break; }
		case 0x56: { uint64_t b = POP(), a = POP(); PUSH(a > b); break; }
		case 0x57: { int64_t b = (int64_t)POP(), a = (int64_t)POP(); PUSH(a <= b); break; }
		case 0x58: { uint64_t b = POP(), a = POP(); PUSH(a <= b); break; }
		case 0x59: { int64_t b = (int64_t)POP(), a = (int64_t)POP(); PUSH(a >= b); break; }
		case 0x5a: { uint64_t b = POP(), a = POP(); PUSH(a >= b); break; }
		/* f32 comparisons */
		case 0x5b: F32CMP(x_ == y_); break;
		case 0x5c: F32CMP(x_ != y_); break;
		case 0x5d: F32CMP(x_ < y_); break;
		case 0x5e: F32CMP(x_ > y_); break;
		case 0x5f: F32CMP(x_ <= y_); break;
		case 0x60: F32CMP(x_ >= y_); break;
		/* f64 comparisons */
		case 0x61: F64CMP(x_ == y_); break;
		case 0x62: F64CMP(x_ != y_); break;
		case 0x63: F64CMP(x_ < y_); break;
		case 0x64: F64CMP(x_ > y_); break;
		case 0x65: F64CMP(x_ <= y_); break;
		case 0x66: F64CMP(x_ >= y_); break;

		/* i32 arithmetic */
		case 0x67: { uint32_t a = (uint32_t)POP(); PUSH(a ? (uint32_t)__builtin_clz(a) : 32); break; }
		case 0x68: { uint32_t a = (uint32_t)POP(); PUSH(a ? (uint32_t)__builtin_ctz(a) : 32); break; }
		case 0x69: PUSH((uint32_t)__builtin_popcount((uint32_t)POP())); break;
		case 0x6a: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH((uint32_t)(a + b)); break; }
		case 0x6b: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH((uint32_t)(a - b)); break; }
		case 0x6c: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH((uint32_t)(a * b)); break; }
		case 0x6d: { int32_t b = (int32_t)POP(), a = (int32_t)POP();
			if (!b || (a == INT32_MIN && b == -1)) { free(locals); return trap("i32.div_s", f, pc); }
			PUSH((uint32_t)(a / b)); break; }
		case 0x6e: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP();
			if (!b) { free(locals); return trap("i32.div_u", f, pc); }
			PUSH(a / b); break; }
		case 0x6f: { int32_t b = (int32_t)POP(), a = (int32_t)POP();
			if (!b) { free(locals); return trap("i32.rem_s", f, pc); }
			PUSH(b == -1 ? 0 : (uint32_t)(a % b)); break; }
		case 0x70: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP();
			if (!b) { free(locals); return trap("i32.rem_u", f, pc); }
			PUSH(a % b); break; }
		case 0x71: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a & b); break; }
		case 0x72: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a | b); break; }
		case 0x73: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a ^ b); break; }
		case 0x74: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH((uint32_t)(a << (b & 31))); break; }
		case 0x75: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH((uint32_t)((int32_t)a >> (b & 31))); break; }
		case 0x76: { uint32_t b = (uint32_t)POP(), a = (uint32_t)POP(); PUSH(a >> (b & 31)); break; }
		case 0x77: { uint32_t b = (uint32_t)POP() & 31, a = (uint32_t)POP(); PUSH(b ? (uint32_t)((a << b) | (a >> (32 - b))) : a); break; }
		case 0x78: { uint32_t b = (uint32_t)POP() & 31, a = (uint32_t)POP(); PUSH(b ? (uint32_t)((a >> b) | (a << (32 - b))) : a); break; }
		/* i64 arithmetic */
		case 0x79: { uint64_t a = POP(); PUSH(a ? (uint64_t)__builtin_clzll(a) : 64); break; }
		case 0x7a: { uint64_t a = POP(); PUSH(a ? (uint64_t)__builtin_ctzll(a) : 64); break; }
		case 0x7b: PUSH((uint64_t)__builtin_popcountll(POP())); break;
		case 0x7c: { uint64_t b = POP(), a = POP(); PUSH(a + b); break; }
		case 0x7d: { uint64_t b = POP(), a = POP(); PUSH(a - b); break; }
		case 0x7e: { uint64_t b = POP(), a = POP(); PUSH(a * b); break; }
		case 0x7f: { int64_t b = (int64_t)POP(), a = (int64_t)POP();
			if (!b || (a == INT64_MIN && b == -1)) { free(locals); return trap("i64.div_s", f, pc); }
			PUSH((uint64_t)(a / b)); break; }
		case 0x80: { uint64_t b = POP(), a = POP();
			if (!b) { free(locals); return trap("i64.div_u", f, pc); }
			PUSH(a / b); break; }
		case 0x81: { int64_t b = (int64_t)POP(), a = (int64_t)POP();
			if (!b) { free(locals); return trap("i64.rem_s", f, pc); }
			PUSH(b == -1 ? 0 : (uint64_t)(a % b)); break; }
		case 0x82: { uint64_t b = POP(), a = POP();
			if (!b) { free(locals); return trap("i64.rem_u", f, pc); }
			PUSH(a % b); break; }
		case 0x83: { uint64_t b = POP(), a = POP(); PUSH(a & b); break; }
		case 0x84: { uint64_t b = POP(), a = POP(); PUSH(a | b); break; }
		case 0x85: { uint64_t b = POP(), a = POP(); PUSH(a ^ b); break; }
		case 0x86: { uint64_t b = POP(), a = POP(); PUSH(a << (b & 63)); break; }
		case 0x87: { uint64_t b = POP(), a = POP(); PUSH((uint64_t)((int64_t)a >> (b & 63))); break; }
		case 0x88: { uint64_t b = POP(), a = POP(); PUSH(a >> (b & 63)); break; }
		case 0x89: { uint64_t b = POP() & 63, a = POP(); PUSH(b ? (a << b) | (a >> (64 - b)) : a); break; }
		case 0x8a: { uint64_t b = POP() & 63, a = POP(); PUSH(b ? (a >> b) | (a << (64 - b)) : a); break; }

		/* f32 arithmetic */
		case 0x8b: PUSH(f32_bits(fabsf(bits_f32(POP())))); break;
		case 0x8c: PUSH(f32_bits(-bits_f32(POP()))); break;
		case 0x8d: PUSH(f32_bits(ceilf(bits_f32(POP())))); break;
		case 0x8e: PUSH(f32_bits(floorf(bits_f32(POP())))); break;
		case 0x8f: PUSH(f32_bits(truncf(bits_f32(POP())))); break;
		case 0x90: PUSH(f32_bits(rintf(bits_f32(POP())))); break;
		case 0x91: PUSH(f32_bits(sqrtf(bits_f32(POP())))); break;
		case 0x92: F32OP(x_ + y_); break;
		case 0x93: F32OP(x_ - y_); break;
		case 0x94: F32OP(x_ * y_); break;
		case 0x95: F32OP(x_ / y_); break;
		/* f64 arithmetic */
		case 0x99: PUSH(f64_bits(fabs(bits_f64(POP())))); break;
		case 0x9a: PUSH(f64_bits(-bits_f64(POP()))); break;
		case 0x9b: PUSH(f64_bits(ceil(bits_f64(POP())))); break;
		case 0x9c: PUSH(f64_bits(floor(bits_f64(POP())))); break;
		case 0x9d: PUSH(f64_bits(trunc(bits_f64(POP())))); break;
		case 0x9e: PUSH(f64_bits(rint(bits_f64(POP())))); break;
		case 0x9f: PUSH(f64_bits(sqrt(bits_f64(POP())))); break;
		case 0xa0: F64OP(x_ + y_); break;
		case 0xa1: F64OP(x_ - y_); break;
		case 0xa2: F64OP(x_ * y_); break;
		case 0xa3: F64OP(x_ / y_); break;

		/* conversions */
		case 0xa7: PUSH((uint32_t)POP()); break;					/* i32.wrap_i64 */
		case 0xac: PUSH((uint64_t)(int64_t)(int32_t)(uint32_t)POP()); break;		/* i64.extend_i32_s */
		case 0xad: PUSH((uint64_t)(uint32_t)POP()); break;				/* i64.extend_i32_u */
		case 0xb2: PUSH(f32_bits((float)(int32_t)(uint32_t)POP())); break;
		case 0xb3: PUSH(f32_bits((float)(uint32_t)POP())); break;
		case 0xb4: PUSH(f32_bits((float)(int64_t)POP())); break;
		case 0xb5: PUSH(f32_bits((float)POP())); break;
		case 0xb6: PUSH(f32_bits((float)bits_f64(POP()))); break;			/* f32.demote_f64 */
		case 0xb7: PUSH(f64_bits((double)(int32_t)(uint32_t)POP())); break;
		case 0xb8: PUSH(f64_bits((double)(uint32_t)POP())); break;
		case 0xb9: PUSH(f64_bits((double)(int64_t)POP())); break;
		case 0xba: PUSH(f64_bits((double)POP())); break;
		case 0xbb: PUSH(f64_bits((double)bits_f32(POP()))); break;			/* f64.promote_f32 */
		case 0xbc: PUSH((uint32_t)POP()); break;					/* i32.reinterpret_f32 */
		case 0xbd: break;								/* i64.reinterpret_f64 */
		case 0xbe: PUSH((uint32_t)POP()); break;					/* f32.reinterpret_i32 */
		case 0xbf: break;								/* f64.reinterpret_i64 */
		case 0xc0: PUSH((uint32_t)(int32_t)(int8_t)POP()); break;
		case 0xc1: PUSH((uint32_t)(int32_t)(int16_t)POP()); break;
		case 0xc2: PUSH((uint64_t)(int64_t)(int8_t)POP()); break;
		case 0xc3: PUSH((uint64_t)(int64_t)(int16_t)POP()); break;
		case 0xc4: PUSH((uint64_t)(int64_t)(int32_t)POP()); break;

		case 0xfc: {	/* saturating truncations */
			uint64_t sub = rd_uleb(&r);

			switch (sub) {
			case 0: PUSH((uint32_t)sat_s(bits_f32(POP()), INT32_MIN, INT32_MAX)); break;
			case 1: PUSH((uint32_t)sat_u(bits_f32(POP()), UINT32_MAX)); break;
			case 2: PUSH((uint32_t)sat_s(bits_f64(POP()), INT32_MIN, INT32_MAX)); break;
			case 3: PUSH((uint32_t)sat_u(bits_f64(POP()), UINT32_MAX)); break;
			case 4: PUSH((uint64_t)sat_s(bits_f32(POP()), INT64_MIN, INT64_MAX)); break;
			case 5: PUSH(sat_u(bits_f32(POP()), UINT64_MAX)); break;
			case 6: PUSH((uint64_t)sat_s(bits_f64(POP()), INT64_MIN, INT64_MAX)); break;
			case 7: PUSH(sat_u(bits_f64(POP()), UINT64_MAX)); break;
			default: goto malformed;
			}
			break;
		}
		case 0xfd: {	/* vectors, as one lane (see the top) */
			uint64_t sub = rd_uleb(&r);

			switch (sub) {
			case 0x12:	/* i64x2.splat */
				break;
			case 0x1d:	/* i64x2.extract_lane */
				if (rd_byte(&r) != 0)
					goto malformed;
				break;
			case 0x107: {	/* f64x2.relaxed_madd: a * b + c, fused */
				double c = bits_f64(POP()), b = bits_f64(POP()), a = bits_f64(POP());

				PUSH(f64_bits(fma(a, b, c)));
				break;
			}
			default:
				goto malformed;
			}
			break;
		}
		case 0xfe: {	/* atomics */
			uint64_t sub = rd_uleb(&r), offset;
			uintptr_t a;

			rd_uleb(&r);
			offset = rd_uleb(&r);
			switch (sub) {
			case 0x10: a = (uintptr_t)(POP() + offset); PUSH(__atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST)); break;
			case 0x11: a = (uintptr_t)(POP() + offset); PUSH(__atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST)); break;
#define RMW32(fn) { uint32_t v = (uint32_t)POP(); a = (uintptr_t)(POP() + offset); PUSH(__atomic_##fn((uint32_t *)a, v, __ATOMIC_SEQ_CST)); }
#define RMW64(fn) { uint64_t v = POP(); a = (uintptr_t)(POP() + offset); PUSH(__atomic_##fn((uint64_t *)a, v, __ATOMIC_SEQ_CST)); }
			case 0x1e: RMW32(fetch_add); break;
			case 0x1f: RMW64(fetch_add); break;
			case 0x25: RMW32(fetch_sub); break;
			case 0x26: RMW64(fetch_sub); break;
			case 0x2c: RMW32(fetch_and); break;
			case 0x2d: RMW64(fetch_and); break;
			case 0x33: RMW32(fetch_or); break;
			case 0x34: RMW64(fetch_or); break;
			case 0x3a: RMW32(fetch_xor); break;
			case 0x3b: RMW64(fetch_xor); break;
			case 0x41: RMW32(exchange_n); break;
			case 0x42: RMW64(exchange_n); break;
			case 0x48: {
				uint32_t repl = (uint32_t)POP(), expected = (uint32_t)POP();

				a = (uintptr_t)(POP() + offset);
				__atomic_compare_exchange_n((uint32_t *)a, &expected, repl, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
				PUSH(expected);
				break;
			}
			case 0x49: {
				uint64_t repl = POP(), expected = POP();

				a = (uintptr_t)(POP() + offset);
				__atomic_compare_exchange_n((uint64_t *)a, &expected, repl, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
				PUSH(expected);
				break;
			}
#undef RMW32
#undef RMW64
			default:
				goto malformed;
			}
			break;
		}
		default:
			free(locals);
			fprintf(stderr, "rv: jit emu: unknown opcode 0x%02x\n", op);
			return trap("unknown instruction", f, pc);
		}
	}
malformed:
	free(locals);
	return trap("malformed code", f, pc);
overflow:
	free(locals);
	return trap("stack overflow", f, pc);
#undef PUSH
#undef POP
#undef TOP
#undef LABEL_PUSH
}

/* Call the region in `slot` as translated code would be called: (cpu, entry, budget). Tail calls chain. */
int rv_emu_run(long slot, struct rv_cpu *cpu, uint32_t entry, uint32_t budget)
{
	struct tail t = { 0, 0, { (uint64_t)(uintptr_t)cpu, entry, budget }, 3 };

	for (;;) {
		struct emu_slot *s;
		uint64_t result;
		int ret;

		if (slot < 0 || (uint64_t)slot >= emu_table_len || !emu_table[slot].m) {
			fprintf(stderr, "rv: jit emu: call to an empty table slot %ld\n", slot);
			return -1;
		}
		s = &emu_table[slot];
		COUNT(emu_stat_calls);
		t.wanted = 0;
		ret = exec(s->m, s->func, t.args, &result, &t, 0);
		if (ret < 0)
			return -1;
		if (!ret)
			return 0;
		COUNT(emu_stat_tails);
		slot = t.slot;
	}
}

/* The thread ends: its table and modules go with it (jit.c: rv_jit_free()). */
void rv_emu_thread_exit(void)
{
	for (uint32_t i = 0; i < emu_table_len; i++)
		slot_clear(i);
	free(emu_table);
	emu_table = NULL;
	emu_table_len = 0;
}

void rv_emu_report(void)
{
	if (emu_stat_installed)
		fprintf(stderr, "rv: jit emu: %llu modules installed, %llu calls of translated code, %llu of them tail calls from region to region\n",
			(unsigned long long)emu_stat_installed, (unsigned long long)emu_stat_calls, (unsigned long long)emu_stat_tails);
}
#endif /* !__wasm__ */
