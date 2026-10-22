/*
 * wgvk transfers (T2): copies, fills, updates, image clears, blits and
 * resolves, with WebGPU's copy commands where they are legal and render
 * passes or driver pipelines (wgvk_meta.c) where not.
 *
 * WebGPU's copy rules that matter here:
 *  - buffer <-> texture: bytesPerRow a multiple of 256 (wgvk goes through a
 *    temporary buffer with padded rows when Vulkan's pitch is not), offsets
 *    multiples of 4;
 *  - buffer -> buffer: offsets and sizes multiples of 4, and never within
 *    one GPUBuffer (two VkBuffers of one VkDeviceMemory are one GPUBuffer in
 *    wgvk: those copies go through a temporary buffer);
 *  - texture -> texture: the same format (up to sRGB); depth and stencil
 *    whole subresources only. Other formats with the same texel size go
 *    through a temporary buffer.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_replay.h"

#include "vk_format.h"

/* ---- buffers */

void
wgvk_cmd_copy_buffer(struct wgvk_replay *rp, const VkCopyBufferInfo2 *info)
{
	VK_FROM_HANDLE(wgvk_buffer, src, info->srcBuffer);
	VK_FROM_HANDLE(wgvk_buffer, dst, info->dstBuffer);
	for (uint32_t i = 0; i < info->regionCount; i++) {
		const VkBufferCopy2 *rg = &info->pRegions[i];
		uint64_t so, dof;
		WGPUBuffer sb = wgvk_buffer_wgpu(src, rg->srcOffset, &so);
		WGPUBuffer db = wgvk_buffer_wgpu(dst, rg->dstOffset, &dof);
		if ((so | dof) & 3) {
			wgvk_warn_once("buffer copy not in whole words (offsets %llu, %llu, size %llu): T2 needs a compute "
				       "copy\n", (unsigned long long)so, (unsigned long long)dof,
				       (unsigned long long)rg->size);
			continue;
		}
		wgvk_upload(rp, src->mem, src->offset + rg->srcOffset, rg->size);
		/* whole words by WebGPU's copy; the last 1-3 bytes of a size that is not a multiple of 4 by a shader
		 * that keeps the destination's other bytes of that word (VK-16) */
		uint64_t whole = rg->size & ~3ull, tail = rg->size & 3;
		if (getenv("WGVK_TRACE")) {
			const float *f = src->mem->bo.window.kind == WGVK_WINDOW_HOST ?
				(const float *)((const char *)src->mem->bo.window.host + src->offset + rg->srcOffset) : NULL;
			wgvk_log("trace:   copy %p+%llu -> %p+%llu, %llu bytes; src flags %u window %d: %g %g %g %g\n",
				 (void *)sb, (unsigned long long)so, (void *)db, (unsigned long long)dof,
				 (unsigned long long)rg->size, src->mem->bo.flags, src->mem->bo.window.kind,
				 f ? f[0] : 0, f ? f[1] : 0, f ? f[2] : 0, f ? f[3] : 0);
		}
		if (whole && sb == db) {
			WGPUBuffer t = wgvk_temp_buffer(rp, whole, WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
			wgpuCommandEncoderCopyBufferToBuffer(rp->enc, sb, so, t, 0, whole);
			wgpuCommandEncoderCopyBufferToBuffer(rp->enc, t, 0, db, dof, whole);
		} else if (whole) {
			wgpuCommandEncoderCopyBufferToBuffer(rp->enc, sb, so, db, dof, whole);
		}
		if (tail) {
			struct wgvk_device *dev = rp->dev;
			WGPUBuffer t = wgvk_temp_buffer(rp, 4, WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst |
								 WGPUBufferUsage_Storage);
			wgpuCommandEncoderCopyBufferToBuffer(rp->enc, sb, so + whole, t, 0, 4);
			WGPUBindGroupLayout bgl;
			WGPUComputePipeline p = wgvk_meta_tail_copy_pipeline(dev, &bgl);
			uint32_t salign = MAX2(dev->pdev->limits.minStorageBufferOffsetAlignment, 4);
			uint64_t at = dof + whole, base = at / salign * salign;
			WGPUBindGroupEntry e[2] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
			e[0].binding = 0;
			e[0].buffer = t;
			e[0].size = 4;
			e[1].binding = 1;
			e[1].buffer = db;
			e[1].offset = base;
			e[1].size = align64(at - base + 4, 4);
			WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
			bd.layout = bgl;
			bd.entryCount = 2;
			bd.entries = e;
			WGPUBindGroup g = wgpuDeviceCreateBindGroup(dev->wdev, &bd);
			util_dynarray_append(&rp->sub->groups, g);
			uint32_t imm[2] = { (uint32_t)((at - base) / 4), (1u << (8 * tail)) - 1 };
			WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(rp->enc, NULL);
			wgpuComputePassEncoderSetPipeline(cp, p);
			wgpuComputePassEncoderSetBindGroup(cp, 0, g, 0, NULL);
			wgpuComputePassEncoderSetImmediates(cp, 0, imm, sizeof(imm));
			wgpuComputePassEncoderDispatchWorkgroups(cp, 1, 1, 1);
			wgpuComputePassEncoderEnd(cp);
			wgpuComputePassEncoderRelease(cp);
		}
		wgvk_wrote(rp, dst->mem, dst->offset + rg->dstOffset, rg->size);
	}
}

/* A buffer of size bytes holding data, made on the CPU (mappedAtCreation). */
static WGPUBuffer
buffer_with(struct wgvk_replay *rp, const void *data, uint64_t size, uint32_t pattern)
{
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	bd.size = align64(size, 4);
	bd.usage = WGPUBufferUsage_CopySrc;
	bd.mappedAtCreation = true;
	WGPUBuffer b = wgpuDeviceCreateBuffer(rp->dev->wdev, &bd);
	uint32_t *p = wgpuBufferGetMappedRange(b, 0, bd.size);
	if (p) {
		if (data) {
			memcpy(p, data, size);
		} else {
			for (uint64_t i = 0; i < bd.size / 4; i++)
				p[i] = pattern;
		}
	}
	wgpuBufferUnmap(b);
	util_dynarray_append(&rp->sub->buffers, b);
	return b;
}

void
wgvk_cmd_fill_buffer(struct wgvk_replay *rp, struct wgvk_buffer *buf, uint64_t off, uint64_t size, uint32_t data)
{
	size = vk_buffer_range(&buf->vk, off, size) & ~3ull;
	if (!size)
		return;
	uint64_t o;
	WGPUBuffer b = wgvk_buffer_wgpu(buf, off, &o);
	if (data == 0)
		wgpuCommandEncoderClearBuffer(rp->enc, b, o, size);
	else
		wgpuCommandEncoderCopyBufferToBuffer(rp->enc, buffer_with(rp, NULL, size, data), 0, b, o, size);
	wgvk_wrote(rp, buf->mem, buf->offset + off, size);
}

void
wgvk_cmd_update_buffer(struct wgvk_replay *rp, struct wgvk_buffer *buf, uint64_t off, uint64_t size, const void *data)
{
	if (!size)
		return;
	uint64_t o;
	WGPUBuffer b = wgvk_buffer_wgpu(buf, off, &o);
	wgpuCommandEncoderCopyBufferToBuffer(rp->enc, buffer_with(rp, data, size, 0), 0, b, o, align64(size, 4));
	wgvk_wrote(rp, buf->mem, buf->offset + off, size);
}

/* ---- buffers <-> images */

struct texel_layout {
	uint32_t bs, bw, bh;		/* block size in bytes, block width and height in texels */
	WGPUTextureAspect aspect;
};

static struct texel_layout
texel_layout(const struct wgvk_image *img, VkImageAspectFlags aspect)
{
	VkFormat f = img->vk.format;
	struct texel_layout t = { .aspect = WGPUTextureAspect_All };
	if (vk_format_is_depth_or_stencil(f) && util_bitcount(aspect) == 1) {
		if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT && vk_format_has_stencil(f))
			t.aspect = WGPUTextureAspect_DepthOnly;
		if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT && vk_format_has_depth(f))
			t.aspect = WGPUTextureAspect_StencilOnly;
		f = vk_format_get_aspect_format(f, aspect);
	}
	t.bs = vk_format_get_blocksize(f);
	t.bw = vk_format_get_blockwidth(f);
	t.bh = vk_format_get_blockheight(f);
	return t;
}

