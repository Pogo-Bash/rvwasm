/*
 * wgvk: the one physical device: what the WebGPU device has, as Vulkan.
 *
 * Features and extensions are the P4 profile (wgvk_p4.c, generated from
 * spike S1's wgmask run: what zink's GL 4.3 and Blender 4.3 ran on), whose
 * missing pieces are planned emulations (design table). Limits start from
 * P4 and are then cut to what WebGPU really has where wgvk does not plan to
 * emulate more (wgvk_limits): texture sizes, inter-stage variables,
 * workgroups, offsets alignment (WebGPU binds buffers at multiples of 256),
 * dynamic buffers per layout. The numbers above WebGPU's that stay are the
 * emulated ones: 16 uniform buffers and 24 storage buffers per stage (the
 * driver UBO and the buffer heap), 256 bytes of push constants (immediates,
 * then a UBO).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"

#include "util/disk_cache.h"
#include "util/mesa-blake3.h"

#include "util/u_math.h"
#include "vk_enum_defines.h"

/* The WebGPU features wgvk uses, and what of them reaches the shader compiler. */
static void
get_wgpu_features(struct wgvk_physical_device *pdev)
{
	struct wgvk_wgpu_features *f = &pdev->wf;
	WGPUDevice d = pdev->wdev;
#define HAS(x) wgpuDeviceHasFeature(d, WGPUFeatureName_##x)
	f->core = HAS(CoreFeaturesAndLimits);
	f->depth_clip_control = HAS(DepthClipControl);
	f->depth32_stencil8 = HAS(Depth32FloatStencil8);
	f->timestamp = HAS(TimestampQuery);
	f->indirect_first_instance = HAS(IndirectFirstInstance);
	f->bc = HAS(TextureCompressionBC);
	f->etc2 = HAS(TextureCompressionETC2);
	f->astc = HAS(TextureCompressionASTC);
	f->shader_f16 = HAS(ShaderF16);
	f->subgroups = HAS(Subgroups);
	f->clip_distances = HAS(ClipDistances);
	f->dual_source_blending = HAS(DualSourceBlending);
	f->primitive_index = HAS(PrimitiveIndex);
	f->rg11b10_renderable = HAS(RG11B10UfloatRenderable);
	f->bgra8_storage = HAS(BGRA8UnormStorage);
	f->float32_filterable = HAS(Float32Filterable);
	f->float32_blendable = HAS(Float32Blendable);
	f->tier1 = HAS(TextureFormatsTier1);
	f->tier2 = HAS(TextureFormatsTier2);
	f->unorm16 = HAS(Unorm16TextureFormats);
	f->swizzle = HAS(TextureComponentSwizzle);
#undef HAS
	pdev->compile_caps = (struct wgvk_compile_caps){
		.shader_f16 = f->shader_f16,
		.subgroups = f->subgroups,
		.clip_distances = f->clip_distances,
		.dual_source_blending = f->dual_source_blending,
		.primitive_index = f->primitive_index,
		.max_immediate_size = pdev->limits.maxImmediateSize == WGPU_LIMIT_U32_UNDEFINED ? 0 :
				      pdev->limits.maxImmediateSize,
		.max_storage_buffers = pdev->limits.maxStorageBuffersPerShaderStage,
		.max_uniform_buffers = pdev->limits.maxUniformBuffersPerShaderStage,
		/* a stage over the storage buffer limit keeps (max - arenas) direct bindings and reaches the rest
		 * through the heap, whose SSBOs may live in up to this many distinct GPUBuffers (wgvk_heap.c) */
		.heap_arenas = WGVK_HEAP_ARENAS,
	};
	if (wgvk_debug("caps"))
		wgvk_log("caps: storage %u uniform %u arenas %u; f16 %d subgroups %d clip %d dual-src %d prim-index %d immediates %u; tier1 %d tier2 %d "
			 "d32s8 %d bc %d etc2 %d astc %d swizzle %d f32-filter %d\n",
			 pdev->compile_caps.max_storage_buffers, pdev->compile_caps.max_uniform_buffers,
			 pdev->compile_caps.heap_arenas, f->shader_f16, f->subgroups, f->clip_distances, f->dual_source_blending, f->primitive_index,
			 pdev->compile_caps.max_immediate_size, f->tier1, f->tier2, f->depth32_stencil8, f->bc, f->etc2,
			 f->astc, f->swizzle, f->float32_filterable);
}

