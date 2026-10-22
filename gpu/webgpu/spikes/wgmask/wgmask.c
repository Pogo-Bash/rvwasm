/*
 * wgmask: a Vulkan layer that makes a full Vulkan driver (lavapipe) look like a
 * Vulkan driver written on WebGPU would: only the device extensions named in
 * $WGMASK_EXTS (a file, one name per line) are reported, the core features in
 * $WGMASK_FEATS (a file of "name" lines) are reported as unsupported, the API
 * version is capped at $WGMASK_API (e.g. 1.1) and some limits are cut to
 * WebGPU's. It is a spike: it answers "what does zink (or ANGLE, Blender...)
 * still give or still need on such a device", nothing more. One instance, one
 * device; no thread safety.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define EXPORT __attribute__((visibility("default")))

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static VkInstance the_instance;

static char *allow[1024]; static int nallow = -1;
static char *maskf[256]; static int nmaskf = -1;

static int load_list(const char *env, char **out, int max)
{
	const char *p = getenv(env);
	int n = 0;
	if (!p) return -1;
	FILE *f = fopen(p, "r");
	if (!f) { fprintf(stderr, "wgmask: cannot open %s\n", p); return -1; }
	char line[256];
	while (n < max && fgets(line, sizeof line, f)) {
		char *s = line; while (*s == ' ' || *s == '\t') s++;
		if (*s == '#' || *s == '\n' || !*s) continue;
		s[strcspn(s, " \t\r\n#")] = 0;
		out[n++] = strdup(s);
	}
	fclose(f);
	return n;
}

static void load(void)
{
	if (nallow == -1) nallow = load_list("WGMASK_EXTS", allow, 1024);
	if (nmaskf == -1) { nmaskf = load_list("WGMASK_FEATS", maskf, 256); if (nmaskf < 0) nmaskf = 0; }
}

static int masked(const char *name)
{
	for (int i = 0; i < nmaskf; i++) if (!strcmp(maskf[i], name)) return 1;
	return 0;
}

static int allowed(const char *ext)
{
	if (nallow < 0) return 1;
	for (int i = 0; i < nallow; i++) if (!strcmp(allow[i], ext)) return 1;
	return 0;
}

#define M(s, f) if (masked(#f)) (s)->f = VK_FALSE
static void mask_core(VkPhysicalDeviceFeatures *f)
{
	M(f, robustBufferAccess); M(f, fullDrawIndexUint32); M(f, imageCubeArray); M(f, independentBlend);
	M(f, geometryShader); M(f, tessellationShader); M(f, sampleRateShading); M(f, dualSrcBlend); M(f, logicOp);
	M(f, multiDrawIndirect); M(f, drawIndirectFirstInstance); M(f, depthClamp); M(f, depthBiasClamp);
	M(f, fillModeNonSolid); M(f, depthBounds); M(f, wideLines); M(f, largePoints); M(f, alphaToOne);
	M(f, multiViewport); M(f, samplerAnisotropy); M(f, textureCompressionETC2); M(f, textureCompressionASTC_LDR);
	M(f, textureCompressionBC); M(f, occlusionQueryPrecise); M(f, pipelineStatisticsQuery);
	M(f, vertexPipelineStoresAndAtomics); M(f, fragmentStoresAndAtomics);
	M(f, shaderTessellationAndGeometryPointSize); M(f, shaderImageGatherExtended);
	M(f, shaderStorageImageExtendedFormats); M(f, shaderStorageImageMultisample);
	M(f, shaderStorageImageReadWithoutFormat); M(f, shaderStorageImageWriteWithoutFormat);
	M(f, shaderUniformBufferArrayDynamicIndexing); M(f, shaderSampledImageArrayDynamicIndexing);
	M(f, shaderStorageBufferArrayDynamicIndexing); M(f, shaderStorageImageArrayDynamicIndexing);
	M(f, shaderClipDistance); M(f, shaderCullDistance); M(f, shaderFloat64); M(f, shaderInt64); M(f, shaderInt16);
	M(f, shaderResourceResidency); M(f, shaderResourceMinLod); M(f, sparseBinding);
	M(f, sparseResidencyBuffer); M(f, sparseResidencyImage2D); M(f, sparseResidencyImage3D);
	M(f, sparseResidency2Samples); M(f, sparseResidency4Samples); M(f, sparseResidency8Samples);
	M(f, sparseResidency16Samples); M(f, sparseResidencyAliased); M(f, variableMultisampleRate);
	M(f, inheritedQueries);
}

static void mask_chain(void *p)
{
	for (VkBaseOutStructure *s = p; s; s = s->pNext) {
		switch (s->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
			VkPhysicalDeviceVulkan11Features *f = (void *)s;
			M(f, storageBuffer16BitAccess); M(f, uniformAndStorageBuffer16BitAccess); M(f, storagePushConstant16);
			M(f, storageInputOutput16); M(f, multiview); M(f, multiviewGeometryShader); M(f, multiviewTessellationShader);
			M(f, variablePointersStorageBuffer); M(f, variablePointers); M(f, protectedMemory);
			M(f, samplerYcbcrConversion); M(f, shaderDrawParameters);
			break; }
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
			VkPhysicalDeviceVulkan12Features *f = (void *)s;
			M(f, samplerMirrorClampToEdge); M(f, drawIndirectCount); M(f, storageBuffer8BitAccess);
			M(f, uniformAndStorageBuffer8BitAccess); M(f, storagePushConstant8); M(f, shaderBufferInt64Atomics);
			M(f, shaderSharedInt64Atomics); M(f, shaderFloat16); M(f, shaderInt8); M(f, descriptorIndexing);
			M(f, shaderSampledImageArrayNonUniformIndexing); M(f, shaderStorageBufferArrayNonUniformIndexing);
			M(f, descriptorBindingPartiallyBound); M(f, runtimeDescriptorArray); M(f, descriptorBindingVariableDescriptorCount);
			M(f, descriptorBindingUpdateUnusedWhilePending); M(f, descriptorBindingSampledImageUpdateAfterBind);
			M(f, samplerFilterMinmax); M(f, scalarBlockLayout); M(f, imagelessFramebuffer);
			M(f, uniformBufferStandardLayout); M(f, shaderSubgroupExtendedTypes); M(f, separateDepthStencilLayouts);
			M(f, hostQueryReset); M(f, timelineSemaphore); M(f, bufferDeviceAddress); M(f, vulkanMemoryModel);
			M(f, vulkanMemoryModelDeviceScope); M(f, shaderOutputViewportIndex); M(f, shaderOutputLayer);
			M(f, subgroupBroadcastDynamicId);
			break; }
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
			VkPhysicalDeviceVulkan13Features *f = (void *)s;
			M(f, robustImageAccess); M(f, inlineUniformBlock); M(f, pipelineCreationCacheControl);
			M(f, shaderDemoteToHelperInvocation); M(f, shaderTerminateInvocation); M(f, subgroupSizeControl);
			M(f, computeFullSubgroups); M(f, synchronization2); M(f, textureCompressionASTC_HDR);
			M(f, shaderZeroInitializeWorkgroupMemory); M(f, dynamicRendering); M(f, shaderIntegerDotProduct);
			M(f, maintenance4);
			break; }
		/* the extension structs of features that are also core in 1.2 */
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES: {
			VkPhysicalDeviceScalarBlockLayoutFeatures *f = (void *)s; M(f, scalarBlockLayout); break; }
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
			VkPhysicalDeviceShaderDrawParametersFeatures *f = (void *)s; M(f, shaderDrawParameters); break; }
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
			VkPhysicalDeviceTimelineSemaphoreFeatures *f = (void *)s; M(f, timelineSemaphore); break; }
		default:
			break;
		}
	}
}

