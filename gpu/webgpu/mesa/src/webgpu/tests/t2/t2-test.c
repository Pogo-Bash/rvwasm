/*
 * T2's native comparison tests: the same Vulkan program runs once on lavapipe
 * and once on wgvk (gpu/webgpu/tests/t2.test runs it twice with
 * VK_DRIVER_FILES naming each), each run writing what every case produced
 * (pixels, buffer contents) into a directory; then `t2-test --compare A B`
 * compares the two directories case by case.
 *
 *   t2-test OUTDIR SPVDIR [CASE...]
 *   t2-test --compare DIR_A DIR_B [CASE...]
 *
 * Cases: clear, triangle (legacy render pass), indexed, instanced, indirect
 * (multi-draw by drawCount 2, indexed indirect), depth, ubo-dyn (dynamic
 * offsets), texture (buffer->image upload with an odd row pitch, sampling),
 * compute (inputs, output and uniform in one VkDeviceMemory), copies (buffer,
 * image, fill, update, blit), clear-att (vkCmdClearAttachments), flip-y
 * (negative viewport height), split (one memory used as a UBO by one draw
 * and written as an SSBO by the next: WebGPU usage-scope splitting), selfdep
 * (one draw reads and writes one memory: snapshot).
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>

#define W 64
#define H 48

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static uint32_t host_type, dev_type;
static const char *outdir, *spvdir;
static char devname[256];

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
	fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

/* ---- setup */

static void
init(void)
{
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
							 .pApplicationInfo = &app }, NULL, &inst));
	uint32_t n = 1;
	VkResult r = vkEnumeratePhysicalDevices(inst, &n, &phys);
	if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) {
		fprintf(stderr, "no physical device\n");
		exit(2);
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(phys, &props);
	snprintf(devname, sizeof(devname), "%s", props.deviceName);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	host_type = dev_type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if (host_type == UINT32_MAX && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
		    (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			host_type = i;
		if (dev_type == UINT32_MAX && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			dev_type = i;
	}
	const char *exts[] = { "VK_KHR_dynamic_rendering", "VK_KHR_create_renderpass2", "VK_KHR_draw_indirect_count",
			       "VK_EXT_provoking_vertex", "VK_EXT_index_type_uint8", "VK_EXT_vertex_attribute_divisor",
			       "VK_KHR_shader_draw_parameters" };
	VkPhysicalDeviceShaderDrawParametersFeatures sdp = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, .shaderDrawParameters = VK_TRUE };
	VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vad = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .pNext = &sdp,
		.vertexAttributeInstanceRateDivisor = VK_TRUE };
	VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &vad,
					 .features.largePoints = VK_TRUE };
	VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, .pNext = &f2,
		.indexTypeUint8 = VK_TRUE };
	/* what the device has of these (wgvk may not advertise everything it can do yet: the cases run anyway) */
	{
		VkPhysicalDeviceShaderDrawParametersFeatures q_sdp = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES };
		VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT q_vad = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .pNext = &q_sdp };
		VkPhysicalDeviceFeatures2 q = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q_vad };
		vkGetPhysicalDeviceFeatures2(phys, &q);
		sdp.shaderDrawParameters = q_sdp.shaderDrawParameters;
		vad.vertexAttributeInstanceRateDivisor = q_vad.vertexAttributeInstanceRateDivisor;
		f2.features.largePoints = q.features.largePoints;
		f2.features.geometryShader = q.features.geometryShader;
		if (!q_sdp.shaderDrawParameters || !q.features.largePoints)
			printf("note: shaderDrawParameters %u largePoints %u\n", q_sdp.shaderDrawParameters,
			       q.features.largePoints);
	}
	VkPhysicalDeviceProvokingVertexFeaturesEXT pv = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, .pNext = &u8,
		.provokingVertexLast = VK_TRUE };
	VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR, .pNext = &pv,
		.dynamicRendering = VK_TRUE };
	float prio = 1.0f;
	CHECK(vkCreateDevice(phys, &(VkDeviceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dr, .queueCreateInfoCount = 1,
		.pQueueCreateInfos = &(VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
								  .queueCount = 1, .pQueuePriorities = &prio },
		.enabledExtensionCount = 7, .ppEnabledExtensionNames = exts }, NULL, &dev));
	vkGetDeviceQueue(dev, 0, 0, &queue);
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT }, NULL, &pool));
}

static PFN_vkCmdBeginRenderingKHR BeginRendering;
static PFN_vkCmdDrawIndirectCountKHR DrawIndirectCount;
/* recorded before the render pass of render() */
static void (*pre_render)(VkCommandBuffer cb, void *data);
static PFN_vkCmdEndRenderingKHR EndRendering;

/* ---- memory, buffers, images */

struct buf {
	VkBuffer b;
	VkDeviceMemory m;
	void *map;
	VkDeviceSize size;
};

static VkDeviceMemory
alloc(VkMemoryRequirements req, uint32_t type)
{
	VkDeviceMemory m;
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
							     .allocationSize = req.size, .memoryTypeIndex = type }, NULL, &m));
	return m;
}

static VkBuffer
make_buffer_raw(VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkBuffer b;
	CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
							 .usage = usage }, NULL, &b));
	return b;
}

static const VkBufferUsageFlags ALL_BUF = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
	VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
	VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

static struct buf
make_buffer(VkDeviceSize size, bool host)
{
	struct buf b = { .size = size };
	b.b = make_buffer_raw(size, ALL_BUF);
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, b.b, &req);
	b.m = alloc(req, host ? host_type : dev_type);
	CHECK(vkBindBufferMemory(dev, b.b, b.m, 0));
	if (host)
		CHECK(vkMapMemory(dev, b.m, 0, VK_WHOLE_SIZE, 0, &b.map));
	return b;
}

static void
free_buffer(struct buf *b)
{
	vkDestroyBuffer(dev, b->b, NULL);
	vkFreeMemory(dev, b->m, NULL);
}

struct img {
	VkImage i;
	VkDeviceMemory m;
	VkImageView v;
	VkFormat f;
	uint32_t w, h;
};

static struct img
make_image(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage)
{
	struct img im = { .f = f, .w = w, .h = h };
	CHECK(vkCreateImage(dev, &(VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
		.extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT }, NULL, &im.i));
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev, im.i, &req);
	im.m = alloc(req, dev_type);
	CHECK(vkBindImageMemory(dev, im.i, im.m, 0));
	bool depth = f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT;
	CHECK(vkCreateImageView(dev, &(VkImageViewCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.i, .viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = f, .subresourceRange = { depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
		NULL, &im.v));
	return im;
}

static void
free_image(struct img *im)
{
	vkDestroyImageView(dev, im->v, NULL);
	vkDestroyImage(dev, im->i, NULL);
	vkFreeMemory(dev, im->m, NULL);
}

/* ---- command buffers */

static VkCommandBuffer
begin(void)
{
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cb));
	CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
	return cb;
}

static void
barrier(VkCommandBuffer cb)
{
	VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			       .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
			       .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
			     NULL, 0, NULL);
}

static void
layout(VkCommandBuffer cb, struct img *im, VkImageLayout from, VkImageLayout to)
{
	bool depth = im->f == VK_FORMAT_D32_SFLOAT || im->f == VK_FORMAT_D16_UNORM || im->f == VK_FORMAT_D24_UNORM_S8_UINT;
	bool stencil = im->f == VK_FORMAT_D24_UNORM_S8_UINT;
	VkImageMemoryBarrier b = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = im->i,
		.subresourceRange = { depth ? VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)
					    : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
			     NULL, 1, &b);
}

static void
submit_wait(VkCommandBuffer cb)
{
	CHECK(vkEndCommandBuffer(cb));
	VkFence f;
	CHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &f));
	CHECK(vkQueueSubmit(queue, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
						       .pCommandBuffers = &cb }, f));
	CHECK(vkWaitForFences(dev, 1, &f, VK_TRUE, 10ull * 1000 * 1000 * 1000));
	vkDestroyFence(dev, f, NULL);
	vkFreeCommandBuffers(dev, pool, 1, &cb);
}