/* INTERFACES.md 6: the device's real WebGPU features for wgvk_compile (overrides the compiler's weak default). */
const struct wgvk_compile_caps *
wgvk_device_compile_caps(struct wgvk_device *dev)
{
	return &dev->pdev->compile_caps;
}

bool
wgvk_debug(const char *what)
{
	const char *d = getenv("WGVK_DEBUG");
	return d && strstr(d, what);
}

/* Cut P4's limits to WebGPU's where wgvk does not emulate more. */
static void
wgvk_limits(struct wgvk_physical_device *pdev, struct vk_properties *p)
{
	const WGPULimits *l = &pdev->limits;
	p->maxImageDimension1D = l->maxTextureDimension2D;	/* 1D images are 2D textures */
	p->maxImageDimension2D = l->maxTextureDimension2D;
	p->maxImageDimension3D = l->maxTextureDimension3D;
	p->maxImageDimensionCube = l->maxTextureDimension2D;
	p->maxImageArrayLayers = l->maxTextureArrayLayers;
	p->maxFramebufferWidth = p->maxFramebufferHeight = l->maxTextureDimension2D;
	p->maxFramebufferLayers = l->maxTextureArrayLayers;
	p->maxViewportDimensions[0] = p->maxViewportDimensions[1] = l->maxTextureDimension2D;
	p->viewportBoundsRange[0] = -2.0f * l->maxTextureDimension2D;
	p->viewportBoundsRange[1] = 2.0f * l->maxTextureDimension2D - 1;

	p->maxUniformBufferRange = l->maxUniformBufferBindingSize;
	p->maxStorageBufferRange = MIN2(l->maxStorageBufferBindingSize, UINT32_MAX);
	p->maxTexelBufferElements = MIN2(p->maxTexelBufferElements, l->maxStorageBufferBindingSize / 4);
	p->maxMemoryAllocationSize = l->maxBufferSize;
	p->maxBufferSize = l->maxBufferSize;
	p->maxBoundDescriptorSets = MIN2(4, l->maxBindGroups);

	/* per stage: uniform buffers (16) and storage buffers (24) stay P4's (emulated); the rest are WebGPU's */
	p->maxPerStageDescriptorSamplers = MIN2(p->maxPerStageDescriptorSamplers, l->maxSamplersPerShaderStage);
	p->maxPerStageDescriptorSampledImages = l->maxSampledTexturesPerShaderStage;
	p->maxPerStageDescriptorStorageImages = l->maxStorageTexturesPerShaderStage;
	p->maxPerStageDescriptorInputAttachments = l->maxSampledTexturesPerShaderStage;
	/* (Vulkan's minimum is 128; the per-stage limits are what bind) */
	p->maxPerStageResources = MAX2(128, p->maxPerStageDescriptorSamplers + p->maxPerStageDescriptorUniformBuffers +
				  p->maxPerStageDescriptorStorageBuffers + p->maxPerStageDescriptorSampledImages +
				  p->maxPerStageDescriptorStorageImages + p->maxColorAttachments);
	/* per pipeline layout: WebGPU numbers bindings below 1000 per group, the driver keeps 900.. */
	uint32_t set_max = WGVK_DRIVER_BINDING_BASE * p->maxBoundDescriptorSets;
	p->maxDescriptorSetSamplers = set_max;
	p->maxDescriptorSetUniformBuffers = set_max;
	p->maxDescriptorSetStorageBuffers = set_max;
	p->maxDescriptorSetSampledImages = set_max;
	p->maxDescriptorSetStorageImages = set_max;
	p->maxDescriptorSetInputAttachments = set_max;
	p->maxDescriptorSetUniformBuffersDynamic = l->maxDynamicUniformBuffersPerPipelineLayout;
	p->maxDescriptorSetStorageBuffersDynamic = l->maxDynamicStorageBuffersPerPipelineLayout;
	p->maxDescriptorSetInlineUniformBlocks = MIN2(p->maxDescriptorSetInlineUniformBlocks, set_max);
	p->maxInlineUniformTotalSize = p->maxInlineUniformBlockSize * p->maxDescriptorSetInlineUniformBlocks;

	p->maxVertexInputAttributes = MIN2(p->maxVertexInputAttributes, l->maxVertexAttributes);
	p->maxVertexInputBindings = MIN2(p->maxVertexInputBindings, l->maxVertexBuffers);
	p->maxVertexInputBindingStride = l->maxVertexBufferArrayStride;
	p->maxVertexInputAttributeOffset = MIN2(p->maxVertexInputAttributeOffset, l->maxVertexBufferArrayStride - 1);
	/* inter-stage variables are vec4s in WebGPU */
	uint32_t io = l->maxInterStageShaderVariables * 4;
	p->maxVertexOutputComponents = MIN2(p->maxVertexOutputComponents, io);
	p->maxFragmentInputComponents = MIN2(p->maxFragmentInputComponents, io);
	p->maxGeometryOutputComponents = MIN2(p->maxGeometryOutputComponents, io);
	p->maxTessellationEvaluationOutputComponents = MIN2(p->maxTessellationEvaluationOutputComponents, io);
	p->maxColorAttachments = MIN2(p->maxColorAttachments, l->maxColorAttachments);
	p->maxFragmentOutputAttachments = p->maxColorAttachments;
	p->maxFragmentCombinedOutputResources =
		p->maxColorAttachments + p->maxPerStageDescriptorStorageBuffers + p->maxPerStageDescriptorStorageImages;

	p->maxComputeSharedMemorySize = l->maxComputeWorkgroupStorageSize;
	p->maxComputeWorkGroupInvocations = l->maxComputeInvocationsPerWorkgroup;
	p->maxComputeWorkGroupSize[0] = l->maxComputeWorkgroupSizeX;
	p->maxComputeWorkGroupSize[1] = l->maxComputeWorkgroupSizeY;
	p->maxComputeWorkGroupSize[2] = l->maxComputeWorkgroupSizeZ;
	for (unsigned i = 0; i < 3; i++)
		p->maxComputeWorkGroupCount[i] = l->maxComputeWorkgroupsPerDimension;

	/* WebGPU binds buffer ranges at multiples of these (256); texel buffers are storage buffers */
	p->minUniformBufferOffsetAlignment = l->minUniformBufferOffsetAlignment;
	p->minStorageBufferOffsetAlignment = l->minStorageBufferOffsetAlignment;
	p->minTexelBufferOffsetAlignment = l->minStorageBufferOffsetAlignment;
	p->storageTexelBufferOffsetAlignmentBytes = l->minStorageBufferOffsetAlignment;
	p->uniformTexelBufferOffsetAlignmentBytes = l->minStorageBufferOffsetAlignment;
	p->storageTexelBufferOffsetSingleTexelAlignment = false;
	p->uniformTexelBufferOffsetSingleTexelAlignment = false;
	/* buffer <-> texture copies want rows of 256 bytes; anything else goes through a staging copy (T2) */
	p->optimalBufferCopyOffsetAlignment = 256;
	p->optimalBufferCopyRowPitchAlignment = 256;
	p->maxSamplerAnisotropy = 16;
	p->timestampPeriod = 1.0f;	/* WebGPU timestamps are nanoseconds */
	p->timestampComputeAndGraphics = pdev->wf.timestamp;
	p->maxSamplerAllocationCount = 1u << 20;
	p->maxMemoryAllocationCount = UINT32_MAX;
	p->bufferImageGranularity = 1;	/* images never share bytes with buffers */
	/* WebGPU multisampling is 1 or 4 samples */
	VkSampleCountFlags s = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
	p->framebufferColorSampleCounts = p->framebufferDepthSampleCounts = p->framebufferStencilSampleCounts = s;
	p->framebufferNoAttachmentsSampleCounts = s;
	p->sampledImageColorSampleCounts = p->sampledImageIntegerSampleCounts = s;
	p->sampledImageDepthSampleCounts = p->sampledImageStencilSampleCounts = s;
	p->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	if (!pdev->wf.subgroups) {
		p->subgroupSize = 1;
		p->subgroupSupportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
	} else {
		/* WebGPU's subgroup size varies by adapter (4..128); the compiler asks per shader */
		p->subgroupSize = 32;
		p->subgroupSupportedStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	}
}

