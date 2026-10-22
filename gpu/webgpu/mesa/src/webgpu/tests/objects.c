/*
 * T1's object tests: wgvk's objects made and used through the Vulkan API
 * (natively through the loader, wgvk as the ICD; gpu/webgpu/tests/objects.test).
 * Each part prints "ok objects-PART: ..." or "FAIL objects-PART: ...".
 *
 *   objects [PART...]     parts: layouts formats images samplers memory descriptors
 *
 * The remap's numbers are checked by the .test script from WGVK_DEBUG=remap.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static int failed;

#define FAIL(part, ...) do { printf("FAIL objects-%s: ", part); printf(__VA_ARGS__); printf("\n"); failed++; return; } while (0)
#define CHECK(part, x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) FAIL(part, "%s: %d (line %d)", #x, r_, __LINE__); } while (0)

static int
setup(void)
{
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	if (vkCreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app }, NULL, &inst) != VK_SUCCESS)
		return -1;
	uint32_t n = 1;
	if (vkEnumeratePhysicalDevices(inst, &n, &phys) < 0 || n < 1)
		return -1;
	if (vkCreateDevice(phys, &(VkDeviceCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
			.pQueuePriorities = (float[]){ 1 } } }, NULL, &dev) != VK_SUCCESS)
		return -1;
	vkGetDeviceQueue(dev, 0, 0, &queue);
	return 0;
}

/* Set layouts given out of binding order, a pipeline layout over them; the
 * remap (WGVK_DEBUG=remap) is compared by objects.test. */
static void
layouts(void)
{
	const char *P = "layouts";
	VkDescriptorSetLayout sl[2];
	VkDescriptorSetLayoutBinding b0[] = {
		{ 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 5, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
		{ 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
	};
	VkDescriptorSetLayoutBinding b1[] = {
		{ 2, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 64, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
	};
	CHECK(P, vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b0 },
		NULL, &sl[0]));
	CHECK(P, vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b1 },
		NULL, &sl[1]));
	VkPipelineLayout pl;
	VkPushConstantRange pc[] = { { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 }, { VK_SHADER_STAGE_FRAGMENT_BIT, 16, 112 } };
	CHECK(P, vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = sl,
		.pushConstantRangeCount = 2, .pPushConstantRanges = pc }, NULL, &pl));
	/* the set layouts may go before the pipeline layout (it holds references) */
	vkDestroyDescriptorSetLayout(dev, sl[0], NULL);
	vkDestroyDescriptorSetLayout(dev, sl[1], NULL);
	vkDestroyPipelineLayout(dev, pl, NULL);

	VkDescriptorSetLayoutSupport sup = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT };
	VkDescriptorSetLayoutBinding big = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 600, VK_SHADER_STAGE_ALL, NULL };
	vkGetDescriptorSetLayoutSupport(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &big }, &sup);
	if (sup.supported)
		FAIL(P, "a set of 1200 WebGPU bindings is reported supported");
	printf("ok objects-%s: two set layouts and a pipeline layout made and destroyed; 600 combined samplers refused\n", P);
}

/* Format features as the format table should give them on Dawn native (lavapipe: core features, tier1+2,
 * BC, no ETC2/ASTC, float32-filterable). */