/* the colour image into a host buffer, then a file */
static void
save_image(struct img *im, const char *name)
{
	uint32_t bpp = im->f == VK_FORMAT_D32_SFLOAT ? 4 : 4;
	struct buf rb = make_buffer((VkDeviceSize)im->w * im->h * bpp, true);
	VkCommandBuffer cb = begin();
	bool depth = im->f == VK_FORMAT_D32_SFLOAT;
	vkCmdCopyImageToBuffer(cb, im->i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &(VkBufferImageCopy){
		.imageSubresource = { depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { im->w, im->h, 1 } });
	submit_wait(cb);
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.rgba", outdir, name);
	FILE *f = fopen(path, "wb");
	fwrite(rb.map, 1, rb.size, f);
	fclose(f);
	if (!depth) {
		/* a PPM to look at */
		snprintf(path, sizeof(path), "%s/%s.ppm", outdir, name);
		f = fopen(path, "wb");
		fprintf(f, "P6 %u %u 255\n", im->w, im->h);
		const uint8_t *p = rb.map;
		for (uint32_t i = 0; i < im->w * im->h; i++)
			fwrite(p + 4 * i, 1, 3, f);
		fclose(f);
	}
	free_buffer(&rb);
}

static void
save_bytes(const void *p, size_t n, const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.bin", outdir, name);
	FILE *f = fopen(path, "wb");
	fwrite(p, 1, n, f);
	fclose(f);
}

/* ---- shaders and pipelines */

static VkShaderModule
shader(const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.spv", spvdir, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "no %s\n", path);
		exit(2);
	}
	static uint32_t code[65536];
	size_t n = fread(code, 1, sizeof(code), f);
	fclose(f);
	VkShaderModule m;
	CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
								      .codeSize = n, .pCode = code }, NULL, &m));
	return m;
}

struct gfx {
	VkPipeline p;
	VkPipelineLayout pl;
	VkDescriptorSetLayout dsl;
};

struct gfx_opts {
	const char *fs;
	VkDescriptorType desc;		/* 0: none */
	uint32_t ndesc;			/* bindings 0..ndesc-1 of that type (default 1) */
	bool instanced, depth, cull_back, provoking_last;
	VkRenderPass rp;		/* legacy render pass, or dynamic rendering */
	VkPrimitiveTopology topo;
	const char *vs;			/* default tri.vert */
	const char *gs;			/* a geometry shader, or NULL */
	uint32_t pc_size;		/* default sizeof(struct pc) */
	/* a vertex input of its own (default: tri.vert's) */
	uint32_t nvb, nva;
	const VkVertexInputBindingDescription *vb;
	const VkVertexInputAttributeDescription *va;
	const VkVertexInputBindingDivisorDescriptionEXT *div;	/* binding 1's divisor */
};

struct pc {
	float off[2], scale, z;
};

static struct gfx
make_gfx(const struct gfx_opts *o)
{
	struct gfx g = { 0 };
	VkDescriptorSetLayoutBinding b[24];
	for (unsigned i = 0; i < 24; i++)
		b[i] = (VkDescriptorSetLayoutBinding){ i, o->desc, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = o->desc ? (o->ndesc ? o->ndesc : 1) : 0, .pBindings = b }, NULL, &g.dsl));
	VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, o->pc_size ? o->pc_size : sizeof(struct pc) };
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g.dsl,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr }, NULL, &g.pl));
	VkShaderModule vs = shader(o->vs ? o->vs : "tri.vert"), fs = shader(o->fs);
	VkShaderModule gs = o->gs ? shader(o->gs) : VK_NULL_HANDLE;
	VkPipelineShaderStageCreateInfo st[3] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main" },
	};
	VkVertexInputBindingDescription vb[2] = {
		{ 0, 24, VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, 8, VK_VERTEX_INPUT_RATE_INSTANCE },
	};
	VkVertexInputAttributeDescription va[3] = {
		{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8 },
		{ 2, 1, VK_FORMAT_R32G32_SFLOAT, 0 },
	};
	VkPipelineVertexInputDivisorStateCreateInfoEXT divs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
		.vertexBindingDivisorCount = 1, .pVertexBindingDivisors = o->div };
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
						    o->div ? &divs : NULL, 0,
						    o->vb ? o->nvb : 2, o->vb ? o->vb : vb, o->va ? o->nva : 3, o->va ? o->va : va };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
						      o->topo };
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL,
						 1, NULL };
	VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
		.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT };
	VkPipelineRasterizationStateCreateInfo rs = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .pNext = o->provoking_last ? &pvs : NULL,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = o->cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
						    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
						     .depthTestEnable = o->depth, .depthWriteEnable = o->depth,
						     .depthCompareOp = VK_COMPARE_OP_LESS };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
						   .attachmentCount = 1, .pAttachments = &cba };
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
	VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
	VkPipelineRenderingCreateInfoKHR ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
						.colorAttachmentCount = 1, .pColorAttachmentFormats = &cf,
						.depthAttachmentFormat = o->depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED };
	CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &(VkGraphicsPipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = o->rp ? NULL : &ri, .stageCount = o->gs ? 3 : 2,
		.pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
		.pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
		.pDynamicState = &dy, .layout = g.pl, .renderPass = o->rp }, NULL, &g.p));
	vkDestroyShaderModule(dev, vs, NULL);
	vkDestroyShaderModule(dev, fs, NULL);
	if (gs)
		vkDestroyShaderModule(dev, gs, NULL);
	return g;
}

static void
free_gfx(struct gfx *g)
{
	vkDestroyPipeline(dev, g->p, NULL);
	vkDestroyPipelineLayout(dev, g->pl, NULL);
	vkDestroyDescriptorSetLayout(dev, g->dsl, NULL);
}

static VkDescriptorPool dpool;

static VkDescriptorSet
make_set(VkDescriptorSetLayout l)
{
	VkDescriptorSet s;
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1,
		.pSetLayouts = &l }, &s));
	return s;
}

static void
write_buffer_desc(VkDescriptorSet s, uint32_t binding, VkDescriptorType t, VkBuffer b, VkDeviceSize off,
		  VkDeviceSize range)
{
	vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s, .dstBinding = binding, .descriptorCount = 1,
		.descriptorType = t, .pBufferInfo = &(VkDescriptorBufferInfo){ b, off, range } }, 0, NULL);
}

/* ---- scene pieces */

/* vertices: pos.xy, colour rgba */
static const float tri_verts[] = {
	-0.8f, -0.8f, 1, 0, 0, 1,
	 0.8f, -0.8f, 0, 1, 0, 1,
	 0.0f,  0.8f, 0, 0, 1, 1,
};
static const float quad_verts[] = {
	-0.5f, -0.5f, 0.1f, 0.2f, 0.9f, 1,
	 0.5f, -0.5f, 0.9f, 0.2f, 0.1f, 1,
	 0.5f,  0.5f, 0.9f, 0.9f, 0.1f, 1,
	-0.5f,  0.5f, 0.1f, 0.9f, 0.9f, 1,
};
static const uint16_t quad_idx[] = { 0, 1, 2, 2, 3, 0 };

static void
begin_dyn(VkCommandBuffer cb, struct img *color, struct img *depth, VkAttachmentLoadOp load, const float clear[4])
{
	VkRenderingAttachmentInfoKHR ca = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, .imageView = color->v,
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = load,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.clearValue.color.float32 = { clear[0], clear[1], clear[2], clear[3] } };
	VkRenderingAttachmentInfoKHR da = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, .imageView = depth ? depth->v : VK_NULL_HANDLE,
		.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.depthStencil = { 1.0f, 0 } };
	BeginRendering(cb, &(VkRenderingInfoKHR){
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, .renderArea = { { 0, 0 }, { color->w, color->h } },
		.layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca,
		.pDepthAttachment = depth ? &da : NULL });
}

static void
viewport(VkCommandBuffer cb, bool flip)
{
	VkViewport v = { 0, 0, W, H, 0, 1 };
	if (flip) {
		v.y = H;
		v.height = -H;
	}
	vkCmdSetViewport(cb, 0, 1, &v);
	vkCmdSetScissor(cb, 0, 1, &(VkRect2D){ { 0, 0 }, { W, H } });
}

