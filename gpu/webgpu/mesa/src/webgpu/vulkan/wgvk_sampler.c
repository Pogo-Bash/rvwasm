/*
 * wgvk: samplers. A VkSampler is a GPUSampler for what WebGPU samplers do,
 * plus flags (wgvk_sampler.emulate) for what the shader must do instead
 * (T3b: border colours, mirror-clamp, LOD bias, unnormalized coordinates,
 * reductions). A second, all-nearest GPUSampler is made on demand for
 * textures WebGPU will not filter (bind group layouts then say
 * "non-filtering"; wgvk_sampler_nonfiltering).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"

static WGPUAddressMode
address_mode(VkSamplerAddressMode m, uint32_t *emulate)
{
	switch (m) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT: return WGPUAddressMode_Repeat;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return WGPUAddressMode_MirrorRepeat;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
		*emulate |= WGVK_SAMPLER_BORDER;
		return WGPUAddressMode_ClampToEdge;
	case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
		*emulate |= WGVK_SAMPLER_MIRROR_CLAMP;
		return WGPUAddressMode_MirrorRepeat;
	default: return WGPUAddressMode_ClampToEdge;
	}
}

static WGPUCompareFunction
compare_function(VkCompareOp op)
{
	switch (op) {
	case VK_COMPARE_OP_NEVER: return WGPUCompareFunction_Never;
	case VK_COMPARE_OP_LESS: return WGPUCompareFunction_Less;
	case VK_COMPARE_OP_EQUAL: return WGPUCompareFunction_Equal;
	case VK_COMPARE_OP_LESS_OR_EQUAL: return WGPUCompareFunction_LessEqual;
	case VK_COMPARE_OP_GREATER: return WGPUCompareFunction_Greater;
	case VK_COMPARE_OP_NOT_EQUAL: return WGPUCompareFunction_NotEqual;
	case VK_COMPARE_OP_GREATER_OR_EQUAL: return WGPUCompareFunction_GreaterEqual;
	default: return WGPUCompareFunction_Always;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateSampler(VkDevice _device, const VkSamplerCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
		   VkSampler *pSampler)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	struct wgvk_sampler *s = vk_sampler_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*s));
	if (!s)
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	s->id = wgvk_object_id(dev);
	WGPUSamplerDescriptor d = WGPU_SAMPLER_DESCRIPTOR_INIT;
	d.addressModeU = address_mode(pCreateInfo->addressModeU, &s->emulate);
	d.addressModeV = address_mode(pCreateInfo->addressModeV, &s->emulate);
	d.addressModeW = address_mode(pCreateInfo->addressModeW, &s->emulate);
	d.magFilter = pCreateInfo->magFilter == VK_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	d.minFilter = pCreateInfo->minFilter == VK_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	d.mipmapFilter = pCreateInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? WGPUMipmapFilterMode_Linear
										   : WGPUMipmapFilterMode_Nearest;
	/* WebGPU: 0 <= lodMinClamp <= lodMaxClamp; Vulkan's VK_LOD_CLAMP_NONE is 1000 */
	d.lodMinClamp = CLAMP(pCreateInfo->minLod, 0.0f, 32.0f);
	d.lodMaxClamp = CLAMP(pCreateInfo->maxLod, d.lodMinClamp, 32.0f);
	if (pCreateInfo->compareEnable) {
		d.compare = compare_function(pCreateInfo->compareOp);
		s->comparison = true;
	}
	/* anisotropy needs every filter linear in WebGPU; otherwise it is dropped */
	d.maxAnisotropy = 1;
	if (pCreateInfo->anisotropyEnable && d.magFilter == WGPUFilterMode_Linear &&
	    d.minFilter == WGPUFilterMode_Linear && d.mipmapFilter == WGPUMipmapFilterMode_Linear)
		d.maxAnisotropy = (uint16_t)CLAMP(pCreateInfo->maxAnisotropy, 1.0f, 16.0f);
	if (pCreateInfo->mipLodBias != 0.0f)
		s->emulate |= WGVK_SAMPLER_LOD_BIAS;
	if (pCreateInfo->unnormalizedCoordinates)
		s->emulate |= WGVK_SAMPLER_UNNORMALIZED;
	if (s->vk.reduction_mode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE)
		s->emulate |= WGVK_SAMPLER_REDUCTION;
	s->filtering = d.magFilter == WGPUFilterMode_Linear || d.minFilter == WGPUFilterMode_Linear ||
		       d.mipmapFilter == WGPUMipmapFilterMode_Linear;
	s->desc = d;
	s->sampler = wgpuDeviceCreateSampler(dev->wdev, &d);
	*pSampler = wgvk_sampler_to_handle(s);
	return VK_SUCCESS;
}

WGPUSampler
wgvk_sampler_nonfiltering(struct wgvk_device *dev, struct wgvk_sampler *s)
{
	if (!s->filtering)
		return s->sampler;
	if (!s->nonfiltering) {
		WGPUSamplerDescriptor d = s->desc;
		d.magFilter = d.minFilter = WGPUFilterMode_Nearest;
		d.mipmapFilter = WGPUMipmapFilterMode_Nearest;
		d.maxAnisotropy = 1;
		s->nonfiltering = wgpuDeviceCreateSampler(dev->wdev, &d);
	}
	return s->nonfiltering;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroySampler(VkDevice _device, VkSampler _sampler, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_sampler, s, _sampler);
	if (!s)
		return;
	wgpuSamplerRelease(s->sampler);
	if (s->nonfiltering)
		wgpuSamplerRelease(s->nonfiltering);
	vk_sampler_destroy(&dev->vk, pAllocator, &s->vk);
}
