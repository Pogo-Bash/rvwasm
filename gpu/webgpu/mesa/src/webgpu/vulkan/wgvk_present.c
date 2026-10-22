/*
 * wgvk: what the page needs to show an image (T7, INTERFACES.md 9).
 *
 * A window's frame is a texture of this device: a virgl resource's zink image
 * (vfront finds the VkImage), or a venus WSI image, which the guest hands the
 * window server as the dma-buf of its memory (vkr knows the VkDeviceMemory of
 * the blob; this file knows which image was bound to that memory). The page's
 * Worker then draws the texture into its canvas.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wgvk_private.h"

#include "util/hash_table.h"
#include "util/simple_mtx.h"

/* memory -> the image last bound to it, per device. One lock for all devices: binds and the page's lookups
 * are rare next to everything else. */
static simple_mtx_t present_mutex = SIMPLE_MTX_INITIALIZER;

void
wgvk_present_bind(struct wgvk_device *dev, struct wgvk_device_memory *mem, struct wgvk_image *img)
{
	if (!mem)
		return;
	simple_mtx_lock(&present_mutex);
	if (!dev->mem_images)
		dev->mem_images = _mesa_pointer_hash_table_create(NULL);
	_mesa_hash_table_insert(dev->mem_images, mem, img);
	simple_mtx_unlock(&present_mutex);
}

/* An image or a memory object goes away: nothing points at it any more. `img->mem` is only used as a key here
 * (the memory may be freed before the image: Vulkan allows it). */
void
wgvk_present_forget(struct wgvk_device *dev, struct wgvk_image *img, struct wgvk_device_memory *mem)
{
	simple_mtx_lock(&present_mutex);
	if (dev->mem_images) {
		if (img && img->mem) {
			struct hash_entry *e = _mesa_hash_table_search(dev->mem_images, img->mem);
			if (e && e->data == img)
				_mesa_hash_table_remove(dev->mem_images, e);
		}
		if (mem) {
			struct hash_entry *e = _mesa_hash_table_search(dev->mem_images, mem);
			if (e)
				_mesa_hash_table_remove(dev->mem_images, e);
		}
	}
	simple_mtx_unlock(&present_mutex);
}

void
wgvk_present_finish(struct wgvk_device *dev)
{
	simple_mtx_lock(&present_mutex);
	if (dev->mem_images)
		_mesa_hash_table_destroy(dev->mem_images, NULL);
	dev->mem_images = NULL;
	simple_mtx_unlock(&present_mutex);
}

/* wgvk.h: the image bound to a memory object last (a WSI image's dedicated memory), or VK_NULL_HANDLE. */
PUBLIC VkImage
wgvk_memory_image(VkDevice device, VkDeviceMemory memory)
{
	VK_FROM_HANDLE(wgvk_device, dev, device);
	/* (the handle is only a key here: a venus blob outlives its memory when the guest frees the memory first,
	 * and VK_FROM_HANDLE would look into the freed object) */
	struct wgvk_device_memory *mem = (struct wgvk_device_memory *)(uintptr_t)memory;
	struct wgvk_image *img = NULL;
	simple_mtx_lock(&present_mutex);
	if (dev && mem && dev->mem_images) {
		struct hash_entry *e = _mesa_hash_table_search(dev->mem_images, mem);
		if (e)
			img = e->data;
	}
	simple_mtx_unlock(&present_mutex);
	return wgvk_image_to_handle(img);
}

/* wgvk.h: the texture that holds an image's latest texels (level 0, layer 0 is what a window shows). When the
 * latest texels are in one of the image's view-format aliases (wgvk_image.alias: a view WebGPU cannot make of
 * the texture itself), they are copied back first, on the device's queue, ahead of whatever the caller then
 * submits. NULL when the image has no texture. */
PUBLIC WGPUTexture
wgvk_image_present_texture(VkImage image, uint32_t *width, uint32_t *height)
{
	VK_FROM_HANDLE(wgvk_image, img, image);
	if (!img || !img->tex)
		return NULL;
	if (img->current != -1) {
		struct wgvk_device *dev = container_of(img->vk.base.device, struct wgvk_device, vk);
		WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev->wdev, NULL);
		wgvk_image_make_current(dev, enc, img, -1);
		WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
		wgpuQueueSubmit(dev->wqueue, 1, &cb);
		wgpuCommandBufferRelease(cb);
		wgpuCommandEncoderRelease(enc);
	}
	if (width)
		*width = img->vk.extent.width;
	if (height)
		*height = img->vk.extent.height;
	return img->tex;
}

static void
on_read_mapped(WGPUMapAsyncStatus status, WGPUStringView message, void *u1, void *u2)
{
	(void)message;
	(void)u2;
	*(bool *)u1 = status == WGPUMapAsyncStatus_Success;
}

/* wgvk.h: a native test bed's present: the texture's texels, in the image's linear layout, into the caller's
 * memory (what the guest's window server then reads from the venus blob). Blocks until the GPU has them. */
PUBLIC VkResult
wgvk_image_read_present(VkImage image, void *dst, uint64_t dst_size, uint32_t *width, uint32_t *height,
			uint32_t *pitch)
{
	VK_FROM_HANDLE(wgvk_image, img, image);
	uint32_t w, h;
	WGPUTexture tex = wgvk_image_present_texture(image, &w, &h);
	if (!tex)
		return VK_ERROR_UNKNOWN;
	if (img->fmt->block_w != 1 || img->fmt->block_h != 1 || (img->fmt->flags & WGVK_FMT_SWIZZLED))
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	struct wgvk_device *dev = container_of(img->vk.base.device, struct wgvk_device, vk);
	const uint32_t row = img->level[0].row_pitch;	/* 256-aligned: a legal bytesPerRow */
	const uint64_t size = (uint64_t)row * h;
	if (size > dst_size)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	bd.size = size;
	bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
	WGPUBuffer buf = wgpuDeviceCreateBuffer(dev->wdev, &bd);
	WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dev->wdev, NULL);
	WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	src.texture = tex;
	WGPUTexelCopyBufferInfo to = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	to.buffer = buf;
	to.layout.bytesPerRow = row;
	to.layout.rowsPerImage = h;
	WGPUExtent3D ext = { w, h, 1 };
	wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &to, &ext);
	WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
	wgpuQueueSubmit(dev->wqueue, 1, &cb);
	wgpuCommandBufferRelease(cb);
	wgpuCommandEncoderRelease(enc);
	bool ok = false;
	WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
	ci.mode = WGPUCallbackMode_AllowProcessEvents;
	ci.callback = on_read_mapped;
	ci.userdata1 = &ok;
	WGPUFuture f = wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, size, ci);
	VkResult r = wgvk_wait(dev, &f, 1, UINT64_MAX);
	if (r == VK_SUCCESS && ok) {
		memcpy(dst, wgpuBufferGetConstMappedRange(buf, 0, size), size);
		wgpuBufferUnmap(buf);
	} else if (r == VK_SUCCESS) {
		r = VK_ERROR_MEMORY_MAP_FAILED;
	}
	wgpuBufferRelease(buf);
	if (width)
		*width = w;
	if (height)
		*height = h;
	if (pitch)
		*pitch = row;
	return r;
}

/* wgvk.h */
PUBLIC void
wgvk_device_set_retire_hook(VkDevice device, void (*fn)(void *data), void *data)
{
	VK_FROM_HANDLE(wgvk_device, dev, device);
	dev->on_retire = fn;
	dev->on_retire_data = data;
}
