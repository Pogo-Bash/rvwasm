/*
 * The clear smoke test: through wgvk's Vulkan, clear an image with
 * vkCmdClearColorImage, copy it into a host-visible buffer, wait on a fence,
 * map and check every pixel. Two sizes: 64x64 (rows of 256 bytes, WebGPU's
 * copy alignment) and 50x30 (rows of 200 bytes: the copy goes through a
 * temporary buffer row by row).
 *
 * The same source runs natively (main(): through the Vulkan loader with wgvk
 * as an ICD; gpu/webgpu/tests/smoke-clear.test) and in the page (rvgpu.wasm
 * calls smoke_clear() with wgvk_GetInstanceProcAddr and keeps the 64x64 image
 * to show it; tools/www/rvgpu-smoke.html).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "smoke-clear.h"

#define LOAD(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	snprintf(s->msg, sizeof(s->msg), "%s failed: %d (line %d)", #x, r_, __LINE__); return -1; } } while (0)

static int
one(struct smoke *s, PFN_vkGetInstanceProcAddr gipa, uint32_t w, uint32_t h, const float rgba[4], VkImage *keep)
{
	VkInstance inst = s->instance;
	VkDevice dev = s->device;
	LOAD(inst, vkGetDeviceProcAddr);
	LOAD(inst, vkGetPhysicalDeviceMemoryProperties);
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
	D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
	D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkCreateCommandPool);
	D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkCmdPipelineBarrier);
	D(vkCmdClearColorImage); D(vkCmdCopyImageToBuffer); D(vkCreateFence); D(vkQueueSubmit);
	D(vkWaitForFences); D(vkMapMemory); D(vkUnmapMemory); D(vkDestroyFence); D(vkDestroyCommandPool);
	D(vkDestroyBuffer); D(vkFreeMemory); D(vkDestroyImage);

	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(s->physical, &mp);
	int host_type = -1, dev_type = -1;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if (host_type < 0 && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			host_type = i;
		if (dev_type < 0 && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			dev_type = i;
	}

	VkImage img;
	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED }, NULL, &img));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, img, &req);
	VkDeviceMemory imem;
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size, .memoryTypeIndex = dev_type }, NULL, &imem));
	CHECK(vkBindImageMemory(dev, img, imem, 0));

	VkDeviceSize size = (VkDeviceSize)w * h * 4;
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT }, NULL, &buf));
	vkGetBufferMemoryRequirements(dev, buf, &req);
	VkDeviceMemory bmem;
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size, .memoryTypeIndex = host_type }, NULL, &bmem));
	CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
	uint8_t *map;
	CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
	memset(map, 0xee, size);

	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = 0 }, NULL, &pool));
	VkCommandBuffer cmd;
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cmd));
	CHECK(vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
		&(VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .image = img, .subresourceRange = range });
	VkClearColorValue color = { .float32 = { rgba[0], rgba[1], rgba[2], rgba[3] } };
	vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
		&(VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.image = img, .subresourceRange = range });
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &(VkBufferImageCopy){
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } });
	CHECK(vkEndCommandBuffer(cmd));
	VkFence fence;
	CHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &fence));
	CHECK(vkQueueSubmit(s->queue, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cmd }, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));

	uint8_t want[4];
	for (int c = 0; c < 4; c++)
		want[c] = (uint8_t)(rgba[c] * 255.0f + 0.5f);
	uint32_t bad = 0, first_bad = 0;
	for (uint32_t i = 0; i < w * h; i++) {
		int off = 0;
		for (int c = 0; c < 4; c++)
			off |= abs((int)map[i * 4 + c] - (int)want[c]) > 1;
		if (off && !bad++)
			first_bad = i;
	}
	snprintf(s->msg, sizeof(s->msg), "%ux%u: pixel 0 = %u %u %u %u (want %u %u %u %u), %u of %u pixels wrong%s",
		 w, h, map[0], map[1], map[2], map[3], want[0], want[1], want[2], want[3], bad, w * h,
		 bad ? " (first at index shown below)" : "");
	if (bad)
		snprintf(s->msg + strlen(s->msg), sizeof(s->msg) - strlen(s->msg), " [%u: %u %u %u %u]", first_bad,
			 map[first_bad * 4], map[first_bad * 4 + 1], map[first_bad * 4 + 2], map[first_bad * 4 + 3]);
	vkUnmapMemory(dev, bmem);
	vkDestroyFence(dev, fence, NULL);
	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroyBuffer(dev, buf, NULL);
	vkFreeMemory(dev, bmem, NULL);
	if (keep) {
		*keep = img;	/* (its memory stays allocated with it) */
	} else {
		vkDestroyImage(dev, img, NULL);
		vkFreeMemory(dev, imem, NULL);
	}
	return bad ? 1 : 0;
}

int
smoke_clear(struct smoke *s, PFN_vkGetInstanceProcAddr gipa, const float rgba[4], const char *device_prefix)
{
	memset(s, 0, sizeof(*s));
	LOAD(NULL, vkCreateInstance);
	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &(VkApplicationInfo){ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName = "wgvk smoke-clear", .apiVersion = VK_API_VERSION_1_1 } }, NULL, &s->instance));
	LOAD(s->instance, vkEnumeratePhysicalDevices);
	LOAD(s->instance, vkGetPhysicalDeviceProperties);
	LOAD(s->instance, vkCreateDevice);
	LOAD(s->instance, vkGetDeviceProcAddr);
	VkPhysicalDevice pds[8];
	uint32_t n = 8;
	CHECK(vkEnumeratePhysicalDevices(s->instance, &n, pds));
	for (uint32_t i = 0; i < n; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(pds[i], &p);
		if (!device_prefix || !strncmp(p.deviceName, device_prefix, strlen(device_prefix))) {
			s->physical = pds[i];
			snprintf(s->device_name, sizeof(s->device_name), "%s", p.deviceName);
			break;
		}
	}
	if (!s->physical) {
		snprintf(s->msg, sizeof(s->msg), "no physical device named %s* among %u", device_prefix, n);
		return -1;
	}
	CHECK(vkCreateDevice(s->physical, &(VkDeviceCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
			.pQueuePriorities = (float[]){ 1.0f } } }, NULL, &s->device));
	PFN_vkGetDeviceQueue vkGetDeviceQueue = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(s->device, "vkGetDeviceQueue");
	vkGetDeviceQueue(s->device, 0, 0, &s->queue);

	int r = one(s, gipa, 50, 30, rgba, NULL);
	if (r)
		return r;
	char first[256];
	snprintf(first, sizeof(first), "%s", s->msg);
	r = one(s, gipa, 64, 64, rgba, &s->image);
	char second[256];
	snprintf(second, sizeof(second), "%s", s->msg);
	snprintf(s->msg, sizeof(s->msg), "%s; %s", first, second);
	return r;
}

#ifndef SMOKE_NO_MAIN
int
main(int argc, char **argv)
{
	struct smoke s;
	const float rgba[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	int r = smoke_clear(&s, vkGetInstanceProcAddr, rgba, argc > 1 ? argv[1] : "wgvk");
	printf("%s smoke-clear: %s: %s\n", r ? "FAIL" : "ok", s.device_name, s.msg);
	return r ? 1 : 0;
}
#endif
