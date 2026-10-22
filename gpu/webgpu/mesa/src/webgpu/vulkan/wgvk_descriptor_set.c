/*
 * wgvk: descriptor pools and sets, their updates (writes, copies,
 * templates), and the WebGPU side: bind group layouts and bind groups,
 * both cached by content (INTERFACES.md 4.3).
 *
 * A set is an array of wgvk_descriptor indexed by WebGPU binding (the
 * remap's numbering, wgvk_remap.c), so a set's bind group is a walk over
 * the bind group layout's entries. Bind group layouts cannot come from
 * Vulkan set layouts alone (WebGPU wants each texture's sample type and
 * dimension and each sampler's filtering type, which only the shaders and
 * the bound views say), so T2 makes them per pipeline from the compiled
 * shaders (wgvk_bgl_entries_from_shaders) and bind groups are made when a
 * set is first used with a layout:
 *   - the set remembers its last (layout, generation) and bind group;
 *   - the device keeps bind groups by content (layout id + each entry's
 *     object id, offset, size), so equal sets share one (zink makes many),
 *     least recently used dropped beyond WGVK_BIND_GROUP_CACHE;
 *   - object ids are never reused, so a freed object's bind group can only
 *     age out, never be hit.
 * Unwritten descriptors (nullDescriptor, or partially bound arrays) get
 * dummy resources of the right kind.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"
#include "wgvk_remap_assign.h"

#include "util/hash_table.h"
#include "util/list.h"
#include "util/u_dynarray.h"
#include "vk_descriptor_update_template.h"
#include "vk_format.h"
#define XXH_INLINE_ALL
#include "util/xxhash.h"

#define WGVK_BIND_GROUP_CACHE 4096

struct cached_bg {
	struct list_head lru;
	uint64_t hash;
	unsigned n;
	uint64_t *key;
	WGPUBindGroup bg;
};

struct dummy_texture {
	uint32_t key;
	WGPUTexture tex;
	WGPUTextureView view;
};

struct wgvk_caches {
	struct hash_table_u64 *bgls;		/* content hash -> struct wgvk_bgl */
	struct hash_table_u64 *bgs;		/* content hash -> struct cached_bg */
	struct list_head lru;			/* most recent first */
	unsigned n_bgs;
	WGPUBuffer dummy_buffer;
	WGPUSampler dummy_sampler[2];		/* filtering, comparison */
	struct util_dynarray dummy_textures;	/* struct dummy_texture */
	uint64_t hits, misses;
};

void
wgvk_caches_init(struct wgvk_device *dev)
{
	struct wgvk_caches *c = calloc(1, sizeof(*c));
	c->bgls = _mesa_hash_table_u64_create(NULL);
	c->bgs = _mesa_hash_table_u64_create(NULL);
	list_inithead(&c->lru);
	util_dynarray_init(&c->dummy_textures, NULL);
	dev->caches = c;
}

void
wgvk_caches_finish(struct wgvk_device *dev)
{
	struct wgvk_caches *c = dev->caches;
	if (!c)
		return;
	if (wgvk_debug("cache"))
		wgvk_log("bind group cache: %llu hits, %llu made, %u kept\n", (unsigned long long)c->hits,
			 (unsigned long long)c->misses, c->n_bgs);
	list_for_each_entry_safe(struct cached_bg, e, &c->lru, lru) {
		wgpuBindGroupRelease(e->bg);
		free(e->key);
		free(e);
	}
	hash_table_u64_foreach(c->bgls, he) {
		struct wgvk_bgl *b = he.data;
		wgpuBindGroupLayoutRelease(b->layout);
		free(b->entries);
		free(b);
	}
	_mesa_hash_table_u64_destroy(c->bgls);
	_mesa_hash_table_u64_destroy(c->bgs);
	if (c->dummy_buffer)
		wgpuBufferRelease(c->dummy_buffer);
	for (unsigned i = 0; i < 2; i++)
		if (c->dummy_sampler[i])
			wgpuSamplerRelease(c->dummy_sampler[i]);
	util_dynarray_foreach(&c->dummy_textures, struct dummy_texture, d) {
		wgpuTextureViewRelease(d->view);
		wgpuTextureRelease(d->tex);
	}
	util_dynarray_fini(&c->dummy_textures);
	free(c);
	dev->caches = NULL;
}

/* ---- pools and sets */

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateDescriptorPool(VkDevice _device, const VkDescriptorPoolCreateInfo *pCreateInfo,
			  const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pPool)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	struct wgvk_descriptor_pool *pool =
		vk_object_zalloc(&dev->vk, pAllocator, sizeof(*pool), VK_OBJECT_TYPE_DESCRIPTOR_POOL);
	if (!pool)
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	list_inithead(&pool->sets);
	*pPool = wgvk_descriptor_pool_to_handle(pool);
	return VK_SUCCESS;
}