/* The Vulkan device type. WebGPU's GPUAdapterInfo has no discrete/integrated field, only isFallbackAdapter (a
 * fallback adapter "has significant performance caveats"), which webgpu.h's adapterType carries as CPU. Dawn native
 * also fills in DiscreteGPU or IntegratedGPU; the page's emdawnwebgpu (library_webgpu.js) says CPU for a fallback adapter
 * and Unknown for everything else. A loader or app that ranks devices (vkcube prefers discrete, then integrated, then
 * virtual, then CPU, and never picks an OTHER one while a CPU device is listed) must see a real adapter as a GPU, so
 * Unknown becomes DISCRETE_GPU: wgvk's memory model is a discrete one (a device-local heap the guest cannot map and a
 * separate host heap, INTERFACES.md 4.1), and the type makes no claim about unified memory.
 * WGVK_DEVICE_TYPE=cpu|integrated|discrete|other overrides (cpu: what zink insists on under LIBGL_ALWAYS_SOFTWARE,
 * EGL surfaceless in the VM). */
static VkPhysicalDeviceType
wgvk_device_type(const struct wgvk_physical_device *pdev)
{
	const char *t = getenv("WGVK_DEVICE_TYPE");
	if (t) {
		if (!strcmp(t, "cpu"))
			return VK_PHYSICAL_DEVICE_TYPE_CPU;
		if (!strcmp(t, "integrated"))
			return VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
		if (!strcmp(t, "discrete"))
			return VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
		if (!strcmp(t, "other"))
			return VK_PHYSICAL_DEVICE_TYPE_OTHER;
	}
	switch (pdev->adapter_type) {
	case WGPUAdapterType_CPU:
		return VK_PHYSICAL_DEVICE_TYPE_CPU;
	case WGPUAdapterType_IntegratedGPU:
		return VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
	default:	/* DiscreteGPU, Unknown (and no adapter info at all) */
		return VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
	}
}

