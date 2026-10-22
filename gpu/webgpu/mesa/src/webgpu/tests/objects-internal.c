/*
 * T1's tests of wgvk's insides that the Vulkan API does not reach yet (T2's pipelines will):
 * descriptor sets -> bind groups through wgvk_bgl_get / wgvk_bind_group_get and their caches.
 * Linked with libwgvk directly (meson target wgvk-objects-internal), wgvk on its own Dawn device.
 * Prints "ok objects-internal-PART: ..." / "FAIL ...".
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "wgvk_private.h"
#include "vk_common_entrypoints.h"

static int failed;
#define FAIL(...) do { printf("FAIL objects-internal: "); printf(__VA_ARGS__); printf("\n"); failed++; return; } while (0)
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) FAIL("%s: %d (line %d)", #x, r_, __LINE__); } while (0)

static void
bindgroups(VkDevice _dev, struct wgvk_device *dev)
{
	/* set 0: UBO 0, combined sampler 1, storage image 2, SSBO 3 (left unwritten), inline block 4 */
	VkDescriptorSetLayoutBinding b[] = {
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 4, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 32, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
	};
	VkDescriptorSetLayout sl;
	CHECK(wgvk_CreateDescriptorSetLayout(_dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b }, NULL, &sl));
	VkDescriptorPool pool;
	CHECK(wgvk_CreateDescriptorPool(_dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4 }, NULL, &pool));
	VkDescriptorSet sets[3];
	VkDescriptorSetLayout sls[3] = { sl, sl, sl };
	CHECK(wgvk_AllocateDescriptorSets(_dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 3,
		.pSetLayouts = sls }, sets));

	/* resources */
	VkBuffer buf;
	CHECK(wgvk_CreateBuffer(_dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT }, NULL, &buf));
	VkDeviceMemory mem;
	CHECK(wgvk_AllocateMemory(_dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = 8192, .memoryTypeIndex = WGVK_MEM_DEVICE_LOCAL }, NULL, &mem));
	CHECK(wgvk_BindBufferMemory2(_dev, 1, &(VkBindBufferMemoryInfo){ .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
		.buffer = buf, .memory = mem, .memoryOffset = 0 }));
	VkImage img;
	CHECK(wgvk_CreateImage(_dev, &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 16, 16, 1 }, .mipLevels = 1,
		.arrayLayers = 1, .samples = 1, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT }, NULL, &img));
	VkImageView view;
	CHECK(wgvk_CreateImageView(_dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }, NULL, &view));
	VkSampler smp;
	CHECK(wgvk_CreateSampler(_dev, &(VkSamplerCreateInfo){ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR }, NULL, &smp));

	uint8_t block[32];
	for (unsigned i = 0; i < 32; i++)
		block[i] = i;
	for (unsigned s = 0; s < 2; s++) {	/* sets 0 and 1 the same, set 2 left empty */
		VkWriteDescriptorSet w[] = {
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[s], .dstBinding = 0, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			  .pBufferInfo = &(VkDescriptorBufferInfo){ buf, 256, 512 } },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[s], .dstBinding = 1, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			  .pImageInfo = &(VkDescriptorImageInfo){ smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[s], .dstBinding = 2, .descriptorCount = 1,
			  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			  .pImageInfo = &(VkDescriptorImageInfo){ NULL, view, VK_IMAGE_LAYOUT_GENERAL } },
			{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[s], .dstBinding = 4, .descriptorCount = 32,
			  .descriptorType = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
			  .pNext = &(VkWriteDescriptorSetInlineUniformBlock){
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK, .dataSize = 32, .pData = block } },
		};
		wgvk_UpdateDescriptorSets(_dev, 4, w, 0, NULL);
	}

	/* the layout a fragment shader using all of it would give: WebGPU bindings 0 UBO, 1 texture,
	 * 2 sampler, 3 storage texture, 4 SSBO, 5 inline UBO */
	WGPUBindGroupLayoutEntry e[6];
	for (unsigned i = 0; i < 6; i++) {
		e[i] = (WGPUBindGroupLayoutEntry)WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
		e[i].binding = i;
		e[i].visibility = WGPUShaderStage_Fragment;
	}
	e[0].buffer.type = WGPUBufferBindingType_Uniform;
	e[1].texture.sampleType = WGPUTextureSampleType_Float;
	e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
	e[2].sampler.type = WGPUSamplerBindingType_Filtering;
	e[3].storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
	e[3].storageTexture.format = WGPUTextureFormat_RGBA8Unorm;
	e[3].storageTexture.viewDimension = WGPUTextureViewDimension_2D;
	e[4].buffer.type = WGPUBufferBindingType_Storage;
	e[5].buffer.type = WGPUBufferBindingType_Uniform;
	struct wgvk_bgl *bgl = wgvk_bgl_get(dev, e, 6);
	if (!bgl || !bgl->layout || wgvk_bgl_get(dev, e, 6) != bgl)
		FAIL("bind group layout not made or not cached");

	struct wgvk_descriptor_set *s0 = wgvk_descriptor_set_from_handle(sets[0]);
	struct wgvk_descriptor_set *s1 = wgvk_descriptor_set_from_handle(sets[1]);
	struct wgvk_descriptor_set *s2 = wgvk_descriptor_set_from_handle(sets[2]);
	WGPUBindGroup g0 = wgvk_bind_group_get(dev, s0, bgl, NULL, 0);
	WGPUBindGroup g0b = wgvk_bind_group_get(dev, s0, bgl, NULL, 0);
	WGPUBindGroup g1 = wgvk_bind_group_get(dev, s1, bgl, NULL, 0);
	WGPUBindGroup g2 = wgvk_bind_group_get(dev, s2, bgl, NULL, 0);	/* all dummies */
	if (!g0 || g0 != g0b)
		FAIL("set 0: %p then %p", (void *)g0, (void *)g0b);
	/* sets 0 and 1 differ only in their inline block's buffer, which is the set's own: different */
	if (!g1 || g1 == g0 || !g2)
		FAIL("set 1 / 2: %p %p (set 0 %p)", (void *)g1, (void *)g2, (void *)g0);
	/* a write moves the set on */
	wgvk_UpdateDescriptorSets(_dev, 1, &(VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = sets[0], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &(VkDescriptorBufferInfo){ buf, 0, VK_WHOLE_SIZE } }, 0, NULL);
	WGPUBindGroup g0c = wgvk_bind_group_get(dev, s0, bgl, NULL, 0);
	if (!g0c || g0c == g0)
		FAIL("after a write: %p (before %p)", (void *)g0c, (void *)g0);
	/* written back as before: the device cache finds the first one again */
	wgvk_UpdateDescriptorSets(_dev, 1, &(VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = sets[0], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.pBufferInfo = &(VkDescriptorBufferInfo){ buf, 256, 512 } }, 0, NULL);
	WGPUBindGroup g0d = wgvk_bind_group_get(dev, s0, bgl, NULL, 0);
	if (g0d != g0)
		FAIL("same content again: %p, first %p", (void *)g0d, (void *)g0);
	/* copy set 0 into set 2 (all but the inline block): equal descriptors */
	wgvk_UpdateDescriptorSets(_dev, 0, NULL, 1, &(VkCopyDescriptorSet){ .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
		.srcSet = sets[0], .srcBinding = 0, .dstSet = sets[2], .dstBinding = 0, .descriptorCount = 3 });
	if (s2->desc[0].id != s0->desc[0].id || s2->desc[1].id != s0->desc[1].id || s2->desc[2].id != s0->desc[2].id ||
	    s2->desc[3].id != s0->desc[3].id)
		FAIL("copy: ids differ");
	/* the non-filtering twin */
	e[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
	e[2].sampler.type = WGPUSamplerBindingType_NonFiltering;
	struct wgvk_bgl *bgl2 = wgvk_bgl_get(dev, e, 6);
	WGPUBindGroup g3 = wgvk_bind_group_get(dev, s0, bgl2, NULL, 0);
	if (!g3 || bgl2 == bgl)
		FAIL("non-filtering layout");
	/* a driver entry in the driver range */
	WGPUBindGroupLayoutEntry de = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	de.binding = WGVK_DRIVER_BINDING_PUSH_CONSTANTS;
	de.visibility = WGPUShaderStage_Fragment;
	de.buffer.type = WGPUBufferBindingType_Uniform;
	struct wgvk_bgl *bgl3 = wgvk_bgl_get(dev, &de, 1);
	WGPUBindGroupEntry dbe = WGPU_BIND_GROUP_ENTRY_INIT;
	dbe.binding = WGVK_DRIVER_BINDING_PUSH_CONSTANTS;
	dbe.buffer = wgvk_buffer_from_handle(buf)->mem->bo.buffer;
	dbe.size = 256;
	WGPUBindGroup g4 = wgvk_bind_group_get(dev, NULL, bgl3, &dbe, 1);
	if (!g4)
		FAIL("driver-only group");

	wgvk_DestroySampler(_dev, smp, NULL);
	wgvk_DestroyImageView(_dev, view, NULL);
	wgvk_DestroyImage(_dev, img, NULL);
	wgvk_DestroyBuffer(_dev, buf, NULL);
	wgvk_FreeMemory(_dev, mem, NULL);
	wgvk_DestroyDescriptorPool(_dev, pool, NULL);
	vk_common_DestroyDescriptorSetLayout(_dev, sl, NULL);
	printf("ok objects-internal-bindgroups: a set's bind group (UBO, combined sampler, storage image, unwritten "
	       "SSBO, inline block) made, reused, remade after a write, found again by content; dummies, the "
	       "non-filtering twin, a driver-only group\n");
}

