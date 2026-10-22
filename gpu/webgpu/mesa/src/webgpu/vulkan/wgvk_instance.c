/*
 * wgvk: the instance (the physical device is wgvk_physical_device.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"

#include "util/u_debug.h"
#include "vk_common_entrypoints.h"

static const struct vk_instance_extension_table instance_extensions = {
	.KHR_device_group_creation = true,
	.KHR_external_fence_capabilities = true,
	.KHR_external_memory_capabilities = true,
	.KHR_external_semaphore_capabilities = true,
	.KHR_get_physical_device_properties2 = true,
	.EXT_debug_report = true,
	.EXT_debug_utils = true,
#ifdef WGVK_WSI
	.KHR_surface = true,
	.KHR_get_surface_capabilities2 = true,
	.EXT_headless_surface = true,
#ifdef VK_USE_PLATFORM_XCB_KHR
	.KHR_xcb_surface = true,
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
	.KHR_xlib_surface = true,
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
	.KHR_wayland_surface = true,
#endif
#endif
};

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
	*pApiVersion = VK_MAKE_API_VERSION(0, 1, 3, VK_HEADER_VERSION);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_EnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount,
					  VkExtensionProperties *pProperties)
{
	if (pLayerName)
		return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
	return vk_enumerate_instance_extension_properties(&instance_extensions, pPropertyCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
		    VkInstance *pInstance)
{
	if (!pAllocator)
		pAllocator = vk_default_allocator();
	struct wgvk_instance *instance = vk_zalloc(pAllocator, sizeof(*instance), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (!instance)
		return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

	struct vk_instance_dispatch_table dispatch_table;
	vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &wgvk_instance_entrypoints, true);
#ifdef WGVK_WSI
	vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &wsi_instance_entrypoints, false);
#endif
	VkResult result = vk_instance_init(&instance->vk, &instance_extensions, &dispatch_table, pCreateInfo, pAllocator);
	if (result != VK_SUCCESS) {
		vk_free(pAllocator, instance);
		return result;
	}
	instance->vk.physical_devices.enumerate = wgvk_enumerate_physical_devices;
	instance->vk.physical_devices.destroy = wgvk_physical_device_destroy;
	*pInstance = wgvk_instance_to_handle(instance);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroyInstance(VkInstance _instance, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_instance, instance, _instance);
	if (!instance)
		return;
	vk_instance_finish(&instance->vk);
	vk_free(&instance->vk.alloc, instance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
wgvk_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
	VK_FROM_HANDLE(wgvk_instance, instance, _instance);
	return vk_instance_get_proc_addr(instance ? &instance->vk : NULL, &wgvk_instance_entrypoints, pName);
}

PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	return wgvk_GetInstanceProcAddr(instance, pName);
}