static void
get_properties(struct wgvk_physical_device *pdev, struct vk_properties *p)
{
	wgvk_p4_properties(p);
	wgvk_limits(pdev, p);
	p->minImportedHostPointerAlignment = 4096;	/* (T7: VK_EXT_external_memory_host, Mesa's software WSI) */
	p->apiVersion = VK_MAKE_API_VERSION(0, 1, 1, VK_HEADER_VERSION);	/* P4: Vulkan 1.1 */
	p->driverVersion = vk_get_driver_version();
	p->vendorID = WGVK_VENDOR_ID;
	p->deviceID = WGVK_DEVICE_ID;
	p->deviceType = wgvk_device_type(pdev);
	snprintf(p->deviceName, sizeof(p->deviceName), "wgvk (WebGPU: %s)", pdev->adapter_name);
	/* (T2) the pipeline cache (Mesa's disk cache under the runtime's VkPipelineCache) holds wgvk's shader
	 * objects: a new build must not read an older build's (their layout changes) */
	blake3_hasher bctx;
	unsigned char b3[BLAKE3_KEY_LEN];
	_mesa_blake3_init(&bctx);
	if (!disk_cache_get_function_identifier((void *)get_properties, &bctx))
		_mesa_blake3_update(&bctx, "wgvk " PACKAGE_VERSION, strlen("wgvk " PACKAGE_VERSION));	/* (the page: no disk cache) */
	_mesa_blake3_final(&bctx, b3);
	memcpy(p->pipelineCacheUUID, b3, VK_UUID_SIZE);
	memcpy(p->shaderBinaryUUID, b3, VK_UUID_SIZE);
	memcpy(p->driverUUID, "wgvk-driver-uuid", VK_UUID_SIZE);
	memcpy(p->deviceUUID, "wgvk-device-uuid", VK_UUID_SIZE);
	p->driverID = VK_DRIVER_ID_MESA_LLVMPIPE;	/* Vulkan has no id for us */
	snprintf(p->driverName, sizeof(p->driverName), "wgvk");
	snprintf(p->driverInfo, sizeof(p->driverInfo), "Mesa " PACKAGE_VERSION);
	p->conformanceVersion = (VkConformanceVersion){ 0, 0, 0, 0 };
}

