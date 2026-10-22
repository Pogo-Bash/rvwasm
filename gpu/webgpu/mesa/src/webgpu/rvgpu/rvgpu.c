/*
 * rvgpu: the page host's C side. The ring of gpuserver (gpu/PROTOCOL.md,
 * ring v2), read from the machine's memory through rvgpu_guest_* (in the
 * page: library_rvgpu.js; natively: a host block, rvgpu_native.c), records
 * dispatched to the front ends that registered their ops, completion in
 * order. INTERFACES.md, section 3.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rvgpu.h"

#define MAX_PENDING 4096

static struct {
	uint64_t ring, data, caps;
	uint32_t data_size, caps_size;
	uint32_t tail, heartbeat;
	bool attached;
	bool draining;	/* inside rvgpu_ring_drain (also while JSPI has it suspended) */
	rvgpu_op_fn fn[RVGPU_FRONT_COUNT][RVGPU_OP_COUNT];
	void *front[RVGPU_FRONT_COUNT][RVGPU_OP_COUNT];
	uint8_t ctx_front[256];		/* RVGPU_FRONT_* of each context */
	bool (*poll)(void *data);
	void *poll_data;
	bool poll_soon;
	uint8_t *buf;
	size_t buf_size;
	/* records that need completion, in ring order: serial and whether done */
	uint32_t pend_serial[MAX_PENDING];
	uint8_t pend_ctx[MAX_PENDING];
	bool pend_done[MAX_PENDING];
	unsigned pend_head, pend_count;
	bool warned[256];
	struct { uint32_t res, flags; WGPUTexture tex; } present[256];
	struct { rvgpu_present_resolve_fn fn; void *front; } resolver[4];
} R;

static bool
needs_completion(uint32_t op)
{
	return op == RVGPU_OP_SUBMIT || op == RVGPU_OP_TRANSFER_PUT || op == RVGPU_OP_TRANSFER_GET ||
	       op == RVGPU_OP_VN_SUBMIT;
}

/* wgvk names the guest-memory functions weak (the native ICD has none), and the linker brings a JavaScript
 * library function into the page's module only for a strong reference: without these, wgvk's uploads from
 * and downloads into guest windows are silently skipped in the page (found by T5: venus fences never
 * signalled). */
__attribute__((used)) static const void *const rvgpu_guest_functions[] = {
	(const void *)rvgpu_guest_load32, (const void *)rvgpu_guest_store32, (const void *)rvgpu_guest_read,
	(const void *)rvgpu_guest_write, (const void *)rvgpu_guest_upload_buffer,
	(const void *)rvgpu_guest_upload_texture, (const void *)rvgpu_guest_from_mapped, (const void *)rvgpu_guest_ptr,
};

int
rvgpu_ring_attach(uint64_t ring, uint32_t ring_size, uint64_t caps, uint32_t caps_size)
{
	uint32_t magic = rvgpu_guest_load32(ring + RVGPU_RING_MAGIC);
	uint32_t version = rvgpu_guest_load32(ring + RVGPU_RING_VERSION);
	if (magic != RVGPU_RING_MAGIC_VALUE || (version != 2 && version != 3)) {
		fprintf(stderr, "rvgpu: no ring at %llu (magic %08x, version %u)\n", (unsigned long long)ring, magic,
			version);
		return -1;
	}
	R.ring = ring;
	R.data_size = rvgpu_guest_load32(ring + RVGPU_RING_DATA_SIZE);
	R.data = ring + rvgpu_guest_load32(ring + RVGPU_RING_DATA_OFF);
	R.caps = caps;
	R.caps_size = caps_size;
	R.tail = rvgpu_guest_load32(ring + RVGPU_RING_TAIL);
	R.attached = true;
	(void)ring_size;
	return 0;
}

void
rvgpu_ring_register_front(unsigned kind, uint32_t op, rvgpu_op_fn fn, void *front)
{
	if (kind < RVGPU_FRONT_COUNT && op < RVGPU_OP_COUNT) {
		R.fn[kind][op] = fn;
		R.front[kind][op] = front;
	}
}

void
rvgpu_ring_register(uint32_t op, rvgpu_op_fn fn, void *front)
{
	rvgpu_ring_register_front(RVGPU_FRONT_VIRGL, op, fn, front);
}