static void
push(VkCommandBuffer cb, struct gfx *g, float x, float y, float s, float z)
{
	struct pc pc = { { x, y }, s, z };
	vkCmdPushConstants(cb, g->pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
}

static const float black[4] = { 0, 0, 0, 1 };

/* A colour target + its pixels saved; draw() records the draws inside dynamic rendering. */
static void
render(const char *name, struct gfx_opts *o, void (*draw)(VkCommandBuffer, struct gfx *, void *), void *data)
{
	struct img color = make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	struct img depth = { 0 };
	if (o->depth)
		depth = make_image(W, H, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	struct gfx g = make_gfx(o);
	VkCommandBuffer cb = begin();
	if (pre_render) {
		pre_render(cb, data);
		barrier(cb);
	}
	layout(cb, &color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (o->depth)
		layout(cb, &depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	begin_dyn(cb, &color, o->depth ? &depth : NULL, VK_ATTACHMENT_LOAD_OP_CLEAR, black);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.p);
	viewport(cb, false);
	draw(cb, &g, data);
	EndRendering(cb);
	layout(cb, &color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	submit_wait(cb);
	save_image(&color, name);
	free_gfx(&g);
	free_image(&color);
	if (o->depth)
		free_image(&depth);
}

/* ---- cases */

static struct buf vbuf;	/* tri_verts at 0, quad_verts at 256, quad_idx at 512, instance offsets at 768 */

static void
fill_vbuf(void)
{
	vbuf = make_buffer(4096, true);
	memcpy(vbuf.map, tri_verts, sizeof(tri_verts));
	memcpy((char *)vbuf.map + 256, quad_verts, sizeof(quad_verts));
	memcpy((char *)vbuf.map + 512, quad_idx, sizeof(quad_idx));
	static const float inst[] = { -0.5f, -0.5f, 0.5f, -0.4f, 0.0f, 0.5f };
	memcpy((char *)vbuf.map + 768, inst, sizeof(inst));
	/* pulled vertices: 3 of 7 bytes (snorm16 x, y; unorm8 r, g, b), instance offsets at 1400 */
	static const int16_t pp[3][2] = { { -20000, -20000 }, { 20000, -20000 }, { 0, 20000 } };
	static const uint8_t pc[3][3] = { { 255, 30, 30 }, { 30, 255, 30 }, { 30, 30, 255 } };
	for (int i = 0; i < 3; i++) {
		memcpy((char *)vbuf.map + 1300 + 7 * i, pp[i], 4);
		memcpy((char *)vbuf.map + 1300 + 7 * i + 4, pc[i], 3);
	}
	static const float io[4] = { -0.5f, -0.3f, 0.5f, 0.3f };
	memcpy((char *)vbuf.map + 1400, io, sizeof(io));
	static const uint8_t idx8[4] = { 3, 2, 1, 0 };
	memcpy((char *)vbuf.map + 1100, idx8, 4);
	static const float zero[2] = { 0, 0 };
	memcpy((char *)vbuf.map + 1024, zero, sizeof(zero));
}

static void
bind_vb(VkCommandBuffer cb, VkDeviceSize off, VkDeviceSize ioff)
{
	VkBuffer b[2] = { vbuf.b, vbuf.b };
	VkDeviceSize o[2] = { off, ioff };
	vkCmdBindVertexBuffers(cb, 0, 2, b, o);
}

static void
case_clear(void)
{
	struct img color = make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	VkCommandBuffer cb = begin();
	layout(cb, &color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	const float c[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	begin_dyn(cb, &color, NULL, VK_ATTACHMENT_LOAD_OP_CLEAR, c);
	EndRendering(cb);
	layout(cb, &color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	submit_wait(cb);
	save_image(&color, "clear");
	free_image(&color);
}

/* a legacy VkRenderPass + VkFramebuffer: Mesa's vk_render_pass.c turns it into dynamic rendering */
static void
case_triangle(void)
{
	VkAttachmentDescription ad = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
				       VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				       VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
				       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
				    .pColorAttachments = &ar };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){ .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
								 .attachmentCount = 1, .pAttachments = &ad,
								 .subpassCount = 1, .pSubpasses = &sd }, NULL, &rp));
	struct img color = make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &(VkFramebufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
		.pAttachments = &color.v, .width = W, .height = H, .layers = 1 }, NULL, &fb));
	struct gfx_opts o = { .fs = "color.frag", .rp = rp, .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	struct gfx g = make_gfx(&o);
	VkCommandBuffer cb = begin();
	VkClearValue cv = { .color.float32 = { 0.1f, 0.1f, 0.1f, 1 } };
	vkCmdBeginRenderPass(cb, &(VkRenderPassBeginInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &cv },
		VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.p);
	viewport(cb, false);
	bind_vb(cb, 0, 1024);
	push(cb, &g, 0, 0, 1, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	submit_wait(cb);
	save_image(&color, "triangle");
	free_gfx(&g);
	vkDestroyFramebuffer(dev, fb, NULL);
	vkDestroyRenderPass(dev, rp, NULL);
	free_image(&color);
}

/* The draws in secondary command buffers, executed inside a legacy render pass begun with
 * VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS (as Qt's RHI does): two secondaries, one vkCmdExecuteCommands.
 * wgvk inlines them into the primary through Mesa's vk_common_CmdExecuteCommands (VK-27: its dispatch table was NULL). */
static void
case_secondary(void)
{
	VkAttachmentDescription ad = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
				       VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				       VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
				       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
				    .pColorAttachments = &ar };
	VkRenderPass rp;
	CHECK(vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){ .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
								 .attachmentCount = 1, .pAttachments = &ad,
								 .subpassCount = 1, .pSubpasses = &sd }, NULL, &rp));
	struct img color = make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	VkFramebuffer fb;
	CHECK(vkCreateFramebuffer(dev, &(VkFramebufferCreateInfo){
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
		.pAttachments = &color.v, .width = W, .height = H, .layers = 1 }, NULL, &fb));
	struct gfx_opts o = { .fs = "color.frag", .rp = rp, .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	struct gfx g = make_gfx(&o);
	VkCommandBuffer sec[2];
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = 2 }, sec));
	VkCommandBufferInheritanceInfo inh = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
					       .renderPass = rp, .subpass = 0, .framebuffer = fb };
	for (int i = 0; i < 2; i++) {
		CHECK(vkBeginCommandBuffer(sec[i], &(VkCommandBufferBeginInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, .pInheritanceInfo = &inh }));
		vkCmdBindPipeline(sec[i], VK_PIPELINE_BIND_POINT_GRAPHICS, g.p);
		viewport(sec[i], false);
		bind_vb(sec[i], 0, 1024);
		push(sec[i], &g, i ? 0.4f : -0.4f, i ? 0.3f : -0.3f, 0.5f, 0.5f);
		vkCmdDraw(sec[i], 3, 1, 0, 0);
		CHECK(vkEndCommandBuffer(sec[i]));
	}
	VkCommandBuffer cb = begin();
	VkClearValue cv = { .color.float32 = { 0.1f, 0.1f, 0.1f, 1 } };
	vkCmdBeginRenderPass(cb, &(VkRenderPassBeginInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
		.renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &cv },
		VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
	vkCmdExecuteCommands(cb, 2, sec);
	vkCmdEndRenderPass(cb);
	submit_wait(cb);
	save_image(&color, "secondary");
	vkFreeCommandBuffers(dev, pool, 2, sec);
	free_gfx(&g);
	vkDestroyFramebuffer(dev, fb, NULL);
	vkDestroyRenderPass(dev, rp, NULL);
	free_image(&color);
}

static void
draw_indexed(VkCommandBuffer cb, struct gfx *g, void *data)
{
	bind_vb(cb, 256, 1024);
	vkCmdBindIndexBuffer(cb, vbuf.b, 512, VK_INDEX_TYPE_UINT16);
	push(cb, g, -0.4f, 0, 0.8f, 0.5f);
	vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
	/* the second triangle alone, moved: firstIndex 3 */
	push(cb, g, 0.45f, 0.3f, 0.6f, 0.5f);
	vkCmdDrawIndexed(cb, 3, 1, 3, 0, 0);
}

static void
draw_instanced(VkCommandBuffer cb, struct gfx *g, void *data)
{
	bind_vb(cb, 0, 768);
	push(cb, g, 0, 0, 0.3f, 0.5f);
	vkCmdDraw(cb, 3, 3, 0, 0);
}

static void
draw_indirect(VkCommandBuffer cb, struct gfx *g, void *data)
{
	struct buf *ind = data;
	bind_vb(cb, 0, 768);
	vkCmdBindIndexBuffer(cb, vbuf.b, 512, VK_INDEX_TYPE_UINT16);
	push(cb, g, 0, 0, 0.3f, 0.5f);
	vkCmdDrawIndirect(cb, ind->b, 0, 2, 16);	/* two draws by a loop */
	bind_vb(cb, 256, 768);
	push(cb, g, 0.2f, 0.2f, 0.4f, 0.5f);
	vkCmdDrawIndexedIndirect(cb, ind->b, 64, 1, 20);
}

/* the arguments written by the GPU in the same command buffer (vkCmdUpdateBuffer): WebGPU's indirect draws;
 * then a draw count from a host buffer (the CPU reads it) */
struct ind_gpu {
	struct buf dev, count;
};

static void
pre_indirect_gpu(VkCommandBuffer cb, void *data)
{
	struct ind_gpu *g = data;
	uint32_t w[24] = { 3, 2, 0, 0, 3, 1, 0, 2 };
	uint32_t di[5] = { 6, 1, 0, 0, 0 };
	memcpy(w + 16, di, 20);
	vkCmdUpdateBuffer(cb, g->dev.b, 0, sizeof(w), w);
}

static void
draw_indirect_gpu(VkCommandBuffer cb, struct gfx *g, void *data)
{
	struct ind_gpu *x = data;
	bind_vb(cb, 0, 768);
	vkCmdBindIndexBuffer(cb, vbuf.b, 512, VK_INDEX_TYPE_UINT16);
	push(cb, g, 0, 0, 0.3f, 0.5f);
	vkCmdDrawIndirect(cb, x->dev.b, 0, 2, 16);
	bind_vb(cb, 256, 768);
	push(cb, g, 0.2f, 0.2f, 0.4f, 0.5f);
	vkCmdDrawIndexedIndirect(cb, x->dev.b, 64, 1, 20);
	/* count 1 of max 2, arguments from the host buffer: only the first draw */
	bind_vb(cb, 0, 768);
	push(cb, g, -0.3f, 0.4f, 0.2f, 0.5f);
	DrawIndirectCount(cb, x->count.b, 16, x->count.b, 0, 2, 16);
}