VkResult
wgvk_enumerate_physical_devices(struct vk_instance *vk_instance)
{
	struct wgvk_instance *instance = container_of(vk_instance, struct wgvk_instance, vk);
	if (wgvk_wgpu_initializing())
		return VK_SUCCESS;	/* Dawn's own Vulkan instance looking for devices: not us */

	WGPUInstance winst;
	WGPUAdapter adapter;
	WGPUDevice wdev;
	if (wgvk_wgpu_get(&winst, &adapter, &wdev) != VK_SUCCESS) {
		wgvk_log("no WebGPU device; no physical device\n");
		return VK_SUCCESS;
	}

	struct wgvk_physical_device *pdev =
		vk_zalloc(&instance->vk.alloc, sizeof(*pdev), 8, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (!pdev)
		return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
	pdev->winst = winst;
	pdev->adapter = adapter;
	pdev->wdev = wdev;
	pdev->limits = (WGPULimits)WGPU_LIMITS_INIT;
	wgpuDeviceGetLimits(wdev, &pdev->limits);
	get_wgpu_features(pdev);
	wgvk_format_table_init(pdev);
	snprintf(pdev->adapter_name, sizeof(pdev->adapter_name), "device");
	pdev->adapter_type = WGPUAdapterType_Unknown;
	if (adapter) {
		WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
		if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
			const WGPUStringView *n = info.device.length ? &info.device : &info.description;
			snprintf(pdev->adapter_name, sizeof(pdev->adapter_name), "%.*s",
				 (int)(n->length == WGPU_STRLEN ? 64 : n->length), n->data ? n->data : "?");
			if (!pdev->adapter_name[0])
				snprintf(pdev->adapter_name, sizeof(pdev->adapter_name), "%.*s",
					 (int)(info.vendor.length == WGPU_STRLEN ? 64 : info.vendor.length),
					 info.vendor.data ? info.vendor.data : "?");
			pdev->adapter_type = info.adapterType;
			wgpuAdapterInfoFreeMembers(info);
		}
	}

	struct vk_device_extension_table exts = { 0 };
	wgvk_p4_extensions(&exts);
#ifdef WGVK_WSI
	exts.KHR_swapchain = true;	/* (wgvk_wsi.c, natively) */
#else
	exts.KHR_swapchain = false;	/* (the page: guests' swapchains are venus', T7) */
#endif
	exts.KHR_incremental_present = false;
	exts.KHR_swapchain_mutable_format = false;
	struct vk_features feats = { 0 };
	wgvk_p4_features(&feats);
	feats.textureCompressionBC = pdev->wf.bc;
	feats.textureCompressionETC2 = pdev->wf.etc2;
	feats.textureCompressionASTC_LDR = pdev->wf.astc;
	feats.shaderFloat16 = pdev->wf.shader_f16;
	feats.dualSrcBlend = pdev->wf.dual_source_blending;
	feats.depthClamp = pdev->wf.depth_clip_control;
	feats.depthClipEnable = pdev->wf.depth_clip_control;
	/* (T2) points larger than a pixel: expanded to quads in the vertex stage (T3b), driven by T2 */
	feats.largePoints = true;
	feats.shaderDrawParameters = true;	/* T3b lowers them, T2 writes them per draw */
	struct vk_properties props = { 0 };
	get_properties(pdev, &props);
	props.pointSizeRange[0] = 1.0f;
	props.pointSizeRange[1] = 255.0f;
	props.pointSizeGranularity = 0.125f;
	/* Faked until their emulation lands (design table, M1 "fake": advertised, logged, no effect), so
	 * zink reaches GL 3.0+ (it drops to 2.1 without transform feedback) and Blender's XOR draws.
	 * WGVK_NO_FAKE=1 leaves them out (the honest profile). */
	if (!getenv("WGVK_NO_FAKE") || !atoi(getenv("WGVK_NO_FAKE"))) {
		exts.EXT_transform_feedback = true;	/* libpoly's passthrough GS writes them (T4, M3) */
		feats.transformFeedback = true;
		feats.geometryStreams = true;
		props.maxTransformFeedbackStreams = 4;
		props.maxTransformFeedbackBuffers = 4;
		props.maxTransformFeedbackBufferSize = UINT32_MAX;
		props.maxTransformFeedbackStreamDataSize = 512;
		props.maxTransformFeedbackBufferDataSize = 512;
		props.maxTransformFeedbackBufferDataStride = 2048;
		props.transformFeedbackQueries = true;
		props.transformFeedbackStreamsLinesTriangles = false;
		props.transformFeedbackRasterizationStreamSelect = false;
		props.transformFeedbackDraw = true;
		feats.logicOp = true;			/* a fragment-shader path later (M3) */
	}

	struct vk_physical_device_dispatch_table dispatch_table;
	vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table, &wgvk_physical_device_entrypoints, true);