void
rvgpu_ring_set_poll(bool (*poll)(void *data), void *data)
{
	R.poll = poll;
	R.poll_data = data;
}

char rvgpu_where[128];
uint32_t rvgpu_debug_words[8];

uint32_t *
rvgpu_debug_words_at(void)
{
	return rvgpu_debug_words;
}

const char *
rvgpu_debug_where(void)
{
	return rvgpu_where;
}

int
rvgpu_poll_ms(void)
{
	return R.poll_soon ? 1 : 500;
}

static void
publish_done(void)
{
	uint32_t last = 0;
	bool any = false;
	while (R.pend_count && R.pend_done[R.pend_head]) {
		last = R.pend_serial[R.pend_head];
		any = true;
		R.pend_head = (R.pend_head + 1) % MAX_PENDING;
		R.pend_count--;
	}
	if (any)
		rvgpu_guest_store32(R.ring + RVGPU_RING_DONE, last);
}

/* A record is done. ctx_done[ctx] moves to the newest serial of that context
 * whose records up to it are all done; done to the same over all contexts. */
void
rvgpu_complete(uint32_t ctx, uint32_t serial)
{
	bool found = false;
	for (unsigned i = 0, j = R.pend_head; i < R.pend_count; i++, j = (j + 1) % MAX_PENDING) {
		if (R.pend_serial[j] == serial && R.pend_ctx[j] == (ctx & 255)) {
			R.pend_done[j] = true;
			found = true;
			break;
		}
	}
	if (ctx && ctx < 256) {
		uint32_t newest = serial;
		bool any = !found;	/* (not tracked: just say so) */
		for (unsigned i = 0, j = R.pend_head; found && i < R.pend_count; i++, j = (j + 1) % MAX_PENDING) {
			if (R.pend_ctx[j] != ctx)
				continue;
			if (!R.pend_done[j])
				break;
			newest = R.pend_serial[j];
			any = true;
		}
		if (any)
			rvgpu_guest_store32(R.ring + RVGPU_RING_CTX_DONE + 4 * ctx, newest);
	}
	publish_done();
}

int
rvgpu_ring_drain(void)
{
	if (!R.attached)
		return -1;
	uint32_t head = rvgpu_guest_load32(R.ring + RVGPU_RING_HEAD);
	int n = 0;
	R.draining = true;
	while (R.tail != head) {
		uint32_t pos = R.tail & (R.data_size - 1);
		struct rvgpu_record rec;
		rvgpu_guest_read(&rec, R.data + pos, sizeof(rec));
		uint32_t padded = (rec.bytes + 15) & ~15u;
		if (rec.op != RVGPU_OP_PAD) {
			if (rec.bytes > R.buf_size) {
				free(R.buf);
				R.buf_size = (rec.bytes + 65535) & ~65535u;
				R.buf = malloc(R.buf_size);
			}
			rvgpu_guest_read(R.buf, R.data + pos + 16, rec.bytes);
			bool track = needs_completion(rec.op) && R.pend_count < MAX_PENDING;
			if (track) {
				unsigned j = (R.pend_head + R.pend_count++) % MAX_PENDING;
				R.pend_serial[j] = rec.serial;
				R.pend_ctx[j] = rec.ctx & 255;
				R.pend_done[j] = false;
			}
			/* the context's front end: VN_CTX_INIT makes it venus', CTX_DESTROY ends that */
			if (rec.op == RVGPU_OP_VN_CTX_INIT)
				R.ctx_front[rec.ctx & 255] = RVGPU_FRONT_VENUS;
			unsigned kind = R.ctx_front[rec.ctx & 255];
			if (rec.op == RVGPU_OP_CTX_DESTROY)
				R.ctx_front[rec.ctx & 255] = RVGPU_FRONT_VIRGL;
			snprintf(rvgpu_where, sizeof(rvgpu_where), "ring op %u of context %u, serial %u", rec.op, rec.ctx,
				 rec.serial);
			if (rec.op < RVGPU_OP_COUNT && R.fn[kind][rec.op]) {
				R.fn[kind][rec.op](R.front[kind][rec.op], &rec, R.buf);
			} else {
				if (!R.warned[rec.op & 255]) {
					R.warned[rec.op & 255] = true;
					fprintf(stderr, "rvgpu: no front end for ring op %u; completed without doing it\n",
						rec.op);
				}
				if (track)
					rvgpu_complete(rec.ctx, rec.serial);
			}
			n++;
		}
		R.tail += 16 + padded;
		rvgpu_guest_store32(R.ring + RVGPU_RING_TAIL, R.tail);
	}
	snprintf(rvgpu_where, sizeof(rvgpu_where), "the front ends' poll");
	R.poll_soon = R.poll ? R.poll(R.poll_data) : false;
	rvgpu_where[0] = 0;
	rvgpu_guest_store32(R.ring + RVGPU_RING_EXEC_HEARTBEAT, ++R.heartbeat);
	R.draining = false;
	return n;
}

