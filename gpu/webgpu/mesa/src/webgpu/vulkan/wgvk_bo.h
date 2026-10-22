/*
 * wgvk_bo: a range of a GPUBuffer and where its bytes are for the CPU.
 * INTERFACES.md 4.1; owner T1.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WGVK_BO_H
#define WGVK_BO_H

#include <stdint.h>
#include <webgpu/webgpu.h>

enum wgvk_window_kind {
	WGVK_WINDOW_NONE,	/* GPU only */
	WGVK_WINDOW_HOST,	/* a pointer in this process (module heap in the page) */
	WGVK_WINDOW_GUEST,	/* a guest address (the machine's shared memory; rvgpu_guest_*) */
};

struct wgvk_window {
	enum wgvk_window_kind kind;
	union {
		void *host;
		uint64_t guest;
	};
};

enum {
	WGVK_BO_HOST_VISIBLE = 1 << 0,
	WGVK_BO_GPU_WRITTEN = 1 << 1,	/* the GPU wrote it since the last download */
};

struct wgvk_bo {
	WGPUBuffer buffer;	/* NULL for memory only images are bound to */
	uint64_t offset, size;	/* the range of `buffer` this bo is */
	struct wgvk_window window;
	uint32_t flags;
};

struct wgvk_device;

/* The memory sync points (INTERFACES.md 4.1/5), for T2's replay; wgvk_memory.c.
 *
 * Upload on use: before a command reads [offset, offset + size) of a host-visible bo from the GPU, put the
 * window's bytes into the GPUBuffer (queue.writeBuffer; from the guest's pages for a GUEST window). The range
 * is widened to 4-byte bounds. Does nothing for a bo with no window. */
void wgvk_bo_upload(struct wgvk_device *dev, const struct wgvk_bo *bo, uint64_t offset, uint64_t size);

/* Download before a fence: `mapped` is a MAP_READ buffer, mapped, holding the GPU's copy of the bo's
 * [offset, offset + size) at mapped_offset; copy it into the window. */
void wgvk_bo_download(struct wgvk_device *dev, const struct wgvk_bo *bo, uint64_t offset, uint64_t size,
		      WGPUBuffer mapped, uint64_t mapped_offset);

/* The CPU address of a bo's window (HOST, or GUEST natively for tests); NULL when there is none (a GUEST
 * window in the page: the guest maps it itself). */
void *wgvk_bo_map(const struct wgvk_bo *bo);

#endif
