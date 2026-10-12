/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * A minimal WebAssembly binary writer: a byte buffer, LEB128 and the opcodes
 * the JIT uses. Overflow is sticky and checked once at the end.
 */
#ifndef WASM_EMIT_H
#define WASM_EMIT_H

#include <stdint.h>
#include <string.h>

struct wbuf {
	uint8_t *data;
	size_t len, cap;
	int overflow;
};

enum {
	W_BLOCK = 0x02, W_LOOP = 0x03, W_IF = 0x04, W_ELSE = 0x05, W_END = 0x0b,
	W_BR = 0x0c, W_BR_IF = 0x0d, W_BR_TABLE = 0x0e, W_RETURN = 0x0f, W_CALL = 0x10,
	W_RETURN_CALL_INDIRECT = 0x13,
	W_SELECT = 0x1b,
	W_LOCAL_GET = 0x20, W_LOCAL_SET = 0x21, W_LOCAL_TEE = 0x22,
	W_I32_LOAD = 0x28, W_I64_LOAD = 0x29, W_I32_LOAD8_U = 0x2d,
	W_I64_LOAD8_S = 0x30, W_I64_LOAD8_U = 0x31, W_I64_LOAD16_S = 0x32,
	W_I64_LOAD16_U = 0x33, W_I64_LOAD32_S = 0x34, W_I64_LOAD32_U = 0x35,
	W_I32_STORE = 0x36, W_I64_STORE = 0x37, W_I64_STORE8 = 0x3c, W_I64_STORE16 = 0x3d, W_I64_STORE32 = 0x3e,
	W_I32_CONST = 0x41, W_I64_CONST = 0x42,
	W_I32_EQZ = 0x45, W_I32_EQ = 0x46, W_I32_NE = 0x47, W_I32_LT_S = 0x48, W_I32_LT_U = 0x49,
	W_I64_EQZ = 0x50, W_I64_EQ = 0x51, W_I64_NE = 0x52, W_I64_LT_S = 0x53,
	W_I64_LT_U = 0x54, W_I64_GT_U = 0x56, W_I64_LE_U = 0x58, W_I64_GE_S = 0x59, W_I64_GE_U = 0x5a,
	W_I32_ADD = 0x6a, W_I32_SUB = 0x6b, W_I32_AND = 0x71, W_I32_OR = 0x72,
	W_I32_SHL = 0x74, W_I32_SHR_S = 0x75, W_I32_SHR_U = 0x76,
	W_I64_ADD = 0x7c, W_I64_SUB = 0x7d, W_I64_MUL = 0x7e,
	W_I64_AND = 0x83, W_I64_OR = 0x84, W_I64_XOR = 0x85,
	W_I64_SHL = 0x86, W_I64_SHR_S = 0x87, W_I64_SHR_U = 0x88,
	W_I32_WRAP_I64 = 0xa7, W_I64_EXTEND_I32_S = 0xac, W_I64_EXTEND_I32_U = 0xad,
	/* Counting and rotating bits, sign extension (for Zbb). */
	W_I32_CLZ = 0x67, W_I32_CTZ = 0x68, W_I32_POPCNT = 0x69, W_I32_ROTL = 0x77, W_I32_ROTR = 0x78,
	W_I64_CLZ = 0x79, W_I64_CTZ = 0x7a, W_I64_POPCNT = 0x7b, W_I64_ROTL = 0x89, W_I64_ROTR = 0x8a,
	W_I64_EXTEND8_S = 0xc2, W_I64_EXTEND16_S = 0xc3,

	/* Floating point. Add, subtract, multiply, divide follow each other, for both widths. */
	W_F64_CONST = 0x44,
	W_F32_EQ = 0x5b, W_F32_LT = 0x5d, W_F32_LE = 0x5f,
	W_F64_EQ = 0x61, W_F64_LT = 0x63, W_F64_LE = 0x65, W_F64_GE = 0x66,
	W_F32_SQRT = 0x91, W_F32_ADD = 0x92,
	W_F64_CEIL = 0x9b, W_F64_FLOOR = 0x9c, W_F64_TRUNC = 0x9d, W_F64_NEAREST = 0x9e, W_F64_SQRT = 0x9f, W_F64_ADD = 0xa0,
	W_F32_CONVERT_I32_S = 0xb2, W_F32_CONVERT_I32_U = 0xb3, W_F32_CONVERT_I64_S = 0xb4, W_F32_CONVERT_I64_U = 0xb5,
	W_F32_DEMOTE_F64 = 0xb6,
	W_F64_CONVERT_I32_S = 0xb7, W_F64_CONVERT_I32_U = 0xb8, W_F64_CONVERT_I64_S = 0xb9, W_F64_CONVERT_I64_U = 0xba,
	W_F64_PROMOTE_F32 = 0xbb,
	W_I32_REINTERPRET_F32 = 0xbc, W_I64_REINTERPRET_F64 = 0xbd,
	W_F32_REINTERPRET_I32 = 0xbe, W_F64_REINTERPRET_I64 = 0xbf,
	/* After the prefix 0xfc: conversions to an integer that never trap (out of range gives the nearest end). */
	W_MISC_PREFIX = 0xfc,
	W_I32_TRUNC_SAT_F64_S = 0x02, W_I32_TRUNC_SAT_F64_U = 0x03,
	W_I64_TRUNC_SAT_F64_S = 0x06, W_I64_TRUNC_SAT_F64_U = 0x07,

