/*
 * wgvk: device memory, buffers, buffer views, and the memory sync points
 * (INTERFACES.md 4.1; wgvk_bo.h).
 *
 * A VkDeviceMemory is one GPUBuffer with every buffer usage (copy, uniform,
 * storage, vertex, index, indirect, query resolve), made when the first
 * buffer is bound to it: memory that only images use never gets one (an
 * image is its own GPUTexture; wgvk_image.c). A VkBuffer is a range of its
 * memory's GPUBuffer.
 *
 * Host-visible memory has its bytes in a window: malloc'd here (the ICD, and
 * zink in the page), or the guest's pages for a venus blob (T5 sets a GUEST
 * window). The CPU maps the window; the GPU sees it through T2's upload on
 * use and download before a fence (wgvk_bo_upload / wgvk_bo_download).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"

/* The page's guest-memory functions (rvgpu.h). Natively the ICD has no rvgpu: weak, NULL. */
void rvgpu_guest_upload_buffer(WGPUQueue q, WGPUBuffer b, uint64_t boff, uint64_t addr, size_t len)
	__attribute__((weak));
void rvgpu_guest_from_mapped(WGPUBuffer mapped, uint64_t boff, uint64_t addr, size_t len) __attribute__((weak));
void *rvgpu_guest_ptr(uint64_t addr, size_t len) __attribute__((weak));
void rvgpu_guest_write(uint64_t addr, const void *src, size_t len) __attribute__((weak));

static const WGPUBufferUsage all_usages = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst |
					  WGPUBufferUsage_Storage | WGPUBufferUsage_Uniform |
					  WGPUBufferUsage_Vertex | WGPUBufferUsage_Index |
					  WGPUBufferUsage_Indirect | WGPUBufferUsage_QueryResolve;

/* ---- memory */

WGPUBuffer
wgvk_memory_buffer(struct wgvk_device *dev, struct wgvk_device_memory *mem)
{
	if (mem->bo.buffer)
		return mem->bo.buffer;
	if (mem->bo.size > dev->pdev->limits.maxBufferSize) {
		wgvk_log("memory of %llu bytes is larger than WebGPU's maxBufferSize (%llu): no buffers in it\n",
			 (unsigned long long)mem->bo.size, (unsigned long long)dev->pdev->limits.maxBufferSize);
		return NULL;
	}
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	/* (T2) 256 bytes past the end: a uniform binding is rounded up to 16 bytes (WebGPU checks the shader's
	 * vec4 arrays against the bound size), and one at the very end of the memory still fits */
	bd.size = mem->bo.size + 256;
	bd.usage = all_usages;
	mem->bo.buffer = wgpuDeviceCreateBuffer(dev->wdev, &bd);
	/* a host-visible memory's GPU copy starts as its window (zeros, or what the CPU wrote before binding) */
	if (mem->bo.buffer && mem->bo.window.kind != WGVK_WINDOW_NONE)
		wgvk_bo_upload(dev, &mem->bo, 0, mem->bo.size);
	return mem->bo.buffer;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_AllocateMemory(VkDevice _device, const VkMemoryAllocateInfo *pAllocateInfo,
		    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMem)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	if (pAllocateInfo->memoryTypeIndex >= WGVK_MEM_TYPE_COUNT)
		return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
	struct wgvk_device_memory *mem = vk_device_memory_create(&dev->vk, pAllocateInfo, pAllocator, sizeof(*mem));
	if (!mem)
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	mem->id = wgvk_object_id(dev);
	mem->bo.offset = 0;
	mem->bo.size = align64(mem->vk.size, 4);	/* WebGPU copies and writes are in 4-byte units */
	/* VK_EXT_external_memory_host: the caller's pages are the window (Mesa's software WSI imports the
	 * wl_shm or MIT-SHM memory it presents from, then blits the swapchain image into it: T7) */
	const VkImportMemoryHostPointerInfoEXT *hp = vk_find_struct_const(pAllocateInfo->pNext,
									  IMPORT_MEMORY_HOST_POINTER_INFO_EXT);
	if (hp && hp->handleType && pAllocateInfo->memoryTypeIndex == WGVK_MEM_HOST) {
		mem->host = hp->pHostPointer;
		mem->host_imported = true;
		mem->bo.size = mem->vk.size & ~3ull;	/* (the import's own size: WebGPU copies 4-byte units) */
		mem->bo.window = (struct wgvk_window){ .kind = WGVK_WINDOW_HOST, .host = mem->host };
		mem->bo.flags = WGVK_BO_HOST_VISIBLE;
	} else if (pAllocateInfo->memoryTypeIndex == WGVK_MEM_HOST) {
		mem->host = calloc(1, mem->bo.size);
		if (!mem->host) {
			vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
			return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
		}
		mem->bo.window = (struct wgvk_window){ .kind = WGVK_WINDOW_HOST, .host = mem->host };
		mem->bo.flags = WGVK_BO_HOST_VISIBLE;
	}
	*pMem = wgvk_device_memory_to_handle(mem);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_FreeMemory(VkDevice _device, VkDeviceMemory _mem, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_device_memory, mem, _mem);
	if (!mem)
		return;
	wgvk_present_forget(dev, NULL, mem);
	if (mem->bo.buffer) {
		/* bind groups in the cache may still hold it; destroy frees the GPU memory now */
		wgpuBufferDestroy(mem->bo.buffer);
		wgpuBufferRelease(mem->bo.buffer);
	}
	if (!mem->host_imported)
		free(mem->host);
	vk_device_memory_destroy(&dev->vk, pAllocator, &mem->vk);
}

