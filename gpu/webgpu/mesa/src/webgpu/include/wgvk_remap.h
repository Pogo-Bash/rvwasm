/*
 * The binding remap: where each Vulkan descriptor (set, binding, array element)
 * is in WebGPU (group, binding). gpu/webgpu/INTERFACES.md, section 4.2. T1
 * (wgvk objects) owns the layout and builds the table from a pipeline layout;
 * T3 (the shader compiler) reads it.
 *
 * Rules (wgvk_descriptor.c, wgvk_set_layout_assign):
 *   - set s is group s;
 *   - within a set, Vulkan bindings in increasing binding order take
 *     consecutive WebGPU bindings from 0, one per array element; a combined
 *     image sampler takes its count textures, then its count samplers; an
 *     inline uniform block takes one (a uniform buffer the set owns);
 *   - texel buffers are storage buffers in WebGPU (the shader decodes the
 *     format), input attachments are textures;
 *   - the driver's bindings (push constants over the immediate size, draw
 *     parameters, buffer heap, poly scratch) are WGVK_DRIVER_BINDING_BASE..999
 *     of driver_group.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WGVK_REMAP_H
#define WGVK_REMAP_H

#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WGVK_DRIVER_BINDING_BASE 900

/* The fixed driver bindings (push constants 900, driver uniforms 901, heap table 902, heap arenas 910 + a,
 * pulled vertex buffers 920 + i, ...) and WGVK_DRIVER_SET are T3's: compiler/wgvk_compile.h. */

/* wgvk_binding_remap.flags */
enum {
	WGVK_REMAP_DYNAMIC = 1 << 0,	/* a dynamic UBO/SSBO: WebGPU dynamic offset */
	WGVK_REMAP_HEAP = 1 << 1,	/* (4.4, not yet) an SSBO reached through the buffer heap: wg_binding is
					 * its first entry in the heap table, not a WebGPU binding */
};

#ifndef WGVK_LAYOUT_REMAP_DEFINED
#define WGVK_LAYOUT_REMAP_DEFINED
struct wgvk_binding_remap {		/* one per (set, binding) that has descriptors */
	uint8_t set, group;
	uint16_t binding;		/* the Vulkan binding number */
	uint16_t wg_binding;		/* first WebGPU binding: element i is wg_binding + i */
	uint16_t wg_sampler_binding;	/* combined image samplers: the sampler of element i is this + i */
	uint16_t count;			/* array elements (an inline uniform block: 1) */
	uint8_t flags;			/* WGVK_REMAP_* */
	VkDescriptorType type;
	VkShaderStageFlags stages;	/* VkDescriptorSetLayoutBinding::stageFlags */
};
struct wgvk_layout_remap {
	const struct wgvk_binding_remap *b;	/* ordered by (set, binding) */
	unsigned n;
	uint8_t driver_group;		/* the group of bindings WGVK_DRIVER_BINDING_BASE.. */
	uint16_t push_constant_bytes;	/* the pipeline layout's push constant size (end of the last range) */
	uint16_t immediate_bytes;	/* push constants <= this are immediates; more go to the driver UBO */
};
#endif

#ifdef __cplusplus
}
#endif

#endif