/* Depth aspects WebGPU cannot copy (T1's format flags): by shaders. Returns false when this copy is not one. */
static bool
depth_copy(struct wgvk_replay *rp, struct wgvk_buffer *buf, struct wgvk_image *img, const VkBufferImageCopy2 *rg,
	   bool to_buffer)
{
	struct wgvk_device *dev = rp->dev;
	if (!(rg->imageSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) || !img->fmt)
		return false;
	uint16_t f = img->fmt->flags;
	bool need = to_buffer ? (f & WGVK_FMT_DEPTH_NO_COPY) : (f & (WGVK_FMT_DEPTH_NO_COPY | WGVK_FMT_DEPTH_NO_COPY_DST));
	if (!need)
		return false;
	VkFormat df = vk_format_get_aspect_format(img->vk.format, VK_IMAGE_ASPECT_DEPTH_BIT);
	uint32_t esz = vk_format_get_blocksize(df);	/* 2 (D16), 4 (X8_D24, D32) */
	uint32_t fmt = df == VK_FORMAT_D16_UNORM ? 0 : df == VK_FORMAT_X8_D24_UNORM_PACK32 ? 1 : 2;
	if (to_buffer && fmt != 1) {
		wgvk_warn_once("depth readback of format %d through a shader: only D24 (T2)\n", img->vk.format);
		return true;
	}
	uint32_t w = rg->imageExtent.width, h = rg->imageExtent.height;
	uint32_t layers = vk_image_subresource_layer_count(&img->vk, &rg->imageSubresource);
	uint32_t row = rg->bufferRowLength ? rg->bufferRowLength : w;
	uint32_t ih = rg->bufferImageHeight ? rg->bufferImageHeight : h;
	uint64_t total = ((uint64_t)(layers - 1) * ih * row + (uint64_t)(h - 1) * row + w) * esz;
	uint64_t boff;
	WGPUBuffer bb = wgvk_buffer_wgpu(buf, rg->bufferOffset, &boff);
	uint32_t salign = MAX2(dev->pdev->limits.minStorageBufferOffsetAlignment, 4);
	uint64_t base = boff / salign * salign;
	uint64_t size = align64(boff - base + total, 4);
	if ((boff - base) % esz) {
		wgvk_warn_once("depth copy at an offset not a multiple of its texel\n");
		return true;
	}
	uint32_t first = (boff - base) / esz;
	/* the window, so words not written keep the buffer's bytes */
	wgvk_upload(rp, buf->mem, base - buf->mem->bo.offset, size);
	if (to_buffer) {
		WGPUBindGroupLayout bgl;
		WGPUComputePipeline p = wgvk_meta_depth_readback_pipeline(dev, &bgl);
		WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
		vd.format = WGPUTextureFormat_Undefined;	/* (the depth aspect's own format) */
		vd.dimension = WGPUTextureViewDimension_2DArray;
		vd.aspect = WGPUTextureAspect_DepthOnly;
		vd.baseArrayLayer = rg->imageSubresource.baseArrayLayer;
		vd.arrayLayerCount = layers;
		WGPUTextureView v = wgpuTextureCreateView(img->tex, &vd);
		WGPUBindGroupEntry e[2] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
		e[0].binding = 0;
		e[0].textureView = v;
		e[1].binding = 1;
		e[1].buffer = bb;
		e[1].offset = base;
		e[1].size = size;
		WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bd.layout = bgl;
		bd.entryCount = 2;
		bd.entries = e;
		WGPUBindGroup g = wgpuDeviceCreateBindGroup(dev->wdev, &bd);
		util_dynarray_append(&rp->sub->groups, g);
		wgpuTextureViewRelease(v);
		uint32_t imm[8] = { rg->imageOffset.x, rg->imageOffset.y, w, h, row, ih, first, rg->imageSubresource.mipLevel };
		WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(rp->enc, NULL);
		wgpuComputePassEncoderSetPipeline(cp, p);
		wgpuComputePassEncoderSetBindGroup(cp, 0, g, 0, NULL);
		wgpuComputePassEncoderSetImmediates(cp, 0, imm, sizeof(imm));
		wgpuComputePassEncoderDispatchWorkgroups(cp, DIV_ROUND_UP(w, 8), DIV_ROUND_UP(h, 8), layers);
		wgpuComputePassEncoderEnd(cp);
		wgpuComputePassEncoderRelease(cp);
		wgvk_wrote(rp, buf->mem, base - buf->mem->bo.offset, size);
		return true;
	}
	WGPUBindGroupLayout bgl;
	WGPURenderPipeline p = wgvk_meta_depth_upload_pipeline(dev, img->wformat, &bgl);
	WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
	e.binding = 0;
	e.buffer = bb;
	e.offset = base;
	e.size = size;
	WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	bd.layout = bgl;
	bd.entryCount = 1;
	bd.entries = &e;
	WGPUBindGroup g = wgpuDeviceCreateBindGroup(dev->wdev, &bd);
	util_dynarray_append(&rp->sub->groups, g);
	bool stencil = vk_format_has_stencil(img->vk.format);
	for (uint32_t l = 0; l < layers; l++) {
		WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
		vd.format = img->wformat;
		vd.dimension = WGPUTextureViewDimension_2D;
		vd.baseMipLevel = rg->imageSubresource.mipLevel;
		vd.mipLevelCount = 1;
		vd.baseArrayLayer = rg->imageSubresource.baseArrayLayer + l;
		vd.arrayLayerCount = 1;
		WGPUTextureView v = wgpuTextureCreateView(img->tex, &vd);
		WGPURenderPassDepthStencilAttachment dsa = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
		dsa.depthClearValue = 0.0f;	/* (webgpu.h's undefined is NaN, which the browser refuses: VK-16) */
		dsa.view = v;
		dsa.depthLoadOp = WGPULoadOp_Load;
		dsa.depthStoreOp = WGPUStoreOp_Store;
		if (stencil) {
			dsa.stencilLoadOp = WGPULoadOp_Load;
			dsa.stencilStoreOp = WGPUStoreOp_Store;
		}
		WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		rpd.depthStencilAttachment = &dsa;
		WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(rp->enc, &rpd);
		wgpuRenderPassEncoderSetPipeline(pass, p);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, g, 0, NULL);
		uint32_t imm[8] = { rg->imageOffset.x, rg->imageOffset.y, row, ih, first, fmt, l, 0 };
		wgpuRenderPassEncoderSetImmediates(pass, 0, imm, sizeof(imm));
		wgpuRenderPassEncoderSetViewport(pass, rg->imageOffset.x, rg->imageOffset.y, w, h, 0, 1);
		wgpuRenderPassEncoderSetScissorRect(pass, rg->imageOffset.x, rg->imageOffset.y, w, h);
		wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
		wgpuTextureViewRelease(v);
	}
	return true;
}