static void
formats(void)
{
	const char *P = "formats";
	static const struct {
		VkFormat f;
		VkFormatFeatureFlags optimal_has, optimal_not, buffer_has;
	} want[] = {
		{ VK_FORMAT_R8G8B8A8_UNORM,
		  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
		  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
		  VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
		  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT,
		  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
		  VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT | VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT },
		{ VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, 0 },
		{ VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
		{ VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
		  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, 0 },
		{ VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, 0 },
		{ VK_FORMAT_D16_UNORM, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, 0 },
		{ VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, 0 },
		{ VK_FORMAT_D32_SFLOAT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, 0 },
		{ VK_FORMAT_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, 0 },
		{ VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT,
		  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT },
		{ VK_FORMAT_R32G32B32_SFLOAT, 0, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
		{ VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
		  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, 0 },
		{ VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
		  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, 0 },
		{ VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT, 0, 0 },
		{ VK_FORMAT_R5G6B5_UNORM_PACK16, 0, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, 0 },
	};
	unsigned n = 0;
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		VkFormatProperties p;
		vkGetPhysicalDeviceFormatProperties(phys, want[i].f, &p);
		if ((p.optimalTilingFeatures & want[i].optimal_has) != want[i].optimal_has)
			FAIL(P, "format %d: optimal features %#x lack %#x", want[i].f, p.optimalTilingFeatures,
			     want[i].optimal_has & ~p.optimalTilingFeatures);
		if (p.optimalTilingFeatures & want[i].optimal_not)
			FAIL(P, "format %d: optimal features %#x have %#x", want[i].f, p.optimalTilingFeatures,
			     p.optimalTilingFeatures & want[i].optimal_not);
		if ((p.bufferFeatures & want[i].buffer_has) != want[i].buffer_has)
			FAIL(P, "format %d: buffer features %#x lack %#x", want[i].f, p.bufferFeatures,
			     want[i].buffer_has & ~p.bufferFeatures);
		n++;
	}
	/* zink needs one of these depth/stencil formats */
	VkImageFormatProperties ip;
	CHECK(P, vkGetPhysicalDeviceImageFormatProperties(phys, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_TYPE_2D,
		VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &ip));
	if (!(ip.sampleCounts & VK_SAMPLE_COUNT_4_BIT))
		FAIL(P, "D32S8: no 4 samples");
	if (vkGetPhysicalDeviceImageFormatProperties(phys, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
		VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &ip) != VK_ERROR_FORMAT_NOT_SUPPORTED)
		FAIL(P, "linear colour attachments reported supported");
	CHECK(P, vkGetPhysicalDeviceImageFormatProperties(phys, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D,
		VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &ip));
	if (ip.maxArrayLayers != 1 || ip.maxExtent.depth < 256)
		FAIL(P, "3D image properties: %u layers, depth %u", ip.maxArrayLayers, ip.maxExtent.depth);
	printf("ok objects-%s: %u formats have the features WebGPU's capability tables give them; image format "
	       "properties for D32S8, linear and 3D\n", P, n);
}

static int mem_type(VkMemoryPropertyFlags want, uint32_t bits)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return -1;
}

