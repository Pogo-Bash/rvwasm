/*
 * rvgpu's guest memory natively (tests): a host block stands in for the
 * machine's memory, guest address = offset in it. INTERFACES.md 3.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "rvgpu.h"

static uint8_t *base;
static uint64_t size;

void
rvgpu_guest_set_host(void *b, uint64_t s)
{
	base = b;
	size = s;
}

void *
rvgpu_guest_ptr(uint64_t addr, size_t len)
{
	return base && addr + len <= size ? base + addr : NULL;
}

uint32_t
rvgpu_guest_load32(uint64_t addr)
{
	return __atomic_load_n((uint32_t *)(base + addr), __ATOMIC_SEQ_CST);
}

void
rvgpu_guest_store32(uint64_t addr, uint32_t v)
{
	__atomic_store_n((uint32_t *)(base + addr), v, __ATOMIC_SEQ_CST);
}

void
rvgpu_guest_read(void *dst, uint64_t addr, size_t len)
{
	memcpy(dst, base + addr, len);
}

void
rvgpu_guest_write(uint64_t addr, const void *src, size_t len)
{
	memcpy(base + addr, src, len);
}

void
rvgpu_guest_upload_buffer(WGPUQueue q, WGPUBuffer b, uint64_t boff, uint64_t addr, size_t len)
{
	wgpuQueueWriteBuffer(q, b, boff, base + addr, len);
}

void
rvgpu_guest_upload_texture(WGPUQueue q, const WGPUTexelCopyTextureInfo *dst, uint64_t addr,
			   const WGPUTexelCopyBufferLayout *layout, const WGPUExtent3D *sz)
{
	WGPUTexelCopyBufferLayout l = *layout;
	wgpuQueueWriteTexture(q, dst, base + addr + l.offset, (size_t)(size - addr - l.offset), &(WGPUTexelCopyBufferLayout){
		0, l.bytesPerRow, l.rowsPerImage }, sz);
}

void
rvgpu_guest_from_mapped(WGPUBuffer mapped, uint64_t boff, uint64_t addr, size_t len)
{
	const void *p = wgpuBufferGetConstMappedRange(mapped, boff, len);
	if (p)
		memcpy(base + addr, p, len);
}