void
wgvk_cmd_copy_buffer_image(struct wgvk_replay *rp, struct wgvk_buffer *buf, struct wgvk_image *img,
			   const VkBufferImageCopy2 *rg, bool to_buffer)
{
	if (depth_copy(rp, buf, img, rg, to_buffer))
		return;
	struct texel_layout tl = texel_layout(img, rg->imageSubresource.aspectMask);
	uint32_t w = rg->imageExtent.width, h = rg->imageExtent.height;
	uint32_t layers = vk_image_subresource_layer_count(&img->vk, &rg->imageSubresource);
	if (img->vk.image_type == VK_IMAGE_TYPE_3D)
		layers = rg->imageExtent.depth;
	uint32_t row_len = rg->bufferRowLength ? rg->bufferRowLength : w;
	uint32_t img_h = rg->bufferImageHeight ? rg->bufferImageHeight : h;
	uint32_t wb = DIV_ROUND_UP(w, tl.bw), hb = DIV_ROUND_UP(h, tl.bh);
	uint64_t pitch = (uint64_t)DIV_ROUND_UP(row_len, tl.bw) * tl.bs;
	uint64_t slice = pitch * DIV_ROUND_UP(img_h, tl.bh);
	uint64_t row = (uint64_t)wb * tl.bs;
	uint64_t total = slice * (layers - 1) + pitch * (hb - 1) + row;

	WGPUTexelCopyTextureInfo t = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	t.texture = img->tex;
	t.mipLevel = rg->imageSubresource.mipLevel;
	t.aspect = tl.aspect;
	t.origin = (WGPUOrigin3D){ rg->imageOffset.x, rg->imageOffset.y,
				   img->vk.image_type == VK_IMAGE_TYPE_3D ? (uint32_t)rg->imageOffset.z
									  : rg->imageSubresource.baseArrayLayer };
	WGPUExtent3D size = { w, h, layers };
	uint64_t boff;
	WGPUBuffer bbuf = wgvk_buffer_wgpu(buf, rg->bufferOffset, &boff);
	WGPUTexelCopyBufferInfo b = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;

	if (!to_buffer)
		wgvk_upload(rp, buf->mem, buf->offset + rg->bufferOffset, total);
	if (pitch % 256 == 0 && boff % 4 == 0 && boff % tl.bs == 0) {
		b.buffer = bbuf;
		b.layout = (WGPUTexelCopyBufferLayout){ boff, (uint32_t)pitch, DIV_ROUND_UP(img_h, tl.bh) };
		if (to_buffer)
			wgpuCommandEncoderCopyTextureToBuffer(rp->enc, &t, &b, &size);
		else
			wgpuCommandEncoderCopyBufferToTexture(rp->enc, &b, &t, &size);
	} else {
		/* through a buffer with rows padded to 256 bytes, row by row */
		if (row % 4 || boff % 4 || pitch % 4) {
			if (tl.bw == 1 && tl.bh == 1 && boff % tl.bs == 0) {
				/* rows that are not whole words (a 270-byte row of 8-bit texels: Blender's glyph atlas, VK-16):
				 * one copy per row, which WebGPU allows with no bytesPerRow, at any offset that is a multiple of
				 * the texel size */
				WGPUTexelCopyBufferInfo rb = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
				rb.buffer = bbuf;
				rb.layout.bytesPerRow = WGPU_COPY_STRIDE_UNDEFINED;
				rb.layout.rowsPerImage = WGPU_COPY_STRIDE_UNDEFINED;
				WGPUExtent3D rsize = { w, 1, 1 };
				WGPUTexelCopyTextureInfo rt = t;
				for (uint32_t a = 0; a < layers; a++) {
					for (uint32_t y = 0; y < h; y++) {
						rb.layout.offset = boff + a * slice + y * pitch;
						rt.origin = t.origin;
						rt.origin.y += y;
						if (img->vk.image_type == VK_IMAGE_TYPE_3D)
							rt.origin.z += a;
						else
							rt.origin.z += a;
						if (to_buffer)
							wgpuCommandEncoderCopyTextureToBuffer(rp->enc, &rt, &rb, &rsize);
						else
							wgpuCommandEncoderCopyBufferToTexture(rp->enc, &rb, &rt, &rsize);
					}
				}
				if (to_buffer)
					wgvk_wrote(rp, buf->mem, buf->offset + rg->bufferOffset, total);
				return;
			}
			wgvk_warn_once("buffer/image copy with rows of %llu bytes at offset %llu: T2 needs a compute copy\n",
				       (unsigned long long)row, (unsigned long long)boff);
			return;
		}
		uint64_t tpitch = align64(row, 256), tslice = tpitch * hb;
		WGPUBuffer tmp = wgvk_temp_buffer(rp, tslice * layers, WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
		b.buffer = tmp;
		b.layout = (WGPUTexelCopyBufferLayout){ 0, (uint32_t)tpitch, hb };
		if (!to_buffer)
			for (uint32_t a = 0; a < layers; a++)
				for (uint32_t y = 0; y < hb; y++)
					wgpuCommandEncoderCopyBufferToBuffer(rp->enc, bbuf, boff + a * slice + y * pitch, tmp,
									     a * tslice + y * tpitch, row);
		if (to_buffer)
			wgpuCommandEncoderCopyTextureToBuffer(rp->enc, &t, &b, &size);
		else
			wgpuCommandEncoderCopyBufferToTexture(rp->enc, &b, &t, &size);
		if (to_buffer)
			for (uint32_t a = 0; a < layers; a++)
				for (uint32_t y = 0; y < hb; y++)
					wgpuCommandEncoderCopyBufferToBuffer(rp->enc, tmp, a * tslice + y * tpitch, bbuf,
									     boff + a * slice + y * pitch, row);
	}
	if (to_buffer)
		wgvk_wrote(rp, buf->mem, buf->offset + rg->bufferOffset, total);
}

/* ---- images */

static bool
same_copy_format(WGPUTextureFormat a, WGPUTextureFormat b)
{
	if (a == b)
		return true;
	/* WebGPU copies between sRGB and linear variants */
	return (a == WGPUTextureFormat_RGBA8Unorm && b == WGPUTextureFormat_RGBA8UnormSrgb) ||
	       (b == WGPUTextureFormat_RGBA8Unorm && a == WGPUTextureFormat_RGBA8UnormSrgb) ||
	       (a == WGPUTextureFormat_BGRA8Unorm && b == WGPUTextureFormat_BGRA8UnormSrgb) ||
	       (b == WGPUTextureFormat_BGRA8Unorm && a == WGPUTextureFormat_BGRA8UnormSrgb);
}

void
wgvk_cmd_copy_image(struct wgvk_replay *rp, const VkCopyImageInfo2 *info)
{
	VK_FROM_HANDLE(wgvk_image, src, info->srcImage);
	VK_FROM_HANDLE(wgvk_image, dst, info->dstImage);
	for (uint32_t i = 0; i < info->regionCount; i++) {
		const VkImageCopy2 *rg = &info->pRegions[i];
		uint32_t layers = vk_image_subresource_layer_count(&src->vk, &rg->srcSubresource);
		bool s3d = src->vk.image_type == VK_IMAGE_TYPE_3D, d3d = dst->vk.image_type == VK_IMAGE_TYPE_3D;
		struct texel_layout sl = texel_layout(src, rg->srcSubresource.aspectMask);
		struct texel_layout dl = texel_layout(dst, rg->dstSubresource.aspectMask);
		WGPUTexelCopyTextureInfo s = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, d = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		s.texture = src->tex;
		s.mipLevel = rg->srcSubresource.mipLevel;
		s.origin = (WGPUOrigin3D){ rg->srcOffset.x, rg->srcOffset.y,
					   s3d ? (uint32_t)rg->srcOffset.z : rg->srcSubresource.baseArrayLayer };
		s.aspect = sl.aspect;
		d.texture = dst->tex;
		d.mipLevel = rg->dstSubresource.mipLevel;
		d.origin = (WGPUOrigin3D){ rg->dstOffset.x, rg->dstOffset.y,
					   d3d ? (uint32_t)rg->dstOffset.z : rg->dstSubresource.baseArrayLayer };
		d.aspect = dl.aspect;
		WGPUExtent3D size = { rg->extent.width, rg->extent.height, s3d ? rg->extent.depth : layers };
		if (same_copy_format(src->wformat, dst->wformat)) {
			wgpuCommandEncoderCopyTextureToTexture(rp->enc, &s, &d, &size);
			continue;
		}
		if (sl.bs != dl.bs || sl.bw != dl.bw || sl.bh != dl.bh) {
			wgvk_warn_once("image copy between formats of different texel sizes\n");
			continue;
		}
		/* different formats of one texel size: through a buffer */
		uint32_t wb = DIV_ROUND_UP(size.width, sl.bw), hb = DIV_ROUND_UP(size.height, sl.bh);
		uint64_t pitch = align64((uint64_t)wb * sl.bs, 256);
		WGPUBuffer tmp = wgvk_temp_buffer(rp, pitch * hb * size.depthOrArrayLayers,
						  WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);
		WGPUTexelCopyBufferInfo b = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
		b.buffer = tmp;
		b.layout = (WGPUTexelCopyBufferLayout){ 0, (uint32_t)pitch, hb };
		wgpuCommandEncoderCopyTextureToBuffer(rp->enc, &s, &b, &size);
		wgpuCommandEncoderCopyBufferToTexture(rp->enc, &b, &d, &size);
	}
}

/* ---- clears outside passes: a render pass that clears, per mip level and layer */

static void
clear_pass(struct wgvk_replay *rp, struct wgvk_image *img, uint32_t level, uint32_t layer, const WGPUColor *color,
	   const VkClearDepthStencilValue *ds, VkImageAspectFlags aspects)
{
	WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	vd.format = img->wformat;
	vd.dimension = img->vk.image_type == VK_IMAGE_TYPE_3D ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
	vd.baseMipLevel = level;
	vd.mipLevelCount = 1;
	vd.baseArrayLayer = img->vk.image_type == VK_IMAGE_TYPE_3D ? 0 : layer;
	vd.arrayLayerCount = 1;
	WGPUTextureView view = wgpuTextureCreateView(img->tex, &vd);
	WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	WGPURenderPassDepthStencilAttachment dsa = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	dsa.depthClearValue = 0.0f;	/* (webgpu.h's undefined is NaN, which the browser refuses: VK-16) */
	WGPURenderPipeline meta = NULL;
	uint8_t imm[32] = { 0 };
	if (color) {
		ca.view = view;
		ca.depthSlice = img->vk.image_type == VK_IMAGE_TYPE_3D ? layer : WGPU_DEPTH_SLICE_UNDEFINED;
		ca.loadOp = WGPULoadOp_Clear;
		ca.storeOp = WGPUStoreOp_Store;
		ca.clearValue = *color;
		/* WebGPU refuses NaN and infinite clear values: cleared to 0, then a driver quad writes the bits */
		float f[4] = { color->r, color->g, color->b, color->a };
		if (!vk_format_is_int(img->vk.format) && !wgvk_color_is_finite(f)) {
			ca.clearValue = (WGPUColor){ 0, 0, 0, 0 };
			struct wgvk_rendering r = { .ncolor = 1, .samples = img->vk.samples };
			r.color_fmt[0] = img->wformat;
			meta = wgvk_meta_clear_pipeline(rp->dev, &r, 1, false, false);
			memcpy(imm, f, 16);
			rp->dev->stats.nonfinite_clears++;
		}
		rpd.colorAttachmentCount = 1;
		rpd.colorAttachments = &ca;
	} else {
		dsa.view = view;
		if (vk_format_has_depth(img->vk.format)) {
			bool c = aspects & VK_IMAGE_ASPECT_DEPTH_BIT;
			dsa.depthLoadOp = c ? WGPULoadOp_Clear : WGPULoadOp_Load;
			dsa.depthStoreOp = WGPUStoreOp_Store;
			dsa.depthClearValue = CLAMP(ds->depth, 0.0f, 1.0f);
		}
		if (vk_format_has_stencil(img->vk.format)) {
			bool c = aspects & VK_IMAGE_ASPECT_STENCIL_BIT;
			dsa.stencilLoadOp = c ? WGPULoadOp_Clear : WGPULoadOp_Load;
			dsa.stencilStoreOp = WGPUStoreOp_Store;
			dsa.stencilClearValue = ds->stencil;
		}
		rpd.depthStencilAttachment = &dsa;
	}
	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(rp->enc, &rpd);
	if (meta) {
		wgpuRenderPassEncoderSetPipeline(pass, meta);
		wgpuRenderPassEncoderSetImmediates(pass, 0, imm, sizeof(imm));
		wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
	}
	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);
	wgpuTextureViewRelease(view);
}