/* fans (non-indexed and 8-bit indexed) and strips, with the provoking vertex last and flat colours: wgvk
 * rewrites the indices on the CPU */
static void
draw_fan(VkCommandBuffer cb, struct gfx *g, void *data)
{
	bind_vb(cb, 256, 1024);		/* the quad's 4 vertices, 4 colours */
	push(cb, g, -0.5f, -0.5f, 0.8f, 0.5f);
	vkCmdDraw(cb, 4, 1, 0, 0);	/* fan 0 1 2 3: triangles (0,1,2) (0,2,3) */
	vkCmdBindIndexBuffer(cb, vbuf.b, 1100, VK_INDEX_TYPE_UINT8_EXT);
	push(cb, g, 0.5f, -0.5f, 0.8f, 0.5f);
	vkCmdDrawIndexed(cb, 4, 1, 0, 0, 0);	/* 8-bit indices 3 2 1 0 */
}

/* the same fans and strips with indices in device-local memory written by the GPU: wgvk rewrites them in a
 * compute pass ahead of the render pass */
static struct buf gpu_idx;

static void
pre_gpu_idx(VkCommandBuffer cb, void *data)
{
	uint16_t idx[8] = { 0, 1, 2, 3, 3, 2, 1, 0 };
	vkCmdUpdateBuffer(cb, gpu_idx.b, 0, sizeof(idx), idx);
	/* indirect arguments written by the GPU too: an indexed draw of indices 4..7, a draw of vertices 0..3 */
	uint32_t args[9] = { 4, 1, 4, 0, 0, 0, 0, 0, 0 };
	vkCmdUpdateBuffer(cb, gpu_idx.b, 64, 20, args);
	uint32_t args2[4] = { 4, 1, 0, 0 };
	vkCmdUpdateBuffer(cb, gpu_idx.b, 96, 16, args2);
}

static void
draw_fan_gpu(VkCommandBuffer cb, struct gfx *g, void *data)
{
	bind_vb(cb, 256, 1024);
	vkCmdBindIndexBuffer(cb, gpu_idx.b, 0, VK_INDEX_TYPE_UINT16);
	push(cb, g, -0.5f, -0.5f, 0.8f, 0.5f);
	vkCmdDrawIndexed(cb, 4, 1, 0, 0, 0);
	push(cb, g, 0.5f, 0.5f, 0.8f, 0.5f);
	vkCmdDrawIndexed(cb, 4, 1, 4, 0, 0);
	push(cb, g, -0.5f, 0.5f, 0.5f, 0.5f);
	vkCmdDrawIndexedIndirect(cb, gpu_idx.b, 64, 1, 20);
	push(cb, g, 0.5f, -0.5f, 0.5f, 0.5f);
	vkCmdDrawIndirect(cb, gpu_idx.b, 96, 1, 16);
}

static void
draw_strip(VkCommandBuffer cb, struct gfx *g, void *data)
{
	bind_vb(cb, 256, 1024);
	push(cb, g, -0.5f, 0.5f, 0.8f, 0.5f);
	vkCmdDraw(cb, 4, 1, 0, 0);
	vkCmdBindIndexBuffer(cb, vbuf.b, 512, VK_INDEX_TYPE_UINT16);
	push(cb, g, 0.5f, 0.5f, 0.8f, 0.5f);
	vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);	/* a list, last vertex */
}

/* vertex pulling: stride 7, R16G16_SNORM + R8G8B8_UNORM, an instance binding with divisor 2 */
static void
draw_pull(VkCommandBuffer cb, struct gfx *g, void *data)
{
	VkBuffer b[2] = { vbuf.b, vbuf.b };
	VkDeviceSize o[2] = { 1300, 1400 };
	vkCmdBindVertexBuffers(cb, 0, 2, b, o);
	vkCmdDraw(cb, 3, 4, 0, 0);	/* 4 instances, 2 offsets */
}

static void
draw_points(VkCommandBuffer cb, struct gfx *g, void *data)
{
	bind_vb(cb, 0, 1024);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdBindIndexBuffer(cb, vbuf.b, 512, VK_INDEX_TYPE_UINT16);
	bind_vb(cb, 256, 1024);
	vkCmdDrawIndexed(cb, 4, 1, 0, 0, 0);
}

static void
draw_bigpc(VkCommandBuffer cb, struct gfx *g, void *data)
{
	float pcv[32] = { 0 };
	for (int i = 0; i < 4; i++)
		pcv[12 + i] = i == 3 ? 1.0f : 0.5f + 0.15f * i;	/* v[3]: a tint */
	pcv[28] = -0.3f; pcv[29] = 0.1f; pcv[30] = 0.6f;	/* v[7]: offset, scale */
	vkCmdPushConstants(cb, g->pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pcv), pcv);
	bind_vb(cb, 0, 1024);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdDraw(cb, 3, 1, 0, 3);	/* gl_BaseInstance 3: moved right */
}

static void
draw_depth(VkCommandBuffer cb, struct gfx *g, void *data)
{
	bind_vb(cb, 0, 1024);
	push(cb, g, -0.2f, 0, 1, 0.6f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	bind_vb(cb, 256, 1024);
	vkCmdBindIndexBuffer(cb, vbuf.b, 512, VK_INDEX_TYPE_UINT16);
	push(cb, g, 0.2f, 0, 1.2f, 0.4f);	/* nearer: in front */
	vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
	push(cb, g, 0.3f, 0.3f, 1, 0.8f);	/* farther: hidden behind both */
	vkCmdDraw(cb, 3, 1, 0, 0);
}

struct ubo_data {
	struct gfx_opts *o;
	VkDescriptorSet set;
};

static void
draw_ubo(VkCommandBuffer cb, struct gfx *g, void *data)
{
	struct ubo_data *u = data;
	if (!u->set) {
		u->set = make_set(g->dsl);
		write_buffer_desc(u->set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, vbuf.b, 2048, 16);
	}
	bind_vb(cb, 0, 1024);
	uint32_t off = 0;
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, 1, &u->set, 1, &off);
	push(cb, g, -0.4f, 0, 0.6f, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	off = 256;
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, 1, &u->set, 1, &off);
	push(cb, g, 0.4f, 0, 0.6f, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
}

static void
case_ubo_dyn(void)
{
	float t0[4] = { 1, 0.5f, 0.25f, 1 }, t1[4] = { 0.25f, 1, 1, 1 };
	memcpy((char *)vbuf.map + 2048, t0, 16);
	memcpy((char *)vbuf.map + 2048 + 256, t1, 16);
	struct gfx_opts o = { .fs = "ubo.frag", .desc = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
			      .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	struct ubo_data u = { &o, VK_NULL_HANDLE };
	render("ubo-dyn", &o, draw_ubo, &u);
}

struct tex_data {
	VkImageView view;
	VkSampler sampler;
	VkDescriptorSet set;
};

static void
draw_tex(VkCommandBuffer cb, struct gfx *g, void *data)
{
	struct tex_data *t = data;
	t->set = make_set(g->dsl);
	vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->set, .dstBinding = 0, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &(VkDescriptorImageInfo){ t->sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } },
		0, NULL);
	bind_vb(cb, 256, 1024);
	vkCmdBindIndexBuffer(cb, vbuf.b, 512, VK_INDEX_TYPE_UINT16);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, 1, &t->set, 0, NULL);
	push(cb, g, 0, 0, 1.6f, 0.5f);
	vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
}