static double api_cap(void)
{
	const char *p = getenv("WGMASK_API");
	return p ? atof(p) : 0;
}

static VkResult VKAPI_CALL wg_EnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *props)
{
	if (layer && !strcmp(layer, "VK_LAYER_RVWASM_wgmask")) { *count = 0; return VK_SUCCESS; }
	load();
	PFN_vkEnumerateDeviceExtensionProperties next = (void *)next_gipa(the_instance, "vkEnumerateDeviceExtensionProperties");
	uint32_t n = 0;
	next(pd, layer, &n, NULL);
	VkExtensionProperties *all = calloc(n ? n : 1, sizeof *all);
	next(pd, layer, &n, all);
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (!allowed(all[i].extensionName)) continue;
		if (props && k < *count) props[k] = all[i];
		k++;
	}
	free(all);
	if (!props) { *count = k; return VK_SUCCESS; }
	VkResult r = k > *count ? VK_INCOMPLETE : VK_SUCCESS;
	if (k < *count) *count = k;
	return r;
}

static void VKAPI_CALL wg_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
	load();
	PFN_vkGetPhysicalDeviceFeatures next = (void *)next_gipa(the_instance, "vkGetPhysicalDeviceFeatures");
	next(pd, f);
	mask_core(f);
}

static void VKAPI_CALL wg_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
	load();
	PFN_vkGetPhysicalDeviceFeatures2 next = (void *)next_gipa(the_instance, "vkGetPhysicalDeviceFeatures2");
	next(pd, f);
	mask_core(&f->features);
	mask_chain(f->pNext);
}

static int lim(const char *name)
{
	const char *p = getenv("WGMASK_LIMITS");
	if (!p) return 0;
	if (!strcmp(p, "all") || !strcmp(p, "1")) return 1;
	size_t n = strlen(name);
	for (const char *q = p; (q = strstr(q, name)); q += n)
		if ((q == p || q[-1] == ',') && (q[n] == ',' || !q[n])) return 1;
	return 0;
}