static void
clear_image(struct wgvk_replay *rp, struct wgvk_image *img, const WGPUColor *color, const VkClearDepthStencilValue *ds,
	    uint32_t n, const VkImageSubresourceRange *ranges)
{
	if (!(img->wusage & WGPUTextureUsage_RenderAttachment)) {
		wgvk_warn_once("clearing an image of a format WebGPU cannot render to (T2: clear through a buffer)\n");
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		const VkImageSubresourceRange *rg = &ranges[i];
		uint32_t levels = vk_image_subresource_level_count(&img->vk, rg);
		for (uint32_t l = 0; l < levels; l++) {
			uint32_t level = rg->baseMipLevel + l;
			uint32_t layers = img->vk.image_type == VK_IMAGE_TYPE_3D ? MAX2(img->vk.extent.depth >> level, 1)
										 : vk_image_subresource_layer_count(&img->vk, rg);
			uint32_t layer0 = img->vk.image_type == VK_IMAGE_TYPE_3D ? 0 : rg->baseArrayLayer;
			for (uint32_t a = 0; a < layers; a++)
				clear_pass(rp, img, level, layer0 + a, color, ds, rg->aspectMask);
		}
	}
}

void
wgvk_cmd_clear_color_image(struct wgvk_replay *rp, struct wgvk_image *img, const VkClearColorValue *c, uint32_t n,
			   const VkImageSubresourceRange *ranges)
{
	WGPUColor color;
	if (vk_format_is_sint(img->vk.format))
		color = (WGPUColor){ c->int32[0], c->int32[1], c->int32[2], c->int32[3] };
	else if (vk_format_is_uint(img->vk.format))
		color = (WGPUColor){ c->uint32[0], c->uint32[1], c->uint32[2], c->uint32[3] };
	else
		color = (WGPUColor){ c->float32[0], c->float32[1], c->float32[2], c->float32[3] };
	clear_image(rp, img, &color, NULL, n, ranges);
}