	W_TYPE_VOID = 0x40, W_TYPE_I64 = 0x7e, W_TYPE_I32 = 0x7f, W_TYPE_FUNC = 0x60,
};

static inline void w_byte(struct wbuf *b, uint8_t v)
{
	if (b->len < b->cap)
		b->data[b->len++] = v;
	else
		b->overflow = 1;
}

static inline void w_bytes(struct wbuf *b, const void *p, size_t n)
{
	if (b->len <= b->cap && n <= b->cap - b->len) {
		memcpy(b->data + b->len, p, n);
		b->len += n;
	} else {
		b->overflow = 1;
	}
}

static inline void w_uleb(struct wbuf *b, uint64_t v)
{
	do {
		uint8_t byte = v & 0x7f;

		v >>= 7;
		w_byte(b, byte | (v ? 0x80 : 0));
	} while (v);
}

static inline void w_sleb(struct wbuf *b, int64_t v)
{
	for (;;) {
		uint8_t byte = v & 0x7f;

		v >>= 7;	/* arithmetic shift */
		if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40))) {
			w_byte(b, byte);
			return;
		}
		w_byte(b, byte | 0x80);
	}
}

static inline void w_name(struct wbuf *b, const char *s)
{
	size_t n = strlen(s);

	w_uleb(b, n);
	w_bytes(b, s, n);
}

static inline void w_op(struct wbuf *b, uint8_t op)
{
	w_byte(b, op);
}

static inline void w_i64(struct wbuf *b, uint64_t v)
{
	w_byte(b, W_I64_CONST);
	w_sleb(b, (int64_t)v);
}

static inline void w_i32(struct wbuf *b, int32_t v)
{
	w_byte(b, W_I32_CONST);
	w_sleb(b, v);
}

static inline void w_local(struct wbuf *b, uint8_t op, uint32_t index)
{
	w_byte(b, op);
	w_uleb(b, index);
}

/*
 * The atomic instructions (the threads proposal): a prefix byte, then one of
 * these, each for an i32 and, one more, for an i64, then alignment and
 * offset like a load. The alignment has to be the operand's own.
 */
enum {
	W_ATOMIC_PREFIX = 0xfe,
	W_ATOMIC_LOAD = 0x10, W_ATOMIC_ADD = 0x1e, W_ATOMIC_AND = 0x2c, W_ATOMIC_OR = 0x33,
	W_ATOMIC_XOR = 0x3a, W_ATOMIC_XCHG = 0x41, W_ATOMIC_CMPXCHG = 0x48,
};

static inline void w_atomic(struct wbuf *b, uint8_t op, int wide, uint64_t offset)
{
	w_byte(b, W_ATOMIC_PREFIX);
	w_uleb(b, (uint64_t)op + (wide ? 1 : 0));
	w_uleb(b, wide ? 3 : 2);
	w_uleb(b, offset);
}

/*
 * Vectors (the SIMD proposal, and of the relaxed one the fused multiply-add,
 * whose result depends on the host: see fma_probe() in jit.c): a prefix
 * byte, then one of these.
 */
enum {
	W_SIMD_PREFIX = 0xfd,
	W_I64X2_SPLAT = 0x12, W_I64X2_EXTRACT_LANE = 0x1d, W_F64X2_RELAXED_MADD = 0x107,
};

static inline void w_simd(struct wbuf *b, uint32_t op)
{
	w_byte(b, W_SIMD_PREFIX);
	w_uleb(b, op);
}

static inline void w_f64(struct wbuf *b, double v)
{
	w_byte(b, W_F64_CONST);
	w_bytes(b, &v, 8);	/* (little-endian hosts only, like everything here) */
}

/* A load or store on the (64-bit) memory: alignment as log2, then the offset. */
static inline void w_mem(struct wbuf *b, uint8_t op, uint32_t align_log2, uint64_t offset)
{
	w_byte(b, op);
	w_uleb(b, align_log2);
	w_uleb(b, offset);
}

static inline void w_br(struct wbuf *b, uint8_t op, uint32_t depth)
{
	w_byte(b, op);
	w_uleb(b, depth);
}

/* Append `content` as a section: id, size, bytes. */
static inline void w_section(struct wbuf *b, uint8_t id, const struct wbuf *content)
{
	w_byte(b, id);
	w_uleb(b, content->len);
	w_bytes(b, content->data, content->len);
}

#endif /* WASM_EMIT_H */