struct wgvk_descriptor_set *
wgvk_descriptor_set_create(struct wgvk_device *dev, struct wgvk_descriptor_set_layout *l)
{
	struct wgvk_descriptor_set *set =
		vk_object_zalloc(&dev->vk, NULL, sizeof(*set) + l->wg_count * sizeof(struct wgvk_descriptor) + l->inline_size,
				 VK_OBJECT_TYPE_DESCRIPTOR_SET);
	if (!set)
		return NULL;
	vk_descriptor_set_layout_ref(&l->vk);
	set->layout = l;
	set->count = l->wg_count;
	set->desc = (struct wgvk_descriptor *)(set + 1);
	set->inline_data = l->inline_size ? (uint8_t *)(set->desc + l->wg_count) : NULL;
	set->id = wgvk_object_id(dev);
	list_inithead(&set->link);
	for (unsigned i = 0; i < l->n_remap; i++) {
		const struct wgvk_binding_remap *r = &l->remap[i];
		for (unsigned e = 0; e < r->count; e++) {
			struct wgvk_descriptor *d = &set->desc[r->wg_binding + e];
			d->type = r->type;
			if (r->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
				d->kind = WGVK_DESC_INLINE;
				d->inl.offset = l->inline_offset[i];
				d->inl.size = 0;
			}
			if (r->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
				set->desc[r->wg_sampler_binding + e].type = r->type;
			if (l->immutable[i]) {
				struct wgvk_descriptor *sd = &set->desc[r->wg_sampler_binding + e];
				sd->kind = WGVK_DESC_SAMPLER;
				sd->smp.sampler = l->immutable[i][e];
				sd->id = sd->smp.sampler->id;
			}
		}
		if (r->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
			/* the block's size: the next entry's offset, or the end */
			uint32_t end = l->inline_size;
			for (unsigned j = 0; j < l->n_remap; j++)
				if (l->remap[j].type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK &&
				    l->inline_offset[j] > l->inline_offset[i] && l->inline_offset[j] < end)
					end = l->inline_offset[j];
			set->desc[r->wg_binding].inl.size = end - l->inline_offset[i];
		}
	}
	set->inline_dirty = true;
	return set;
}

void
wgvk_descriptor_set_destroy(struct wgvk_device *dev, struct wgvk_descriptor_set *set)
{
	list_del(&set->link);
	if (set->last_bg)
		wgpuBindGroupRelease(set->last_bg);
	if (set->inline_buffer) {
		wgpuBufferDestroy(set->inline_buffer);
		wgpuBufferRelease(set->inline_buffer);
	}
	vk_descriptor_set_layout_unref(&dev->vk, &set->layout->vk);
	vk_object_free(&dev->vk, NULL, set);
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_AllocateDescriptorSets(VkDevice _device, const VkDescriptorSetAllocateInfo *pAllocateInfo,
			    VkDescriptorSet *pDescriptorSets)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_descriptor_pool, pool, pAllocateInfo->descriptorPool);
	for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
		VK_FROM_HANDLE(wgvk_descriptor_set_layout, l, pAllocateInfo->pSetLayouts[i]);
		struct wgvk_descriptor_set *set = wgvk_descriptor_set_create(dev, l);
		if (!set) {
			for (uint32_t j = 0; j < i; j++)
				wgvk_descriptor_set_destroy(dev, wgvk_descriptor_set_from_handle(pDescriptorSets[j]));
			for (uint32_t j = 0; j < pAllocateInfo->descriptorSetCount; j++)
				pDescriptorSets[j] = VK_NULL_HANDLE;
			return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
		}
		set->pool = pool;
		list_addtail(&set->link, &pool->sets);
		pDescriptorSets[i] = wgvk_descriptor_set_to_handle(set);
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_FreeDescriptorSets(VkDevice _device, VkDescriptorPool descriptorPool, uint32_t count,
			const VkDescriptorSet *pDescriptorSets)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	for (uint32_t i = 0; i < count; i++) {
		VK_FROM_HANDLE(wgvk_descriptor_set, set, pDescriptorSets[i]);
		if (set)
			wgvk_descriptor_set_destroy(dev, set);
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_ResetDescriptorPool(VkDevice _device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_descriptor_pool, pool, descriptorPool);
	list_for_each_entry_safe(struct wgvk_descriptor_set, set, &pool->sets, link)
		wgvk_descriptor_set_destroy(dev, set);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroyDescriptorPool(VkDevice _device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_descriptor_pool, pool, descriptorPool);
	if (!pool)
		return;
	wgvk_ResetDescriptorPool(_device, descriptorPool, 0);
	vk_object_free(&dev->vk, pAllocator, pool);
}

/* ---- writes */

static const struct wgvk_binding_remap *
find_binding(const struct wgvk_descriptor_set_layout *l, uint32_t binding, unsigned *index)
{
	unsigned lo = 0, hi = l->n_remap;
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		if (l->remap[mid].binding == binding) {
			*index = mid;
			return &l->remap[mid];
		}
		if (l->remap[mid].binding < binding)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/* The element after (binding, element): Vulkan's writes run on into the next bindings. */
static const struct wgvk_binding_remap *
advance(const struct wgvk_descriptor_set_layout *l, unsigned *index, uint32_t *element)
{
	while (*index < l->n_remap && *element >= l->remap[*index].count) {
		*element -= l->remap[*index].count;
		(*index)++;
	}
	return *index < l->n_remap ? &l->remap[*index] : NULL;
}

static void
write_image(struct wgvk_descriptor_set *set, const struct wgvk_binding_remap *r, unsigned li, uint32_t e,
	    const VkDescriptorImageInfo *ii)
{
	bool has_image = r->type != VK_DESCRIPTOR_TYPE_SAMPLER;
	bool has_sampler = r->type == VK_DESCRIPTOR_TYPE_SAMPLER || r->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	if (has_image) {
		struct wgvk_descriptor *d = &set->desc[r->wg_binding + e];
		VK_FROM_HANDLE(wgvk_image_view, v, ii->imageView);
		d->kind = v ? WGVK_DESC_IMAGE : WGVK_DESC_NONE;
		d->img.view = v;
		d->img.layout = ii->imageLayout;
		d->id = v ? v->id : 0;
	}
	if (has_sampler && !set->layout->immutable[li]) {
		struct wgvk_descriptor *d = &set->desc[r->wg_sampler_binding + e];
		VK_FROM_HANDLE(wgvk_sampler, s, ii->sampler);
		d->kind = s ? WGVK_DESC_SAMPLER : WGVK_DESC_NONE;
		d->smp.sampler = s;
		d->id = s ? s->id : 0;
	}
}

static void
write_buffer(struct wgvk_descriptor_set *set, const struct wgvk_binding_remap *r, uint32_t e,
	     const VkDescriptorBufferInfo *bi)
{
	struct wgvk_descriptor *d = &set->desc[r->wg_binding + e];
	VK_FROM_HANDLE(wgvk_buffer, b, bi->buffer);
	d->kind = b ? WGVK_DESC_BUFFER : WGVK_DESC_NONE;
	d->buf.buffer = b;
	d->buf.offset = bi->offset;
	d->buf.range = b ? vk_buffer_range(&b->vk, bi->offset, bi->range) : 0;
	d->id = b ? b->id : 0;
}

static void
write_texel(struct wgvk_descriptor_set *set, const struct wgvk_binding_remap *r, uint32_t e, VkBufferView view)
{
	struct wgvk_descriptor *d = &set->desc[r->wg_binding + e];
	VK_FROM_HANDLE(wgvk_buffer_view, v, view);
	d->kind = v ? WGVK_DESC_TEXEL : WGVK_DESC_NONE;
	d->texel.view = v;
	d->id = v ? v->id : 0;
}

static void
write_inline(struct wgvk_descriptor_set *set, const struct wgvk_binding_remap *r, unsigned li, uint32_t byte_offset,
	     const void *data, uint32_t size)
{
	memcpy(set->inline_data + set->layout->inline_offset[li] + byte_offset, data, size);
	set->inline_dirty = true;
	set->inline_generation++;
}

void
wgvk_descriptor_set_write(struct wgvk_device *dev, struct wgvk_descriptor_set *set, const VkWriteDescriptorSet *w)
{
	const struct wgvk_descriptor_set_layout *l = set->layout;
	unsigned li;
	const struct wgvk_binding_remap *r = find_binding(l, w->dstBinding, &li);
	if (!r)
		return;
	set->generation++;
	if (w->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
		const VkWriteDescriptorSetInlineUniformBlock *ib =
			vk_find_struct_const(w->pNext, WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK);
		if (ib)
			write_inline(set, r, li, w->dstArrayElement, ib->pData, ib->dataSize);
		return;
	}
	uint32_t e = w->dstArrayElement;
	for (uint32_t i = 0; i < w->descriptorCount; i++, e++) {
		r = advance(l, &li, &e);
		if (!r)
			return;
		switch (w->descriptorType) {
		case VK_DESCRIPTOR_TYPE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			write_image(set, r, li, e, &w->pImageInfo[i]);
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
			write_buffer(set, r, e, &w->pBufferInfo[i]);
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			write_texel(set, r, e, w->pTexelBufferView[i]);
			break;
		default:
			break;
		}
	}
}

VKAPI_ATTR void VKAPI_CALL
wgvk_UpdateDescriptorSets(VkDevice _device, uint32_t writeCount, const VkWriteDescriptorSet *pWrites,
			  uint32_t copyCount, const VkCopyDescriptorSet *pCopies)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	for (uint32_t i = 0; i < writeCount; i++)
		wgvk_descriptor_set_write(dev, wgvk_descriptor_set_from_handle(pWrites[i].dstSet), &pWrites[i]);
	for (uint32_t i = 0; i < copyCount; i++) {
		const VkCopyDescriptorSet *c = &pCopies[i];
		VK_FROM_HANDLE(wgvk_descriptor_set, src, c->srcSet);
		VK_FROM_HANDLE(wgvk_descriptor_set, dst, c->dstSet);
		unsigned si, di;
		const struct wgvk_binding_remap *sr = find_binding(src->layout, c->srcBinding, &si);
		const struct wgvk_binding_remap *dr = find_binding(dst->layout, c->dstBinding, &di);
		if (!sr || !dr)
			continue;
		dst->generation++;
		if (sr->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
			write_inline(dst, dr, di, c->dstArrayElement,
				     src->inline_data + src->layout->inline_offset[si] + c->srcArrayElement, c->descriptorCount);
			continue;
		}
		uint32_t se = c->srcArrayElement, de = c->dstArrayElement;
		for (uint32_t k = 0; k < c->descriptorCount; k++, se++, de++) {
			sr = advance(src->layout, &si, &se);
			dr = advance(dst->layout, &di, &de);
			if (!sr || !dr)
				break;
			dst->desc[dr->wg_binding + de] = src->desc[sr->wg_binding + se];
			if (sr->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && !dst->layout->immutable[di])
				dst->desc[dr->wg_sampler_binding + de] = src->desc[sr->wg_sampler_binding + se];
		}
	}
}

void
wgvk_descriptor_set_write_template(struct wgvk_device *dev, struct wgvk_descriptor_set *set,
				   const struct vk_descriptor_update_template *t, const void *data)
{
	const struct wgvk_descriptor_set_layout *l = set->layout;
	set->generation++;
	for (uint32_t i = 0; i < t->entry_count; i++) {
		const struct vk_descriptor_template_entry *te = &t->entries[i];
		unsigned li;
		const struct wgvk_binding_remap *r = find_binding(l, te->binding, &li);
		if (!r)
			continue;
		if (te->type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
			write_inline(set, r, li, te->array_element, (const uint8_t *)data + te->offset, te->array_count);
			continue;
		}
		uint32_t e = te->array_element;
		for (uint32_t k = 0; k < te->array_count; k++, e++) {
			r = advance(l, &li, &e);
			if (!r)
				break;
			const void *p = (const uint8_t *)data + te->offset + k * te->stride;
			switch (te->type) {
			case VK_DESCRIPTOR_TYPE_SAMPLER:
			case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
			case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
				write_image(set, r, li, e, p);
				break;
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
				write_buffer(set, r, e, p);
				break;
			case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
			case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
				write_texel(set, r, e, *(const VkBufferView *)p);
				break;
			default:
				break;
			}
		}
	}
}

VKAPI_ATTR void VKAPI_CALL
wgvk_UpdateDescriptorSetWithTemplate(VkDevice _device, VkDescriptorSet descriptorSet,
				     VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void *pData)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_descriptor_set, set, descriptorSet);
	VK_FROM_HANDLE(vk_descriptor_update_template, t, descriptorUpdateTemplate);
	wgvk_descriptor_set_write_template(dev, set, t, pData);
}

