/*
 * wgvk: images and image views (INTERFACES.md 4.3, 4.5).
 *
 * A VkImage is a GPUTexture made at vkCreateImage; its memory is nominal,
 * except for linear images, whose texels the CPU sees in the bound
 * (host-visible) memory: there rows are 256-byte aligned so T2 copies the
 * memory's GPUBuffer to and from the texture in one copy each way.
 *
 * Textures get every usage their format allows that wgvk itself may need
 * beyond the app's: copies, sampling (blits and depth copies read through a
 * shader), render attachment (clears and blit destinations draw). 1D images
 * are 2D textures one texel high (WebGPU's 1D textures have no mips, layers
 * or attachments; the shader side does the same, T3).
 *
 * A view makes up to three WebGPU views, one per use (sampled with the
 * swizzle, storage, attachment); see struct wgvk_image_view.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"

#include "util/u_math.h"

static void
image_layout(struct wgvk_image *img)
{
	const struct wgvk_format *f = img->fmt;
	uint64_t off = 0;
	for (uint32_t l = 0; l < img->vk.mip_levels && l < ARRAY_SIZE(img->level); l++) {
		uint32_t w = u_minify(img->vk.extent.width, l), h = u_minify(img->vk.extent.height, l);
		uint32_t d = u_minify(img->vk.extent.depth, l);
		uint64_t row = (uint64_t)DIV_ROUND_UP(w, f->block_w) * f->block_bytes;
		struct wgvk_image_level *lv = &img->level[l];
		lv->offset = off;
		lv->row_pitch = align64(row, 256);
		lv->layer_pitch = lv->row_pitch * DIV_ROUND_UP(h, f->block_h);
		lv->size = lv->layer_pitch * (img->vk.image_type == VK_IMAGE_TYPE_3D ? d : img->vk.array_layers) *
			   img->vk.samples;
		off = align64(off + lv->size, 256);
	}
	img->size = MAX2(off, 256);
}

static WGPUTextureUsage
texture_usage(const struct wgvk_image *img, const struct wgvk_format *f)
{
	VkImageUsageFlags u = img->vk.usage | img->vk.stencil_usage;
	WGPUTextureUsage w = WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
	if ((f->caps & WGVK_CAP_RENDER) && !(f->flags & WGVK_FMT_SWIZZLED))
		w |= WGPUTextureUsage_RenderAttachment;
	if ((u & VK_IMAGE_USAGE_STORAGE_BIT) && img->vk.samples == 1) {
		if ((f->caps & (WGVK_CAP_STORAGE_W | WGVK_CAP_STORAGE_RW)) && !(f->flags & WGVK_FMT_SWIZZLED))
			w |= WGPUTextureUsage_StorageBinding;
		else
			wgvk_log("image of format %d asks for storage, which WebGPU cannot do with %d: left out\n",
				 img->vk.format, f->wf);
	}
	return w;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateImage(VkDevice _device, const VkImageCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
		 VkImage *pImage)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	const struct wgvk_format *f = wgvk_format_get(dev->pdev, pCreateInfo->format);
	if (!f || !f->wf)
		return vk_errorf(dev, VK_ERROR_FORMAT_NOT_SUPPORTED, "image format %d", pCreateInfo->format);
	struct wgvk_image *img = vk_image_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*img));
	if (!img)
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	img->id = wgvk_object_id(dev);
	img->fmt = f;
	img->wformat = f->wf;
	img->bpp = f->block_bytes;
	img->wdim = img->vk.image_type == VK_IMAGE_TYPE_3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
	img->wg_layers = img->vk.image_type == VK_IMAGE_TYPE_3D ? img->vk.extent.depth : img->vk.array_layers;
	img->wusage = texture_usage(img, f);
	img->current = -1;
	image_layout(img);

	WGPUTextureFormat view_formats[2];
	WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
	td.usage = img->wusage;
	td.dimension = img->wdim;
	td.size = (WGPUExtent3D){ img->vk.extent.width, img->vk.extent.height, img->wg_layers };
	td.format = f->wf;
	td.mipLevelCount = img->vk.mip_levels;
	td.sampleCount = img->vk.samples;
	if (img->vk.create_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
		/* WebGPU reinterprets only between sRGB and not; other view formats are emulated (M2) */
		WGPUTextureFormat p = wgvk_wgpu_srgb_partner(f->wf);
		if (p != WGPUTextureFormat_Undefined) {
			view_formats[td.viewFormatCount++] = p;
			td.viewFormats = view_formats;
		}
	}
	if (img->vk.samples > 1 && img->wg_layers > 1) {
		wgvk_log("multisampled image with %u layers: WebGPU has one (layer 0 only)\n", img->wg_layers);
		td.size.depthOrArrayLayers = img->wg_layers = 1;
	}
	img->tex = wgpuDeviceCreateTexture(dev->wdev, &td);
	if (!img->tex) {
		vk_image_destroy(&dev->vk, pAllocator, &img->vk);
		return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
	}
	*pImage = wgvk_image_to_handle(img);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroyImage(VkDevice _device, VkImage _image, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_image, img, _image);
	if (!img)
		return;
	wgvk_present_forget(dev, img, NULL);
	wgpuTextureDestroy(img->tex);
	wgpuTextureRelease(img->tex);
	for (unsigned i = 0; i < img->n_alias; i++) {
		wgpuTextureDestroy(img->alias[i].tex);
		wgpuTextureRelease(img->alias[i].tex);
	}
	vk_image_destroy(&dev->vk, pAllocator, &img->vk);
}