void
wgvk_cmd_clear_ds_image(struct wgvk_replay *rp, struct wgvk_image *img, const VkClearDepthStencilValue *v, uint32_t n,
			const VkImageSubresourceRange *ranges)
{
	clear_image(rp, img, NULL, v, n, ranges);
}

/* ---- blits: a quad per region (wgvk_meta.c) */

static int
sample_kind(const struct wgvk_image *img, VkFilter filter)
{
	if (vk_format_is_depth_or_stencil(img->vk.format))
		return 3;
	if (vk_format_is_uint(img->vk.format))
		return 1;
	if (vk_format_is_sint(img->vk.format))
		return 2;
	return 0;
}

void
wgvk_cmd_blit_image(struct wgvk_replay *rp, const VkBlitImageInfo2 *info)
{
	struct wgvk_device *dev = rp->dev;
	VK_FROM_HANDLE(wgvk_image, src, info->srcImage);
	VK_FROM_HANDLE(wgvk_image, dst, info->dstImage);
	if (!(src->wusage & WGPUTextureUsage_TextureBinding) || !(dst->wusage & WGPUTextureUsage_RenderAttachment)) {
		wgvk_warn_once("blit between images WebGPU cannot sample or render to\n");
		return;
	}
	bool s3d = src->vk.image_type == VK_IMAGE_TYPE_3D, d3d = dst->vk.image_type == VK_IMAGE_TYPE_3D;
	for (uint32_t i = 0; i < info->regionCount; i++) {
		const VkImageBlit2 *rg = &info->pRegions[i];
		/* destination layers (or 3D slices) and where each samples the source: a layer, or a normalised z */
		uint32_t layers = vk_image_subresource_layer_count(&src->vk, &rg->srcSubresource);
		int32_t dz0 = d3d ? rg->dstOffsets[0].z : 0, dz1 = d3d ? rg->dstOffsets[1].z : (int32_t)layers;
		float sz0 = rg->srcOffsets[0].z, sz1 = rg->srcOffsets[1].z;
		if (dz1 < dz0) {
			int32_t t = dz0; dz0 = dz1; dz1 = t;
			float f = sz0; sz0 = sz1; sz1 = f;
		}
		uint32_t sd = MAX2(src->vk.extent.depth >> rg->srcSubresource.mipLevel, 1);
		if (!d3d)
			layers = vk_image_subresource_layer_count(&dst->vk, &rg->dstSubresource);
		uint32_t nout = d3d ? (uint32_t)(dz1 - dz0) : layers;
		int kind = sample_kind(src, info->filter);
		bool depth = kind == 3;
		WGPUTextureViewDimension sdim = s3d ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2DArray;
		WGPUBindGroupLayout bgl;
		WGPURenderPipeline p = wgvk_meta_blit_pipeline(dev, dst->wformat, sdim, kind, &bgl);
		uint32_t sw = MAX2(src->vk.extent.width >> rg->srcSubresource.mipLevel, 1);
		uint32_t sh = MAX2(src->vk.extent.height >> rg->srcSubresource.mipLevel, 1);
		/* the source rectangle in normalised coordinates; a mirrored destination swaps it */
		float sx0 = rg->srcOffsets[0].x / (float)sw, sx1 = rg->srcOffsets[1].x / (float)sw;
		float sy0 = rg->srcOffsets[0].y / (float)sh, sy1 = rg->srcOffsets[1].y / (float)sh;
		int32_t dx0 = rg->dstOffsets[0].x, dx1 = rg->dstOffsets[1].x;
		int32_t dy0 = rg->dstOffsets[0].y, dy1 = rg->dstOffsets[1].y;
		if (dx1 < dx0) {
			int32_t t = dx0; dx0 = dx1; dx1 = t;
			float f = sx0; sx0 = sx1; sx1 = f;
		}
		if (dy1 < dy0) {
			int32_t t = dy0; dy0 = dy1; dy1 = t;
			float f = sy0; sy0 = sy1; sy1 = f;
		}
		if (dx1 == dx0 || dy1 == dy0)
			continue;
		WGPUTextureViewDescriptor svd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
		svd.format = src->wformat;
		svd.dimension = sdim;
		svd.baseMipLevel = rg->srcSubresource.mipLevel;
		svd.mipLevelCount = 1;
		svd.baseArrayLayer = s3d ? 0 : rg->srcSubresource.baseArrayLayer;
		svd.arrayLayerCount = s3d ? 1 : vk_image_subresource_layer_count(&src->vk, &rg->srcSubresource);
		svd.aspect = depth && vk_format_has_stencil(src->vk.format) ? WGPUTextureAspect_DepthOnly
									     : WGPUTextureAspect_All;
		WGPUTextureView sv = wgpuTextureCreateView(src->tex, &svd);
		WGPUBindGroupEntry be[2] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
		be[0].binding = 0;
		be[0].textureView = sv;
		be[1].binding = 1;
		be[1].sampler = info->filter == VK_FILTER_LINEAR ? dev->meta_linear : dev->meta_nearest;
		WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bd.layout = bgl;
		bd.entryCount = kind == 0 ? 2 : 1;
		bd.entries = be;
		WGPUBindGroup bg = wgpuDeviceCreateBindGroup(dev->wdev, &bd);
		for (uint32_t a = 0; a < nout; a++) {
			WGPUTextureViewDescriptor dvd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
			dvd.format = dst->wformat;
			dvd.dimension = d3d ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
			dvd.baseMipLevel = rg->dstSubresource.mipLevel;
			dvd.mipLevelCount = 1;
			dvd.baseArrayLayer = d3d ? 0 : rg->dstSubresource.baseArrayLayer + a;
			dvd.arrayLayerCount = 1;
			WGPUTextureView dv = wgpuTextureCreateView(dst->tex, &dvd);
			WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			WGPURenderPassDepthStencilAttachment dsa = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
			dsa.depthClearValue = 0.0f;	/* (webgpu.h's undefined is NaN, which the browser refuses: VK-16) */
			WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
			if (vk_format_is_depth_or_stencil(dst->vk.format)) {
				dsa.view = dv;
				dsa.depthLoadOp = WGPULoadOp_Load;
				dsa.depthStoreOp = WGPUStoreOp_Store;
				if (vk_format_has_stencil(dst->vk.format)) {
					dsa.stencilLoadOp = WGPULoadOp_Load;
					dsa.stencilStoreOp = WGPUStoreOp_Store;
				}
				rpd.depthStencilAttachment = &dsa;
			} else {
				ca.view = dv;
				ca.depthSlice = d3d ? (uint32_t)(dz0 + a) : WGPU_DEPTH_SLICE_UNDEFINED;
				ca.loadOp = WGPULoadOp_Load;
				ca.storeOp = WGPUStoreOp_Store;
				rpd.colorAttachmentCount = 1;
				rpd.colorAttachments = &ca;
			}
			WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(rp->enc, &rpd);
			wgpuRenderPassEncoderSetPipeline(pass, p);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
			/* the source: layer a (2D), or the z at the middle of this destination slice (3D) */
			float zsrc = s3d ? (sz0 + (a + 0.5f) * (sz1 - sz0) / MAX2(nout, 1)) / sd
					 : (float)(d3d ? rg->srcSubresource.baseArrayLayer * 0 + a : a);
			float imm[8] = { sx0, sy0, sx1, sy1, 0.0f, zsrc, 0, 0 };
			wgpuRenderPassEncoderSetImmediates(pass, 0, imm, sizeof(imm));
			wgpuRenderPassEncoderSetViewport(pass, dx0, dy0, dx1 - dx0, dy1 - dy0, 0, 1);
			wgpuRenderPassEncoderDraw(pass, 4, 1, 0, 0);
			wgpuRenderPassEncoderEnd(pass);
			wgpuRenderPassEncoderRelease(pass);
			wgpuTextureViewRelease(dv);
		}
		wgpuBindGroupRelease(bg);
		wgpuTextureViewRelease(sv);
	}
}