/* Images of every type with views for every use, bound and destroyed. */
static void
images(void)
{
	const char *P = "images";
	static const struct {
		VkImageType type;
		VkFormat f;
		VkExtent3D ext;
		uint32_t levels, layers;
		VkSampleCountFlagBits samples;
		VkImageUsageFlags usage;
		VkImageCreateFlags flags;
		VkImageViewType view;
	} cases[] = {
		{ VK_IMAGE_TYPE_1D, VK_FORMAT_R8G8B8A8_UNORM, { 64, 1, 1 }, 7, 4, 1,
		  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0,
		  VK_IMAGE_VIEW_TYPE_1D_ARRAY },
		{ VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_SRGB, { 33, 17, 1 }, 6, 1, 1,
		  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
		  VK_IMAGE_VIEW_TYPE_2D },
		{ VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, { 64, 64, 1 }, 1, 12, 1,
		  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		  VK_IMAGE_VIEW_TYPE_CUBE_ARRAY },
		{ VK_IMAGE_TYPE_3D, VK_FORMAT_R32_SFLOAT, { 16, 16, 16 }, 5, 1, 1,
		  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		  VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, VK_IMAGE_VIEW_TYPE_3D },
		{ VK_IMAGE_TYPE_2D, VK_FORMAT_D24_UNORM_S8_UINT, { 128, 64, 1 }, 1, 1, 1,
		  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, VK_IMAGE_VIEW_TYPE_2D },
		{ VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT_S8_UINT, { 128, 64, 1 }, 1, 1, 4,
		  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, VK_IMAGE_VIEW_TYPE_2D },
		{ VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 256, 256, 1 }, 1, 1, 4,
		  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, VK_IMAGE_VIEW_TYPE_2D },
		{ VK_IMAGE_TYPE_2D, VK_FORMAT_BC1_RGB_UNORM_BLOCK, { 64, 64, 1 }, 7, 2, 1,
		  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, VK_IMAGE_VIEW_TYPE_2D_ARRAY },
	};
	unsigned n = 0;
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		VkImage img;
		CHECK(P, vkCreateImage(dev, &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.flags = cases[i].flags, .imageType = cases[i].type, .format = cases[i].f, .extent = cases[i].ext,
			.mipLevels = cases[i].levels, .arrayLayers = cases[i].layers, .samples = cases[i].samples,
			.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = cases[i].usage }, NULL, &img));
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, img, &req);
		int t = mem_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.memoryTypeBits);
		if (t < 0 || !req.size)
			FAIL(P, "case %u: memory requirements %llu bytes, types %#x", i, (unsigned long long)req.size,
			     req.memoryTypeBits);
		VkDeviceMemory mem;
		CHECK(P, vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = req.size, .memoryTypeIndex = t }, NULL, &mem));
		CHECK(P, vkBindImageMemory(dev, img, mem, 0));
		VkImageAspectFlags aspect = cases[i].f == VK_FORMAT_D24_UNORM_S8_UINT || cases[i].f == VK_FORMAT_D32_SFLOAT_S8_UINT
			? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		VkImageView v;
		CHECK(P, vkCreateImageView(dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = img, .viewType = cases[i].view, .format = cases[i].f,
			.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE },
			.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } }, NULL, &v));
		vkDestroyImageView(dev, v, NULL);
		if (cases[i].flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
			CHECK(P, vkCreateImageView(dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
				.subresourceRange = { aspect, 1, 1, 0, 1 } }, NULL, &v));
			vkDestroyImageView(dev, v, NULL);
		}
		if (cases[i].type == VK_IMAGE_TYPE_3D) {	/* a slice as an attachment */
			CHECK(P, vkCreateImageView(dev, &(VkImageViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.pNext = &(VkImageViewUsageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
					.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT },
				.image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = cases[i].f,
				.subresourceRange = { aspect, 1, 1, 3, 1 } }, NULL, &v));
			vkDestroyImageView(dev, v, NULL);
		}
		vkDestroyImage(dev, img, NULL);
		vkFreeMemory(dev, mem, NULL);
		n++;
	}
	/* a linear image: rows of 256 bytes in host-visible memory */
	VkImage lin;
	CHECK(P, vkCreateImage(dev, &(VkImageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { 50, 30, 1 }, .mipLevels = 1,
		.arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_LINEAR,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT }, NULL, &lin));
	VkSubresourceLayout sl;
	vkGetImageSubresourceLayout(dev, lin, &(VkImageSubresource){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, &sl);
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, lin, &req);
	if (sl.rowPitch != 256 || req.size < 256 * 30 || mem_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits) < 0)
		FAIL(P, "linear 50x30: row pitch %llu, size %llu, types %#x", (unsigned long long)sl.rowPitch,
		     (unsigned long long)req.size, req.memoryTypeBits);
	vkDestroyImage(dev, lin, NULL);
	printf("ok objects-%s: %u images (1D array, 2D mutable sRGB, cube array, 3D + slice attachment, D24S8, "
	       "D32S8 x4, RGBA8 x4, BC1 RGB) with views; linear layout pitch 256\n", P, n);
}

