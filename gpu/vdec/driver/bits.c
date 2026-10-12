/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
#include "bits.h"
#include "vdec.h"

/* Emulation prevention bytes out: 00 00 03 xx -> 00 00 xx. `out` holds `n` bytes. */
size_t nal_unescape(const uint8_t *in, size_t n, uint8_t *out)
{
	size_t o = 0;
	int zeros = 0;

	for (size_t i = 0; i < n; i++) {
		if (zeros >= 2 && in[i] == 3) {
			zeros = 0;
			continue;
		}
		out[o++] = in[i];
		zeros = in[i] ? 0 : zeros + 1;
	}
	return o;
}

/* A start code and the NAL unit, with emulation prevention bytes put in (H.264 7.4.1, HEVC 7.4.2). */
int nal_put(struct buf *b, const uint8_t *rbsp, size_t n)
{
	static const uint8_t start[4] = { 0, 0, 0, 1 };
	int zeros = 0;

	if (buf_put(b, start, 4))
		return -1;
	for (size_t i = 0; i < n; i++) {
		if (zeros >= 2 && rbsp[i] <= 3) {
			if (buf_byte(b, 3))
				return -1;
			zeros = 0;
		}
		if (buf_byte(b, rbsp[i]))
			return -1;
		zeros = rbsp[i] ? 0 : zeros + 1;
	}
	return 0;
}
