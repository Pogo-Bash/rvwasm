/*
 * vkr-bench: what a venus command costs (T5). A compute pipeline whose shader adds its push constant to a
 * host-visible storage word; one command buffer with N x (vkCmdPushConstants + vkCmdDispatch(1,1,1)), submitted,
 * waited for, the word checked (= N), the whole repeated R times. Run it on any Vulkan driver: through venus
 * (guest side, venus -> vkr -> wgvk), on wgvk directly, on lavapipe.
 *
 *   vkr-bench SPV [N [R]]        SPV: add.comp compiled (gpu/webgpu/vkr/tests/add.comp)
 *
 * Prints per round: record time per command pair, submit -> fence time, and the check.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static double
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: vkr-bench SPV [N [ROUNDS]]\n");
		return 2;
	}
	const unsigned n = argc > 2 ? atoi(argv[2]) : 10000, rounds = argc > 3 ? atoi(argv[3]) : 5;
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 1;
	}
	static uint32_t code[65536];
	size_t code_size = fread(code, 1, sizeof(code), f);
	fclose(f);

	double t0 = now_ms();
	VkInstance inst;
	CHECK(vkCreateInstance(&(VkInstanceCreateInfo){ .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &(VkApplicationInfo){ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vkr-bench", .apiVersion = VK_API_VERSION_1_1 } }, NULL, &inst));
	uint32_t count = 1;
	VkPhysicalDevice pd;
	CHECK(vkEnumeratePhysicalDevices(inst, &count, &pd) < 0 ? VK_ERROR_UNKNOWN : VK_SUCCESS);
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &(VkDeviceCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &(float){ 1 } } },
		NULL, &dev));
	VkQueue q;
	vkGetDeviceQueue(dev, 0, 0, &q);
	printf("device: %s; instance + device in %.1f ms\n", props.deviceName, now_ms() - t0);

	/* the storage word: host-visible, coherent */
	VkBuffer buf;
	CHECK(vkCreateBuffer(dev, &(VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT }, NULL, &buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, buf, &req);
	uint32_t type = 0;
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	while (type < mp.memoryTypeCount && (!(req.memoryTypeBits & (1u << type)) ||
					     (mp.memoryTypes[type].propertyFlags & want) != want))
		type++;
	VkDeviceMemory mem;
	CHECK(vkAllocateMemory(dev, &(VkMemoryAllocateInfo){ .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size, .memoryTypeIndex = type }, NULL, &mem));
	CHECK(vkBindBufferMemory(dev, buf, mem, 0));
	volatile uint32_t *word;
	CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&word));

	VkDescriptorSetLayout dsl;
	CHECK(vkCreateDescriptorSetLayout(dev, &(VkDescriptorSetLayoutCreateInfo){
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
		.pBindings = &(VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
		VK_SHADER_STAGE_COMPUTE_BIT } }, NULL, &dsl));
	VkPipelineLayout pl;
	CHECK(vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1,
		.pPushConstantRanges = &(VkPushConstantRange){ VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 } }, NULL, &pl));
	VkShaderModule sm;
	CHECK(vkCreateShaderModule(dev, &(VkShaderModuleCreateInfo){ .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code_size, .pCode = code }, NULL, &sm));
	t0 = now_ms();
	VkPipeline pipe;
	CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &(VkComputePipelineCreateInfo){
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = sm, .pName = "main" } }, NULL, &pipe));
	printf("compute pipeline in %.1f ms\n", now_ms() - t0);
	VkDescriptorPool dp;
	CHECK(vkCreateDescriptorPool(dev, &(VkDescriptorPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &(VkDescriptorPoolSize){ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } },
		NULL, &dp));
	VkDescriptorSet ds;
	CHECK(vkAllocateDescriptorSets(dev, &(VkDescriptorSetAllocateInfo){ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl }, &ds));
	vkUpdateDescriptorSets(dev, 1, &(VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds,
		.descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &(VkDescriptorBufferInfo){ buf, 0, 256 } }, 0, NULL);
	VkCommandPool cp;
	CHECK(vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT }, NULL, &cp));
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 }, &cb));
	VkFence fence;
	CHECK(vkCreateFence(dev, &(VkFenceCreateInfo){ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }, NULL, &fence));

	int bad = 0;
	for (unsigned r = 0; r < rounds; r++) {
		word[0] = 0;
		double a = now_ms();
		CHECK(vkBeginCommandBuffer(cb, &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT }));
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
		const uint32_t one = 1;
		for (unsigned i = 0; i < n; i++) {
			vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &one);
			vkCmdDispatch(cb, 1, 1, 1);
		}
		CHECK(vkEndCommandBuffer(cb));
		double b = now_ms();
		CHECK(vkQueueSubmit(q, 1, &(VkSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
			.pCommandBuffers = &cb }, fence));
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 60ull * 1000 * 1000 * 1000));
		double c = now_ms();
		CHECK(vkResetFences(dev, 1, &fence));
		const uint32_t got = word[0];
		bad += got != n;
		printf("round %u: %u x (push constants + dispatch): record %.2f us/pair, submit -> fence %.1f ms; word %u %s\n",
		       r, n, (b - a) * 1e3 / n, c - b, got, got == n ? "ok" : "WRONG");
	}

	/* an empty submit's round trip: submit -> fence with no work */
	double e = now_ms();
	const unsigned trips = 100;
	for (unsigned i = 0; i < trips; i++) {
		CHECK(vkQueueSubmit(q, 0, NULL, fence));
		CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));
		CHECK(vkResetFences(dev, 1, &fence));
	}
	printf("empty submit -> fence: %.3f ms each (%u)\n", (now_ms() - e) / trips, trips);
	vkDeviceWaitIdle(dev);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(inst, NULL);
	printf("vkr-bench: %s\n", bad ? "FAIL" : "ok");
	return bad != 0;
}
