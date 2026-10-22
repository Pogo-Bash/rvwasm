/*
 * wgvk: the device and its queue.
 *
 * Memory and buffers are wgvk_memory.c, images and views wgvk_image.c,
 * samplers wgvk_sampler.c, layouts wgvk_descriptor.c, sets and bind groups
 * wgvk_descriptor_set.c, syncs and the queue's serials wgvk_sync.c, queries
 * wgvk_query.c.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"

#include "vk_cmd_enqueue_entrypoints.h"

/* ---- device */

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
		  const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	VK_FROM_HANDLE(wgvk_physical_device, pdev, physicalDevice);
	struct wgvk_device *dev = vk_zalloc2(&pdev->vk.instance->alloc, pAllocator, sizeof(*dev), 8,
					     VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (!dev)
		return vk_error(pdev, VK_ERROR_OUT_OF_HOST_MEMORY);

	struct vk_device_dispatch_table dt;
	wgvk_record_dispatch(&dt);	/* (T2) wgvk_cmd.c */
#ifdef WGVK_WSI
	vk_device_dispatch_table_from_entrypoints(&dt, &wsi_device_entrypoints, false);
#endif
	VkResult r = vk_device_init(&dev->vk, &pdev->vk, &dt, pCreateInfo, pAllocator);
	if (r != VK_SUCCESS) {
		vk_free(&dev->vk.alloc, dev);
		return r;
	}
	/* vkCmdExecuteCommands is Mesa's (vk_common_CmdExecuteCommands): it replays each secondary's recorded queue into
	 * the primary through command_dispatch_table, which vk_device_init leaves NULL. The recording table inlines them
	 * (the secondary's commands are recorded into the primary as if the program had called them there). Without it,
	 * the first secondary read a "table" at address 0 and the ring drain died on a call_indirect signature mismatch
	 * (Qt Quick on QSG_RHI_BACKEND=vulkan: Qt's RHI draws through secondaries; VK-27). */
	dev->vk.command_dispatch_table = &dev->vk.dispatch_table;
	/* No submit thread: every WebGPU call on one thread (INTERFACES.md 2). */
	dev->vk.submit_mode = VK_QUEUE_SUBMIT_MODE_IMMEDIATE;
	dev->vk.command_buffer_ops = &wgvk_cmd_buffer_ops;
	dev->vk.shader_ops = &wgvk_device_shader_ops;	/* (T2) wgvk_shader.c: Mesa's common pipelines on WGSL */
	dev->pdev = pdev;
	dev->winst = pdev->winst;
	dev->wdev = pdev->wdev;
	dev->wqueue = wgpuDeviceGetQueue(dev->wdev);
	wgvk_caches_init(dev);
	wgvk_device_init_t2(dev);

	const VkDeviceQueueCreateInfo dummy = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
		.pQueuePriorities = (const float[]){ 1.0f },
	};
	r = vk_queue_init(&dev->queue.vk, &dev->vk,
			  pCreateInfo->queueCreateInfoCount ? &pCreateInfo->pQueueCreateInfos[0] : &dummy, 0);
	if (r != VK_SUCCESS) {
		vk_device_finish(&dev->vk);
		vk_free(&dev->vk.alloc, dev);
		return r;
	}
	dev->queue.vk.driver_submit = wgvk_queue_submit;
	*pDevice = wgvk_device_to_handle(dev);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	if (!dev)
		return;
	wgvk_device_finish_t2(dev);	/* waits for the queue */
	vk_queue_finish(&dev->queue.vk);
	wgvk_caches_finish(dev);
	wgvk_present_finish(dev);
	wgpuQueueRelease(dev->wqueue);
	vk_device_finish(&dev->vk);
	vk_free(&dev->vk.alloc, dev);
}

