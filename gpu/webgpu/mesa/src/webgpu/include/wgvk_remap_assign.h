/*
 * The remap's numbering as functions (vulkan/wgvk_remap.c). T1's; the
 * compiler's test tools link them too, so a test layout numbers like wgvk.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WGVK_REMAP_ASSIGN_H
#define WGVK_REMAP_ASSIGN_H

#include "wgvk_remap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WebGPU bindings one array element of `type` takes (2 for a combined image sampler). */
unsigned wgvk_remap_slots(VkDescriptorType type);

/* Number one set layout's bindings: fills out[] (at most n entries, ordered by
 * binding; bindings with no descriptors are left out) with group = set, sets
 * *n_out, and returns how many WebGPU bindings the set uses (0..ret-1). */
unsigned wgvk_remap_assign_set(const VkDescriptorSetLayoutBinding *bindings, unsigned n, uint8_t set,
			       struct wgvk_binding_remap *out, unsigned *n_out);

/* The entry of (set, binding), or NULL. */
const struct wgvk_binding_remap *wgvk_remap_find(const struct wgvk_layout_remap *l, unsigned set,
						 unsigned binding);

#ifdef __cplusplus
}
#endif

#endif