void
rvgpu_set_state(uint32_t state, uint32_t error)
{
	rvgpu_guest_store32(R.ring + RVGPU_RING_EXEC_ERROR, error);
	rvgpu_guest_store32(R.ring + RVGPU_RING_EXEC_STATE, state);
}

void
rvgpu_set_caps(const void *caps, uint32_t len)
{
	if (len > R.caps_size)
		len = R.caps_size;
	rvgpu_guest_write(R.caps, caps, len);
	rvgpu_guest_store32(R.ring + RVGPU_RING_CAPS_LEN, len);
}

void
rvgpu_set_venus_caps(const void *capset, uint32_t len)
{
	if (R.caps_size <= 2048 || len > R.caps_size - 2048) {
		fprintf(stderr, "rvgpu: no room for a venus capset of %u bytes in %u bytes of caps\n", len, R.caps_size);
		return;
	}
	rvgpu_guest_write(R.caps + 2048, capset, len);
	rvgpu_guest_store32(R.ring + RVGPU_RING_VCAPS_LEN, len);
	if (!rvgpu_guest_load32(R.ring + RVGPU_RING_EXEC_STATE))
		rvgpu_guest_store32(R.ring + RVGPU_RING_EXEC_STATE, 1);
}

void
rvgpu_present_register(uint32_t res, WGPUTexture tex, uint32_t flags)
{
	unsigned free_slot = 256;
	for (unsigned i = 0; i < 256; i++) {
		if (R.present[i].res == res) {
			free_slot = i;
			break;
		}
		if (!R.present[i].res && free_slot == 256)
			free_slot = i;
	}
	if (free_slot < 256) {
		R.present[free_slot].res = res;
		R.present[free_slot].flags = flags;
		R.present[free_slot].tex = tex;
	}
}

/* (rvgpu_page.c's venus blobs forget theirs too) */
void (*rvgpu_present_forget_hook)(uint32_t res);

void
rvgpu_present_unregister(uint32_t res)
{
	for (unsigned i = 0; i < 256; i++)
		if (R.present[i].res == res)
			R.present[i].res = 0;
	if (rvgpu_present_forget_hook)
		rvgpu_present_forget_hook(res);
}

void
rvgpu_present_add_resolver(rvgpu_present_resolve_fn fn, void *front)
{
	for (unsigned i = 0; i < 4; i++) {
		if (!R.resolver[i].fn || (R.resolver[i].fn == fn && R.resolver[i].front == front)) {
			R.resolver[i].fn = fn;
			R.resolver[i].front = front;
			return;
		}
	}
	fprintf(stderr, "rvgpu: no room for another present resolver\n");
}

bool
rvgpu_ring_busy(void)
{
	return R.draining;
}

bool
rvgpu_present_find(uint32_t res, struct rvgpu_present_image *out)
{
	for (unsigned i = 0; res && i < 256; i++) {
		if (R.present[i].res == res) {
			out->tex = R.present[i].tex;
			out->flags = R.present[i].flags;
			out->width = wgpuTextureGetWidth(out->tex);
			out->height = wgpuTextureGetHeight(out->tex);
			return true;
		}
	}
	for (unsigned i = 0; res && i < 4 && R.resolver[i].fn; i++)
		if (R.resolver[i].fn(R.resolver[i].front, res, out) && out->tex)
			return true;
	return false;
}

WGPUTexture
rvgpu_present_lookup(uint32_t res, uint32_t *flags)
{
	struct rvgpu_present_image img;
	if (!rvgpu_present_find(res, &img))
		return NULL;
	if (flags)
		*flags = img.flags;
	return img.tex;
}
