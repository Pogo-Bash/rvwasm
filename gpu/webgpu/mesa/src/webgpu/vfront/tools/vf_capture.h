/*
 * vfront capture files: what a vfront executor was given, in the ring's own record format (gpu/PROTOCOL.md,
 * ring version 2) plus the backing bytes the decoder read and wrote, so a capture replays exactly, into
 * vfront or into virglrenderer (the reference).
 *
 * File: the 8 bytes "VFCAP001", then records, each {u32 op, u32 ctx, u32 serial, u32 bytes} and `bytes` of
 * payload (little-endian dwords; no padding).
 *
 * Ring records (payloads exactly as in the ring; backing addresses are 0 in a capture):
 *   1 CTX_CREATE    name (UTF-8, NUL-terminated)
 *   2 CTX_DESTROY   -
 *   3 RES_CREATE    res, target, format, bind, width, height, depth, array_size, last_level, nr_samples,
 *                   flags, size, addr_lo, addr_hi
 *   4 RES_UNREF     res
 *   5 SUBMIT        the command buffer's dwords
 *   6 TRANSFER_PUT  res, level, x, y, z, w, h, d, offset, data_size
 *   7 TRANSFER_GET  res, level, x, y, z, w, h, d, offset, data_size
 * Capture-only records:
 *  64 DATA          res, offset, size, then `size` bytes: backing bytes the decoder read while doing the next
 *                   ring record (in the order it read them)
 *  65 RESULT        res, offset, size, hash_lo, hash_hi: what the decoder wrote into a backing (readbacks,
 *                   query results) when the flush it belongs to completed, as a 64-bit FNV-1a hash of the
 *                   bytes (vfcap_hash)
 *  66 FLUSH         serial: everything of ctx so far is one unit of completion
 *  67 CAPS          the caps the guest was given (struct virgl_caps_v2)
 *  68 WAIT          serial: the guest waited for this serial of ctx (RESOURCE_BUSY_WAIT with WAIT)
 */
#ifndef VF_CAPTURE_H
#define VF_CAPTURE_H

#include <stdint.h>

#define VFCAP_MAGIC "VFCAP001"

enum {
	VFCAP_CTX_CREATE = 1,
	VFCAP_CTX_DESTROY = 2,
	VFCAP_RES_CREATE = 3,
	VFCAP_RES_UNREF = 4,
	VFCAP_SUBMIT = 5,
	VFCAP_TRANSFER_PUT = 6,
	VFCAP_TRANSFER_GET = 7,
	VFCAP_DATA = 64,
	VFCAP_RESULT = 65,
	VFCAP_FLUSH = 66,
	VFCAP_CAPS = 67,
	VFCAP_WAIT = 68,
};

struct vfcap_hdr {
	uint32_t op, ctx, serial, bytes;
};

static inline uint64_t vfcap_hash(const void *p, uint64_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	uint64_t h = 0xcbf29ce484222325ull;
	for (uint64_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3ull;
	return h;
}

#endif