/* ---- bind group layouts */

static uint64_t
hash_bgl_entries(const WGPUBindGroupLayoutEntry *e, unsigned n)
{
	XXH64_state_t st;
	XXH64_reset(&st, 0);
	for (unsigned i = 0; i < n; i++) {
		uint64_t k[12] = {
			e[i].binding, e[i].visibility, e[i].buffer.type, e[i].buffer.hasDynamicOffset,
			e[i].buffer.minBindingSize, e[i].sampler.type, e[i].texture.sampleType,
			e[i].texture.viewDimension, e[i].texture.multisampled, e[i].storageTexture.access,
			e[i].storageTexture.format, e[i].storageTexture.viewDimension,
		};
		XXH64_update(&st, k, sizeof(k));
	}
	return XXH64_digest(&st);
}

struct wgvk_bgl *
wgvk_bgl_get(struct wgvk_device *dev, const WGPUBindGroupLayoutEntry *entries, unsigned n)
{
	struct wgvk_caches *c = dev->caches;
	uint64_t h = hash_bgl_entries(entries, n);
	struct wgvk_bgl *b = _mesa_hash_table_u64_search(c->bgls, h);
	if (b && b->n == n)
		return b;
	b = calloc(1, sizeof(*b));
	b->entries = malloc(MAX2(n, 1) * sizeof(*entries));
	memcpy(b->entries, entries, n * sizeof(*entries));
	b->n = n;
	b->id = wgvk_object_id(dev);
	WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	d.entryCount = n;
	d.entries = b->entries;
	b->layout = wgpuDeviceCreateBindGroupLayout(dev->wdev, &d);
	_mesa_hash_table_u64_insert(c->bgls, h, b);
	return b;
}

