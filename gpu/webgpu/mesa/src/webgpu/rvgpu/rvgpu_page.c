/*
 * rvgpu in the page: the WebGPU instance, the Worker's device handed to wgvk,
 * and the clear smoke test (tools/www/rvgpu-smoke.html). INTERFACES.md 1, 8.
 *
 * SPDX-License-Identifier: MIT
 */
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#include "rvgpu.h"
#include "smoke-clear.h"
#include "wgvk.h"

/* library_rvgpu.js: WebGPU.importJsDevice(Module.rvgpuDevice, instance) */
extern WGPUDevice rvgpu_js_import_device(WGPUInstance instance);
WGPUTexture wgvk_image_texture(VkImage image);

int rvgpu_smoke_clear(float r, float g, float b, float a);
int rvgpu_smoke_draw(void);
int smoke_draw(struct smoke *s, PFN_vkGetInstanceProcAddr gipa, const char *device_prefix);
const char *rvgpu_smoke_msg(void);
const char *rvgpu_smoke_device(void);
WGPUTexture rvgpu_smoke_texture(void);

static WGPUInstance instance;

EMSCRIPTEN_KEEPALIVE int
rvgpu_init(void)
{
	WGPUInstanceFeatureName feature = WGPUInstanceFeatureName_TimedWaitAny;
	WGPUInstanceDescriptor desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
	desc.requiredFeatureCount = 1;
	desc.requiredFeatures = &feature;
	instance = wgpuCreateInstance(&desc);
	if (!instance)
		return -1;
	WGPUDevice device = rvgpu_js_import_device(instance);
	if (!device)
		return -1;
	wgvk_use_device(instance, NULL, device);
	return 0;
}

/* ---- T2: the pipeline cache in IndexedDB (rvgpu-worker.js), and polling wgvk from the Worker's loop */

/* library_rvgpu.js: hands a record to Module.rvgpuCacheStore (the Worker batches them into IndexedDB) */
extern void rvgpu_js_cache_store(const void *record, size_t size);

void rvgpu_pipeline_cache(const void *blob, size_t size);
unsigned rvgpu_poll(void);
const char *rvgpu_smoke_stats(void);

static void
cache_store(void *data, const void *record, size_t size)
{
	rvgpu_js_cache_store(record, size);
}

/* The records IndexedDB had (back to back), before the front ends make their devices. */
EMSCRIPTEN_KEEPALIVE void
rvgpu_pipeline_cache(const void *blob, size_t size)
{
	wgvk_set_pipeline_cache(cache_store, NULL, blob, size);
}

EMSCRIPTEN_KEEPALIVE unsigned
rvgpu_poll(void)
{
	return wgvk_poll_all();
}

static struct smoke smoke;

/* A JSPI export (it waits for the GPU inside vkQueueSubmit): returns a Promise in JavaScript. */
EMSCRIPTEN_KEEPALIVE int
rvgpu_smoke_clear(float r, float g, float b, float a)
{
	const float rgba[4] = { r, g, b, a };
	return smoke_clear(&smoke, wgvk_GetInstanceProcAddr, rgba, "wgvk");
}

/* T2's draw smoke test (tests/smoke-draw.c), a JSPI export too. */
EMSCRIPTEN_KEEPALIVE int
rvgpu_smoke_draw(void)
{
	return smoke_draw(&smoke, wgvk_GetInstanceProcAddr, "wgvk");
}

EMSCRIPTEN_KEEPALIVE const char *
rvgpu_smoke_msg(void)
{
	return smoke.msg;
}

EMSCRIPTEN_KEEPALIVE const char *
rvgpu_smoke_device(void)
{
	return smoke.device_name;
}

EMSCRIPTEN_KEEPALIVE WGPUTexture
rvgpu_smoke_texture(void)
{
	return smoke.image ? wgvk_image_texture(smoke.image) : NULL;
}

void wgvk_device_stats_text(VkDevice device, char *buf, size_t size);

/* the smoke test device's pipeline counters */
EMSCRIPTEN_KEEPALIVE const char *
rvgpu_smoke_stats(void)
{
	static char buf[256];
	buf[0] = 0;
	if (smoke.device)
		wgvk_device_stats_text(smoke.device, buf, sizeof(buf));
	return buf;
}

/* The page Worker's present (T7, INTERFACES.md 9): what resource `res` shows now, as four words {texture
 * (the webgpu.h handle: Module.rvgpuJsObject gives the GPUTexture), width, height, RVGPU_PRESENT_* flags},
 * or NULL when no front end knows the resource or it has no texture; {0, 0, 0, RVGPU_PRESENT_BUSY} while
 * the ring is being drained (ask again). Called once the ring's `done` has
 * passed the frame, so the work that drew it has been submitted; what the Worker submits next is ordered
 * after it on the one queue. */
const uint32_t *rvgpu_present_grab(uint32_t res);

EMSCRIPTEN_KEEPALIVE const uint32_t *
rvgpu_present_grab(uint32_t res)
{
	static uint32_t out[4];
	struct rvgpu_present_image img;
	if (rvgpu_ring_busy()) {
		/* (a front end is mid-record, maybe waiting for the GPU: its work for this frame may not be flushed
		 * yet, and flushing it from here would cut into that record; the Worker asks again shortly) */
		out[0] = out[1] = out[2] = 0;
		out[3] = RVGPU_PRESENT_BUSY;
		return out;
	}
	if (!rvgpu_present_find(res, &img))
		return NULL;
	out[0] = (uint32_t)(uintptr_t)img.tex;
	out[1] = img.width;
	out[2] = img.height;
	out[3] = img.flags;
	return out;
}

/* Venus blobs that may be window buffers (rvgpu.h): res -> (device, memory), resolved when a window shows one. */
static struct {
	uint32_t res;
	VkDevice device;
	VkDeviceMemory memory;
} blobs[256];

static bool
memory_resolve(void *front, uint32_t res, struct rvgpu_present_image *out)
{
	(void)front;
	for (unsigned i = 0; i < 256; i++) {
		if (blobs[i].res != res)
			continue;
		VkImage image = wgvk_memory_image(blobs[i].device, blobs[i].memory);
		if (image == VK_NULL_HANDLE)
			return false;
		out->tex = wgvk_image_present_texture(image, &out->width, &out->height);
		out->flags = RVGPU_PRESENT_Y_0_TOP;	/* (Vulkan images: row 0 is the top) */
		return out->tex != NULL;
	}
	return false;
}

extern void (*rvgpu_present_forget_hook)(uint32_t res);

static void
memory_forget(uint32_t res)
{
	for (unsigned i = 0; i < 256; i++)
		if (blobs[i].res == res)
			blobs[i].res = 0;
}

void
rvgpu_present_register_memory(uint32_t res, uint64_t device, uint64_t memory)
{
	static bool added;
	unsigned slot = 256;
	for (unsigned i = 0; i < 256; i++) {
		if (blobs[i].res == res || (!blobs[i].res && slot == 256))
			slot = i;
		if (blobs[i].res == res)
			break;
	}
	if (slot == 256)
		return;
	blobs[slot].res = res;
	blobs[slot].device = (VkDevice)(uintptr_t)device;
	memcpy(&blobs[slot].memory, &memory, sizeof(blobs[slot].memory));
	if (!added) {
		rvgpu_present_add_resolver(memory_resolve, NULL);
		rvgpu_present_forget_hook = memory_forget;
		added = true;
	}
}