static void
case_texture(void)
{
	/* a 10x7 checker image, uploaded from a buffer with rows of 10 texels (40 bytes) */
	const uint32_t tw = 10, th = 7;
	struct img tex = make_image(tw, th, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
	struct buf up = make_buffer(tw * th * 4, true);
	uint8_t *p = up.map;
	for (uint32_t y = 0; y < th; y++)
		for (uint32_t x = 0; x < tw; x++) {
			uint8_t *q = p + 4 * (y * tw + x);
			bool on = (x + y) & 1;
			q[0] = on ? 240 : 20 + 20 * x;
			q[1] = on ? 200 : 30 * y;
			q[2] = on ? 40 : 128;
			q[3] = 255;
		}
	VkCommandBuffer cb = begin();
	layout(cb, &tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyBufferToImage(cb, up.b, tex.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkBufferImageCopy){
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { tw, th, 1 } });
	layout(cb, &tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	submit_wait(cb);
	struct tex_data t = { .view = tex.v };
	CHECK(vkCreateSampler(dev, &(VkSamplerCreateInfo){
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = 0.25f }, NULL, &t.sampler));
	struct gfx_opts o = { .fs = "tex.frag", .desc = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			      .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	render("texture", &o, draw_tex, &t);
	vkDestroySampler(dev, t.sampler, NULL);
	free_buffer(&up);
	free_image(&tex);
}

static void
case_compute(void)
{
	/* in at 0, out at 1024 (256 words), k at 2048: one memory, one GPUBuffer in wgvk */
	struct buf b = make_buffer(4096, true);
	uint32_t *w = b.map;
	for (uint32_t i = 0; i < 256; i++)
		w[i] = i * 7 + 3;
	w[512] = 1000;
	VkDescriptorSetLayoutBinding bd[3] = {
		{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
	};
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bd },
		NULL, &dsl));
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl },
		NULL, &pl));
	VkShaderModule cs = shader("square.comp");
	VkPipeline p;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs,
			   "main" }, .layout = pl }, NULL, &p));
	VkDescriptorSet s = make_set(dsl);
	write_buffer_desc(s, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b.b, 0, 1024);
	write_buffer_desc(s, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b.b, 1024, 1024);
	write_buffer_desc(s, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b.b, 2048, 16);
	VkCommandBuffer cb = begin();
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s, 0, NULL);
	vkCmdDispatch(cb, 4, 1, 1);
	submit_wait(cb);
	save_bytes((char *)b.map + 1024, 1024, "compute");
	/* indirect: groups from the buffer, a second run over the output */
	w[768] = 2; w[769] = 1; w[770] = 1;
	cb = begin();
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s, 0, NULL);
	vkCmdDispatchIndirect(cb, b.b, 3072);
	submit_wait(cb);
	save_bytes((char *)b.map + 1024, 1024, "compute-indirect");
	vkDestroyPipeline(dev, p, NULL);
	vkDestroyShaderModule(dev, cs, NULL);
	vkDestroyPipelineLayout(dev, pl, NULL);
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	free_buffer(&b);
}

/* the buffer heap: 21 storage buffers in one compute stage, from two memory objects */
static void
case_heap(void)
{
	struct buf m0 = make_buffer(65536, true), m1 = make_buffer(65536, true);
	for (uint32_t i = 0; i < 16384; i++) {
		((uint32_t *)m0.map)[i] = i * 3 + 1;
		((uint32_t *)m1.map)[i] = i * 5 + 7;
	}
	VkDescriptorSetLayoutBinding bd[21];
	for (int i = 0; i < 21; i++)
		bd[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 21, .pBindings = bd },
		NULL, &dsl));
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl },
		NULL, &pl));
	VkShaderModule cs = shader("heap.comp");
	VkPipeline p;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs,
			   "main" }, .layout = pl }, NULL, &p));
	VkDescriptorPool hp;
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1,
		.pPoolSizes = &(VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 21 } }, NULL, &hp));
	VkDescriptorSet set;
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = hp, .descriptorSetCount = 1,
		.pSetLayouts = &dsl }, &set));
	/* buffers 0..9 in m0, 10..19 in m1 (1 KiB apart, 512 bytes each), the output at the end of m0 */
	for (int i = 0; i < 20; i++)
		write_buffer_desc(set, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, i < 10 ? m0.b : m1.b, (i % 10) * 1024 + 256 * (i & 1),
				  512);
	write_buffer_desc(set, 20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, m0.b, 32768, 256);
	VkCommandBuffer cb = begin();
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
	vkCmdDispatch(cb, 1, 1, 1);
	submit_wait(cb);
	save_bytes((char *)m0.map + 32768, 256, "heap");
	save_bytes(m1.map, 12288, "heap-mem1");
	vkDestroyPipeline(dev, p, NULL);
	vkDestroyShaderModule(dev, cs, NULL);
	vkDestroyDescriptorPool(dev, hp, NULL);
	vkDestroyPipelineLayout(dev, pl, NULL);
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	free_buffer(&m0);
	free_buffer(&m1);
}

static void
draw_heap(VkCommandBuffer cb, struct gfx *g, void *data)
{
	struct buf *m = data;
	VkDescriptorSet set = make_set(g->dsl);
	for (int i = 0; i < 20; i++)
		write_buffer_desc(set, i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, m[i / 10].b, (i % 10) * 1024, 512);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->pl, 0, 1, &set, 0, NULL);
	bind_vb(cb, 0, 1024);
	push(cb, g, 0, 0, 1, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
}

static void
case_heap_draw(void)
{
	struct buf m[2] = { make_buffer(65536, true), make_buffer(65536, true) };
	for (uint32_t i = 0; i < 16384; i++) {
		((uint32_t *)m[0].map)[i] = i * 3 + 1;
		((uint32_t *)m[1].map)[i] = i * 5 + 7;
	}
	struct gfx_opts o = { .fs = "heap.frag", .desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .ndesc = 20,
			      .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	render("heap-draw", &o, draw_heap, m);
	free_buffer(&m[0]);
	free_buffer(&m[1]);
}

/* a feedback loop: the second draw samples the image the pass renders to (the left half, written by the
 * first draw) and writes the right half */
static void
case_feedback(void)
{
	struct img color = make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM,
				      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	struct gfx_opts o1 = { .fs = "color.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	struct gfx_opts o2 = { .fs = "fb.frag", .desc = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			       .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	struct gfx g1 = make_gfx(&o1), g2 = make_gfx(&o2);
	VkSampler smp;
	CHECK(vkCreateSampler(dev, &(VkSamplerCreateInfo){ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE }, NULL, &smp));
	VkDescriptorSet set = make_set(g2.dsl);
	vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &(VkDescriptorImageInfo){ smp, color.v, VK_IMAGE_LAYOUT_GENERAL } }, 0, NULL);
	VkCommandBuffer cb = begin();
	layout(cb, &color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	VkRenderingAttachmentInfoKHR ca = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR, .imageView = color.v,
		.imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = { 0.1f, 0.1f, 0.1f, 1 } };
	BeginRendering(cb, &(VkRenderingInfoKHR){
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1,
		.colorAttachmentCount = 1, .pColorAttachments = &ca });
	viewport(cb, false);
	bind_vb(cb, 0, 1024);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g1.p);
	push(cb, &g1, -0.5f, 0, 0.5f, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	/* (a by-region barrier between the draws, as a feedback loop asks) */
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			     VK_DEPENDENCY_BY_REGION_BIT, 1, &(VkMemoryBarrier){ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			     .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT },
			     0, NULL, 0, NULL);
	bind_vb(cb, 256, 1024);
	vkCmdBindIndexBuffer(cb, vbuf.b, 512, VK_INDEX_TYPE_UINT16);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g2.p);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g2.pl, 0, 1, &set, 0, NULL);
	push(cb, &g2, 0.5f, 0, 0.9f, 0.5f);
	vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
	EndRendering(cb);
	layout(cb, &color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	submit_wait(cb);
	save_image(&color, "feedback");
	vkDestroySampler(dev, smp, NULL);
	free_gfx(&g1);
	free_gfx(&g2);
	free_image(&color);
}

/* occlusion queries: counted over draws, one query across a split pass (a feedback-free scope split by an
 * SSBO-writing draw), results copied on the GPU (vkCmdCopyQueryPoolResults) and read on the CPU */