static WGPUTextureViewDimension
wgsl_view_dimension(unsigned dim, bool arrayed)
{
	switch (dim) {
	case GLSL_SAMPLER_DIM_3D: return WGPUTextureViewDimension_3D;
	case GLSL_SAMPLER_DIM_CUBE: return arrayed ? WGPUTextureViewDimension_CubeArray : WGPUTextureViewDimension_Cube;
	default: return arrayed ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
	}
}

static int
cmp_entry(const void *a, const void *b)
{
	return (int)((const WGPUBindGroupLayoutEntry *)a)->binding - (int)((const WGPUBindGroupLayoutEntry *)b)->binding;
}

unsigned
wgvk_bgl_entries_from_shaders(struct wgvk_device *dev, const struct wgvk_layout_remap *remap,
			      const struct wgvk_compiled_shader *const *shaders, const WGPUShaderStage *stages,
			      unsigned n_shaders, unsigned group, WGPUBindGroupLayoutEntry *out, unsigned max)
{
	unsigned n = 0;
	for (unsigned s = 0; s < n_shaders; s++) {
		const struct wgvk_compiled_shader *sh = shaders[s];
		for (unsigned i = 0; sh && i < sh->n_bindings; i++) {
			const struct wgvk_shader_binding *b = &sh->bindings[i];
			if (b->group != group)
				continue;
			WGPUBindGroupLayoutEntry *e = NULL;
			for (unsigned k = 0; k < n; k++)
				if (out[k].binding == b->binding)
					e = &out[k];
			if (e) {
				e->visibility |= stages[s];
				/* (T2) the largest size a stage declares: WebGPU checks bindings against it */
				if (e->buffer.type != WGPUBufferBindingType_BindingNotUsed &&
				    e->buffer.type != WGPUBufferBindingType_Undefined && b->kind == WGSL_BINDING_UNIFORM_BUFFER)
					e->buffer.minBindingSize = MAX2(e->buffer.minBindingSize, b->min_binding_size);
				/* a buffer one stage writes and another reads is writable for both */
				if (e->buffer.type == WGPUBufferBindingType_ReadOnlyStorage &&
				    b->kind == WGSL_BINDING_STORAGE_BUFFER)
					e->buffer.type = WGPUBufferBindingType_Storage;
				continue;
			}
			if (n == max)
				return n;
			e = &out[n++];
			*e = (WGPUBindGroupLayoutEntry)WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
			e->binding = b->binding;
			e->visibility = stages[s];
			const struct wgvk_binding_remap *r =
				remap ? wgvk_remap_find(remap, b->vk_set, b->vk_binding) : NULL;
			switch (b->kind) {
			case WGSL_BINDING_UNIFORM_BUFFER:
			case WGSL_BINDING_STORAGE_BUFFER:
			case WGSL_BINDING_READONLY_STORAGE_BUFFER:
				e->buffer.type = b->kind == WGSL_BINDING_UNIFORM_BUFFER ? WGPUBufferBindingType_Uniform :
						 b->kind == WGSL_BINDING_STORAGE_BUFFER ? WGPUBufferBindingType_Storage
											: WGPUBufferBindingType_ReadOnlyStorage;
				e->buffer.hasDynamicOffset = r && (r->flags & WGVK_REMAP_DYNAMIC);
				/* (T2) uniform buffers: the size the shader declares (Vulkan may bind less and read less) */
				if (b->kind == WGSL_BINDING_UNIFORM_BUFFER)
					e->buffer.minBindingSize = b->min_binding_size;
				break;
			case WGSL_BINDING_TEXTURE:
				e->texture.sampleType = b->sample_type == WGSL_SAMPLE_SINT ? WGPUTextureSampleType_Sint :
							b->sample_type == WGSL_SAMPLE_UINT ? WGPUTextureSampleType_Uint :
							b->sample_type == WGSL_SAMPLE_DEPTH ? WGPUTextureSampleType_Depth :
							b->multisampled ? WGPUTextureSampleType_UnfilterableFloat
									: WGPUTextureSampleType_Float;
				e->texture.viewDimension = wgsl_view_dimension(b->view_dimension, b->arrayed);
				e->texture.multisampled = b->multisampled;
				break;
			case WGSL_BINDING_STORAGE_TEXTURE: {
				VkFormat vf = vk_format_from_pipe_format(b->format);
				const struct wgvk_format *f = wgvk_format_get(dev->pdev, vf);
				e->storageTexture.format = f ? f->wf : WGPUTextureFormat_R32Uint;
				e->storageTexture.access = (b->access & ACCESS_NON_WRITEABLE) ? WGPUStorageTextureAccess_ReadOnly :
							   (b->access & ACCESS_NON_READABLE) ? WGPUStorageTextureAccess_WriteOnly
											     : WGPUStorageTextureAccess_ReadWrite;
				e->storageTexture.viewDimension = b->view_dimension == GLSL_SAMPLER_DIM_3D
									  ? WGPUTextureViewDimension_3D
								  : b->arrayed || b->view_dimension == GLSL_SAMPLER_DIM_CUBE
									  ? WGPUTextureViewDimension_2DArray
									  : WGPUTextureViewDimension_2D;
				break;
			}
			case WGSL_BINDING_SAMPLER:
				e->sampler.type = WGPUSamplerBindingType_Filtering;
				break;
			case WGSL_BINDING_COMPARISON_SAMPLER:
				e->sampler.type = WGPUSamplerBindingType_Comparison;
				break;
			}
		}
	}
	qsort(out, n, sizeof(*out), cmp_entry);
	return n;
}