/* The heap table and arenas for three storage buffers in two memory objects, and one unwritten slot. */
static void
heap(VkDevice _dev, struct wgvk_device *dev)
{
	VkDeviceMemory m[2];
	VkBuffer b[3];
	for (unsigned i = 0; i < 2; i++)
		CHECK(wgvk_AllocateMemory(_dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = 1 << 20, .memoryTypeIndex = WGVK_MEM_DEVICE_LOCAL }, NULL, &m[i]));
	static const uint64_t at[3] = { 4096, 65536, 512 };
	for (unsigned i = 0; i < 3; i++) {
		CHECK(wgvk_CreateBuffer(_dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1024,
			.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT }, NULL, &b[i]));
		CHECK(wgvk_BindBufferMemory2(_dev, 1, &(VkBindBufferMemoryInfo){ .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
			.buffer = b[i], .memory = m[i == 2], .memoryOffset = at[i] }));
	}
	struct wgvk_descriptor d[3];
	memset(d, 0, sizeof(d));
	for (unsigned i = 0; i < 3; i++) {
		d[i].kind = WGVK_DESC_BUFFER;
		d[i].buf.buffer = wgvk_buffer_from_handle(b[i]);
		d[i].buf.offset = 16 * i;
		d[i].buf.range = 100;
	}
	const struct wgvk_descriptor *slots[4] = { &d[0], NULL, &d[1], &d[2] };
	uint32_t t[16];
	struct wgvk_heap_arena ar[WGVK_HEAP_ARENAS];
	int na = wgvk_heap_build(dev, slots, 4, t, ar);
	/* arena 0: memory 0's window over 4096 .. 65552 + 100; arena 1: memory 1's over 544 .. 644; the window
	 * starts are aligned down to minStorageBufferOffsetAlignment */
	uint32_t al = dev->pdev->limits.minStorageBufferOffsetAlignment;
	if (na != 2 || t[0] != 0 || t[1] + ar[0].offset != 4096 || t[2] != 100 || t[4] || t[5] || t[6] ||
	    t[8] != 0 || t[9] + ar[0].offset != 65552 || t[10] != 100 || t[12] != 1 || t[13] + ar[1].offset != 544 ||
	    t[14] != 100 || ar[0].offset % al || ar[1].offset % al || ar[0].offset + ar[0].size < 65652 ||
	    ar[1].offset + ar[1].size < 644)
		FAIL("heap: %d arenas (%llu+%llu, %llu+%llu), table %u %u %u / %u %u %u / %u %u %u", na,
		     (unsigned long long)ar[0].offset, (unsigned long long)ar[0].size, (unsigned long long)ar[1].offset,
		     (unsigned long long)ar[1].size, t[0], t[1], t[2], t[8], t[9], t[10], t[12], t[13], t[14]);
	for (unsigned i = 0; i < 3; i++)
		wgvk_DestroyBuffer(_dev, b[i], NULL);
	for (unsigned i = 0; i < 2; i++)
		wgvk_FreeMemory(_dev, m[i], NULL);
	printf("ok objects-internal-heap: 3 storage buffers in 2 memory objects + an unwritten slot: table and "
	       "arena windows as T3b's lowering reads them\n");
}

