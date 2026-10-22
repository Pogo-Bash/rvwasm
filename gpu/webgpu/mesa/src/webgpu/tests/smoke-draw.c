/*
 * The draw smoke test (T2): through wgvk's Vulkan, render a small scene into
 * a 64x64 image and check pixels well inside each shape, then run a compute
 * shader and check every word. Self-checking, so the same source runs
 * natively (main(), through the Vulkan loader: gpu/webgpu/tests/t2.test) and
 * in the page (rvgpu.wasm's rvgpu_smoke_draw, tools/www/rvgpu-smoke.html
 * ?test=draw), where the GPU under WebGPU is not lavapipe and only pixels far
 * from edges can be compared exactly.
 *
 * The scene (dynamic rendering, depth test LESS, push constants, a vertex
 * buffer with a per-instance binding, an index buffer):
 *   A  red quad,    x 4..28,  y 4..28,  z 0.5, drawn second
 *   B  green quad,  x 16..40, y 16..40, z 0.3, drawn first: wins the overlap only by depth
 *   C  textured quad x 36..60, y 4..28: a 2x2 texture (red green / blue white), nearest
 *   D  three instances of a triangle at y 44..60, white tinted blue by a uniform buffer
 * and the compute: out[i] = in[i]^2 + k over 256 words, in/out/k in one memory.
 *
 * SPDX-License-Identifier: MIT
 */
/* the entry points are pointers of this file's own (through gipa: no loader in the page) */
#define VK_NO_PROTOTYPES
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "smoke-clear.h"
#include "t2/spv/color.frag.h"
#include "t2/spv/square.comp.h"
#include "t2/spv/tex.frag.h"
#include "t2/spv/tri.vert.h"
#include "t2/spv/ubo.frag.h"

#define S 64

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	snprintf(s->msg, sizeof(s->msg), "%s failed: %d (line %d)", #x, r_, __LINE__); return -1; } } while (0)

/* every entry point through gipa / vkGetDeviceProcAddr, as in the page there is no loader */
#define FNS(X) \
	X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory) \
	X(vkCreateImageView) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
	X(vkMapMemory) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
	X(vkCreateFence) X(vkQueueSubmit) X(vkWaitForFences) X(vkResetFences) X(vkCreateShaderModule) \
	X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
	X(vkCreateComputePipelines) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) \
	X(vkUpdateDescriptorSets) X(vkCreateSampler) X(vkCmdBeginRenderingKHR) X(vkCmdEndRenderingKHR) \
	X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdBindVertexBuffers) \
	X(vkCmdBindIndexBuffer) X(vkCmdPushConstants) X(vkCmdBindDescriptorSets) X(vkCmdDraw) \
	X(vkCmdDrawIndexed) X(vkCmdDispatch) X(vkGetDeviceQueue)
#define DECL(n) static PFN_##n n;
FNS(DECL)

static uint32_t host_type, dev_type;

static VkDeviceMemory
mem_for(struct smoke *s, VkMemoryRequirements req, uint32_t type)
{
	VkDeviceMemory m = VK_NULL_HANDLE;
	vkAllocateMemory(s->device, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
							      .allocationSize = req.size, .memoryTypeIndex = type }, NULL, &m);
	return m;
}

static VkBuffer
host_buffer(struct smoke *s, VkDeviceSize size, void **map)
{
	VkBuffer b;
	vkCreateBuffer(s->device, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
			 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT }, NULL, &b);
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(s->device, b, &req);
	VkDeviceMemory m = mem_for(s, req, host_type);
	vkBindBufferMemory(s->device, b, m, 0);
	vkMapMemory(s->device, m, 0, VK_WHOLE_SIZE, 0, map);
	return b;
}

static VkImage
image(struct smoke *s, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkImageAspectFlags aspect,
      VkImageView *view)
{
	VkImage i;
	vkCreateImage(s->device, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
		.extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage }, NULL, &i);
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s->device, i, &req);
	vkBindImageMemory(s->device, i, mem_for(s, req, dev_type), 0);
	vkCreateImageView(s->device, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = f, .subresourceRange = { aspect, 0, 1, 0, 1 } }, NULL, view);
	return i;
}