/* ---- dummies for unwritten descriptors */

WGPUBuffer
wgvk_dummy_buffer(struct wgvk_device *dev)
{
	struct wgvk_caches *c = dev->caches;
	if (!c->dummy_buffer) {
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.size = 65536;
		bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
		c->dummy_buffer = wgpuDeviceCreateBuffer(dev->wdev, &bd);
	}
	return c->dummy_buffer;
}

static WGPUSampler
dummy_sampler(struct wgvk_device *dev, bool comparison)
{
	struct wgvk_caches *c = dev->caches;
	if (!c->dummy_sampler[comparison]) {
		WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
		if (comparison)
			sd.compare = WGPUCompareFunction_Never;
		c->dummy_sampler[comparison] = wgpuDeviceCreateSampler(dev->wdev, &sd);
	}
	return c->dummy_sampler[comparison];
}

static WGPUTextureView
dummy_view(struct wgvk_device *dev, const WGPUBindGroupLayoutEntry *e)
{
	struct wgvk_caches *c = dev->caches;
	bool storage = e->storageTexture.access != WGPUStorageTextureAccess_BindingNotUsed &&
		       e->storageTexture.access != WGPUStorageTextureAccess_Undefined;
	WGPUTextureViewDimension dim = storage ? e->storageTexture.viewDimension : e->texture.viewDimension;
	WGPUTextureFormat fmt = storage ? e->storageTexture.format :
				e->texture.sampleType == WGPUTextureSampleType_Sint ? WGPUTextureFormat_R32Sint :
				e->texture.sampleType == WGPUTextureSampleType_Uint ? WGPUTextureFormat_R32Uint :
				e->texture.sampleType == WGPUTextureSampleType_Depth ? WGPUTextureFormat_Depth32Float :
				WGPUTextureFormat_RGBA8Unorm;
	bool ms = !storage && e->texture.multisampled;
	uint32_t key = dim | fmt << 4 | ms << 15 | storage << 16;
	util_dynarray_foreach(&c->dummy_textures, struct dummy_texture, d)
		if (d->key == key)
			return d->view;
	WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
	td.usage = storage ? WGPUTextureUsage_StorageBinding : WGPUTextureUsage_TextureBinding;
	if (ms)
		td.usage |= WGPUTextureUsage_RenderAttachment;
	td.dimension = dim == WGPUTextureViewDimension_3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
	bool cube = dim == WGPUTextureViewDimension_Cube || dim == WGPUTextureViewDimension_CubeArray;
	td.size = (WGPUExtent3D){ 1, 1, cube ? 6 : 1 };
	td.format = fmt;
	td.sampleCount = ms ? 4 : 1;
	struct dummy_texture d = { .key = key, .tex = wgpuDeviceCreateTexture(dev->wdev, &td) };
	WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	vd.dimension = dim;
	d.view = wgpuTextureCreateView(d.tex, &vd);
	util_dynarray_append_typed(&c->dummy_textures, struct dummy_texture, d);
	return d.view;
}