static void
on_map(WGPUMapAsyncStatus status, WGPUStringView message, void *u1, void *u2)
{
	*(int *)u1 = status == WGPUMapAsyncStatus_Success;
}

/* level 0 of `tex` (8x4 RGBA8-sized texels) into memory */
static bool
read_level0(struct wgvk_device *dev, WGPUCommandEncoder enc, WGPUTexture tex, uint32_t out[32])
{
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	bd.size = 256 * 4;
	bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
	WGPUBuffer rb = wgpuDeviceCreateBuffer(dev->wdev, &bd);
	WGPUTexelCopyTextureInfo t = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	t.texture = tex;
	WGPUTexelCopyBufferInfo b = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	b.buffer = rb;
	b.layout = (WGPUTexelCopyBufferLayout){ 0, 256, 4 };
	wgpuCommandEncoderCopyTextureToBuffer(enc, &t, &b, &(WGPUExtent3D){ 8, 4, 1 });
	WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
	wgpuQueueSubmit(dev->wqueue, 1, &cb);
	wgpuCommandBufferRelease(cb);
	int ok = 0;
	WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
	ci.mode = WGPUCallbackMode_WaitAnyOnly;
	ci.callback = on_map;
	ci.userdata1 = &ok;
	WGPUFuture f = wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, bd.size, ci);
	wgvk_wait(dev, &f, 1, UINT64_MAX);
	if (ok) {
		const uint8_t *p = wgpuBufferGetConstMappedRange(rb, 0, bd.size);
		for (unsigned y = 0; y < 4; y++)
			memcpy(&out[8 * y], p + 256 * y, 32);
		wgpuBufferUnmap(rb);
	}
	wgpuBufferRelease(rb);
	return ok;
}

