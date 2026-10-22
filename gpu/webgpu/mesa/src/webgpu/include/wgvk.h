/*
 * wgvk: a Vulkan driver on WebGPU (webgpu.h). The public entry points, for the
 * page host (rvgpu) and for consumers built into the same module (zink in
 * vfront, vkr). See gpu/webgpu/INTERFACES.md, section 1.
 */
#ifndef WGVK_H
#define WGVK_H

#include <vulkan/vulkan_core.h>
#include <webgpu/webgpu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hand wgvk the one WebGPU device it runs on, before the first vkCreateInstance.
 * wgvk takes a reference on each object. Without it (a native ICD), wgvk makes
 * its own Dawn device on first use. */
void wgvk_use_device(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device);

/* wgvk_use_device + vkCreateInstance, for consumers that do not go through the
 * Vulkan loader. *gipa is the instance's vkGetInstanceProcAddr. */
VkResult wgvk_create_instance(WGPUDevice device, const VkInstanceCreateInfo *info,
                              const VkAllocationCallbacks *alloc, VkInstance *instance,
                              PFN_vkGetInstanceProcAddr *gipa);

PFN_vkVoidFunction wgvk_GetInstanceProcAddr(VkInstance instance, const char *name);

/* T5 (vkr): from now on the bytes of host-visible memory `mem` are the guest's pages at guest address
 * `guest` (a venus blob the guest mapped: INTERFACES.md 4.1, 7.1); what the memory's own window held is
 * copied there first. guest 0: the window goes (the guest let go of the pages; the GPU's copy stays). */
void wgvk_memory_set_guest_window(VkDevice device, VkDeviceMemory mem, uint64_t guest, uint64_t size);

/* Presenting (T7, INTERFACES.md 9). The texture that holds an image's latest texels (a window shows level 0,
 * layer 0), and the image's size. If the latest texels are in a view-format alias of the image, they are
 * copied back first, on the device's queue, before anything the caller submits next. NULL when the image has
 * no texture. Not referenced: valid while the image lives. */
WGPUTexture wgvk_image_present_texture(VkImage image, uint32_t *width, uint32_t *height);
/* The image bound last to a memory object (a venus WSI image's dedicated memory: the guest hands the window
 * server that memory's dma-buf), or VK_NULL_HANDLE. */
VkImage wgvk_memory_image(VkDevice device, VkDeviceMemory memory);
/* Native test beds (no page to draw the texture): read an image's latest texels (level 0, layer 0) back into
 * `dst`, rows `*pitch` bytes apart (the image's own linear layout: VkSubresourceLayout.rowPitch, 256-aligned),
 * waiting for the GPU. `dst_size` bounds the write. Fills width/height/pitch when not NULL. VK_SUCCESS, or an
 * error (the image has no texture, `dst_size` is too small, the texture is not a plain colour format). */
VkResult wgvk_image_read_present(VkImage image, void *dst, uint64_t dst_size, uint32_t *width, uint32_t *height,
				 uint32_t *pitch);

/* Native test beds: fn(data) runs each time a submission of `device` has finished on the GPU, before its
 * downloads reach the guest's pages (so before the guest can learn that it completed): the point to read a
 * window's image back (wgvk_image_read_present). fn may call wgvk_image_read_present. */
void wgvk_device_set_retire_hook(VkDevice device, void (*fn)(void *data), void *data);

/* (T2) The persistent pipeline cache, module-wide: store(data, record, size) gets a record for every new
 * render pipeline (the page keeps them in IndexedDB); blob (records back to back, from earlier runs) is
 * preloaded into every device made afterwards, so its pipelines are created before a draw needs them. */
void wgvk_set_pipeline_cache(void (*store)(void *data, const void *record, size_t size), void *data,
                             const void *blob, size_t size);

/* (T2) Retire what WebGPU has finished on every device (downloads into memory windows, syncs, queries,
 * completion callbacks); never blocks. Returns the submissions still in flight. The page's loop calls it
 * after its events. */
unsigned wgvk_poll_all(void);

#ifdef __cplusplus
}
#endif

#endif