/* ---- bind groups */

static void
upload_inline(struct wgvk_device *dev, struct wgvk_descriptor_set *set)
{
	uint32_t size = set->layout->inline_size;
	if (!set->inline_buffer) {
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.size = size;
		bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		set->inline_buffer = wgpuDeviceCreateBuffer(dev->wdev, &bd);
	}
	wgpuQueueWriteBuffer(dev->wqueue, set->inline_buffer, 0, set->inline_data, size);
	set->inline_dirty = false;
}

/* One entry of the bind group from a descriptor; key[0..2] = what identifies it. */
static void
fill_entry(struct wgvk_device *dev, struct wgvk_descriptor_set *set, const WGPUBindGroupLayoutEntry *le,
	   WGPUBindGroupEntry *be, uint64_t key[3])
{
	const WGPULimits *lim = &dev->pdev->limits;
	const struct wgvk_descriptor *d = set && le->binding < set->count ? &set->desc[le->binding] : NULL;
	*be = (WGPUBindGroupEntry)WGPU_BIND_GROUP_ENTRY_INIT;
	be->binding = le->binding;
	key[0] = key[1] = key[2] = 0;
	if (le->buffer.type != WGPUBufferBindingType_BindingNotUsed && le->buffer.type != WGPUBufferBindingType_Undefined) {
		bool uniform = le->buffer.type == WGPUBufferBindingType_Uniform;
		uint64_t max = uniform ? lim->maxUniformBufferBindingSize : lim->maxStorageBufferBindingSize;
		if (d && d->kind == WGVK_DESC_BUFFER && d->buf.buffer->mem && d->buf.buffer->mem->bo.buffer) {
			struct wgvk_buffer *b = d->buf.buffer;
			be->buffer = b->mem->bo.buffer;
			be->offset = b->mem->bo.offset + b->offset + d->buf.offset;
			uint64_t room = b->mem->bo.size - (be->offset - b->mem->bo.offset);
			/* storage bindings are whole words; the memory is (its size is 4-aligned) */
			be->size = MIN3(align64(d->buf.range, uniform ? 16 : 4), max, room + (uniform ? 256 : 0));
			/* (T2) a uniform block declared larger than what is bound: bind the declared size, the bytes
			 * past the range are not read */
			if (uniform && be->size < le->buffer.minBindingSize) {
				if (le->buffer.minBindingSize <= MIN2(max, room + 256))
					be->size = le->buffer.minBindingSize;
				else
					wgvk_warn_once("a uniform buffer bound with %llu bytes where the shader declares %llu, "
						       "at the end of its memory: WebGPU refuses it (T2)\n",
						       (unsigned long long)be->size,
						       (unsigned long long)le->buffer.minBindingSize);
			}
			key[0] = d->id, key[1] = d->buf.offset, key[2] = be->size;
		} else if (d && d->kind == WGVK_DESC_TEXEL && d->texel.view->vk.buffer) {
			struct wgvk_buffer *b = container_of(d->texel.view->vk.buffer, struct wgvk_buffer, vk);
			be->buffer = b->mem ? b->mem->bo.buffer : NULL;
			be->offset = b->mem ? b->mem->bo.offset + b->offset + d->texel.view->vk.offset : 0;
			be->size = MIN2(align64(d->texel.view->vk.range, 4), max);
			key[0] = d->id;
		} else if (d && d->kind == WGVK_DESC_IMAGE && d->img.view && !uniform) {
			/* (T2) a storage image the shader uses atomically: the view's image buffer (T3b) */
			be->buffer = wgvk_image_view_ibuf(dev, d->img.view);
			be->offset = 0;
			be->size = d->img.view->ibuf_size;
			key[0] = d->id, key[1] = 3;
		} else if (d && d->kind == WGVK_DESC_INLINE && set->inline_data) {
			if (set->inline_dirty)
				upload_inline(dev, set);
			be->buffer = set->inline_buffer;
			be->offset = d->inl.offset;
			be->size = d->inl.size;
			key[0] = set->id, key[1] = d->inl.offset, key[2] = set->inline_generation;
		}
		if (!be->buffer) {
			be->buffer = wgvk_dummy_buffer(dev);
			be->offset = 0;
			be->size = 65536;
			key[0] = 0, key[1] = 1;
		}
	} else if (le->sampler.type != WGPUSamplerBindingType_BindingNotUsed &&
		   le->sampler.type != WGPUSamplerBindingType_Undefined) {
		bool cmp = le->sampler.type == WGPUSamplerBindingType_Comparison;
		if (d && d->kind == WGVK_DESC_SAMPLER) {
			bool nonfilter = le->sampler.type == WGPUSamplerBindingType_NonFiltering;
			be->sampler = nonfilter ? wgvk_sampler_nonfiltering(dev, d->smp.sampler) : d->smp.sampler->sampler;
			key[0] = d->id, key[1] = nonfilter;
		} else {
			be->sampler = dummy_sampler(dev, cmp);
			key[0] = 0, key[1] = 2 + cmp;
		}
	} else {
		bool storage = le->storageTexture.access != WGPUStorageTextureAccess_BindingNotUsed &&
			       le->storageTexture.access != WGPUStorageTextureAccess_Undefined;
		if (d && d->kind == WGVK_DESC_IMAGE)
			be->textureView = wgvk_image_view_for_dim(d->img.view, storage,
								  storage ? le->storageTexture.viewDimension
									  : le->texture.viewDimension);
		if (be->textureView && storage && d->img.view->wformat != le->storageTexture.format) {
			/* GL lets an image unit's format differ from the shader's format qualifier (undefined
			 * results; Blender binds 1x1 R32F placeholders to rgba32f images); WebGPU refuses the bind
			 * group: the binding gets a placeholder of the shader's format */
			wgvk_warn_once("storage image of format %d bound where the shader declares %d: a placeholder "
				       "stands in\n", d->img.view->wformat, le->storageTexture.format);
			be->textureView = NULL;
		}
		if (be->textureView) {
			key[0] = d->id, key[1] = storage;
		} else {
			be->textureView = dummy_view(dev, le);
			key[0] = 0, key[1] = 4 + storage, key[2] = le->binding;
		}
	}
}

