/*
 * wgvk: descriptor set layouts and pipeline layouts, and the binding remap
 * they give the shader compiler (INTERFACES.md 4.2, include/wgvk_remap.h).
 *
 * A set layout numbers its bindings for WebGPU once (wgvk_remap_assign_set);
 * a pipeline layout (or the runtime's set layouts + push constants, for the
 * vk_shader path) concatenates the sets' tables with set s = group s and
 * picks the driver group: its own group after the app's sets while one is
 * free, else the last group, shared with the app's set 3.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"
#include "wgvk_remap_assign.h"

#include "util/mesa-blake3.h"

static unsigned
stage_index(VkShaderStageFlagBits s)
{
	switch (s) {
	case VK_SHADER_STAGE_VERTEX_BIT: return 0;
	case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return 1;
	case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return 2;
	case VK_SHADER_STAGE_GEOMETRY_BIT: return 3;
	case VK_SHADER_STAGE_FRAGMENT_BIT: return 4;
	default: return 5;
	}
}

static void
set_layout_destroy(struct vk_device *vk_dev, struct vk_descriptor_set_layout *vk_layout)
{
	struct wgvk_descriptor_set_layout *l = container_of(vk_layout, struct wgvk_descriptor_set_layout, vk);
	vk_descriptor_set_layout_destroy(vk_dev, &l->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateDescriptorSetLayout(VkDevice _device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
			       const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	unsigned n = pCreateInfo->bindingCount, n_immutable = 0;
	for (unsigned i = 0; i < n; i++) {
		const VkDescriptorSetLayoutBinding *b = &pCreateInfo->pBindings[i];
		if (b->pImmutableSamplers && (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
					      b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
			n_immutable += b->descriptorCount;
	}
	VK_MULTIALLOC(ma);
	VK_MULTIALLOC_DECL(&ma, struct wgvk_descriptor_set_layout, l, 1);
	VK_MULTIALLOC_DECL(&ma, struct wgvk_binding_remap, remap, MAX2(n, 1));
	VK_MULTIALLOC_DECL(&ma, uint32_t, inline_offset, MAX2(n, 1));
	VK_MULTIALLOC_DECL(&ma, struct wgvk_sampler **, immutable, MAX2(n, 1));
	VK_MULTIALLOC_DECL(&ma, struct wgvk_sampler *, samplers, MAX2(n_immutable, 1));
	if (!vk_descriptor_set_layout_multizalloc(&dev->vk, &ma, pCreateInfo))
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	l->vk.destroy = set_layout_destroy;
	l->remap = remap;
	l->inline_offset = inline_offset;
	l->immutable = immutable;
	l->wg_count = wgvk_remap_assign_set(pCreateInfo->pBindings, n, 0, remap, &l->n_remap);
	for (unsigned i = 0; i < l->n_remap; i++) {
		const VkDescriptorSetLayoutBinding *b = NULL;
		for (unsigned j = 0; j < n && !b; j++)
			if (pCreateInfo->pBindings[j].binding == remap[i].binding)
				b = &pCreateInfo->pBindings[j];
		if (b->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
			/* WebGPU binds uniform ranges at multiples of 256 */
			l->inline_offset[i] = l->inline_size;
			l->inline_size += align(b->descriptorCount, 256);
		}
		if (b->pImmutableSamplers && (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
					      b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
			l->immutable[i] = samplers;
			for (unsigned e = 0; e < b->descriptorCount; e++)
				*samplers++ = wgvk_sampler_from_handle(b->pImmutableSamplers[e]);
		}
	}

	for (unsigned i = 0; i < l->n_remap; i++) {
		const struct wgvk_binding_remap *r = &remap[i];
		if (r->flags & WGVK_REMAP_DYNAMIC)
			l->dynamic_count += r->count;
		u_foreach_bit(s, r->stages & VK_SHADER_STAGE_ALL) {
			unsigned si = stage_index(1u << s);
			switch (r->type) {
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
				l->stage_ubos[si] += r->count;
				break;
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
			case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
				l->stage_ssbos[si] += r->count;
				break;
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
				l->stage_samplers[si] += r->count;
				FALLTHROUGH;
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
			case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
				l->stage_textures[si] += r->count;
				break;
			case VK_DESCRIPTOR_TYPE_SAMPLER:
				l->stage_samplers[si] += r->count;
				break;
			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
				l->stage_images[si] += r->count;
				break;
			default:
				break;
			}
		}
	}
	if (l->wg_count > WGVK_DRIVER_BINDING_BASE)
		wgvk_log("descriptor set layout with %u WebGPU bindings (more than %u): the driver bindings collide\n",
			 l->wg_count, WGVK_DRIVER_BINDING_BASE);

	/* the runtime's pipeline caches key shaders by this */
	struct mesa_blake3 ctx;
	_mesa_blake3_init(&ctx);
	for (unsigned i = 0; i < l->n_remap; i++) {
		const struct wgvk_binding_remap *r = &remap[i];
		uint32_t key[5] = { r->binding, r->type, r->count, r->stages, r->flags };
		_mesa_blake3_update(&ctx, key, sizeof(key));
	}
	_mesa_blake3_final(&ctx, l->vk.blake3);

	*pSetLayout = wgvk_descriptor_set_layout_to_handle(l);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetDescriptorSetLayoutSupport(VkDevice _device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
				   VkDescriptorSetLayoutSupport *pSupport)
{
	unsigned wg = 0;
	for (unsigned i = 0; i < pCreateInfo->bindingCount; i++) {
		const VkDescriptorSetLayoutBinding *b = &pCreateInfo->pBindings[i];
		unsigned count = b->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK ? 1 : b->descriptorCount;
		wg += count * wgvk_remap_slots(b->descriptorType);
	}
	pSupport->supported = wg <= WGVK_DRIVER_BINDING_BASE;
	VkDescriptorSetVariableDescriptorCountLayoutSupport *v =
		vk_find_struct(pSupport->pNext, DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT);
	if (v)
		v->maxVariableDescriptorCount = 0;
}