static void cap_props(VkPhysicalDeviceProperties *p)
{
	double cap = api_cap();
	if (cap > 0) {
		uint32_t maj = (uint32_t)cap, min = (uint32_t)((cap - maj) * 10 + 0.5);
		if (VK_MAKE_API_VERSION(0, maj, min, 0) < p->apiVersion) p->apiVersion = VK_MAKE_API_VERSION(0, maj, min, 0);
	}
	if (getenv("WGMASK_LIMITS")) {
		/* WGMASK_LIMITS=all, or a comma list of the names below (sets, ssbo, images, ...) */
		/* Chrome's WebGPU limits on Apple silicon as requested by a native-feeling driver
		 * (adapter limits; see the design doc), not the spec's defaults */
		VkPhysicalDeviceLimits *l = &p->limits;
		if (lim("sets")) l->maxBoundDescriptorSets = 4;
		if (lim("samplers")) l->maxPerStageDescriptorSamplers = 16;
		if (lim("ubos")) l->maxPerStageDescriptorUniformBuffers = atoi(getenv("WGMASK_UBOS") ? getenv("WGMASK_UBOS") : "12");
		if (lim("ssbo")) l->maxPerStageDescriptorStorageBuffers = atoi(getenv("WGMASK_SSBO") ? getenv("WGMASK_SSBO") : "10");
		if (lim("textures")) l->maxPerStageDescriptorSampledImages = 16;
		if (lim("images")) l->maxPerStageDescriptorStorageImages = atoi(getenv("WGMASK_IMAGES") ? getenv("WGMASK_IMAGES") : "8");
		if (lim("rts")) l->maxColorAttachments = 8;
		if (lim("vattrs")) { l->maxVertexInputAttributes = 16; l->maxVertexInputBindings = 8; }
		if (lim("shared")) l->maxComputeSharedMemorySize = 32768;
		if (lim("invocations")) l->maxComputeWorkGroupInvocations = 1024;
		if (lim("uborange")) l->maxUniformBufferRange = 65536;
		if (lim("viewports")) l->maxViewports = 1;
		if (lim("lodbias")) l->maxSamplerLodBias = 0;
		if (lim("samples")) {
			l->framebufferColorSampleCounts &= VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
			l->framebufferDepthSampleCounts &= VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
			l->sampledImageColorSampleCounts &= VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
		}
		if (lim("lines")) { l->lineWidthRange[1] = 1; l->pointSizeRange[1] = 1; }
	}
}

static void VKAPI_CALL wg_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
	PFN_vkGetPhysicalDeviceProperties next = (void *)next_gipa(the_instance, "vkGetPhysicalDeviceProperties");
	next(pd, p);
	cap_props(p);
}

static void VKAPI_CALL wg_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
	PFN_vkGetPhysicalDeviceProperties2 next = (void *)next_gipa(the_instance, "vkGetPhysicalDeviceProperties2");
	next(pd, p);
	cap_props(&p->properties);
}

static VkResult VKAPI_CALL wg_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *inst)
{
	VkLayerInstanceCreateInfo *li = (void *)ci->pNext;
	while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
		li = (void *)li->pNext;
	if (!li) return VK_ERROR_INITIALIZATION_FAILED;
	next_gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	li->u.pLayerInfo = li->u.pLayerInfo->pNext;
	PFN_vkCreateInstance next = (void *)next_gipa(NULL, "vkCreateInstance");
	VkResult r = next(ci, a, inst);
	if (r == VK_SUCCESS) the_instance = *inst;
	return r;
}

static VkResult VKAPI_CALL wg_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *dev)
{
	VkLayerDeviceCreateInfo *li = (void *)ci->pNext;
	while (li && !(li->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && li->function == VK_LAYER_LINK_INFO))
		li = (void *)li->pNext;
	if (!li) return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = li->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	next_gdpa = li->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	li->u.pLayerInfo = li->u.pLayerInfo->pNext;
	PFN_vkCreateDevice next = (void *)gipa(the_instance, "vkCreateDevice");
	if (getenv("WGMASK_LOG")) {
		fprintf(stderr, "wgmask: device extensions enabled by the app (%u):", ci->enabledExtensionCount);
		for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) fprintf(stderr, " %s", ci->ppEnabledExtensionNames[i]);
		fprintf(stderr, "\n");
	}
	return next(pd, ci, a, dev);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wg_GetDeviceProcAddr(VkDevice dev, const char *name)
{
	if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)wg_GetDeviceProcAddr;
	return next_gdpa(dev, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL wg_GetInstanceProcAddr(VkInstance inst, const char *name)
{
#define F(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)wg_##n
#define F2(n, m) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)wg_##m
	F(CreateInstance); F(CreateDevice); F(EnumerateDeviceExtensionProperties);
	F(GetPhysicalDeviceFeatures); F(GetPhysicalDeviceFeatures2); F2(GetPhysicalDeviceFeatures2KHR, GetPhysicalDeviceFeatures2);
	F(GetPhysicalDeviceProperties); F(GetPhysicalDeviceProperties2); F2(GetPhysicalDeviceProperties2KHR, GetPhysicalDeviceProperties2);
	F(GetInstanceProcAddr); F(GetDeviceProcAddr);
	return next_gipa ? next_gipa(inst, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
	if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
	v->pfnGetInstanceProcAddr = wg_GetInstanceProcAddr;
	v->pfnGetDeviceProcAddr = wg_GetDeviceProcAddr;
	v->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}