void *
wgvk_bo_map(const struct wgvk_bo *bo)
{
	switch (bo->window.kind) {
	case WGVK_WINDOW_HOST:
		return bo->window.host;
	case WGVK_WINDOW_GUEST:
		return rvgpu_guest_ptr ? rvgpu_guest_ptr(bo->window.guest, bo->size) : NULL;
	default:
		return NULL;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_MapMemory2KHR(VkDevice _device, const VkMemoryMapInfoKHR *info, void **ppData)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_device_memory, mem, info->memory);
	void *p = wgvk_bo_map(&mem->bo);
	if (!p)
		return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
	*ppData = (char *)p + info->offset;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_UnmapMemory2KHR(VkDevice _device, const VkMemoryUnmapInfoKHR *info)
{
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_FlushMappedMemoryRanges(VkDevice _device, uint32_t count, const VkMappedMemoryRange *ranges)
{
	return VK_SUCCESS;	/* coherent: uploads happen on use */
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_InvalidateMappedMemoryRanges(VkDevice _device, uint32_t count, const VkMappedMemoryRange *ranges)
{
	return VK_SUCCESS;	/* downloads happen before a fence signals */
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetDeviceMemoryCommitment(VkDevice _device, VkDeviceMemory _mem, VkDeviceSize *pCommitted)
{
	VK_FROM_HANDLE(wgvk_device_memory, mem, _mem);
	*pCommitted = mem->vk.size;
}

void
wgvk_memory_set_guest_window(VkDevice _device, VkDeviceMemory _mem, uint64_t guest, uint64_t size)
{
	VK_FROM_HANDLE(wgvk_device_memory, mem, _mem);
	if (!mem || !(mem->bo.flags & WGVK_BO_HOST_VISIBLE))
		return;
	if (guest) {
		if (mem->host) {
			const size_t n = MIN2(size, mem->bo.size);
			if (rvgpu_guest_write)
				rvgpu_guest_write(guest, mem->host, n);
			else if (rvgpu_guest_ptr && rvgpu_guest_ptr(guest, n))
				memcpy(rvgpu_guest_ptr(guest, n), mem->host, n);
			free(mem->host);
			mem->host = NULL;
		}
		mem->bo.window = (struct wgvk_window){ .kind = WGVK_WINDOW_GUEST, .guest = guest };
	} else {
		mem->bo.window = (struct wgvk_window){ .kind = WGVK_WINDOW_NONE };
	}
}

/* ---- the memory sync points */

void
wgvk_bo_upload(struct wgvk_device *dev, const struct wgvk_bo *bo, uint64_t offset, uint64_t size)
{
	if (bo->window.kind == WGVK_WINDOW_NONE || !bo->buffer)
		return;
	uint64_t at = offset & ~3ull, end = MIN2(align64(offset + size, 4), bo->size);
	if (end <= at)
		return;
	if (bo->window.kind == WGVK_WINDOW_HOST)
		wgpuQueueWriteBuffer(dev->wqueue, bo->buffer, bo->offset + at, (char *)bo->window.host + at, end - at);
	else if (rvgpu_guest_upload_buffer)
		rvgpu_guest_upload_buffer(dev->wqueue, bo->buffer, bo->offset + at, bo->window.guest + at, end - at);
	else if (rvgpu_guest_ptr)
		wgpuQueueWriteBuffer(dev->wqueue, bo->buffer, bo->offset + at, rvgpu_guest_ptr(bo->window.guest + at, end - at),
				     end - at);
}

void
wgvk_bo_download(struct wgvk_device *dev, const struct wgvk_bo *bo, uint64_t offset, uint64_t size,
		 WGPUBuffer mapped, uint64_t mapped_offset)
{
	if (bo->window.kind == WGVK_WINDOW_HOST) {
		const void *p = wgpuBufferGetConstMappedRange(mapped, mapped_offset, size);
		if (p)
			memcpy((char *)bo->window.host + offset, p, size);
		else
			wgvk_log("download: the staging buffer is not mapped\n");
	} else if (bo->window.kind == WGVK_WINDOW_GUEST) {
		if (rvgpu_guest_from_mapped)
			rvgpu_guest_from_mapped(mapped, mapped_offset, bo->window.guest + offset, size);
	}
}

/* ---- buffers */

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateBuffer(VkDevice _device, const VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
		  VkBuffer *pBuffer)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	if (pCreateInfo->size > dev->pdev->limits.maxBufferSize)
		return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
	struct wgvk_buffer *buf = vk_buffer_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*buf));
	if (!buf)
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	buf->id = wgvk_object_id(dev);
	*pBuffer = wgvk_buffer_to_handle(buf);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroyBuffer(VkDevice _device, VkBuffer _buffer, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_buffer, buf, _buffer);
	if (buf)
		vk_buffer_destroy(&dev->vk, pAllocator, &buf->vk);
}

static void
buffer_requirements(struct wgvk_device *dev, uint64_t size, VkMemoryRequirements2 *req)
{
	/* 256: WebGPU binds uniform and storage ranges at multiples of minUniform/StorageBufferOffsetAlignment */
	uint32_t align = MAX2(dev->pdev->limits.minUniformBufferOffsetAlignment,
			      dev->pdev->limits.minStorageBufferOffsetAlignment);
	req->memoryRequirements = (VkMemoryRequirements){
		.size = align64(size, 4),
		.alignment = MAX2(align, 4),
		.memoryTypeBits = (1u << WGVK_MEM_TYPE_COUNT) - 1,
	};
	VkMemoryDedicatedRequirements *d = vk_find_struct(req->pNext, MEMORY_DEDICATED_REQUIREMENTS);
	if (d) {
		d->prefersDedicatedAllocation = false;
		d->requiresDedicatedAllocation = false;
	}
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetBufferMemoryRequirements2(VkDevice _device, const VkBufferMemoryRequirementsInfo2 *pInfo,
				  VkMemoryRequirements2 *req)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_buffer, buf, pInfo->buffer);
	buffer_requirements(dev, buf->vk.size, req);
}