#ifdef WGVK_WSI
	vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_physical_device_entrypoints, false);
#endif
	VkResult result = vk_physical_device_init(&pdev->vk, &instance->vk, &exts, &feats, &props, &dispatch_table);
	if (result != VK_SUCCESS) {
		vk_free(&instance->vk.alloc, pdev);
		return result;
	}
	pdev->sync_types[0] = &wgvk_sync_type;
	pdev->sync_types[1] = NULL;
	pdev->vk.supported_sync_types = pdev->sync_types;
#ifdef WGVK_WSI
	/* (after the sync types: the common WSI asks about semaphores) */
	result = wgvk_wsi_init(pdev);
	if (result != VK_SUCCESS) {
		vk_physical_device_finish(&pdev->vk);
		vk_free(&instance->vk.alloc, pdev);
		return result;
	}
#endif
	list_addtail(&pdev->vk.link, &instance->vk.physical_devices.list);
	return VK_SUCCESS;
}

void
wgvk_physical_device_destroy(struct vk_physical_device *vk_pdev)
{
	struct wgvk_physical_device *pdev = container_of(vk_pdev, struct wgvk_physical_device, vk);
#ifdef WGVK_WSI
	wgvk_wsi_finish(pdev);
#endif
	vk_physical_device_finish(&pdev->vk);
	vk_free(&pdev->vk.instance->alloc, pdev);
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t *pCount,
					     VkQueueFamilyProperties2 *pProps)
{
	VK_FROM_HANDLE(wgvk_physical_device, pdev, physicalDevice);
	VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pProps, pCount);
	vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
		p->queueFamilyProperties = (VkQueueFamilyProperties){
			.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
			.queueCount = 1,
			.timestampValidBits = pdev->wf.timestamp ? 64 : 0,
			.minImageTransferGranularity = { 1, 1, 1 },
		};
	}
}

/* Memory (INTERFACES.md 4.1): DEVICE_LOCAL is GPU only; HOST_VISIBLE | COHERENT | CACHED has its bytes in
 * a window (this process's memory, or the guest's for venus blobs), moved to and from the GPU by T2 at
 * use and before fences. WebGPU says nothing about sizes: the heaps are nominal. */
VKAPI_ATTR void VKAPI_CALL
wgvk_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *p)
{
	VkPhysicalDeviceMemoryProperties *m = &p->memoryProperties;
	*m = (VkPhysicalDeviceMemoryProperties){
		.memoryTypeCount = WGVK_MEM_TYPE_COUNT,
		.memoryTypes = {
			[WGVK_MEM_DEVICE_LOCAL] = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 },
			[WGVK_MEM_HOST] = { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
						    VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1 },
		},
		.memoryHeapCount = 2,
		.memoryHeaps = {
			{ WGVK_DEVICE_HEAP_SIZE, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT },
			{ WGVK_HOST_HEAP_SIZE, 0 },
		},
	};
	VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = vk_find_struct(p->pNext, PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT);
	if (b) {
		memset(b->heapBudget, 0, sizeof(b->heapBudget));
		memset(b->heapUsage, 0, sizeof(b->heapUsage));
		b->heapBudget[0] = WGVK_DEVICE_HEAP_SIZE;
		b->heapBudget[1] = WGVK_HOST_HEAP_SIZE;
	}
}

/* External memory (T7): a window's image is a dma-buf to the guest (venus over vtest: the blob's file), so its
 * memory must be exportable as one, or Mesa's venus offers no swapchain. Exported here means: vkr (T5) makes
 * the blob from the memory itself; wgvk hands out no file descriptors (vkGetMemoryFdKHR is not offered). */
#define WGVK_EXPORT_HANDLE_TYPES (VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT | \
				  VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
static const VkExternalMemoryProperties wgvk_export_memory_properties = {
	.externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT,
	.exportFromImportedHandleTypes = 0,
	.compatibleHandleTypes = WGVK_EXPORT_HANDLE_TYPES,
};