static void
image_requirements(struct wgvk_image *img, VkMemoryRequirements2 *req)
{
	bool linear = img->vk.tiling != VK_IMAGE_TILING_OPTIMAL;
	req->memoryRequirements = (VkMemoryRequirements){
		.size = img->size,
		.alignment = 256,
		/* optimal images keep nothing in memory: device-local; linear ones are CPU-visible bytes */
		.memoryTypeBits = linear ? (1u << WGVK_MEM_TYPE_COUNT) - 1 : 1u << WGVK_MEM_DEVICE_LOCAL,
	};
	VkMemoryDedicatedRequirements *d = vk_find_struct(req->pNext, MEMORY_DEDICATED_REQUIREMENTS);
	if (d) {
		d->prefersDedicatedAllocation = false;
		d->requiresDedicatedAllocation = false;
	}
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetImageMemoryRequirements2(VkDevice _device, const VkImageMemoryRequirementsInfo2 *pInfo,
				 VkMemoryRequirements2 *req)
{
	VK_FROM_HANDLE(wgvk_image, img, pInfo->image);
	image_requirements(img, req);
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetDeviceImageMemoryRequirements(VkDevice _device, const VkDeviceImageMemoryRequirements *pInfo,
				      VkMemoryRequirements2 *req)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	const struct wgvk_format *f = wgvk_format_get(dev->pdev, pInfo->pCreateInfo->format);
	if (!f || !f->wf) {
		req->memoryRequirements = (VkMemoryRequirements){ 0 };
		return;
	}
	/* the layout without making a texture */
	struct wgvk_image tmp;
	memset(&tmp, 0, sizeof(tmp));
	vk_image_init(&dev->vk, &tmp.vk, pInfo->pCreateInfo);
	tmp.fmt = f;
	image_layout(&tmp);
	image_requirements(&tmp, req);
	vk_image_finish(&tmp.vk);
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetImageSparseMemoryRequirements2(VkDevice device, const VkImageSparseMemoryRequirementsInfo2 *pInfo,
				       uint32_t *pCount, VkSparseImageMemoryRequirements2 *pReqs)
{
	*pCount = 0;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetDeviceImageSparseMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo,
					    uint32_t *pCount, VkSparseImageMemoryRequirements2 *pReqs)
{
	*pCount = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_BindImageMemory2(VkDevice _device, uint32_t count, const VkBindImageMemoryInfo *infos)
{
	for (uint32_t i = 0; i < count; i++) {
		VK_FROM_HANDLE(wgvk_image, img, infos[i].image);
		VK_FROM_HANDLE(wgvk_device_memory, mem, infos[i].memory);
		img->mem = mem;	/* (NULL for a swapchain image: T7) */
		img->mem_offset = infos[i].memoryOffset;
		wgvk_present_bind(container_of(img->vk.base.device, struct wgvk_device, vk), mem, img);
		const VkBindMemoryStatusKHR *st = vk_find_struct_const(infos[i].pNext, BIND_MEMORY_STATUS_KHR);
		if (st && st->pResult)
			*st->pResult = VK_SUCCESS;
	}
	return VK_SUCCESS;
}

static void
subresource_layout(const struct wgvk_image *img, const VkImageSubresource2KHR *pSubresource,
		   VkSubresourceLayout2KHR *pLayout)
{
	const VkImageSubresource *s = &pSubresource->imageSubresource;
	const struct wgvk_image_level *lv = &img->level[MIN2(s->mipLevel, ARRAY_SIZE(img->level) - 1)];
	pLayout->subresourceLayout = (VkSubresourceLayout){
		.offset = lv->offset + (uint64_t)s->arrayLayer * lv->layer_pitch,
		.size = img->vk.image_type == VK_IMAGE_TYPE_3D ? lv->size : lv->layer_pitch,
		.rowPitch = lv->row_pitch,
		.arrayPitch = lv->layer_pitch,
		.depthPitch = lv->layer_pitch,
	};
	VkSubresourceHostMemcpySizeEXT *m = vk_find_struct(pLayout->pNext, SUBRESOURCE_HOST_MEMCPY_SIZE_EXT);
	if (m)
		m->size = pLayout->subresourceLayout.size;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetImageSubresourceLayout2KHR(VkDevice _device, VkImage _image, const VkImageSubresource2KHR *pSubresource,
				   VkSubresourceLayout2KHR *pLayout)
{
	VK_FROM_HANDLE(wgvk_image, img, _image);
	subresource_layout(img, pSubresource, pLayout);
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetDeviceImageSubresourceLayoutKHR(VkDevice _device, const VkDeviceImageSubresourceInfoKHR *pInfo,
					VkSubresourceLayout2KHR *pLayout)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	const struct wgvk_format *f = wgvk_format_get(dev->pdev, pInfo->pCreateInfo->format);
	struct wgvk_image tmp;
	memset(&tmp, 0, sizeof(tmp));
	if (!f || !f->wf) {
		memset(&pLayout->subresourceLayout, 0, sizeof(pLayout->subresourceLayout));
		return;
	}
	vk_image_init(&dev->vk, &tmp.vk, pInfo->pCreateInfo);
	tmp.fmt = f;
	image_layout(&tmp);
	subresource_layout(&tmp, pInfo->pSubresource, pLayout);
	vk_image_finish(&tmp.vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_GetImageDrmFormatModifierPropertiesEXT(VkDevice device, VkImage image,
					    VkImageDrmFormatModifierPropertiesEXT *pProperties)
{
	pProperties->drmFormatModifier = 0;	/* linear */
	return VK_SUCCESS;
}

/* For the page's smoke test and T7: the GPUTexture behind a VkImage. */
WGPUTexture wgvk_image_texture(VkImage image);
WGPUTexture
wgvk_image_texture(VkImage image)
{
	VK_FROM_HANDLE(wgvk_image, img, image);
	return img->tex;
}

/* ---- image views */

static WGPUTextureViewDimension
view_dimension(VkImageViewType t)
{
	switch (t) {
	case VK_IMAGE_VIEW_TYPE_1D:
	case VK_IMAGE_VIEW_TYPE_2D: return WGPUTextureViewDimension_2D;
	case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
	case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return WGPUTextureViewDimension_2DArray;
	case VK_IMAGE_VIEW_TYPE_CUBE: return WGPUTextureViewDimension_Cube;
	case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return WGPUTextureViewDimension_CubeArray;
	case VK_IMAGE_VIEW_TYPE_3D: return WGPUTextureViewDimension_3D;
	default: return WGPUTextureViewDimension_Undefined;
	}
}

static WGPUComponentSwizzle
swz_of(VkComponentSwizzle s, WGPUComponentSwizzle identity)
{
	switch (s) {
	case VK_COMPONENT_SWIZZLE_ZERO: return WGPUComponentSwizzle_Zero;
	case VK_COMPONENT_SWIZZLE_ONE: return WGPUComponentSwizzle_One;
	case VK_COMPONENT_SWIZZLE_R: return WGPUComponentSwizzle_R;
	case VK_COMPONENT_SWIZZLE_G: return WGPUComponentSwizzle_G;
	case VK_COMPONENT_SWIZZLE_B: return WGPUComponentSwizzle_B;
	case VK_COMPONENT_SWIZZLE_A: return WGPUComponentSwizzle_A;
	default: return identity;
	}
}

/* The view's mapping applied on top of the format's own (WGVK_FMT_SWIZZLED) */
static void
view_swizzle(const struct wgvk_image_view *v, WGPUComponentSwizzle out[4])
{
	static const WGPUComponentSwizzle id[4] = { WGPUComponentSwizzle_R, WGPUComponentSwizzle_G,
						    WGPUComponentSwizzle_B, WGPUComponentSwizzle_A };
	const VkComponentSwizzle vs[4] = { v->vk.swizzle.r, v->vk.swizzle.g, v->vk.swizzle.b, v->vk.swizzle.a };
	for (unsigned i = 0; i < 4; i++) {
		WGPUComponentSwizzle s = swz_of(vs[i], id[i]);
		if (s >= WGPUComponentSwizzle_R && (v->fmt->flags & WGVK_FMT_SWIZZLED))
			s = v->fmt->swizzle[s - WGPUComponentSwizzle_R];
		out[i] = s;
	}
}

/* ---- textures in other formats (views WebGPU cannot reinterpret) */

static int
image_alias(struct wgvk_device *dev, struct wgvk_image *img, const struct wgvk_format *f)
{
	for (unsigned i = 0; i < img->n_alias; i++)
		if (img->alias[i].fmt->wf == f->wf || wgvk_wgpu_srgb_partner(img->alias[i].fmt->wf) == f->wf)
			return i;
	/* texel-for-texel copies through a buffer: the same block size and dimensions, colour, one sample */
	if (img->n_alias == ARRAY_SIZE(img->alias) || f->block_bytes != img->fmt->block_bytes ||
	    f->block_w != img->fmt->block_w || f->block_h != img->fmt->block_h || img->vk.samples > 1 ||
	    (f->flags | img->fmt->flags) & (WGVK_FMT_DEPTH | WGVK_FMT_STENCIL))
		return -1;
	struct wgvk_image_alias *a = &img->alias[img->n_alias];
	a->fmt = f;
	a->usage = texture_usage(img, f);
	WGPUTextureFormat vf = wgvk_wgpu_srgb_partner(f->wf);
	WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
	td.usage = a->usage;
	td.dimension = img->wdim;
	td.size = (WGPUExtent3D){ img->vk.extent.width, img->vk.extent.height, img->wg_layers };
	td.format = f->wf;
	td.mipLevelCount = img->vk.mip_levels;
	td.sampleCount = 1;
	if (vf != WGPUTextureFormat_Undefined) {
		td.viewFormatCount = 1;
		td.viewFormats = &vf;
	}
	a->tex = wgpuDeviceCreateTexture(dev->wdev, &td);
	if (!a->tex)
		return -1;
	return img->n_alias++;
}

static WGPUTexture
texture_of(const struct wgvk_image *img, int alias)
{
	return alias < 0 ? img->tex : img->alias[alias].tex;
}

void
wgvk_image_make_current(struct wgvk_device *dev, WGPUCommandEncoder enc, struct wgvk_image *img, int alias)
{
	if (img->current == alias)
		return;
	WGPUTexture from = texture_of(img, img->current), to = texture_of(img, alias);
	const struct wgvk_format *f = img->fmt;
	for (uint32_t l = 0; l < img->vk.mip_levels; l++) {
		uint32_t w = u_minify(img->vk.extent.width, l), h = u_minify(img->vk.extent.height, l);
		uint32_t d = img->wdim == WGPUTextureDimension_3D ? u_minify(img->vk.extent.depth, l) : img->wg_layers;
		/* compressed levels copy whole blocks */
		uint32_t bw = DIV_ROUND_UP(w, f->block_w), bh = DIV_ROUND_UP(h, f->block_h);
		uint32_t bpr = align(bw * f->block_bytes, 256);
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.size = (uint64_t)bpr * bh * d;
		bd.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
		WGPUBuffer tmp = wgpuDeviceCreateBuffer(dev->wdev, &bd);
		WGPUTexelCopyBufferInfo b = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
		b.buffer = tmp;
		b.layout = (WGPUTexelCopyBufferLayout){ 0, bpr, bh };
		WGPUExtent3D size = { bw * f->block_w, bh * f->block_h, d };
		WGPUTexelCopyTextureInfo t = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		t.mipLevel = l;
		t.texture = from;
		wgpuCommandEncoderCopyTextureToBuffer(enc, &t, &b, &size);
		t.texture = to;
		wgpuCommandEncoderCopyBufferToTexture(enc, &b, &t, &size);
		wgpuBufferRelease(tmp);	/* the encoder keeps it until the work is done */
		dev->stats.alias_bytes += bd.size;
	}
	dev->stats.alias_copies++;
	static int trace = -1;
	if (trace < 0)
		trace = getenv("WGVK_TRACE_ALIAS") != NULL;
	if (trace)
		wgvk_log("alias: image %ux%ux%u fmt %d (%u levels): texture %d -> %d (%s)\n", img->vk.extent.width,
			 img->vk.extent.height, img->wg_layers, img->vk.format, img->vk.mip_levels, img->current, alias,
			 alias >= 0 ? "view format" : "own");
	img->current = alias;
}

static WGPUTextureView
make_view(struct wgvk_image *img, WGPUTextureFormat format, WGPUTextureViewDimension dim, uint32_t level,
	  uint32_t levels, uint32_t layer, uint32_t layers, WGPUTextureAspect aspect, WGPUTextureUsage usage,
	  const WGPUComponentSwizzle *swz, WGPUTexture tex)
{
	WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	vd.format = format;
	vd.dimension = dim;
	vd.baseMipLevel = level;
	vd.mipLevelCount = levels;
	vd.baseArrayLayer = layer;
	vd.arrayLayerCount = layers;
	vd.aspect = aspect;
	vd.usage = usage;
	WGPUTextureComponentSwizzleDescriptor sd = WGPU_TEXTURE_COMPONENT_SWIZZLE_DESCRIPTOR_INIT;
	if (swz) {
		sd.swizzle = (WGPUTextureComponentSwizzle){ swz[0], swz[1], swz[2], swz[3] };
		vd.nextInChain = &sd.chain;
	}
	return wgpuTextureCreateView(tex, &vd);
}

/* The view a binding of dimension want gets: the sampled or storage view, or its 2D <-> 2D-array twin (a
 * one-layer 2D view bound to an arrayed image or sampler, or a one-layer array view to a 2D one: GL and
 * Vulkan allow it, WebGPU needs the dimension the layout says). Other mismatches return the view as is. */
WGPUTextureView
wgvk_image_view_for_dim(struct wgvk_image_view *v, bool storage, WGPUTextureViewDimension want)
{
	WGPUTextureView base = storage ? v->storage : v->sampled;
	WGPUTextureViewDimension cur = storage ? v->sdim : v->wdim;
	if (!base || want == cur || want == WGPUTextureViewDimension_Undefined)
		return base;
	bool to_array = cur == WGPUTextureViewDimension_2D && want == WGPUTextureViewDimension_2DArray;
	bool to_2d = cur == WGPUTextureViewDimension_2DArray && want == WGPUTextureViewDimension_2D &&
		     v->vk.layer_count == 1;
	if (!to_array && !to_2d)
		return base;
	if (v->twin[storage])
		return v->twin[storage];
	struct wgvk_image *img = container_of(v->vk.image, struct wgvk_image, vk);
	WGPUTexture tex = v->alias >= 0 ? img->alias[v->alias].tex : img->tex;
	WGPUTextureFormat vf = v->aspect == WGPUTextureAspect_All ? v->wformat : WGPUTextureFormat_Undefined;
	WGPUComponentSwizzle swz[4];
	bool identity = true;
	struct wgvk_device *dev = container_of(v->vk.base.device, struct wgvk_device, vk);
	if (!storage && dev->pdev->wf.swizzle) {
		view_swizzle(v, swz);
		identity = swz[0] == WGPUComponentSwizzle_R && swz[1] == WGPUComponentSwizzle_G &&
			   swz[2] == WGPUComponentSwizzle_B && swz[3] == WGPUComponentSwizzle_A;
	}
	v->twin[storage] = make_view(img, vf, want, v->vk.base_mip_level, storage ? 1 : v->vk.level_count,
				     v->vk.base_array_layer, 1, v->aspect,
				     storage ? WGPUTextureUsage_StorageBinding : WGPUTextureUsage_TextureBinding,
				     identity ? NULL : swz, tex);
	return v->twin[storage];
}

/* (T2) The size of a view's image buffer: its level's width and height, and its layers (3D: depth). */
void
wgvk_image_view_ibuf_extent(const struct wgvk_image_view *v, uint32_t *w, uint32_t *h, uint32_t *layers)
{
	const struct vk_image *img = v->vk.image;
	uint32_t l = v->vk.base_mip_level;
	*w = MAX2(img->extent.width >> l, 1);
	*h = MAX2(img->extent.height >> l, 1);
	*layers = img->image_type == VK_IMAGE_TYPE_3D ? MAX2(img->extent.depth >> l, 1) : v->vk.layer_count;
}

/* (T2) The view's image buffer (T3b's IMAGE_BUFFER lowering), made with its header on first use. Its texels
 * are not current: wgvk_image_before copies them in. */
WGPUBuffer
wgvk_image_view_ibuf(struct wgvk_device *dev, struct wgvk_image_view *v)
{
	if (v->ibuf)
		return v->ibuf;
	uint32_t w, h, n;
	wgvk_image_view_ibuf_extent(v, &w, &h, &n);
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	bd.size = 16 + (uint64_t)w * h * n * 4;
	if (bd.size > dev->pdev->limits.maxStorageBufferBindingSize)
		wgvk_warn_once("an image used atomically needs a %llu-byte storage binding, over the device's %llu "
			       "(EEVEE: a smaller shadow pool)\n", (unsigned long long)bd.size,
			       (unsigned long long)dev->pdev->limits.maxStorageBufferBindingSize);
	bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
	bd.mappedAtCreation = true;
	v->ibuf = wgpuDeviceCreateBuffer(dev->wdev, &bd);
	if (!v->ibuf)
		return NULL;
	uint32_t *hdr = wgpuBufferGetMappedRange(v->ibuf, 0, 16);
	if (hdr) {
		hdr[0] = w, hdr[1] = h, hdr[2] = n, hdr[3] = 0;
	}
	wgpuBufferUnmap(v->ibuf);
	v->ibuf_size = bd.size;
	v->ibuf_gen = UINT64_MAX;
	return v->ibuf;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateImageView(VkDevice _device, const VkImageViewCreateInfo *pCreateInfo,
		     const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_image, img, pCreateInfo->image);
	struct wgvk_image_view *v = vk_image_view_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*v));
	if (!v)
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	v->id = wgvk_object_id(dev);
	v->depth_slice = WGPU_DEPTH_SLICE_UNDEFINED;
	v->fmt = wgvk_format_get(dev->pdev, v->vk.format);
	if (!v->fmt || !v->fmt->wf)
		v->fmt = img->fmt;
	v->wformat = v->fmt->wf;
	v->alias = -1;
	WGPUTexture tex = img->tex;
	WGPUTextureUsage tusage = img->wusage;
	if (v->wformat != img->wformat && v->wformat != wgvk_wgpu_srgb_partner(img->wformat)) {
		/* another format of the same texel size: a texture of its own, kept coherent by copies */
		v->alias = image_alias(dev, img, v->fmt);
		if (v->alias < 0) {
			wgvk_warn_once("view format %d of an image of format %d: WebGPU cannot reinterpret it and "
				       "wgvk cannot copy it (depth, multisampled or another block size)\n",
				       v->vk.format, img->vk.format);
			v->emulated = true;
			v->fmt = img->fmt;
			v->wformat = img->wformat;
		} else {
			tex = img->alias[v->alias].tex;
			tusage = img->alias[v->alias].usage;
		}
	}
	/* aspects: one aspect of a depth/stencil texture is its own format in WebGPU */
	VkImageAspectFlags as = v->vk.aspects;
	v->aspect = WGPUTextureAspect_All;
	WGPUTextureFormat vf = v->wformat;
	if (wgvk_wgpu_format_is_depth_stencil(img->wformat) &&
	    (img->fmt->flags & (WGVK_FMT_DEPTH | WGVK_FMT_STENCIL)) == (WGVK_FMT_DEPTH | WGVK_FMT_STENCIL) &&
	    as != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
		v->aspect = as == VK_IMAGE_ASPECT_STENCIL_BIT ? WGPUTextureAspect_StencilOnly : WGPUTextureAspect_DepthOnly;
		vf = WGPUTextureFormat_Undefined;	/* the aspect's format */
	}
	VkImageUsageFlags usage = v->vk.usage;
	uint32_t level = v->vk.base_mip_level, levels = v->vk.level_count;
	uint32_t layer = v->vk.base_array_layer, layers = v->vk.layer_count;
	bool view_3d_of_2d = img->vk.image_type == VK_IMAGE_TYPE_3D && v->vk.view_type != VK_IMAGE_VIEW_TYPE_3D;
	v->wdim = view_dimension(v->vk.view_type);

	if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
		if (view_3d_of_2d) {
			wgvk_log("a 2D view of a 3D image cannot be sampled in WebGPU\n");
		} else {
			WGPUComponentSwizzle swz[4];
			view_swizzle(v, swz);
			bool identity = swz[0] == WGPUComponentSwizzle_R && swz[1] == WGPUComponentSwizzle_G &&
					swz[2] == WGPUComponentSwizzle_B && swz[3] == WGPUComponentSwizzle_A;
			if (!identity && !dev->pdev->wf.swizzle) {
				static bool warned;
				if (!warned) {
					warned = true;
					wgvk_log("component swizzles need texture-component-swizzle: ignored\n");
				}
				identity = true;
			}
			v->sampled = make_view(img, vf, v->wdim, level, levels, layer,
					       v->wdim == WGPUTextureViewDimension_3D ? 1 : layers, v->aspect,
					       WGPUTextureUsage_TextureBinding, identity ? NULL : swz, tex);
		}
	}
	if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && (tusage & WGPUTextureUsage_StorageBinding)) {
		WGPUTextureViewDimension sd = v->wdim == WGPUTextureViewDimension_Cube ||
							      v->wdim == WGPUTextureViewDimension_CubeArray
						      ? WGPUTextureViewDimension_2DArray
						      : v->wdim;
		if (view_3d_of_2d)
			sd = WGPUTextureViewDimension_3D, layer = 0, layers = 1;
		v->storage = make_view(img, vf, sd, level, 1, layer, sd == WGPUTextureViewDimension_3D ? 1 : layers,
				       v->aspect, WGPUTextureUsage_StorageBinding, NULL, tex);
		v->sdim = sd;
		layer = v->vk.base_array_layer, layers = v->vk.layer_count;
	}
	if ((usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
	    (tusage & WGPUTextureUsage_RenderAttachment)) {
		if (img->vk.image_type == VK_IMAGE_TYPE_3D) {
			/* a slice of a 3D texture: the 3D view of one level, and depthSlice in the pass */
			v->attachment = make_view(img, v->wformat, WGPUTextureViewDimension_3D, level, 1, 0, 1,
						  WGPUTextureAspect_All, WGPUTextureUsage_RenderAttachment, NULL, tex);
			v->depth_slice = v->vk.base_array_layer;
		} else {
			/* WebGPU attachments are one layer; layered rendering is T2's (one pass per layer) */
			v->attachment = make_view(img, v->wformat, WGPUTextureViewDimension_2D, level, 1, layer, 1,
						  WGPUTextureAspect_All, WGPUTextureUsage_RenderAttachment, NULL, tex);
		}
	}
	*pView = wgvk_image_view_to_handle(v);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroyImageView(VkDevice _device, VkImageView _view, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_image_view, v, _view);
	if (!v)
		return;
	if (v->sampled)
		wgpuTextureViewRelease(v->sampled);
	if (v->storage)
		wgpuTextureViewRelease(v->storage);
	if (v->attachment)
		wgpuTextureViewRelease(v->attachment);
	for (unsigned i = 0; i < 2; i++)
		if (v->twin[i])
			wgpuTextureViewRelease(v->twin[i]);
	if (v->depth_ro)
		wgpuTextureViewRelease(v->depth_ro);
	if (v->ibuf) {
		struct wgvk_image *img = container_of(v->vk.image, struct wgvk_image, vk);
		if (img->ib_owner == v) {
			/* its texels were never copied back: do it now, ahead of anything submitted later */
			wgvk_image_ibuf_flush_now(dev, img);
		}
		wgpuBufferRelease(v->ibuf);
	}
	vk_image_view_destroy(&dev->vk, pAllocator, &v->vk);
}
