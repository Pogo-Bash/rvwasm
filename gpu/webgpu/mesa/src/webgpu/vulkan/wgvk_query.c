/*
 * wgvk: query pools. Occlusion and timestamp queries are a GPUQuerySet plus
 * a resolve buffer (QueryResolve | CopySrc); T2 writes them in passes,
 * resolves them and, at completion, puts the results into `results` and
 * sets `available` (wgvk_query_pool_complete). vkGetQueryPoolResults reads
 * those, waiting for the submission that writes them (`serial`).
 * WebGPU's occlusion result is a sample count in practice but only
 * "non-zero" by the spec; timestamps are nanoseconds (timestampPeriod 1).
 * Pipeline statistics and transform feedback queries have no WebGPU set:
 * they are faked (zeros, available once their submission is done).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"

#include "util/bitscan.h"
#include "util/os_time.h"

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateQueryPool(VkDevice _device, const VkQueryPoolCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
		     VkQueryPool *pQueryPool)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	struct wgvk_query_pool *pool = vk_query_pool_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*pool));
	if (!pool)
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	uint32_t n = pool->vk.query_count;
	pool->values = pCreateInfo->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS
			       ? util_bitcount(pool->vk.pipeline_statistics)
		       : pCreateInfo->queryType == VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT ? 2 : 1;
	pool->results = calloc(n, pool->values * sizeof(uint64_t));
	pool->available = calloc(n, sizeof(uint8_t));
	pool->serial = calloc(n, sizeof(uint64_t));
	if (!pool->results || !pool->available || !pool->serial) {
		free(pool->results);
		free(pool->available);
		free(pool->serial);
		vk_query_pool_destroy(&dev->vk, pAllocator, &pool->vk);
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	}
	WGPUQueryType qt = pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION ? WGPUQueryType_Occlusion :
			   pCreateInfo->queryType == VK_QUERY_TYPE_TIMESTAMP && dev->pdev->wf.timestamp
				   ? WGPUQueryType_Timestamp
				   : (WGPUQueryType)0;
	if (qt) {
		WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
		qd.type = qt;
		qd.count = n;
		pool->set = wgpuDeviceCreateQuerySet(dev->wdev, &qd);
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.size = align64((uint64_t)n * 8, 256);
		bd.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
		pool->resolve = wgpuDeviceCreateBuffer(dev->wdev, &bd);
	}
	*pQueryPool = wgvk_query_pool_to_handle(pool);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroyQueryPool(VkDevice _device, VkQueryPool queryPool, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_query_pool, pool, queryPool);
	if (!pool)
		return;
	if (pool->set) {
		wgpuQuerySetDestroy(pool->set);
		wgpuQuerySetRelease(pool->set);
	}
	if (pool->resolve) {
		wgpuBufferDestroy(pool->resolve);
		wgpuBufferRelease(pool->resolve);
	}
	free(pool->results);
	free(pool->available);
	free(pool->serial);
	vk_query_pool_destroy(&dev->vk, pAllocator, &pool->vk);
}

void
wgvk_query_pool_reset(struct wgvk_query_pool *pool, uint32_t first, uint32_t count)
{
	memset(pool->available + first, 0, count);
	memset(pool->results + (uint64_t)first * pool->values, 0, (uint64_t)count * pool->values * sizeof(uint64_t));
	memset(pool->serial + first, 0, count * sizeof(uint64_t));
}

void
wgvk_query_pool_complete(struct wgvk_query_pool *pool, uint32_t query, const uint64_t *values)
{
	if (values)
		memcpy(pool->results + (uint64_t)query * pool->values, values, pool->values * sizeof(uint64_t));
	pool->available[query] = 1;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_ResetQueryPool(VkDevice _device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
{
	VK_FROM_HANDLE(wgvk_query_pool, pool, queryPool);
	wgvk_query_pool_reset(pool, firstQuery, queryCount);
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_GetQueryPoolResults(VkDevice _device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount,
			 size_t dataSize, void *pData, VkDeviceSize stride, VkQueryResultFlags flags)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_query_pool, pool, queryPool);
	if (vk_device_is_lost(&dev->vk))
		return VK_ERROR_DEVICE_LOST;
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < queryCount; i++) {
		uint32_t q = firstQuery + i;
		if (!pool->available[q] && pool->serial[q] && (flags & VK_QUERY_RESULT_WAIT_BIT)) {
			VkResult r = wgvk_queue_wait_serial(dev, pool->serial[q], UINT64_MAX);
			if (r != VK_SUCCESS)
				return r;
		}
		/* a faked query (no WebGPU set) is done when its submission is */
		if (!pool->available[q] && !pool->set && pool->serial[q] && pool->serial[q] <= dev->queue.completed)
			pool->available[q] = 1;
		bool avail = pool->available[q];
		if (!avail)
			result = VK_NOT_READY;
		uint8_t *out = (uint8_t *)pData + i * stride;
		bool is64 = flags & VK_QUERY_RESULT_64_BIT;
		unsigned k;
		for (k = 0; k < pool->values; k++) {
			if (!avail && !(flags & VK_QUERY_RESULT_PARTIAL_BIT))
				continue;
			uint64_t v = pool->results[(uint64_t)q * pool->values + k];
			if (is64)
				((uint64_t *)out)[k] = v;
			else
				((uint32_t *)out)[k] = (uint32_t)v;
		}
		if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
			if (is64)
				((uint64_t *)out)[k] = avail;
			else
				((uint32_t *)out)[k] = avail;
		}
	}
	(void)dataSize;
	return result;
}