/* ---- resolves: an empty render pass with a resolve target, for whole subresources */

void
wgvk_cmd_resolve_image(struct wgvk_replay *rp, const VkResolveImageInfo2 *info)
{
	VK_FROM_HANDLE(wgvk_image, src, info->srcImage);
	VK_FROM_HANDLE(wgvk_image, dst, info->dstImage);
	for (uint32_t i = 0; i < info->regionCount; i++) {
		const VkImageResolve2 *rg = &info->pRegions[i];
		uint32_t w = MAX2(src->vk.extent.width >> rg->srcSubresource.mipLevel, 1);
		uint32_t h = MAX2(src->vk.extent.height >> rg->srcSubresource.mipLevel, 1);
		if (rg->srcOffset.x || rg->srcOffset.y || rg->dstOffset.x || rg->dstOffset.y || rg->extent.width != w ||
		    rg->extent.height != h) {
			wgvk_warn_once("resolve of part of an image (T2: meta resolve)\n");
		}
		uint32_t layers = vk_image_subresource_layer_count(&src->vk, &rg->srcSubresource);
		for (uint32_t a = 0; a < layers; a++) {
			WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
			vd.format = src->wformat;
			vd.dimension = WGPUTextureViewDimension_2D;
			vd.baseMipLevel = rg->srcSubresource.mipLevel;
			vd.mipLevelCount = 1;
			vd.baseArrayLayer = rg->srcSubresource.baseArrayLayer + a;
			vd.arrayLayerCount = 1;
			WGPUTextureView sv = wgpuTextureCreateView(src->tex, &vd);
			vd.format = dst->wformat;
			vd.baseMipLevel = rg->dstSubresource.mipLevel;
			vd.baseArrayLayer = rg->dstSubresource.baseArrayLayer + a;
			WGPUTextureView dv = wgpuTextureCreateView(dst->tex, &vd);
			WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			ca.view = sv;
			ca.resolveTarget = dv;
			ca.loadOp = WGPULoadOp_Load;
			ca.storeOp = WGPUStoreOp_Store;
			WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
			rpd.colorAttachmentCount = 1;
			rpd.colorAttachments = &ca;
			WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(rp->enc, &rpd);
			wgpuRenderPassEncoderEnd(pass);
			wgpuRenderPassEncoderRelease(pass);
			wgpuTextureViewRelease(sv);
			wgpuTextureViewRelease(dv);
		}
	}
}

