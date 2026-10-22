/*
 * wgvk's own window system integration (T7): VK_KHR_surface, VK_KHR_swapchain, VK_EXT_headless_surface and,
 * when the native build has the platforms (gpu/webgpu/build/native.sh: x11,wayland), VK_KHR_xcb_surface,
 * VK_KHR_xlib_surface and VK_KHR_wayland_surface, through Mesa's common WSI, for programs that run on wgvk
 * directly (natively: tests, vkcube, zink's kopper on X11 for Blender's UI and glxinfo under Xvfb). Built
 * natively only (WGVK_WSI).
 *
 * Swapchain images are ordinary wgvk images; the common WSI's "CPU" kind with a buffer blit is used: at
 * present the image is copied into a host-visible buffer (vkCmdCopyImageToBuffer, which wgvk downloads before
 * the present's fence moves), the way a software driver hands frames to a compositor: on X11 the common WSI
 * puts it with MIT-SHM (or XPutImage), on Wayland into a wl_shm buffer. Headless presents show nothing.
 *
 * Guest programs do not come here: Mesa's venus in the guest has its own WSI and hands the window server a
 * dma-buf of the image's memory; the page then shows the image through rvgpu's present (wgvk_present.c,
 * rvgpu_present_register_memory). wgvk's part in that is what wgvk_physical_device.c says about external
 * memory, the LINEAR modifier and sync files.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wgvk_private.h"

#ifdef WGVK_WSI

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wgvk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
	VK_FROM_HANDLE(wgvk_physical_device, pdev, physicalDevice);
	return vk_instance_get_proc_addr_unchecked(pdev->vk.instance, pName);
}

VkResult
wgvk_wsi_init(struct wgvk_physical_device *pdev)
{
	VkResult r = wsi_device_init(&pdev->wsi_device, wgvk_physical_device_to_handle(pdev), wgvk_wsi_proc_addr,
				     &pdev->vk.instance->alloc, -1, NULL,
				     &(struct wsi_device_options){ .sw_device = true });
	if (r != VK_SUCCESS)
		return r;
	/* (optimal images and a blit into a host-visible buffer: wgvk's linear images keep no texels in memory) */
	pdev->wsi_device.wants_linear = false;
	pdev->vk.wsi_device = &pdev->wsi_device;
	return VK_SUCCESS;
}

void
wgvk_wsi_finish(struct wgvk_physical_device *pdev)
{
	if (!pdev->vk.wsi_device)
		return;
	pdev->vk.wsi_device = NULL;
	wsi_device_finish(&pdev->wsi_device, &pdev->vk.instance->alloc);
}

#endif