static VkShaderModule
module(struct smoke *s, const uint32_t *code, size_t size)
{
	VkShaderModule m;
	vkCreateShaderModule(s->device, &(VkShaderModuleCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code }, NULL, &m);
	return m;
}

static void
layout_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
			     NULL, 1, &(VkImageMemoryBarrier){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from,
		.newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = { aspect, 0, 1, 0, 1 } });
}

/* a pixel edge in NDC */
static float
ndc(float px)
{
	return px / (S / 2) - 1.0f;
}

/* quad as 4 vertices {x, y, r, g, b, a} */
static void
quad(float *v, float x0, float y0, float x1, float y1, const float c[4], bool uv)
{
	const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
	const float us[4] = { 0, 1, 1, 0 }, vs[4] = { 0, 0, 1, 1 };
	for (int i = 0; i < 4; i++) {
		v[6 * i + 0] = ndc(xs[i]);
		v[6 * i + 1] = ndc(ys[i]);
		v[6 * i + 2] = uv ? us[i] : c[0];
		v[6 * i + 3] = uv ? vs[i] : c[1];
		v[6 * i + 4] = uv ? 0 : c[2];
		v[6 * i + 5] = 1;
	}
}

struct pc {
	float off[2], scale, z;
};

int smoke_draw(struct smoke *s, PFN_vkGetInstanceProcAddr gipa, const char *device_prefix);

int
smoke_draw(struct smoke *s, PFN_vkGetInstanceProcAddr gipa, const char *device_prefix)
{
	memset(s, 0, sizeof(*s));
	PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	CHECK(CreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
						      .pApplicationInfo = &app }, NULL, &s->instance));
	PFN_vkEnumeratePhysicalDevices Enum = (PFN_vkEnumeratePhysicalDevices)gipa(s->instance, "vkEnumeratePhysicalDevices");
	PFN_vkGetPhysicalDeviceProperties Props = (PFN_vkGetPhysicalDeviceProperties)gipa(s->instance, "vkGetPhysicalDeviceProperties");
	PFN_vkGetPhysicalDeviceMemoryProperties MemProps =
		(PFN_vkGetPhysicalDeviceMemoryProperties)gipa(s->instance, "vkGetPhysicalDeviceMemoryProperties");
	PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)gipa(s->instance, "vkCreateDevice");
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(s->instance, "vkGetDeviceProcAddr");
	VkPhysicalDevice devs[8];
	uint32_t n = 8;
	Enum(s->instance, &n, devs);
	for (uint32_t i = 0; i < n && !s->physical; i++) {
		VkPhysicalDeviceProperties p;
		Props(devs[i], &p);
		if (!strncmp(p.deviceName, device_prefix, strlen(device_prefix))) {
			s->physical = devs[i];
			snprintf(s->device_name, sizeof(s->device_name), "%s", p.deviceName);
		}
	}
	if (!s->physical) {
		snprintf(s->msg, sizeof(s->msg), "no device named %s...", device_prefix);
		return -1;
	}
	VkPhysicalDeviceMemoryProperties mp;
	MemProps(s->physical, &mp);
	host_type = dev_type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if (host_type == UINT32_MAX && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
		    (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			host_type = i;
		if (dev_type == UINT32_MAX && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			dev_type = i;
	}
	const char *ext = "VK_KHR_dynamic_rendering";
	VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR, .dynamicRendering = VK_TRUE };
	float prio = 1;
	CHECK(CreateDevice(s->physical, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dr, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
								  .queueCount = 1, .pQueuePriorities = &prio },
		.enabledExtensionCount = 1, .ppEnabledExtensionNames = &ext }, NULL, &s->device));
