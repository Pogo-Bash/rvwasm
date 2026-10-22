/*
 * T7's native test of wgvk's presenting pieces, through the Vulkan loader (tests/wsi.test):
 *
 * 1. wgvk's own WSI (wgvk_wsi.c): a headless surface, a swapchain, frames acquired, cleared to a colour per
 *    frame and presented, then every swapchain image read back: each must hold the colour of the last frame
 *    drawn into it.
 * 2. What Mesa's venus in the guest asks the host before it offers a swapchain (vn_physical_device.c):
 *    memory exportable as a dma-buf, the LINEAR DRM modifier with colour attachment features for the window
 *    formats, an image made with that modifier and dedicated, exportable memory (the guest's WSI image), and
 *    binary semaphores that import and export sync files (an "already signalled" one, fd -1, imported).
 *
 * Prints "ok NAME: ..." / "FAIL NAME: ..." lines; exit status non-zero on a failure.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 64
#define H 48
#define FRAMES 9

static int failed;
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL wsi: %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static void
result(bool ok, const char *name, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	printf("%s %s: %s\n", ok ? "ok" : "FAIL", name, msg);
	if (!ok)
		failed = 1;
}

static uint32_t
memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return 0;
}

static const float colours[FRAMES][4] = {
	{ 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 0, 1 }, { 0, 1, 1, 1 },
	{ 1, 0, 1, 1 }, { 1, 1, 1, 1 }, { 0.5f, 0.25f, 0, 1 }, { 0, 0.5f, 0.25f, 1 },
};

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	const char *iexts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
	VkInstance inst;
	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &(VkApplicationInfo){ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
							  .apiVersion = VK_API_VERSION_1_1 },
		.enabledExtensionCount = 2, .ppEnabledExtensionNames = iexts }, NULL, &inst));
	uint32_t n = 1;
	VkPhysicalDevice pd;
	CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS);
	if (!n) {
		printf("FAIL wsi: no device\n");
		return 1;
	}

	/* ---- 2. what venus asks (before making anything) */
	{
		VkPhysicalDeviceExternalImageFormatInfo ei = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, .pNext = &ei,
			.drmFormatModifier = 0, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
		VkExternalImageFormatProperties ep = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
		VkImageFormatProperties2 ip = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ep };
		VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &(VkPhysicalDeviceImageFormatInfo2){
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &mi,
			.format = VK_FORMAT_B8G8R8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
			.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				 VK_IMAGE_USAGE_TRANSFER_DST_BIT }, &ip);
		result(r == VK_SUCCESS && (ep.externalMemoryProperties.externalMemoryFeatures &
					   VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT),
		       "venus-image-format", "B8G8R8A8 with the LINEAR modifier as a colour attachment, dma-buf: %d, "
		       "features 0x%x", r, ep.externalMemoryProperties.externalMemoryFeatures);

		VkDrmFormatModifierPropertiesEXT mods[4];
		VkDrmFormatModifierPropertiesListEXT ml = {
			.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT, .drmFormatModifierCount = 4,
			.pDrmFormatModifierProperties = mods };
		VkFormatProperties2 fp = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml };
		vkGetPhysicalDeviceFormatProperties2(pd, VK_FORMAT_B8G8R8A8_SRGB, &fp);
		result(ml.drmFormatModifierCount == 1 && mods[0].drmFormatModifier == 0 &&
		       (mods[0].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT),
		       "venus-modifiers", "B8G8R8A8_SRGB: %u modifier(s), LINEAR features 0x%x", ml.drmFormatModifierCount,
		       ml.drmFormatModifierCount ? mods[0].drmFormatModifierTilingFeatures : 0);

		VkExternalSemaphoreProperties sp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
		vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &(VkPhysicalDeviceExternalSemaphoreInfo){
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
			.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT }, &sp);
		VkExternalSemaphoreFeatureFlags want = VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT |
						       VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
		result((sp.externalSemaphoreFeatures & want) == want, "venus-sync-fd",
		       "binary semaphores and sync files: features 0x%x", sp.externalSemaphoreFeatures);
	}

	VkSurfaceKHR surf;
	PFN_vkCreateHeadlessSurfaceEXT create_headless =
		(PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(inst, "vkCreateHeadlessSurfaceEXT");
	if (!create_headless) {
		printf("FAIL wsi: no vkCreateHeadlessSurfaceEXT\n");
		return 1;
	}
	CHECK(create_headless(inst, &(VkHeadlessSurfaceCreateInfoEXT){
		.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT }, NULL, &surf));
	VkBool32 supported = 0;
	CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &supported));
	uint32_t nf = 0;
	CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
	VkSurfaceFormatKHR formats[32];
	nf = nf > 32 ? 32 : nf;
	CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, formats));
	VkFormat fmt = VK_FORMAT_UNDEFINED;
	for (uint32_t i = 0; i < nf; i++)
		if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM)
			fmt = formats[i].format;
	result(supported && fmt, "wsi-surface", "headless surface supported %u, %u formats, B8G8R8A8_UNORM %s",
	       supported, nf, fmt ? "among them" : "missing");
	if (!fmt)
		return 1;

	const char *dexts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
				VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
				VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
								 .queueCount = 1, .pQueuePriorities = (float[]){ 1 } },
		.enabledExtensionCount = 5, .ppEnabledExtensionNames = dexts }, NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);

	/* ---- 2b. a venus-style WSI image: LINEAR modifier, dedicated exportable memory; a sync fd import */
	{
		VkImage img;
		uint64_t mod = 0;
		VkResult r = vkCreateImage(dev, &(VkImageCreateInfo){
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.pNext = &(VkImageDrmFormatModifierListCreateInfoEXT){
				.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
				.pNext = &(VkExternalMemoryImageCreateInfo){
					.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
					.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT },
				.drmFormatModifierCount = 1, .pDrmFormatModifiers = &mod },
			.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = { W, H, 1 },
			.mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT },
			NULL, &img);
		VkMemoryRequirements mr = { 0 };
		VkDeviceMemory mem = VK_NULL_HANDLE;
		if (r == VK_SUCCESS) {
			vkGetImageMemoryRequirements(dev, img, &mr);
			r = vkAllocateMemory(dev, &(VkMemoryAllocateInfo){
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.pNext = &(VkMemoryDedicatedAllocateInfo){
					.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img,
					.pNext = &(VkExportMemoryAllocateInfo){
						.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
						.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT } },
				.allocationSize = mr.size,
				.memoryTypeIndex = memory_type(pd, mr.memoryTypeBits, 0) }, NULL, &mem);
		}
		if (r == VK_SUCCESS)
			r = vkBindImageMemory(dev, img, mem, 0);
		VkSubresourceLayout lay = { 0 };
		if (r == VK_SUCCESS)
			vkGetImageSubresourceLayout(dev, img, &(VkImageSubresource){
				VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 }, &lay);
		result(r == VK_SUCCESS && lay.rowPitch >= W * 4, "venus-wsi-image",
		       "LINEAR-modifier image with dedicated dma-buf memory: %d, %llu bytes, row pitch %llu", r,
		       (unsigned long long)mr.size, (unsigned long long)lay.rowPitch);
		if (mem)
			vkFreeMemory(dev, mem, NULL);
		if (r == VK_SUCCESS || img)
			vkDestroyImage(dev, img, NULL);

		VkSemaphore sem;
		CHECK(vkCreateSemaphore(dev, &(VkSemaphoreCreateInfo){ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO },
					NULL, &sem));
		PFN_vkImportSemaphoreFdKHR import_fd =
			(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR");
		r = import_fd ? import_fd(dev, &(VkImportSemaphoreFdInfoKHR){
			.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, .semaphore = sem,
			.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
			.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = -1 }) : VK_ERROR_EXTENSION_NOT_PRESENT;
		/* a submission that waits on it must go through */
		VkFence fence;
		CHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &fence));
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		if (r == VK_SUCCESS)
			r = vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
								.waitSemaphoreCount = 1, .pWaitSemaphores = &sem,
								.pWaitDstStageMask = &stage }, fence);
		if (r == VK_SUCCESS)
			r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
		result(r == VK_SUCCESS, "venus-sync-import", "a signalled sync file (fd -1) imported and waited on: %d", r);
		vkDestroyFence(dev, fence, NULL);
		vkDestroySemaphore(dev, sem, NULL);
	}

	/* ---- 1. the swapchain */
	VkSurfaceCapabilitiesKHR caps;
	CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
	VkSwapchainKHR sc;
	CHECK(vkCreateSwapchainKHR(dev, &(VkSwapchainCreateInfoKHR){
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf,
		.minImageCount = caps.minImageCount > 3 ? caps.minImageCount : 3, .imageFormat = fmt,
		.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = { W, H }, .imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE }, NULL, &sc));
	uint32_t ni = 0;
	CHECK(vkGetSwapchainImagesKHR(dev, sc, &ni, NULL));
	VkImage images[8];
	ni = ni > 8 ? 8 : ni;
	CHECK(vkGetSwapchainImagesKHR(dev, sc, &ni, images));

	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT }, NULL, &pool));
	VkCommandBuffer cbs[FRAMES];
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES }, cbs));
	VkSemaphore acquired[FRAMES], drawn[FRAMES];
	for (int i = 0; i < FRAMES; i++) {
		CHECK(vkCreateSemaphore(dev, &(VkSemaphoreCreateInfo){ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO },
					NULL, &acquired[i]));
		CHECK(vkCreateSemaphore(dev, &(VkSemaphoreCreateInfo){ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO },
					NULL, &drawn[i]));
	}
	int last[8];
	for (unsigned i = 0; i < 8; i++)
		last[i] = -1;
	int presented = 0;
	for (int f = 0; f < FRAMES; f++) {
		uint32_t idx;
		CHECK(vkAcquireNextImageKHR(dev, sc, 5000000000ull, acquired[f], VK_NULL_HANDLE, &idx));
		VkCommandBuffer cb = cbs[f];
		CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
				     &(VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .image = images[idx],
					.subresourceRange = range });
		VkClearColorValue cv;
		memcpy(cv.float32, colours[f], sizeof(cv.float32));
		vkCmdClearColorImage(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
				     &(VkImageMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .image = images[idx],
					.subresourceRange = range });
		CHECK(vkEndCommandBuffer(cb));
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &acquired[f], .pWaitDstStageMask = &stage,
			.commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1,
			.pSignalSemaphores = &drawn[f] }, VK_NULL_HANDLE));
		VkResult pr = vkQueuePresentKHR(q, &(VkPresentInfoKHR){ .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.waitSemaphoreCount = 1, .pWaitSemaphores = &drawn[f], .swapchainCount = 1,
			.pSwapchains = &sc, .pImageIndices = &idx });
		if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR)
			presented++;
		if (idx < 8)
			last[idx] = f;
	}
	CHECK(vkDeviceWaitIdle(dev));

	/* every image read back: the colour of the last frame drawn into it */
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT }, NULL, &buf));
	VkMemoryRequirements br;
	vkGetBufferMemoryRequirements(dev, buf, &br);
	VkDeviceMemory bmem;
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = br.size, .memoryTypeIndex = memory_type(pd, br.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) }, NULL, &bmem));
	CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
	int right = 0, drawnto = 0;
	char detail[256] = "";
	for (uint32_t i = 0; i < ni; i++) {
		if (last[i] < 0)
			continue;
		drawnto++;
		VkCommandBuffer cb = cbs[0];
		CHECK(vkResetCommandBuffer(cb, 0));
		CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
		vkCmdCopyImageToBuffer(cb, images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, buf, 1, &(VkBufferImageCopy){
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } });
		CHECK(vkEndCommandBuffer(cb));
		CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1, .pCommandBuffers = &cb }, VK_NULL_HANDLE));
		CHECK(vkQueueWaitIdle(q));
		uint8_t *p;
		CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&p));
		const float *c = colours[last[i]];
		/* B8G8R8A8: bytes B, G, R, A */
		uint8_t want[4] = { (uint8_t)(c[2] * 255 + 0.5f), (uint8_t)(c[1] * 255 + 0.5f), (uint8_t)(c[0] * 255 + 0.5f), 255 };
		const uint8_t *px = p + ((H / 2) * W + W / 2) * 4;
		bool ok = true;
		for (int k = 0; k < 4; k++)
			ok &= abs(px[k] - want[k]) <= 1;
		right += ok;
		snprintf(detail + strlen(detail), sizeof(detail) - strlen(detail), " [%u: frame %d %u,%u,%u]", i, last[i],
			 px[2], px[1], px[0]);
		vkUnmapMemory(dev, bmem);
	}
	result(presented == FRAMES && right == drawnto && drawnto > 1, "wsi-swapchain",
	       "%u images, %d of %d frames presented, %d of %d images hold their last frame's colour (R,G,B):%s",
	       ni, presented, FRAMES, right, drawnto, detail);

	vkDestroyBuffer(dev, buf, NULL);
	vkFreeMemory(dev, bmem, NULL);
	for (int i = 0; i < FRAMES; i++) {
		vkDestroySemaphore(dev, acquired[i], NULL);
		vkDestroySemaphore(dev, drawn[i], NULL);
	}
	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroySwapchainKHR(dev, sc, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroySurfaceKHR(inst, surf, NULL);
	vkDestroyInstance(inst, NULL);
	return failed;
}