VKAPI_ATTR void VKAPI_CALL
wgvk_GetDeviceBufferMemoryRequirements(VkDevice _device, const VkDeviceBufferMemoryRequirements *pInfo,
				       VkMemoryRequirements2 *req)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	buffer_requirements(dev, pInfo->pCreateInfo->size, req);
}

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_BindBufferMemory2(VkDevice _device, uint32_t count, const VkBindBufferMemoryInfo *infos)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VkResult result = VK_SUCCESS;
	for (uint32_t i = 0; i < count; i++) {
		VK_FROM_HANDLE(wgvk_buffer, buf, infos[i].buffer);
		VK_FROM_HANDLE(wgvk_device_memory, mem, infos[i].memory);
		VkResult r = wgvk_memory_buffer(dev, mem) ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
		buf->mem = mem;
		buf->offset = infos[i].memoryOffset;
		const VkBindMemoryStatusKHR *st = vk_find_struct_const(infos[i].pNext, BIND_MEMORY_STATUS_KHR);
		if (st && st->pResult)
			*st->pResult = r;
		if (r != VK_SUCCESS)
			result = r;
	}
	return result;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL
wgvk_GetBufferDeviceAddress(VkDevice _device, const VkBufferDeviceAddressInfo *pInfo)
{
	return 0;	/* no buffer device addresses on WebGPU */
}

/* ---- buffer views (texel buffers: storage buffers the shader decodes, T3b) */

VKAPI_ATTR VkResult VKAPI_CALL
wgvk_CreateBufferView(VkDevice _device, const VkBufferViewCreateInfo *pCreateInfo,
		      const VkAllocationCallbacks *pAllocator, VkBufferView *pView)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	const struct wgvk_format *f = wgvk_format_get(dev->pdev, pCreateInfo->format);
	if (!f || !(f->buffer & (VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT |
				 VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT)))
		return vk_errorf(dev, VK_ERROR_FORMAT_NOT_SUPPORTED, "texel buffer of format %d", pCreateInfo->format);
	struct wgvk_buffer_view *v = vk_buffer_view_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*v));
	if (!v)
		return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
	v->fmt = f;
	v->id = wgvk_object_id(dev);
	*pView = wgvk_buffer_view_to_handle(v);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
wgvk_DestroyBufferView(VkDevice _device, VkBufferView _view, const VkAllocationCallbacks *pAllocator)
{
	VK_FROM_HANDLE(wgvk_device, dev, _device);
	VK_FROM_HANDLE(wgvk_buffer_view, v, _view);
	if (v)
		vk_buffer_view_destroy(&dev->vk, pAllocator, &v->vk);
}

/* VK_EXT_external_memory_host (T7): any host pointer can be the window of host-visible memory. */
VKAPI_ATTR VkResult VKAPI_CALL
wgvk_GetMemoryHostPointerPropertiesEXT(VkDevice _device, VkExternalMemoryHandleTypeFlagBits handleType,
				       const void *pHostPointer,
				       VkMemoryHostPointerPropertiesEXT *pMemoryHostPointerProperties)
{
	pMemoryHostPointerProperties->memoryTypeBits = 1u << WGVK_MEM_HOST;
	return VK_SUCCESS;
}
