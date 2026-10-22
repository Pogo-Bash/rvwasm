/*
 * wgvk: the buffer heap's driver side (INTERFACES.md 4.4). A stage that
 * reads more storage buffers than WebGPU's per-stage limit has the excess
 * lowered by T3b to the heap: a table at driver binding 902 with one uvec4
 * {arena, byte offset, byte size, 0} per heap slot, and the arenas, whole
 * storage buffers at driver bindings 910 + a. An "arena" here is simply a
 * GPUBuffer the slots' VkBuffers live in (a memory object's buffer), bound
 * over the window that covers them; a stage may use WGVK_HEAP_ARENAS of them
 * (compile caps), so its heap slots must fall in that many memory objects.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "wgvk_private.h"

int
wgvk_heap_build(struct wgvk_device *dev, const struct wgvk_descriptor *const *slots, unsigned n, uint32_t *table,
		struct wgvk_heap_arena arenas[WGVK_HEAP_ARENAS])
{
	const uint64_t max_bind = dev->pdev->limits.maxStorageBufferBindingSize;
	const uint32_t align = dev->pdev->limits.minStorageBufferOffsetAlignment;
	unsigned na = 0;
	uint64_t lo[WGVK_HEAP_ARENAS], hi[WGVK_HEAP_ARENAS];
	/* first pass: which buffers, and the window each must cover */
	for (unsigned i = 0; i < n; i++) {
		const struct wgvk_descriptor *d = slots[i];
		memset(&table[4 * i], 0, 16);
		if (!d || d->kind != WGVK_DESC_BUFFER || !d->buf.buffer->mem || !d->buf.buffer->mem->bo.buffer)
			continue;	/* size 0: reads 0, writes nothing */
		const struct wgvk_buffer *b = d->buf.buffer;
		WGPUBuffer wb = b->mem->bo.buffer;
		uint64_t start = b->mem->bo.offset + b->offset + d->buf.offset, end = start + d->buf.range;
		unsigned a;
		for (a = 0; a < na && arenas[a].buffer != wb; a++)
			;
		if (a == na) {
			if (na == WGVK_HEAP_ARENAS) {
				wgvk_warn_once("buffer heap: the storage buffers of a stage are in more than %u memory "
					       "objects; the rest read 0\n", WGVK_HEAP_ARENAS);
				continue;
			}
			arenas[na].buffer = wb;
			lo[na] = start, hi[na] = end;
			na++;
		}
		lo[a] = MIN2(lo[a], start);
		hi[a] = MAX2(hi[a], end);
	}
	for (unsigned a = 0; a < na; a++) {
		arenas[a].offset = lo[a] / align * align;
		arenas[a].size = align64(hi[a] - arenas[a].offset, 4);
		if (arenas[a].size > max_bind) {
			wgvk_warn_once("buffer heap: storage buffers %llu bytes apart in one memory object (WebGPU binds "
				       "at most %llu)\n", (unsigned long long)arenas[a].size, (unsigned long long)max_bind);
			arenas[a].size = max_bind;
		}
	}
	/* second pass: the table, relative to each arena's window */
	for (unsigned i = 0; i < n; i++) {
		const struct wgvk_descriptor *d = slots[i];
		if (!d || d->kind != WGVK_DESC_BUFFER || !d->buf.buffer->mem || !d->buf.buffer->mem->bo.buffer)
			continue;
		const struct wgvk_buffer *b = d->buf.buffer;
		uint64_t start = b->mem->bo.offset + b->offset + d->buf.offset;
		for (unsigned a = 0; a < na; a++) {
			if (arenas[a].buffer != b->mem->bo.buffer)
				continue;
			uint64_t rel = start - arenas[a].offset;
			table[4 * i + 0] = a;
			table[4 * i + 1] = rel;
			table[4 * i + 2] = rel < arenas[a].size ? MIN2(d->buf.range, arenas[a].size - rel) : 0;
		}
	}
	/* unused arena bindings still need a buffer */
	for (unsigned a = na; a < WGVK_HEAP_ARENAS; a++)
		arenas[a] = (struct wgvk_heap_arena){ wgvk_dummy_buffer(dev), 0, 256 };
	return na;
}