static void
samplers(void)
{
	const char *P = "samplers";
	VkSamplerCreateInfo ci[] = {
		{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
		  .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .anisotropyEnable = 1, .maxAnisotropy = 16, .maxLod = VK_LOD_CLAMP_NONE },
		{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		  .compareEnable = 1, .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL, .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
		  .minLod = 2, .maxLod = 1, .mipLodBias = 0.5f },
		{ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .unnormalizedCoordinates = 1,
		  .addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE },
	};
	for (unsigned i = 0; i < 3; i++) {
		VkSampler s;
		CHECK(P, vkCreateSampler(dev, &ci[i], NULL, &s));
		vkDestroySampler(dev, s, NULL);
	}
	printf("ok objects-%s: anisotropic, comparison + border + bias, unnormalized + mirror-clamp\n", P);
}

/* Host-visible memory: map, write, a buffer in it, bigger than one page. */
static void
memory(void)
{
	const char *P = "memory";
	VkBuffer b;
	CHECK(P, vkCreateBuffer(dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1000,
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT }, NULL, &b));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b, &req);
	int host = mem_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, req.memoryTypeBits);
	int local = mem_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.memoryTypeBits);
	if (host < 0 || local < 0 || req.alignment < 4 || req.size < 1000)
		FAIL(P, "buffer requirements: size %llu align %llu types %#x", (unsigned long long)req.size,
		     (unsigned long long)req.alignment, req.memoryTypeBits);
	VkDeviceMemory m;
	CHECK(P, vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = 1 << 20, .memoryTypeIndex = host }, NULL, &m));
	uint32_t *p;
	CHECK(P, vkMapMemory(dev, m, 4096, VK_WHOLE_SIZE, 0, (void **)&p));
	for (unsigned i = 0; i < 1000; i++)
		p[i] = i;
	CHECK(P, vkBindBufferMemory(dev, b, m, 4096));
	VkBufferView bv;
	CHECK(P, vkCreateBufferView(dev, &(VkBufferViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
		.buffer = b, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0, .range = VK_WHOLE_SIZE }, NULL, &bv));
	vkDestroyBufferView(dev, bv, NULL);
	vkUnmapMemory(dev, m);
	vkDestroyBuffer(dev, b, NULL);
	vkFreeMemory(dev, m, NULL);
	VkDeviceMemory big;
	VkResult r = vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = 64 << 20, .memoryTypeIndex = local }, NULL, &big);
	if (r == VK_SUCCESS)
		vkFreeMemory(dev, big, NULL);
	printf("ok objects-%s: host-visible memory mapped and written, a buffer and a texel view in it; 64 MiB device-local: %d\n",
	       P, r);
}