static void
case_queries(void)
{
	VkQueryPool qp;
	CHECK(vkCreateQueryPool(dev, &(VkQueryPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
							     .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 4 }, NULL, &qp));
	struct img color = make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	struct img depth = make_image(W, H, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	struct gfx_opts o = { .fs = "color.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .depth = true };
	struct gfx_opts os = { .fs = "ssbo.frag", .desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .depth = true,
			       .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	struct gfx g = make_gfx(&o), gsb = make_gfx(&os);
	struct buf m = make_buffer(8192, true), res = make_buffer(256, true);
	memset(m.map, 0, 8192);
	memset(res.map, 0xab, 256);	/* (what the copies do not write stays this) */
	memcpy(m.map, tri_verts, sizeof(tri_verts));
	VkDescriptorSet ss = make_set(gsb.dsl);
	write_buffer_desc(ss, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, m.b, 4096, 256);
	VkCommandBuffer cb = begin();
	vkCmdResetQueryPool(cb, qp, 0, 4);
	layout(cb, &color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	layout(cb, &depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	begin_dyn(cb, &color, &depth, VK_ATTACHMENT_LOAD_OP_CLEAR, black);
	viewport(cb, false);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.p);
	bind_vb(cb, 0, 1024);
	push(cb, &g, -0.2f, 0, 1, 0.4f);	/* a near triangle */
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdBeginQuery(cb, qp, 0, 0);
	bind_vb(cb, 256, 1024);
	vkCmdBindIndexBuffer(cb, vbuf.b, 512, VK_INDEX_TYPE_UINT16);
	push(cb, &g, 0.2f, 0, 1.2f, 0.6f);	/* behind it: partly hidden */
	vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
	/* a draw that reads vertices from the memory it writes (a snapshot), then one that reads that memory as
	 * vertices after the writes (a split inside the query) */
	VkBuffer vb2[2] = { m.b, m.b };
	VkDeviceSize vo2[2] = { 0, 2048 };
	vkCmdBindVertexBuffers(cb, 0, 2, vb2, vo2);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gsb.p);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gsb.pl, 0, 1, &ss, 0, NULL);
	push(cb, &gsb, 0.5f, 0.5f, 0.3f, 0.3f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.p);
	VkDeviceSize vo3[2] = { 4096 - 1024, 2048 };	/* (reads the SSBO memory as vertices: a scope conflict) */
	vkCmdBindVertexBuffers(cb, 0, 2, vb2, vo3);
	push(cb, &g, -0.5f, -0.5f, 0.3f, 0.3f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndQuery(cb, qp, 0);
	vkCmdBeginQuery(cb, qp, 1, 0);
	bind_vb(cb, 0, 1024);
	push(cb, &g, 0.5f, -0.4f, 0.4f, 0.1f);	/* in front of everything */
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndQuery(cb, qp, 1);
	EndRendering(cb);
	vkCmdCopyQueryPoolResults(cb, qp, 0, 2, res.b, 0, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
				  VK_QUERY_RESULT_WAIT_BIT);
	/* (query 2 is never begun: lavapipe writes its value although the spec says not to without PARTIAL, so
	 * only its availability is compared: a copy of 1 query with the availability alone) */
	vkCmdCopyQueryPoolResults(cb, qp, 0, 2, res.b, 64, 8, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	vkCmdCopyQueryPoolResults(cb, qp, 2, 1, res.b, 80, 8, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT);
	layout(cb, &color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	submit_wait(cb);
	uint64_t cpu[4] = { 0 };
	vkGetQueryPoolResults(dev, qp, 0, 2, sizeof(cpu), cpu, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	save_bytes(res.map, 96, "queries-gpu");
	save_bytes(cpu, sizeof(cpu), "queries-cpu");
	save_image(&color, "queries");
	printf("queries: gpu %llu %llu, %llu %llu; cpu %llu %llu, %llu %llu\n",
	       (unsigned long long)((uint64_t *)res.map)[0], (unsigned long long)((uint64_t *)res.map)[1],
	       (unsigned long long)((uint64_t *)res.map)[2], (unsigned long long)((uint64_t *)res.map)[3],
	       (unsigned long long)cpu[0], (unsigned long long)cpu[1], (unsigned long long)cpu[2], (unsigned long long)cpu[3]);
	vkDestroyQueryPool(dev, qp, NULL);
	free_gfx(&g);
	free_gfx(&gsb);
	free_image(&color);
	free_image(&depth);
	free_buffer(&m);
	free_buffer(&res);
}

static void
case_depthread(void)
{
	struct img d = make_image(16, 16, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
	struct buf out = make_buffer(1024, true);
	VkDescriptorSetLayoutBinding bd[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
	};
	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bd }, NULL, &dsl));
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl }, NULL, &pl));
	VkShaderModule cs = shader("depthread.comp");
	VkPipeline p;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" },
		.layout = pl }, NULL, &p));
	VkSampler smp;
	CHECK(vkCreateSampler(dev, &(VkSamplerCreateInfo){ .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO }, NULL, &smp));
	VkDescriptorSet set = make_set(dsl);
	vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &(VkDescriptorImageInfo){ smp, d.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } }, 0, NULL);
	write_buffer_desc(set, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out.b, 0, 1024);
	VkCommandBuffer cb = begin();
	layout(cb, &d, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdClearDepthStencilImage(cb, d.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &(VkClearDepthStencilValue){ 0.25f, 0 }, 1,
				    &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 });
	layout(cb, &d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
	vkCmdDispatch(cb, 2, 2, 1);
	submit_wait(cb);
	save_bytes(out.map, 1024, "depthread");
	vkDestroySampler(dev, smp, NULL);
	vkDestroyPipeline(dev, p, NULL);
	vkDestroyShaderModule(dev, cs, NULL);
	vkDestroyPipelineLayout(dev, pl, NULL);
	vkDestroyDescriptorSetLayout(dev, dsl, NULL);
	free_image(&d);
	free_buffer(&out);
}

/* depth uploads and readbacks WebGPU does not copy (D24: neither way; D32: not into the texture): a region of a
 * 20x12 image written from a buffer, then the whole image read back */
