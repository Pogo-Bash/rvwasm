/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/* Bit writing and reading for parameter sets and headers (MSB first, as H.264, HEVC, VP9 and AV1 have them). */
#ifndef RVVDEC_BITS_H
#define RVVDEC_BITS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct bw {
	uint8_t p[4096];
	size_t bits;
	int over;
};

static inline void bw_init(struct bw *w)
{
	memset(w, 0, sizeof(*w));
}

static inline void bw_u(struct bw *w, uint32_t v, int n)
{
	for (int i = n - 1; i >= 0; i--) {
		if (w->bits >= 8 * sizeof(w->p)) {
			w->over = 1;
			return;
		}
		if ((v >> i) & 1)
			w->p[w->bits >> 3] |= (uint8_t)(0x80 >> (w->bits & 7));
		w->bits++;
	}
}

static inline void bw_flag(struct bw *w, int v)
{
	bw_u(w, v ? 1 : 0, 1);
}

/* Exp-Golomb (H.264 9.1, HEVC 9.2) */
static inline void bw_ue(struct bw *w, uint32_t v)
{
	uint64_t x = (uint64_t)v + 1;
	int n = 0;

	while ((x >> n) > 1)
		n++;
	bw_u(w, 0, n);
	bw_u(w, (uint32_t)(x >> 32), n + 1 > 32 ? n + 1 - 32 : 0);
	bw_u(w, (uint32_t)x, n + 1 > 32 ? 32 : n + 1);
}

static inline void bw_se(struct bw *w, int32_t v)
{
	bw_ue(w, v > 0 ? 2 * (uint32_t)v - 1 : 2 * (uint32_t)(-(int64_t)v));
}

/* rbsp_trailing_bits() */
static inline void bw_trailing(struct bw *w)
{
	bw_u(w, 1, 1);
	while (w->bits & 7)
		bw_u(w, 0, 1);
}

static inline size_t bw_bytes(const struct bw *w)
{
	return (w->bits + 7) >> 3;
}

/* `n` bits of another bit string (from bit `at`) appended */
static inline void bw_copy(struct bw *w, const uint8_t *src, size_t at, size_t n)
{
	for (size_t i = 0; i < n; i++)
		bw_u(w, (src[(at + i) >> 3] >> (7 - ((at + i) & 7))) & 1, 1);
}

struct br {
	const uint8_t *p;
	size_t size;	/* bytes */
	size_t pos;	/* bits */
	int over;
};

static inline void br_init(struct br *r, const uint8_t *p, size_t size)
{
	r->p = p;
	r->size = size;
	r->pos = 0;
	r->over = 0;
}

static inline uint32_t br_u(struct br *r, int n)
{
	uint32_t v = 0;

	for (int i = 0; i < n; i++) {
		if (r->pos >= 8 * r->size) {
			r->over = 1;
			return v;
		}
		v = (v << 1) | ((r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1);
		r->pos++;
	}
	return v;
}

static inline uint32_t br_ue(struct br *r)
{
	int n = 0;

	while (!br_u(r, 1)) {
		if (r->over || ++n > 31) {
			r->over = 1;
			return 0;
		}
	}
	return n ? ((1u << n) - 1) + br_u(r, n) : 0;
}

static inline int32_t br_se(struct br *r)
{
	uint32_t k = br_ue(r);

	return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

/* Little-endian, LSB first (VP8's frame tag) is written by hand; AV1's leb128: */
static inline size_t leb128(uint8_t *out, uint64_t v)
{
	size_t n = 0;

	do {
		uint8_t b = v & 0x7f;

		v >>= 7;
		out[n++] = b | (v ? 0x80 : 0);
	} while (v);
	return n;
}

/* NAL units: emulation prevention bytes (H.264 7.4.1, HEVC 7.4.2) out and in. */
size_t nal_unescape(const uint8_t *in, size_t n, uint8_t *out);
struct buf;
int nal_put(struct buf *b, const uint8_t *rbsp, size_t n);	/* start code + the NAL, escaped */

#endif