/* Pools, sets, writes running on into the next binding, copies, templates, reset. */
static void
descriptors(void)
{
	const char *P = "descriptors";
	VkDescriptorSetLayoutBinding b[] = {
		{ 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, VK_SHADER_STAGE_ALL, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL },
	};
	VkDescriptorSetLayout sl;
	CHECK(P, vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b }, NULL, &sl));
	VkDescriptorPool pool;
	CHECK(P, vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 8, .poolSizeCount = 1,
		.pPoolSizes = &(VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 24 } }, NULL, &pool));
	VkDescriptorSet sets[2];
	CHECK(P, vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pool, .descriptorSetCount = 2, .pSetLayouts = (VkDescriptorSetLayout[]){ sl, sl } }, sets));
	VkBuffer buf;
	CHECK(P, vkCreateBuffer(dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT }, NULL, &buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, buf, &req);
	VkDeviceMemory mem;
	CHECK(P, vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size, .memoryTypeIndex = mem_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.memoryTypeBits) },
		NULL, &mem));
	CHECK(P, vkBindBufferMemory(dev, buf, mem, 0));
	VkBufferView bv;
	CHECK(P, vkCreateBufferView(dev, &(VkBufferViewCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
		.buffer = buf, .format = VK_FORMAT_R8G8B8A8_UNORM, .range = VK_WHOLE_SIZE }, NULL, &bv));
	/* three UBOs from binding 0 element 0: runs on into binding 1 */
	VkDescriptorBufferInfo bi[3] = { { buf, 0, 256 }, { buf, 256, 256 }, { buf, 512, VK_WHOLE_SIZE } };
	vkUpdateDescriptorSets(dev, 2, (VkWriteDescriptorSet[]){
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 0, .descriptorCount = 3,
		  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = bi },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 2, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &bv } }, 1,
		&(VkCopyDescriptorSet){ .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = sets[0], .srcBinding = 0,
			.srcArrayElement = 1, .dstSet = sets[1], .dstBinding = 0, .descriptorCount = 2 });
	/* a template writing binding 1 and 2 */
	struct { VkDescriptorBufferInfo ub; VkBufferView tb; } data = { { buf, 1024, 64 }, bv };
	VkDescriptorUpdateTemplate t;
	VkDescriptorUpdateTemplateEntry te[] = {
		{ 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, sizeof(data) },
		{ 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, sizeof(VkDescriptorBufferInfo), sizeof(data) },
	};
	CHECK(P, vkCreateDescriptorUpdateTemplate(dev, &(VkDescriptorUpdateTemplateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, .descriptorUpdateEntryCount = 2,
		.pDescriptorUpdateEntries = te, .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
		.descriptorSetLayout = sl }, NULL, &t));
	vkUpdateDescriptorSetWithTemplate(dev, sets[1], t, &data);
	vkDestroyDescriptorUpdateTemplate(dev, t, NULL);
	CHECK(P, vkFreeDescriptorSets(dev, pool, 1, &sets[0]));
	CHECK(P, vkResetDescriptorPool(dev, pool, 0));
	vkDestroyDescriptorPool(dev, pool, NULL);
	vkDestroyDescriptorSetLayout(dev, sl, NULL);
	vkDestroyBufferView(dev, bv, NULL);
	vkDestroyBuffer(dev, buf, NULL);
	vkFreeMemory(dev, mem, NULL);
	printf("ok objects-%s: pool, sets, a write running on into the next binding, a copy, a template, free, reset\n", P);
}

/* Query pools of every kind made, host-reset, read back as not ready. */
static void
queries(void)
{
	const char *P = "queries";
	VkQueryType types[] = { VK_QUERY_TYPE_OCCLUSION, VK_QUERY_TYPE_TIMESTAMP, VK_QUERY_TYPE_PIPELINE_STATISTICS };
	for (unsigned i = 0; i < 3; i++) {
		VkQueryPool qp;
		CHECK(P, vkCreateQueryPool(dev, &(VkQueryPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = types[i], .queryCount = 16,
			.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
					      VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT }, NULL, &qp));
		PFN_vkResetQueryPool reset = (PFN_vkResetQueryPool)vkGetDeviceProcAddr(dev, "vkResetQueryPoolEXT");
		if (reset)
			reset(dev, qp, 0, 16);
		uint64_t r[16 * 3];
		VkResult res = vkGetQueryPoolResults(dev, qp, 0, 4, sizeof(r), r, 24,
						     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		if (res != VK_NOT_READY || r[i == 2 ? 2 : 1] != 0)
			FAIL(P, "type %d: %d, availability %llu", types[i], res, (unsigned long long)r[i == 2 ? 2 : 1]);
		vkDestroyQueryPool(dev, qp, NULL);
	}
	printf("ok objects-%s: occlusion, timestamp, pipeline statistics pools made, reset, not ready\n", P);
}

int
main(int argc, char **argv)
{
	if (setup()) {
		printf("FAIL objects: no wgvk device\n");
		return 1;
	}
	static const struct { const char *name; void (*fn)(void); } parts[] = {
		{ "layouts", layouts },
		{ "formats", formats },
		{ "images", images },
		{ "samplers", samplers },
		{ "memory", memory },
		{ "descriptors", descriptors },
		{ "queries", queries },
	};
	for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		int want = argc < 2;
		for (int a = 1; a < argc; a++)
			want |= !strcmp(argv[a], parts[i].name);
		if (want)
			parts[i].fn();
	}
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	return failed ? 1 : 0;
}