/* VK_EXT_image_drm_format_modifier's LINEAR modifier (T7): what a window's image is made with (Mesa's WSI in
 * the guest: venus asks us). The image is a texture like an optimal one, so it has the optimal features;
 * its layout is linear (rows 256-byte aligned) as far as anyone asks, but nobody reads its memory: the page
 * shows the texture (rvgpu's present). Plain 2D colour formats only. */
static VkFormatFeatureFlags2
modifier_features(const struct wgvk_format *f)
{
	if (!f || !f->wf || (f->flags & (WGVK_FMT_DEPTH | WGVK_FMT_STENCIL | WGVK_FMT_COMPRESSED)))
		return 0;
	return f->optimal & ~(VK_FORMAT_FEATURE_2_DISJOINT_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_CUBIC_BIT);
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format,
					VkFormatProperties2 *pFormatProperties)
{
	VK_FROM_HANDLE(wgvk_physical_device, pdev, physicalDevice);
	const struct wgvk_format *f = wgvk_format_get(pdev, format);
	VkFormatFeatureFlags2 o = f ? f->optimal : 0, l = f ? f->linear : 0, b = f ? f->buffer : 0;
	pFormatProperties->formatProperties = (VkFormatProperties){
		.linearTilingFeatures = vk_format_features2_to_features(l),
		.optimalTilingFeatures = vk_format_features2_to_features(o),
		.bufferFeatures = vk_format_features2_to_features(b),
	};
	VkFormatProperties3 *p3 = vk_find_struct(pFormatProperties->pNext, FORMAT_PROPERTIES_3);
	if (p3) {
		p3->linearTilingFeatures = l;
		p3->optimalTilingFeatures = o;
		p3->bufferFeatures = b;
	}
	VkDrmFormatModifierPropertiesListEXT *mods = vk_find_struct(pFormatProperties->pNext,
								     DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT);
	if (mods) {
		/* only the linear modifier (modifier_features) */
		VK_OUTARRAY_MAKE_TYPED(VkDrmFormatModifierPropertiesEXT, out, mods->pDrmFormatModifierProperties,
				       &mods->drmFormatModifierCount);
		VkFormatFeatureFlags2 mf = modifier_features(f);
		if (mf) {
			vk_outarray_append_typed(VkDrmFormatModifierPropertiesEXT, &out, m) {
				m->drmFormatModifier = 0;	/* DRM_FORMAT_MOD_LINEAR */
				m->drmFormatModifierPlaneCount = 1;
				m->drmFormatModifierTilingFeatures = vk_format_features2_to_features(mf);
			}
		}
	}
	VkDrmFormatModifierPropertiesList2EXT *mods2 = vk_find_struct(pFormatProperties->pNext,
								       DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT);
	if (mods2) {
		VK_OUTARRAY_MAKE_TYPED(VkDrmFormatModifierProperties2EXT, out, mods2->pDrmFormatModifierProperties,
				       &mods2->drmFormatModifierCount);
		VkFormatFeatureFlags2 mf = modifier_features(f);
		if (mf) {
			vk_outarray_append_typed(VkDrmFormatModifierProperties2EXT, &out, m) {
				m->drmFormatModifier = 0;
				m->drmFormatModifierPlaneCount = 1;
				m->drmFormatModifierTilingFeatures = mf;
			}
		}
	}
}