VkResult
wgvk_layout_remap_build(struct wgvk_physical_device *pdev, struct vk_descriptor_set_layout *const *set_layouts,
			unsigned n, uint32_t push_bytes, struct wgvk_layout_remap *out)
{
	unsigned total = 0, top = 0;
	for (unsigned s = 0; s < n; s++) {
		if (!set_layouts[s])
			continue;
		total += container_of(set_layouts[s], struct wgvk_descriptor_set_layout, vk)->n_remap;
		top = s + 1;
	}
	struct wgvk_binding_remap *b = calloc(MAX2(total, 1), sizeof(*b));
	if (!b)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	unsigned k = 0;
	for (unsigned s = 0; s < n; s++) {
		if (!set_layouts[s])
			continue;
		const struct wgvk_descriptor_set_layout *l =
			container_of(set_layouts[s], struct wgvk_descriptor_set_layout, vk);
		for (unsigned i = 0; i < l->n_remap; i++) {
			b[k] = l->remap[i];
			b[k].set = b[k].group = s;
			k++;
		}
	}
	uint32_t max_groups = pdev ? MIN2(pdev->limits.maxBindGroups, 4) : 4;
	*out = (struct wgvk_layout_remap){
		.b = b,
		.n = k,
		/* its own group after the app's sets while one is free; else shared with the last */
		.driver_group = top < max_groups ? top : max_groups - 1,
		.push_constant_bytes = push_bytes,
		.immediate_bytes = pdev ? pdev->compile_caps.max_immediate_size : 0,
	};
	return VK_SUCCESS;
}

static void
pipeline_layout_destroy(struct vk_device *vk_dev, struct vk_pipeline_layout *vk_layout)
{
	struct wgvk_pipeline_layout *l = container_of(vk_layout, struct wgvk_pipeline_layout, vk);
	free(l->b);
	vk_pipeline_layout_destroy(vk_dev, &l->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreatePipelineLayout(VkDevice _device, const VkPipelineLayoutCreateInfo *pCreateInfo,
			  const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	struct wgvk_pipeline_layout *l = vk_pipeline_layout_zalloc(&dev->vk, sizeof(*l), pCreateInfo);
	if (!l)
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	l->vk.destroy = pipeline_layout_destroy;
	uint32_t push = 0;
	for (unsigned i = 0; i < pCreateInfo->pushConstantRangeCount; i++)
		push = MAX2(push, pCreateInfo->pPushConstantRanges[i].offset + pCreateInfo->pPushConstantRanges[i].size);
	VkResult r = wgvk_layout_remap_build(dev->pdev, l->vk.set_layouts, l->vk.set_count, push, &l->remap);
	if (r != VK_SUCCESS) {
		vk_pipeline_layout_destroy(&dev->vk, &l->vk);
		return vk_error(dev, r);
	}
	l->b = (struct wgvk_binding_remap *)l->remap.b;
	if (getenv("WGVK_DEBUG") && strstr(getenv("WGVK_DEBUG"), "remap")) {
		wgvk_log("remap: %u entries, driver group %u, push constants %u (immediates up to %u)\n", l->remap.n,
			 l->remap.driver_group, l->remap.push_constant_bytes, l->remap.immediate_bytes);
		for (unsigned i = 0; i < l->remap.n; i++) {
			const struct wgvk_binding_remap *b = &l->remap.b[i];
			wgvk_log("remap: set %u binding %u type %u count %u -> group %u binding %u sampler %u flags %u\n",
				 b->set, b->binding, b->type, b->count, b->group, b->wg_binding, b->wg_sampler_binding,
				 b->flags);
		}
	}
	*pPipelineLayout = wgvk_pipeline_layout_to_handle(l);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroyPipelineLayout(VkDevice _device, VkPipelineLayout _layout, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_pipeline_layout, l, _layout);
	if (l)
		vk_pipeline_layout_unref(&dev->vk, &l->vk);
}
