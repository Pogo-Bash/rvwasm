/*
 * The binding remap's numbering (include/wgvk_remap.h, INTERFACES.md 4.2).
 * Pure functions, no device: the descriptor set layout uses them, and so do
 * the compiler's test tools (wgsl-corpus -layout), so both number alike.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_remap.h"
#include "wgvk_remap_assign.h"

static int
by_binding(const void *a, const void *b)
{
	const VkDescriptorSetLayoutBinding *x = *(const VkDescriptorSetLayoutBinding *const *)a;
	const VkDescriptorSetLayoutBinding *y = *(const VkDescriptorSetLayoutBinding *const *)b;
	return (int)x->binding - (int)y->binding;
}

/* How many WebGPU bindings one element of a descriptor type takes. */
unsigned
wgvk_remap_slots(VkDescriptorType type)
{
	return type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? 2 : 1;
}

unsigned
wgvk_remap_assign_set(const VkDescriptorSetLayoutBinding *bindings, unsigned n, uint8_t set,
		      struct wgvk_binding_remap *out, unsigned *n_out)
{
	const VkDescriptorSetLayoutBinding *sorted_small[64];
	const VkDescriptorSetLayoutBinding **sorted = n <= 64 ? sorted_small : malloc(n * sizeof(*sorted));
	for (unsigned i = 0; i < n; i++)
		sorted[i] = &bindings[i];
	qsort(sorted, n, sizeof(*sorted), by_binding);

	unsigned next = 0, k = 0;
	for (unsigned i = 0; i < n; i++) {
		const VkDescriptorSetLayoutBinding *b = sorted[i];
		if (!b->descriptorCount)
			continue;
		/* an inline uniform block's descriptorCount is its size in bytes: one uniform buffer */
		unsigned count = b->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? 1 : b->descriptorCount;
		struct wgvk_binding_remap *r = &out[k++];
		*r = (struct wgvk_binding_remap){
			.set = set,
			.group = set,
			.binding = b->binding,
			.wg_binding = next,
			.wg_sampler_binding = next,
			.count = count,
			.type = b->descriptorType,
			.stages = b->stageFlags,
		};
		if (b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
			r->wg_sampler_binding = next + count;
		if (b->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
		    b->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
			r->flags |= WGVK_REMAP_DYNAMIC;
		next += count * wgvk_remap_slots(b->descriptorType);
	}
	if (sorted != sorted_small)
		free(sorted);
	*n_out = k;
	return next;
}

const struct wgvk_binding_remap *
wgvk_remap_find(const struct wgvk_layout_remap *l, unsigned set, unsigned binding)
{
	/* ordered by (set, binding): binary search */
	unsigned lo = 0, hi = l->n;
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		const struct wgvk_binding_remap *r = &l->b[mid];
		if (r->set == set && r->binding == binding)
			return r;
		if (r->set < set || (r->set == set && r->binding < binding))
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}
