/*
 * rvgpu_vkr: vkr (virglrenderer's venus renderer) as rvgpu's venus front end (T5; gpu/webgpu/INTERFACES.md 7).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVGPU_VKR_H
#define RVGPU_VKR_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start vkr on the given Vulkan driver (wgvk_GetInstanceProcAddr in the page) and register the venus ring
 * ops with rvgpu: the venus capset goes to the caps area (+2048, vcaps_len) and exec_state becomes 1.
 * `set_guest` makes a VkDeviceMemory's bytes the guest pages at an address (wgvk_memory_set_guest_window);
 * `device_poll` moves the driver's completion and returns a device's pending submissions (wgvk_device_poll):
 * the guest learns of completion only from shared memory, so the poll hook must ask.
 * After rvgpu_ring_attach. Returns 0, or -1. */
int rvgpu_vkr_start(PFN_vkGetInstanceProcAddr gipa,
		    void (*set_guest)(VkDevice dev, VkDeviceMemory mem, uint64_t guest, uint64_t size),
		    unsigned (*device_poll)(VkDevice dev));

/* The venus capset (struct virgl_renderer_capset_venus): its size; filled into `out` when not NULL. */
uint32_t rvgpu_vkr_capset(void *out);

#ifdef __cplusplus
}
#endif

#endif