#define LOAD(n) n = (PFN_##n)GetDeviceProcAddr(s->device, #n);
	FNS(LOAD)
	vkGetDeviceQueue(s->device, 0, 0, &s->queue);
	VkDevice dev = s->device;

	/* buffers: vertices at 0, indices at 1024, instance offsets at 1280, tint at 1536, compute at 4096 */
	void *map;
	VkBuffer buf = host_buffer(s, 8192, &map);
	float *v = map;
	const float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, white[4] = { 1, 1, 1, 1 };
	quad(v + 0, 4, 4, 28, 28, red, false);		/* A: vertices 0..3 */
	quad(v + 24, 16, 16, 40, 40, green, false);	/* B: 4..7 */
	quad(v + 48, 36, 4, 60, 28, NULL, true);	/* C: 8..11, uv in rg */
	/* D: a triangle around the origin, 12 px wide; instances at x 12, 32, 52, y 52 */
	float tri[18] = { ndc(32 - 6) + 1.0f - 1.0f, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1 };
	tri[0] = -6.0f / 32; tri[1] = -6.0f / 32;
	tri[6] = 6.0f / 32;  tri[7] = -6.0f / 32;
	tri[12] = 0;         tri[13] = 8.0f / 32;
	memcpy(v + 72, tri, sizeof(tri));		/* 12..14 */
	(void)white;
	uint16_t *idx = (uint16_t *)((char *)map + 1024);
	const uint16_t q[6] = { 0, 1, 2, 2, 3, 0 };
	memcpy(idx, q, sizeof(q));
	float *inst = (float *)((char *)map + 1280);
	const float io[8] = { ndc(12) , ndc(52), ndc(32), ndc(52), ndc(52), ndc(52), 0, 0 };
	memcpy(inst, io, sizeof(io));
	float *tint = (float *)((char *)map + 1536);
	tint[0] = 0; tint[1] = 0; tint[2] = 1; tint[3] = 1;
	uint32_t *cw = (uint32_t *)((char *)map + 4096);
	for (uint32_t i = 0; i < 256; i++)
		cw[i] = i * 7 + 3;
	cw[512] = 1000;

	/* the texture: 2x2, uploaded from the buffer at 2048 */
	uint8_t *tp = (uint8_t *)map + 2048;
	const uint8_t texels[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
	memcpy(tp, texels, 16);
	VkImageView tex_view, color_view, depth_view;
	VkImage tex = image(s, 2, 2, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			    VK_IMAGE_ASPECT_COLOR_BIT, &tex_view);
	s->image = image(s, S, S, VK_FORMAT_R8G8B8A8_UNORM,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			 VK_IMAGE_ASPECT_COLOR_BIT, &color_view);
	VkImage depth = image(s, S, S, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			      VK_IMAGE_ASPECT_DEPTH_BIT, &depth_view);
	VkSampler sampler;
	CHECK(vkCreateSampler(dev, &(VkSamplerCreateInfo){ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE }, NULL, &sampler));

	/* descriptor sets: 0 none (colour), texture, UBO; compute */
	VkDescriptorSetLayoutBinding tb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutBinding ub = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutBinding cb3[3] = {
		{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
	};
	VkDescriptorSetLayout dsl[4];
	const VkDescriptorSetLayoutBinding *bs[4] = { NULL, &tb, &ub, cb3 };
	const uint32_t bn[4] = { 0, 1, 1, 3 };
	VkPipelineLayout pl[4];
	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(struct pc) };
	for (int i = 0; i < 4; i++) {
		CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = bn[i],
			.pBindings = bs[i] }, NULL, &dsl[i]));
		CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl[i],
			.pushConstantRangeCount = i < 3, .pPushConstantRanges = &pcr }, NULL, &pl[i]));
	}
	VkDescriptorPool dp;
	VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
				       { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 } };
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 3, .pPoolSizes = ps },
		NULL, &dp));
	VkDescriptorSet set[4];
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 4,
		.pSetLayouts = dsl }, set));
	VkWriteDescriptorSet w[5] = {
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set[1], .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		  .pImageInfo = &(VkDescriptorImageInfo){ sampler, tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set[2], .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		  .pBufferInfo = &(VkDescriptorBufferInfo){ buf, 1536, 16 } },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set[3], .dstBinding = 0, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .pBufferInfo = &(VkDescriptorBufferInfo){ buf, 4096, 1024 } },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set[3], .dstBinding = 1, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .pBufferInfo = &(VkDescriptorBufferInfo){ buf, 5120, 1024 } },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set[3], .dstBinding = 2, .descriptorCount = 1,
		  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		  .pBufferInfo = &(VkDescriptorBufferInfo){ buf, 6144, 16 } },
	};
	vkUpdateDescriptorSets(dev, 5, w, 0, NULL);

	/* pipelines */
	VkShaderModule vs = module(s, spv_tri_vert, sizeof(spv_tri_vert));
	VkShaderModule fss[3] = { module(s, spv_color_frag, sizeof(spv_color_frag)),
				  module(s, spv_tex_frag, sizeof(spv_tex_frag)),
				  module(s, spv_ubo_frag, sizeof(spv_ubo_frag)) };
	VkVertexInputBindingDescription vb[2] = { { 0, 24, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 8, VK_VERTEX_INPUT_RATE_INSTANCE } };
	VkVertexInputAttributeDescription va[3] = { { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
						    { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8 },
						    { 2, 1, VK_FORMAT_R32G32_SFLOAT, 0 } };
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
						    2, vb, 3, va };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
						      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
						      .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
						    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
						     .depthTestEnable = 1, .depthWriteEnable = 1,
						     .depthCompareOp = VK_COMPARE_OP_LESS };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
						    .attachmentCount = 1, .pAttachments = &cba };
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dys = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
	VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
	VkPipelineRenderingCreateInfoKHR ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
						.colorAttachmentCount = 1, .pColorAttachmentFormats = &cf,
						.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT };
	VkPipeline gp[3];
	for (int i = 0; i < 3; i++) {
		VkPipelineShaderStageCreateInfo st[2] = {
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fss[i], "main" },
		};
		CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
			.pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
			.pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
			.pColorBlendState = &cbs, .pDynamicState = &dys, .layout = pl[i] }, NULL, &gp[i]));
	}
	VkShaderModule csm = module(s, spv_square_comp, sizeof(spv_square_comp));
	VkPipeline cp;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, csm,
			   "main" }, .layout = pl[3] }, NULL, &cp));

	/* the commands */
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO },
				  NULL, &pool));
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cb));
	CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
	layout_barrier(cb, tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyBufferToImage(cb, buf, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkBufferImageCopy){
		.bufferOffset = 2048, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 2, 2, 1 } });
	layout_barrier(cb, tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	layout_barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	layout_barrier(cb, depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	vkCmdBeginRenderingKHR(cb, &(VkRenderingInfoKHR){
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, .renderArea = { { 0, 0 }, { S, S } }, .layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &(VkRenderingAttachmentInfoKHR){
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, .imageView = color_view,
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = { 0.1f, 0.1f, 0.1f, 1 } },
		.pDepthAttachment = &(VkRenderingAttachmentInfoKHR){
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, .imageView = depth_view,
			.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .clearValue.depthStencil = { 1, 0 } } });
	vkCmdSetViewport(cb, 0, 1, &(VkViewport){ 0, 0, S, S, 0, 1 });
	vkCmdSetScissor(cb, 0, 1, &(VkRect2D){ { 0, 0 }, { S, S } });
	VkBuffer vbs[2] = { buf, buf };
	VkDeviceSize vos[2] = { 0, 1280 + 24 };	/* (a zero offset for the non-instanced draws: the last pair is 0, 0) */
	vkCmdBindVertexBuffers(cb, 0, 2, vbs, vos);
	vkCmdBindIndexBuffer(cb, buf, 1024, VK_INDEX_TYPE_UINT16);
	struct pc pc = { { 0, 0 }, 1, 0.3f };
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp[0]);
	vkCmdPushConstants(cb, pl[0], VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	vkCmdDrawIndexed(cb, 6, 1, 0, 4, 0);		/* B */
	pc.z = 0.5f;
	vkCmdPushConstants(cb, pl[0], VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);		/* A */
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp[1]);
	vkCmdPushConstants(cb, pl[1], VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl[1], 0, 1, &set[1], 0, NULL);
	vkCmdDrawIndexed(cb, 6, 1, 0, 8, 0);		/* C */
	vos[1] = 1280;
	vkCmdBindVertexBuffers(cb, 0, 2, vbs, vos);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp[2]);
	vkCmdPushConstants(cb, pl[2], VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl[2], 0, 1, &set[2], 0, NULL);
	vkCmdDraw(cb, 3, 3, 12, 0);			/* D */
	vkCmdEndRenderingKHR(cb);
	layout_barrier(cb, s->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &(VkBufferImageCopy){
		.bufferOffset = 8192 - S * S * 4 < 6400 ? 0 : 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { 1, 1, 1 } });	/* (exercises a 1-texel copy; the real readback below) */
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl[3], 0, 1, &set[3], 0, NULL);
	vkCmdDispatch(cb, 4, 1, 1);
	CHECK(vkEndCommandBuffer(cb));
	VkFence fence;
	CHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &fence));
	CHECK(vkQueueSubmit(s->queue, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
							  .commandBufferCount = 1, .pCommandBuffers = &cb }, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));

	/* the image into a buffer of its own */
	void *pix;
	VkBuffer rb = host_buffer(s, S * S * 4, &pix);
	VkCommandBuffer cb2;
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cb2));
	CHECK(vkBeginCommandBuffer(cb2, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
	vkCmdCopyImageToBuffer(cb2, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &(VkBufferImageCopy){
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { S, S, 1 } });
	CHECK(vkEndCommandBuffer(cb2));
	CHECK(vkResetFences(dev, 1, &fence));
	CHECK(vkQueueSubmit(s->queue, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
							  .commandBufferCount = 1, .pCommandBuffers = &cb2 }, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));

	/* the checks */
	static const struct { int x, y; uint8_t c[4]; const char *what; } want[] = {
		{ 8, 8, { 255, 0, 0, 255 }, "A" },
		{ 22, 22, { 0, 255, 0, 255 }, "A/B overlap (depth)" },
		{ 36, 36, { 0, 255, 0, 255 }, "B" },
		{ 42, 10, { 255, 0, 0, 255 }, "C texel 0,0" },
		{ 54, 10, { 0, 255, 0, 255 }, "C texel 1,0" },
		{ 42, 22, { 0, 0, 255, 255 }, "C texel 0,1" },
		{ 54, 22, { 255, 255, 255, 255 }, "C texel 1,1" },
		{ 12, 53, { 0, 0, 255, 255 }, "D instance 0" },
		{ 32, 53, { 0, 0, 255, 255 }, "D instance 1" },
		{ 52, 53, { 0, 0, 255, 255 }, "D instance 2" },
		{ 2, 62, { 26, 26, 26, 255 }, "background" },
		{ 32, 42, { 26, 26, 26, 255 }, "between B and D" },
	};
	const uint8_t *p = pix;
	int bad = 0;
	char *m = s->msg;
	size_t left = sizeof(s->msg);
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		const uint8_t *q = p + 4 * (want[i].y * S + want[i].x);
		bool ok = true;
		for (int c = 0; c < 4; c++)
			ok &= abs((int)q[c] - want[i].c[c]) <= 2;
		if (!ok) {
			bad++;
			int k = snprintf(m, left, "%s at (%d,%d): %u %u %u %u, want %u %u %u %u; ", want[i].what, want[i].x,
					 want[i].y, q[0], q[1], q[2], q[3], want[i].c[0], want[i].c[1], want[i].c[2], want[i].c[3]);
			if (k > 0 && (size_t)k < left) {
				m += k;
				left -= k;
			}
		}
	}
	const uint32_t *out = (const uint32_t *)((char *)map + 5120);
	int cbad = 0;
	for (uint32_t i = 0; i < 256; i++)
		cbad += out[i] != (i * 7 + 3) * (i * 7 + 3) + 1000;
	if (cbad)
		snprintf(m, left, "compute: %d of 256 words wrong (out[1] = %u, want %u)", cbad, out[1], 10 * 10 + 1000);
	if (!bad && !cbad)
		snprintf(s->msg, sizeof(s->msg), "%s: 12 sample pixels and 256 compute words right", s->device_name);
	s->pixels = pix;
	return bad || cbad ? 1 : 0;
}

#ifndef SMOKE_NO_MAIN
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name);

int
main(int argc, char **argv)
{
	struct smoke s;
	int r = smoke_draw(&s, vkGetInstanceProcAddr, argc > 1 ? argv[1] : "wgvk");
	printf("%s smoke-draw: %s\n", r == 0 ? "ok" : "FAIL", s.msg);
	if (argc > 2 && s.pixels) {
		FILE *f = fopen(argv[2], "wb");
		fprintf(f, "P6 %d %d 255\n", S, S);
		for (int i = 0; i < S * S; i++)
			fwrite((const uint8_t *)s.pixels + 4 * i, 1, 3, f);
		fclose(f);
	}
	return r == 0 ? 0 : 1;
}
#endif