static void
depthcopy(VkFormat f, const char *name)
{
	const uint32_t iw = 20, ih = 12;
	struct img d = make_image(iw, ih, f, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	struct buf up = make_buffer(4096, true), down = make_buffer(4096, true);
	uint32_t *u = up.map;
	for (uint32_t i = 0; i < 1024; i++) {
		if (f == VK_FORMAT_D32_SFLOAT) {
			float v = (float)(i % 97) / 97.0f;
			memcpy(&u[i], &v, 4);
		} else {
			u[i] = (i * 40503u) & 0xffffff;
		}
	}
	memset(down.map, 0xab, 4096);
	VkCommandBuffer cb = begin();
	layout(cb, &d, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkImageAspectFlags asp = VK_IMAGE_ASPECT_DEPTH_BIT;
	vkCmdClearDepthStencilImage(cb, d.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &(VkClearDepthStencilValue){ 0.5f, 0 }, 1,
				    &(VkImageSubresourceRange){ f == VK_FORMAT_D32_SFLOAT ? asp : asp | VK_IMAGE_ASPECT_STENCIL_BIT,
								0, 1, 0, 1 });
	barrier(cb);
	/* rows of 9 texels in the buffer, a 7x5 region at (3, 4), from word 8 */
	vkCmdCopyBufferToImage(cb, up.b, d.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkBufferImageCopy){
		.bufferOffset = 32, .bufferRowLength = 9, .imageSubresource = { asp, 0, 0, 1 },
		.imageOffset = { 3, 4, 0 }, .imageExtent = { 7, 5, 1 } });
	layout(cb, &d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cb, d.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down.b, 1, &(VkBufferImageCopy){
		.bufferOffset = 64, .imageSubresource = { asp, 0, 0, 1 }, .imageExtent = { iw, ih, 1 } });
	submit_wait(cb);
	save_bytes(down.map, 64 + iw * ih * 4 + 64, name);
	free_image(&d);
	free_buffer(&up);
	free_buffer(&down);
}

static void
case_copies(void)
{
	struct buf a = make_buffer(4096, true), d = make_buffer(4096, false), r = make_buffer(4096, true);
	uint32_t *w = a.map;
	for (uint32_t i = 0; i < 1024; i++)
		w[i] = 0x01020304u * (i + 1);
	struct img i1 = make_image(16, 16, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
	struct img i2 = make_image(16, 16, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
	struct img i3 = make_image(32, 32, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	struct img i4 = make_image(5, 3, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
	VkCommandBuffer cb = begin();
	vkCmdFillBuffer(cb, d.b, 0, VK_WHOLE_SIZE, 0x11111111);	/* nothing left undefined */
	barrier(cb);
	vkCmdCopyBuffer(cb, a.b, d.b, 1, &(VkBufferCopy){ 0, 0, 2048 });
	vkCmdFillBuffer(cb, d.b, 2048, 512, 0xdeadbeef);
	vkCmdFillBuffer(cb, d.b, 2560, 256, 0);
	uint32_t upd[16];
	for (int i = 0; i < 16; i++)
		upd[i] = 0xa0000000u + i;
	vkCmdUpdateBuffer(cb, d.b, 3072, sizeof(upd), upd);
	/* a size that is not a whole number of words (VK-16): the 2 bytes after the 32 keep the fill's */
	vkCmdCopyBuffer(cb, a.b, d.b, 1, &(VkBufferCopy){ 8, 3328, 34 });
	barrier(cb);
	/* buffer -> image (row pitch 64 bytes), image -> image, blit 2x, image -> buffer */
	layout(cb, &i1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyBufferToImage(cb, a.b, i1.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkBufferImageCopy){
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 16, 16, 1 } });
	layout(cb, &i1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	layout(cb, &i2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdClearColorImage(cb, i2.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			     &(VkClearColorValue){ .float32 = { 0.5f, 0, 0, 1 } }, 1,
			     &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
	barrier(cb);
	vkCmdCopyImage(cb, i1.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i2.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		       &(VkImageCopy){ { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 2, 3, 0 },
				       { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 5, 1, 0 }, { 8, 9, 1 } });
	layout(cb, &i3, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdClearColorImage(cb, i3.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			     &(VkClearColorValue){ .float32 = { 0, 0, 0.5f, 1 } }, 1,
			     &(VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
	vkCmdBlitImage(cb, i1.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i3.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		       &(VkImageBlit){ { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { 8, 8, 1 } },
				       { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 4, 4, 0 }, { 20, 20, 1 } } },
		       VK_FILTER_NEAREST);
	layout(cb, &i2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	layout(cb, &i3, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	/* rows that are not whole words, both ways (VK-16): 5 x 3 texels of 8 bits, tight, up and back into d */
	layout(cb, &i4, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyBufferToImage(cb, a.b, i4.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &(VkBufferImageCopy){
		.bufferOffset = 1, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 5, 3, 1 } });
	layout(cb, &i4, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkCmdCopyImageToBuffer(cb, i4.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.b, 1, &(VkBufferImageCopy){
		.bufferOffset = 3585, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 5, 3, 1 } });
	barrier(cb);
	vkCmdCopyBuffer(cb, d.b, r.b, 1, &(VkBufferCopy){ 0, 0, 4096 });
	submit_wait(cb);
	save_bytes(r.map, 4096, "copies-buffer");
	save_image(&i2, "copies-image");
	save_image(&i3, "copies-blit");
	free_image(&i1);
	free_image(&i2);
	free_image(&i3);
	free_image(&i4);
	free_buffer(&a);
	free_buffer(&d);
	free_buffer(&r);
}

static void
draw_clear_att(VkCommandBuffer cb, struct gfx *g, void *data)
{
	bind_vb(cb, 0, 1024);
	push(cb, g, 0, 0, 1, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdClearAttachments(cb, 1, &(VkClearAttachment){ VK_IMAGE_ASPECT_COLOR_BIT, 0,
							   { .color.float32 = { 1, 1, 0, 1 } } },
			      2, (VkClearRect[]){ { { { 4, 4 }, { 10, 8 } }, 0, 1 }, { { { 40, 30 }, { 20, 10 } }, 0, 1 } });
	push(cb, g, 0.5f, 0.5f, 0.3f, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
}

static void
case_flip(void)
{
	struct gfx_opts o = { .fs = "color.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .cull_back = true };
	struct img color = make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	struct gfx g = make_gfx(&o);
	VkCommandBuffer cb = begin();
	layout(cb, &color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	begin_dyn(cb, &color, NULL, VK_ATTACHMENT_LOAD_OP_CLEAR, black);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.p);
	bind_vb(cb, 0, 1024);
	/* both windings, half with a flipped viewport: back-face culling must agree with lavapipe */
	viewport(cb, true);
	push(cb, &g, -0.4f, 0.2f, 0.5f, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	viewport(cb, false);
	push(cb, &g, 0.4f, 0.2f, 0.5f, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	push(cb, &g, 0.4f, -0.5f, -0.5f, 0.5f);	/* mirrored: the other winding */
	vkCmdDraw(cb, 3, 1, 0, 0);
	viewport(cb, true);
	push(cb, &g, -0.4f, -0.5f, -0.5f, 0.5f);
	vkCmdDraw(cb, 3, 1, 0, 0);
	EndRendering(cb);
	layout(cb, &color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	submit_wait(cb);
	save_image(&color, "flip-y");
	free_gfx(&g);
	free_image(&color);
}

/* One memory: vertices, a UBO (draw 1), an SSBO written by the fragment shader (draw 2). In WebGPU the
 * two draws cannot share a pass (draw 1 reads the GPUBuffer that draw 2 writes): the pass is split. */
static void
case_split(bool selfdep)
{
	struct buf m = make_buffer(8192, true);
	memcpy(m.map, tri_verts, sizeof(tri_verts));
	float tint[4] = { 0.5f, 1, 0.5f, 1 };
	memcpy((char *)m.map + 1024, tint, 16);
	memset((char *)m.map + 4096, 0, 256);
	memset((char *)m.map + 2048, 0, 8);
	struct gfx_opts ou = { .fs = "ubo.frag", .desc = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			       .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	struct gfx_opts os = { .fs = "ssbo.frag", .desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			       .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	struct gfx gu = make_gfx(&ou), gs = make_gfx(&os);
	VkDescriptorSet su = make_set(gu.dsl), ss = make_set(gs.dsl);
	write_buffer_desc(su, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, m.b, 1024, 16);
	write_buffer_desc(ss, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, m.b, 4096, 256);
	struct img color = make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	VkCommandBuffer cb = begin();
	layout(cb, &color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	begin_dyn(cb, &color, NULL, VK_ATTACHMENT_LOAD_OP_CLEAR, black);
	viewport(cb, false);
	VkBuffer vb[2] = { m.b, m.b };
	VkDeviceSize vo[2] = { 0, 2048 };
	vkCmdBindVertexBuffers(cb, 0, 2, vb, vo);
	for (int round = 0; round < 3; round++) {
		if (!selfdep) {
			vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gu.p);
			vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gu.pl, 0, 1, &su, 0, NULL);
			push(cb, &gu, -0.5f + 0.1f * round, 0, 0.4f, 0.5f);
			vkCmdDraw(cb, 3, 1, 0, 0);
		}
		/* the SSBO draw reads its vertices from the memory it writes: a snapshot of the vertex range */
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gs.p);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gs.pl, 0, 1, &ss, 0, NULL);
		push(cb, &gs, 0.5f - 0.1f * round, 0, 0.4f, 0.5f);
		vkCmdDraw(cb, 3, 1, 0, 0);
	}
	EndRendering(cb);
	layout(cb, &color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	submit_wait(cb);
	save_image(&color, selfdep ? "selfdep" : "split");
	save_bytes((char *)m.map + 4096, 4, selfdep ? "selfdep-count" : "split-count");
	free_gfx(&gu);
	free_gfx(&gs);
	free_image(&color);
	free_buffer(&m);
}

/* ---- comparing two runs */

static int
compare(const char *a, const char *b, const char *name, const char *ext, bool image)
{
	char pa[512], pb[512];
	snprintf(pa, sizeof(pa), "%s/%s.%s", a, name, ext);
	snprintf(pb, sizeof(pb), "%s/%s.%s", b, name, ext);
	FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
	if (!fa || !fb) {
		printf("FAIL %s: missing %s\n", name, !fa ? pa : pb);
		if (fa) fclose(fa);
		if (fb) fclose(fb);
		return 1;
	}
	static uint8_t da[1 << 20], db[1 << 20];
	size_t na = fread(da, 1, sizeof(da), fa), nb = fread(db, 1, sizeof(db), fb);
	fclose(fa);
	fclose(fb);
	if (na != nb) {
		printf("FAIL %s: sizes %zu vs %zu\n", name, na, nb);
		return 1;
	}
	/* images: a channel may differ by 2 (unorm rounding, interpolation); a pixel that differs more is wrong.
	 * Edge pixels of triangles may differ by rasterisation rules: allow 0.5% of pixels. */
	size_t bad = 0, first = SIZE_MAX, px = image ? na / 4 : na;
	for (size_t i = 0; i < px; i++) {
		bool diff = false;
		if (image) {
			for (int c = 0; c < 4; c++)
				diff |= abs((int)da[4 * i + c] - (int)db[4 * i + c]) > 2;
		} else {
			diff = da[i] != db[i];
		}
		if (diff) {
			bad++;
			if (first == SIZE_MAX)
				first = i;
		}
	}
	size_t allowed = image ? px / 200 : 0;
	if (bad > allowed) {
		if (image)
			printf("FAIL %s: %zu of %zu pixels differ; first at (%zu,%zu): %u %u %u %u vs %u %u %u %u\n", name,
			       bad, px, first % W, first / W, da[4 * first], da[4 * first + 1], da[4 * first + 2],
			       da[4 * first + 3], db[4 * first], db[4 * first + 1], db[4 * first + 2], db[4 * first + 3]);
		else
			printf("FAIL %s: %zu of %zu bytes differ; first at %zu: %02x vs %02x\n", name, bad, px, first,
			       da[first], db[first]);
		return 1;
	}
	printf("ok %s: %zu %s, %zu differ (allowed %zu)\n", name, px, image ? "pixels" : "bytes", bad, allowed);
	return 0;
}

static const struct {
	const char *name;
	const char *files[3];	/* "name.ext" without the directory; .rgba are images */
	bool xfail;		/* run only when named */
} outputs[] = {
	{ "clear", { "clear.rgba" } },
	{ "triangle", { "triangle.rgba" } },
	{ "secondary", { "secondary.rgba" } },
	{ "indexed", { "indexed.rgba" } },
	{ "instanced", { "instanced.rgba" } },
	{ "indirect", { "indirect.rgba" } },
	{ "indirect-gpu", { "indirect-gpu.rgba" } },
	{ "fan-last", { "fan-last.rgba" } },
	{ "pull", { "pull.rgba" } },
	{ "gs-dup", { "gs-dup.rgba" } },
	{ "gs-quad", { "gs-quad.rgba" } },
	{ "points", { "points.rgba" }, true },	/* xfail: T3b's point expansion (see points.vert) */
	{ "bigpc", { "bigpc.rgba" } },
	{ "strip-last", { "strip-last.rgba" } },
	{ "fan-gpu", { "fan-gpu.rgba" } },
	{ "strip-gpu", { "strip-gpu.rgba" } },
	{ "depth", { "depth.rgba" } },
	{ "ubo-dyn", { "ubo-dyn.rgba" } },
	{ "texture", { "texture.rgba" } },
	{ "compute", { "compute.bin", "compute-indirect.bin" } },
	{ "heap", { "heap.bin", "heap-mem1.bin" } },
	{ "heap-draw", { "heap-draw.rgba" } },
	{ "feedback", { "feedback.rgba" } },
	{ "depthread", { "depthread.bin" } },
	{ "depthcopy", { "depthcopy-d32.bin", "depthcopy-d24.bin" } },
	{ "queries", { "queries.rgba", "queries-gpu.bin", "queries-cpu.bin" } },
	{ "copies", { "copies-buffer.bin", "copies-image.rgba", "copies-blit.rgba" } },
	{ "clear-att", { "clear-att.rgba" } },
	{ "flip-y", { "flip-y.rgba" } },
	{ "split", { "split.rgba", "split-count.bin" } },
	{ "selfdep", { "selfdep.rgba", "selfdep-count.bin" } },
};

static bool
wanted(const char *name, int argc, char **argv, int from)
{
	if (argc <= from) {
		for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++)
			if (!strcmp(outputs[i].name, name))
				return !outputs[i].xfail;
		return true;
	}
	for (int i = from; i < argc; i++)
		if (!strcmp(argv[i], name))
			return true;
	return false;
}

static void
run(const char *name)
{
	if (!strcmp(name, "clear")) {
		case_clear();
	} else if (!strcmp(name, "triangle")) {
		case_triangle();
	} else if (!strcmp(name, "secondary")) {
		case_secondary();
	} else if (!strcmp(name, "indexed")) {
		struct gfx_opts o = { .fs = "color.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
		render("indexed", &o, draw_indexed, NULL);
	} else if (!strcmp(name, "instanced")) {
		struct gfx_opts o = { .fs = "color.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
		render("instanced", &o, draw_instanced, NULL);
	} else if (!strcmp(name, "indirect")) {
		struct buf ind = make_buffer(256, true);
		uint32_t *w = ind.map;
		/* draw 0: 3 vertices, 2 instances; draw 1: 3 vertices from vertex 0, instance 2 only */
		uint32_t d0[4] = { 3, 2, 0, 0 }, d1[4] = { 3, 1, 0, 2 }, di[5] = { 6, 1, 0, 0, 0 };
		memcpy(w, d0, 16);
		memcpy(w + 4, d1, 16);
		memcpy(w + 16, di, 20);
		struct gfx_opts o = { .fs = "color.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
		render("indirect", &o, draw_indirect, &ind);
		free_buffer(&ind);
	} else if (!strcmp(name, "indirect-gpu")) {
		struct ind_gpu g = { make_buffer(256, false), make_buffer(256, true) };
		uint32_t *w = g.count.map;
		uint32_t cnt = 1, d0[4] = { 3, 3, 0, 0 }, d1[4] = { 3, 1, 0, 1 };
		w[0] = cnt;
		memcpy(w + 4, d0, 16);
		memcpy(w + 8, d1, 16);
		struct gfx_opts o = { .fs = "color.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
		pre_render = pre_indirect_gpu;
		render("indirect-gpu", &o, draw_indirect_gpu, &g);
		pre_render = NULL;
		free_buffer(&g.dev);
		free_buffer(&g.count);
	} else if (!strcmp(name, "gs-dup")) {
		struct gfx_opts o = { .fs = "color.frag", .gs = "dup.geom", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
		render("gs-dup", &o, draw_indexed, NULL);
	} else if (!strcmp(name, "gs-quad")) {
		struct gfx_opts o = { .fs = "color.frag", .gs = "quad.geom", .topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST };
		render("gs-quad", &o, draw_points, NULL);
	} else if (!strcmp(name, "pull")) {
		static const VkVertexInputBindingDescription vb[2] = { { 0, 7, VK_VERTEX_INPUT_RATE_VERTEX },
								       { 1, 8, VK_VERTEX_INPUT_RATE_INSTANCE } };
		static const VkVertexInputAttributeDescription va[3] = { { 0, 0, VK_FORMAT_R16G16_SNORM, 0 },
									 { 1, 0, VK_FORMAT_R8G8B8_UNORM, 4 },
									 { 2, 1, VK_FORMAT_R32G32_SFLOAT, 0 } };
		static const VkVertexInputBindingDivisorDescriptionEXT div = { 1, 2 };
		struct gfx_opts o = { .fs = "color.frag", .vs = "pull.vert", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
				      .nvb = 2, .vb = vb, .nva = 3, .va = va, .div = &div };
		render("pull", &o, draw_pull, NULL);
	} else if (!strcmp(name, "points")) {
		struct gfx_opts o = { .fs = "points.frag", .vs = "points.vert", .topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST };
		render("points", &o, draw_points, NULL);
	} else if (!strcmp(name, "bigpc")) {
		struct gfx_opts o = { .fs = "color.frag", .vs = "bigpc.vert", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
				      .pc_size = 128 };
		render("bigpc", &o, draw_bigpc, NULL);
	} else if (!strcmp(name, "fan-last")) {
		struct gfx_opts o = { .fs = "flat.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, .provoking_last = true };
		render("fan-last", &o, draw_fan, NULL);
	} else if (!strcmp(name, "fan-gpu") || !strcmp(name, "strip-gpu")) {
		bool fan = !strcmp(name, "fan-gpu");
		gpu_idx = make_buffer(256, false);	/* (device-local: the CPU does not see what the GPU wrote) */
		struct gfx_opts o = { .fs = "flat.frag", .provoking_last = true,
				      .topo = fan ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
		pre_render = pre_gpu_idx;
		render(name, &o, draw_fan_gpu, NULL);
		pre_render = NULL;
		free_buffer(&gpu_idx);
	} else if (!strcmp(name, "strip-last")) {
		struct gfx_opts o = { .fs = "flat.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, .provoking_last = true };
		render("strip-last", &o, draw_strip, NULL);
	} else if (!strcmp(name, "depth")) {
		struct gfx_opts o = { .fs = "color.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .depth = true };
		render("depth", &o, draw_depth, NULL);
	} else if (!strcmp(name, "ubo-dyn")) {
		case_ubo_dyn();
	} else if (!strcmp(name, "texture")) {
		case_texture();
	} else if (!strcmp(name, "queries")) {
		case_queries();
	} else if (!strcmp(name, "depthcopy")) {
		depthcopy(VK_FORMAT_D32_SFLOAT, "depthcopy-d32");
		depthcopy(VK_FORMAT_D24_UNORM_S8_UINT, "depthcopy-d24");
	} else if (!strcmp(name, "depthread")) {
		case_depthread();
	} else if (!strcmp(name, "feedback")) {
		case_feedback();
	} else if (!strcmp(name, "heap-draw")) {
		case_heap_draw();
	} else if (!strcmp(name, "heap")) {
		case_heap();
	} else if (!strcmp(name, "compute")) {
		case_compute();
	} else if (!strcmp(name, "copies")) {
		case_copies();
	} else if (!strcmp(name, "clear-att")) {
		struct gfx_opts o = { .fs = "color.frag", .topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
		render("clear-att", &o, draw_clear_att, NULL);
	} else if (!strcmp(name, "flip-y")) {
		case_flip();
	} else if (!strcmp(name, "split")) {
		case_split(false);
	} else if (!strcmp(name, "selfdep")) {
		case_split(true);
	}
}

int
main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "--compare")) {
		int failed = 0, n = 0;
		for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
			if (!wanted(outputs[i].name, argc, argv, 4))
				continue;
			for (int f = 0; f < 3 && outputs[i].files[f]; f++) {
				char name[128];
				snprintf(name, sizeof(name), "%s", outputs[i].files[f]);
				char *dot = strrchr(name, '.');
				*dot = 0;
				failed += compare(argv[2], argv[3], name, dot + 1, !strcmp(dot + 1, "rgba"));
				n++;
			}
		}
		printf("t2-test: %d outputs compared, %d differ\n", n, failed);
		return failed ? 1 : 0;
	}
	if (argc < 3) {
		fprintf(stderr, "usage: t2-test OUTDIR SPVDIR [CASE...] | --compare A B [CASE...]\n");
		return 2;
	}
	outdir = argv[1];
	spvdir = argv[2];
	mkdir(outdir, 0755);
	init();
	BeginRendering = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR");
	EndRendering = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(dev, "vkCmdEndRenderingKHR");
	DrawIndirectCount = (PFN_vkCmdDrawIndirectCountKHR)vkGetDeviceProcAddr(dev, "vkCmdDrawIndirectCountKHR");
	VkDescriptorPoolSize ps[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64 },
				      { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 64 },
				      { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 128 },
				      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 } };
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, .maxSets = 64, .poolSizeCount = 4,
		.pPoolSizes = ps }, NULL, &dpool));
	fill_vbuf();
	printf("device: %s\n", devname);
	for (unsigned i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
		if (!wanted(outputs[i].name, argc, argv, 3))
			continue;
		printf("case %s\n", outputs[i].name);
		fflush(stdout);
		run(outputs[i].name);
	}
	free_buffer(&vbuf);
	vkDestroyDescriptorPool(dev, dpool, NULL);
	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	return 0;
}