/* (T2) The entries wgvk_bind_group_get would make, without making the group: the replay rebuilds a group
 * with some buffers replaced by snapshot copies (usage scopes). out has bgl->n entries. */
void
wgvk_bind_group_entries(struct wgvk_device *dev, struct wgvk_descriptor_set *set, const struct wgvk_bgl *bgl,
			const WGPUBindGroupEntry *driver, unsigned n_driver, WGPUBindGroupEntry *out)
{
	for (unsigned i = 0; i < bgl->n; i++) {
		const WGPUBindGroupLayoutEntry *le = &bgl->entries[i];
		uint64_t key[3];
		if (le->binding >= WGVK_DRIVER_BINDING_BASE) {
			out[i] = (WGPUBindGroupEntry)WGPU_BIND_GROUP_ENTRY_INIT;
			for (unsigned j = 0; j < n_driver; j++)
				if (driver[j].binding == le->binding)
					out[i] = driver[j];
			continue;
		}
		fill_entry(dev, set, le, &out[i], key);
	}
}

WGPUBindGroup
wgvk_bind_group_get(struct wgvk_device *dev, struct wgvk_descriptor_set *set, const struct wgvk_bgl *bgl,
		    const WGPUBindGroupEntry *driver, unsigned n_driver)
{
	struct wgvk_caches *c = dev->caches;
	uint64_t driver_hash = 0;
	if (n_driver)
		driver_hash = XXH64(driver, n_driver * sizeof(*driver), 0);
	if (set && set->last_bg && set->last_bgl == bgl->id && set->last_generation == set->generation &&
	    set->last_driver == driver_hash && !set->inline_dirty) {
		c->hits++;
		return set->last_bg;
	}
	unsigned n = bgl->n;
	WGPUBindGroupEntry *be = malloc(MAX2(n, 1) * sizeof(*be));
	uint64_t *key = malloc((3 * n + 2) * sizeof(*key));
	key[0] = bgl->id;
	key[1] = driver_hash;
	unsigned k = 0;
	for (unsigned i = 0; i < n; i++) {
		const WGPUBindGroupLayoutEntry *le = &bgl->entries[i];
		if (le->binding >= WGVK_DRIVER_BINDING_BASE) {
			for (unsigned j = 0; j < n_driver; j++)
				if (driver[j].binding == le->binding)
					be[k] = driver[j];
			memset(&key[2 + 3 * k], 0, 3 * sizeof(*key));
			k++;
			continue;
		}
		fill_entry(dev, set, le, &be[k], &key[2 + 3 * k]);
		k++;
	}
	uint64_t h = XXH64(key, (3 * n + 2) * sizeof(*key), 0);
	struct cached_bg *e = _mesa_hash_table_u64_search(c->bgs, h);
	if (e && e->n == n && !memcmp(e->key, key, (3 * n + 2) * sizeof(*key))) {
		list_del(&e->lru);
		list_add(&e->lru, &c->lru);
		free(key);
		c->hits++;
	} else {
		if (e) {	/* a collision: the newer wins */
			_mesa_hash_table_u64_remove(c->bgs, h);
			list_del(&e->lru);
			wgpuBindGroupRelease(e->bg);
			free(e->key);
			free(e);
			c->n_bgs--;
		}
		WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		d.layout = bgl->layout;
		d.entryCount = k;
		d.entries = be;
		e = calloc(1, sizeof(*e));
		e->hash = h;
		e->n = n;
		e->key = key;
		e->bg = wgpuDeviceCreateBindGroup(dev->wdev, &d);
		_mesa_hash_table_u64_insert(c->bgs, h, e);
		list_add(&e->lru, &c->lru);
		c->misses++;
		if (++c->n_bgs > WGVK_BIND_GROUP_CACHE) {
			struct cached_bg *old = list_last_entry(&c->lru, struct cached_bg, lru);
			_mesa_hash_table_u64_remove(c->bgs, old->hash);
			list_del(&old->lru);
			wgpuBindGroupRelease(old->bg);
			free(old->key);
			free(old);
			c->n_bgs--;
		}
	}
	free(be);
	if (set) {
		/* the set holds its own reference: the cache may drop the entry */
		if (set->last_bg != e->bg) {
			if (set->last_bg)
				wgpuBindGroupRelease(set->last_bg);
			wgpuBindGroupAddRef(e->bg);
			set->last_bg = e->bg;
		}
		set->last_bgl = bgl->id;
		set->last_generation = set->generation;
		set->last_driver = driver_hash;
	}
	return e->bg;
}