/* ---- image buffers (T3b's IMAGE_BUFFER): a storage image used atomically is a storage buffer per view;
 * the texels move between it and the texture when the other is used (wgvk_image's ib_owner, ib_gen) */

static void
ibuf_copy(struct wgvk_device *dev, WGPUCommandEncoder enc, struct wgvk_image *img, struct wgvk_image_view *v,
	  bool to_buffer)
{
	uint32_t w, h, n;
	wgvk_image_view_ibuf_extent(v, &w, &h, &n);
	bool is3d = img->vk.image_type == VK_IMAGE_TYPE_3D;
	wgvk_image_make_current(dev, enc, img, v->alias);
	WGPUTexelCopyTextureInfo t = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	t.texture = v->alias >= 0 ? img->alias[v->alias].tex : img->tex;
	t.mipLevel = v->vk.base_mip_level;
	uint32_t z0 = is3d ? 0 : v->vk.base_array_layer;
	WGPUTexelCopyBufferInfo b = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	b.buffer = v->ibuf;
	if ((w * 4) % 256 == 0) {
		t.origin = (WGPUOrigin3D){ 0, 0, z0 };
		b.layout.offset = 16;
		b.layout.bytesPerRow = w * 4;
		b.layout.rowsPerImage = h;
		WGPUExtent3D ext = { w, h, n };
		if (to_buffer)
			wgpuCommandEncoderCopyTextureToBuffer(enc, &t, &b, &ext);
		else
			wgpuCommandEncoderCopyBufferToTexture(enc, &b, &t, &ext);
		return;
	}
	/* rows not 256-byte multiples: the buffer's rows are packed, WebGPU's are not; a copy per row */
	if ((uint64_t)h * n > 4096)
		wgvk_warn_once("image buffer of %ux%ux%u: %u row copies each way (T2: a compute copy)\n", w, h, n,
			       h * n);
	WGPUExtent3D ext = { w, 1, 1 };
	for (uint32_t z = 0; z < n; z++) {
		for (uint32_t y = 0; y < h; y++) {
			t.origin = (WGPUOrigin3D){ 0, y, z0 + z };
			b.layout.offset = 16 + ((uint64_t)z * h + y) * w * 4;
			b.layout.bytesPerRow = WGPU_COPY_STRIDE_UNDEFINED;
			b.layout.rowsPerImage = WGPU_COPY_STRIDE_UNDEFINED;
			if (to_buffer)
				wgpuCommandEncoderCopyTextureToBuffer(enc, &t, &b, &ext);
			else
				wgpuCommandEncoderCopyBufferToTexture(enc, &b, &t, &ext);
		}
	}
}

