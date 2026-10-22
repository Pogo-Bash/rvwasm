/*
 * wgvk's format table: VkFormat -> WebGPU texture and vertex formats, and the
 * Vulkan format features that follow from WebGPU's per-format capabilities
 * (wgvk_format.c). INTERFACES.md 4.5. Owner T1.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WGVK_FORMAT_H
#define WGVK_FORMAT_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#include <webgpu/webgpu.h>

struct wgvk_physical_device;

enum wgvk_format_flags {
	WGVK_FMT_DEPTH = 1 << 0,
	WGVK_FMT_STENCIL = 1 << 1,
	WGVK_FMT_COMPRESSED = 1 << 2,
	/* stored as `wf` with the components in another order (or a constant alpha): views read it through
	 * `swizzle` (texture-component-swizzle); it cannot be rendered to or used as a storage image */
	WGVK_FMT_SWIZZLED = 1 << 3,
	/* the depth aspect is neither copy source nor destination in WebGPU (depth24plus*): T2 copies it
	 * through a shader */
	WGVK_FMT_DEPTH_NO_COPY = 1 << 4,
	/* the depth aspect cannot be a copy destination (depth32float*): T2 writes it through a shader */
	WGVK_FMT_DEPTH_NO_COPY_DST = 1 << 5,
	/* stored in another format with less precision (D32S8 as depth24plus-stencil8 without the
	 * depth32float-stencil8 feature) */
	WGVK_FMT_SUBSTITUTE = 1 << 6,
	/* a vertex format WebGPU lacks: the vertex shader fetches it from a storage buffer (vertex pulling,
	 * T3b; wgvk_shader_key.vertex_pulling_mask) */
	WGVK_FMT_VERTEX_PULL = 1 << 7,
};

/* WebGPU capabilities of a texture format on this device (Dawn's Format.cpp rules, which are the spec's) */
enum wgvk_wgpu_cap {
	WGVK_CAP_RENDER = 1 << 0,
	WGVK_CAP_BLEND = 1 << 1,
	WGVK_CAP_MULTISAMPLE = 1 << 2,
	WGVK_CAP_RESOLVE = 1 << 3,
	WGVK_CAP_STORAGE_W = 1 << 4,	/* write-only storage */
	WGVK_CAP_STORAGE_R = 1 << 5,	/* read-only storage */
	WGVK_CAP_STORAGE_RW = 1 << 6,	/* read-write storage */
	WGVK_CAP_FILTER = 1 << 7,	/* sample type "float" (filterable) */
	WGVK_CAP_SAMPLE = 1 << 8,	/* usable as a texture at all (format supported) */
};

struct wgvk_format {
	WGPUTextureFormat wf;		/* WGPUTextureFormat_Undefined: no images of this format */
	WGPUVertexFormat vf;		/* 0 (undefined): not a vertex format (vertex pulling later, T3b) */
	uint16_t flags;			/* enum wgvk_format_flags */
	uint16_t caps;			/* enum wgvk_wgpu_cap of wf */
	WGPUComponentSwizzle swizzle[4];	/* WGVK_FMT_SWIZZLED: per r, g, b, a of the Vulkan format */
	uint8_t block_bytes, block_w, block_h;	/* of the Vulkan format (copies, linear layouts) */
	VkFormatFeatureFlags2 optimal, linear, buffer;
};

/* The device's table entry, or NULL for a format wgvk does not know. */
const struct wgvk_format *wgvk_format_get(const struct wgvk_physical_device *pdev, VkFormat f);
void wgvk_format_table_init(struct wgvk_physical_device *pdev);

/* The sRGB / non-sRGB partner WebGPU allows as a view format, or Undefined. */
WGPUTextureFormat wgvk_wgpu_srgb_partner(WGPUTextureFormat f);
bool wgvk_wgpu_format_is_depth_stencil(WGPUTextureFormat f);

/* Old name, kept for the skeleton's callers */
WGPUTextureFormat wgvk_format_to_wgpu(VkFormat f, uint32_t *bpp);

#endif