static VkFormatFeatureFlags2
usage_features(VkImageUsageFlags u)
{
	VkFormatFeatureFlags2 need = 0;
	if (u & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) need |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
	if (u & VK_IMAGE_USAGE_TRANSFER_DST_BIT) need |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
	if (u & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
		need |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
	if (u & VK_IMAGE_USAGE_STORAGE_BIT) need |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
	if (u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
	if (u & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) need |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
	return need;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
					     const VkPhysicalDeviceImageFormatInfo2 *info,
					     VkImageFormatProperties2 *props)
{
	VK_FROM_HANDLE(wgvk_physical_device, pdev, physicalDevice);
	const struct wgvk_format *f = wgvk_format_get(pdev, info->format);
	props->imageFormatProperties = (VkImageFormatProperties){ 0 };
	if (!f || !f->wf)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	bool linear = info->tiling == VK_IMAGE_TILING_LINEAR ||
		      info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	VkFormatFeatureFlags2 have = linear ? f->linear : f->optimal;
	if (info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
		const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *mi =
			vk_find_struct_const(info->pNext, PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);
		if (mi && mi->drmFormatModifier != 0)
			return VK_ERROR_FORMAT_NOT_SUPPORTED;
		have = modifier_features(f);
	}
	VkImageUsageFlags usage = info->usage;
	const VkImageStencilUsageCreateInfo *su = vk_find_struct_const(info->pNext, IMAGE_STENCIL_USAGE_CREATE_INFO);
	if (su)
		usage |= su->stencilUsage;
	/* EXTENDED_USAGE: a usage only some view format has is fine at image level */
	if (!(info->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) && (usage_features(usage) & ~have))
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (!have)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (info->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
			   VK_IMAGE_CREATE_SPARSE_ALIASED_BIT))
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (linear && info->type != VK_IMAGE_TYPE_2D)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((f->flags & (WGVK_FMT_DEPTH | WGVK_FMT_STENCIL)) && info->type == VK_IMAGE_TYPE_3D)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	if ((f->flags & WGVK_FMT_COMPRESSED) && info->type == VK_IMAGE_TYPE_1D)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	const WGPULimits *l = &pdev->limits;
	VkExtent3D ext;
	uint32_t layers = l->maxTextureArrayLayers;
	switch (info->type) {
	case VK_IMAGE_TYPE_1D: ext = (VkExtent3D){ l->maxTextureDimension2D, 1, 1 }; break;
	case VK_IMAGE_TYPE_3D:
		ext = (VkExtent3D){ l->maxTextureDimension3D, l->maxTextureDimension3D, l->maxTextureDimension3D };
		layers = 1;
		break;
	default: ext = (VkExtent3D){ l->maxTextureDimension2D, l->maxTextureDimension2D, 1 }; break;
	}
	VkSampleCountFlags samples = VK_SAMPLE_COUNT_1_BIT;
	if (!linear && info->type == VK_IMAGE_TYPE_2D && (f->caps & WGVK_CAP_MULTISAMPLE) &&
	    !(info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && !(usage & VK_IMAGE_USAGE_STORAGE_BIT))
		samples |= VK_SAMPLE_COUNT_4_BIT;
	props->imageFormatProperties = (VkImageFormatProperties){
		.maxExtent = ext,
		.maxMipLevels = linear ? 1 : util_logbase2(MAX3(ext.width, ext.height, ext.depth)) + 1,
		.maxArrayLayers = linear ? 1 : layers,
		.sampleCounts = samples,
		.maxResourceSize = l->maxBufferSize,
	};

	VkSamplerYcbcrConversionImageFormatProperties *y =
		vk_find_struct(props->pNext, SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES);
	if (y)
		y->combinedImageSamplerDescriptorCount = 1;
	const VkPhysicalDeviceExternalImageFormatInfo *ei =
		vk_find_struct_const(info->pNext, PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
	VkExternalImageFormatProperties *e = vk_find_struct(props->pNext, EXTERNAL_IMAGE_FORMAT_PROPERTIES);
	if (ei && ei->handleType) {
		if (!(ei->handleType & WGVK_EXPORT_HANDLE_TYPES))
			return VK_ERROR_FORMAT_NOT_SUPPORTED;
		if (e)
			e->externalMemoryProperties = wgvk_export_memory_properties;
	} else if (e) {
		e->externalMemoryProperties = (VkExternalMemoryProperties){ 0 };
	}
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice physicalDevice,
						   const VkPhysicalDeviceSparseImageFormatInfo2 *info,
						   uint32_t *pCount, VkSparseImageFormatProperties2 *props)
{
	*pCount = 0;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice,
					       const VkPhysicalDeviceExternalBufferInfo *info,
					       VkExternalBufferProperties *props)
{
	if (info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) {
		props->externalMemoryProperties = (VkExternalMemoryProperties){
			.externalMemoryFeatures = VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
			.compatibleHandleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
		};
		return;
	}
	props->externalMemoryProperties = (info->handleType & WGVK_EXPORT_HANDLE_TYPES) ?
					  wgvk_export_memory_properties : (VkExternalMemoryProperties){ 0 };
}