/* the owner's texels back into the texture (which then is current, and so is the owner's buffer) */
static void
ibuf_flush(struct wgvk_device *dev, WGPUCommandEncoder enc, struct wgvk_image *img)
{
	struct wgvk_image_view *o = img->ib_owner;
	if (!o)
		return;
	ibuf_copy(dev, enc, img, o, false);
	img->ib_owner = NULL;
	img->ib_gen++;
	o->ibuf_gen = img->ib_gen;
	dev->stats.ibuf_copies++;
}

void
wgvk_image_ibuf_flush_now(struct wgvk_device *dev, struct wgvk_image *img)
{
	if (!img->ib_owner)
		return;
	WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev->wdev, NULL);
	ibuf_flush(dev, enc, img);
	WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
	wgpuQueueSubmit(dev->wqueue, 1, &cb);
	wgpuCommandBufferRelease(cb);
	wgpuCommandEncoderRelease(enc);
}

/* Whether wgvk_image_before / wgvk_image_ibuf_use would encode copies (callers inside a compute pass end
 * it first). */
bool
wgvk_image_needs_sync(const struct wgvk_image *img, const struct wgvk_image_view *ibuf_view, int alias)
{
	if (ibuf_view)
		return img->ib_owner != ibuf_view;
	return img->ib_owner || (img->n_alias && img->current != alias);
}

/* Before a command uses the image's texture (through texture `alias`, -1: its own): texels an image buffer
 * holds go back, an alias's are copied over; a write makes the views' buffers stale. Outside passes. */
void
wgvk_image_before(struct wgvk_replay *rp, struct wgvk_image *img, int alias, bool write)
{
	if (!img)
		return;
	if (img->ib_owner)
		ibuf_flush(rp->dev, rp->enc, img);
	if (img->n_alias)
		wgvk_image_make_current(rp->dev, rp->enc, img, alias);
	if (write)
		img->ib_gen++;
}

/* Before a command uses view v's image buffer: it gets the latest texels and owns them. Outside passes. */
void
wgvk_image_ibuf_use(struct wgvk_replay *rp, struct wgvk_image_view *v)
{
	struct wgvk_image *img = container_of(v->vk.image, struct wgvk_image, vk);
	if (img->ib_owner == v || !v->ibuf)
		return;
	if (img->ib_owner)
		ibuf_flush(rp->dev, rp->enc, img);
	if (v->ibuf_gen != img->ib_gen) {
		ibuf_copy(rp->dev, rp->enc, img, v, true);
		v->ibuf_gen = img->ib_gen;
		rp->dev->stats.ibuf_copies++;
	}
	img->ib_owner = v;
}

/* The images a run of texture uses touches: before (textures) or ibuf_use (image buffers). */
void
wgvk_tuses_before(struct wgvk_replay *rp, const struct wgvk_tuse *t, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		if (!t[i].iv)
			continue;
		if (t[i].ibuf) {
			wgvk_image_ibuf_use(rp, t[i].iv);
		} else {
			struct wgvk_image *img = container_of(t[i].iv->vk.image, struct wgvk_image, vk);
			wgvk_image_before(rp, img, t[i].iv->alias, t[i].write);
		}
	}
}

bool
wgvk_tuses_need_sync(const struct wgvk_tuse *t, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		if (!t[i].iv)
			continue;
		struct wgvk_image *img = container_of(t[i].iv->vk.image, struct wgvk_image, vk);
		if (wgvk_image_needs_sync(img, t[i].ibuf ? t[i].iv : NULL, t[i].iv->alias))
			return true;
	}
	return false;
}