/* A view in a format WebGPU cannot reinterpret (R32_UINT of an RGBA8 image): its own texture, made current
 * by copies both ways. */
static void
alias_views(VkDevice _dev, struct wgvk_device *dev)
{
	VkImage _img;
	CHECK(wgvk_CreateImage(_dev, &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = { 8, 4, 1 }, .mipLevels = 2, .arrayLayers = 1, .samples = 1,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT }, NULL, &_img));
	struct wgvk_image *img = wgvk_image_from_handle(_img);
	VkImageView _v;
	CHECK(wgvk_CreateImageView(_dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = _img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32_UINT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1 } }, NULL, &_v));
	struct wgvk_image_view *v = wgvk_image_view_from_handle(_v);
	if (v->alias != 0 || img->n_alias != 1 || !v->sampled || !v->storage)
		FAIL("alias view: alias %d of %u, sampled %p storage %p", v->alias, img->n_alias, (void *)v->sampled,
		     (void *)v->storage);
	uint32_t a[32], b[32], got[32];
	for (unsigned i = 0; i < 32; i++)
		a[i] = 0x01020304u * (i + 1), b[i] = ~a[i];
	WGPUTexelCopyTextureInfo t = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	t.texture = img->tex;
	WGPUTexelCopyBufferLayout lay = { 0, 32, 4 };
	wgpuQueueWriteTexture(dev->wqueue, &t, a, sizeof(a), &lay, &(WGPUExtent3D){ 8, 4, 1 });
	WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev->wdev, NULL);
	wgvk_image_make_current(dev, enc, img, v->alias);
	if (!read_level0(dev, enc, img->alias[0].tex, got) || memcmp(got, a, sizeof(a)))
		FAIL("image -> alias: got %08x %08x, want %08x %08x", got[0], got[31], a[0], a[31]);
	wgpuCommandEncoderRelease(enc);
	/* as if a storage write through the view: the alias has the latest texels */
	t.texture = img->alias[0].tex;
	wgpuQueueWriteTexture(dev->wqueue, &t, b, sizeof(b), &lay, &(WGPUExtent3D){ 8, 4, 1 });
	enc = wgpuDeviceCreateCommandEncoder(dev->wdev, NULL);
	wgvk_image_make_current(dev, enc, img, -1);
	if (!read_level0(dev, enc, img->tex, got) || memcmp(got, b, sizeof(b)) || img->current != -1)
		FAIL("alias -> image: got %08x %08x, want %08x %08x", got[0], got[31], b[0], b[31]);
	wgpuCommandEncoderRelease(enc);
	wgvk_DestroyImageView(_dev, _v, NULL);
	wgvk_DestroyImage(_dev, _img, NULL);
	printf("ok objects-internal-alias: an R32_UINT view of an RGBA8 image is a texture of its own; texels "
	       "copied image -> view texture and back, equal\n");
}

int
main(void)
{
	VkInstance inst;
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	if (wgvk_CreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app }, NULL, &inst) != VK_SUCCESS) {
		printf("FAIL objects-internal: no instance\n");
		return 1;
	}
	uint32_t n = 1;
	VkPhysicalDevice phys;
	if (vk_common_EnumeratePhysicalDevices(inst, &n, &phys) < 0 || !n) {
		printf("FAIL objects-internal: no physical device\n");
		return 1;
	}
	VkDevice _dev;
	if (wgvk_CreateDevice(phys, &(VkDeviceCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
			.pQueuePriorities = (float[]){ 1 } } }, NULL, &_dev) != VK_SUCCESS) {
		printf("FAIL objects-internal: no device\n");
		return 1;
	}
	bindgroups(_dev, wgvk_device_from_handle(_dev));
	heap(_dev, wgvk_device_from_handle(_dev));
	alias_views(_dev, wgvk_device_from_handle(_dev));
	wgvk_DestroyDevice(_dev, NULL);
	wgvk_DestroyInstance(inst, NULL);
	return failed;
}
