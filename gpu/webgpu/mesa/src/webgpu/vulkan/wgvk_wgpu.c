/*
 * The WebGPU device under wgvk, and waiting on it (INTERFACES.md 1 and 2).
 *
 * In the page the Worker hands wgvk its device (wgvk_use_device). As a
 * native ICD nobody does, and wgvk makes a Dawn native device itself. Dawn's
 * Vulkan backend then loads the Vulkan loader, which loads every ICD, wgvk
 * included: while wgvk is making its device it reports no physical device of
 * its own (wgvk_wgpu_initializing), so Dawn sees only the real driver
 * (lavapipe in the build VM).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"

static WGPUInstance the_instance;
static WGPUAdapter the_adapter;
static WGPUDevice the_device;
static bool initializing;

bool
wgvk_wgpu_initializing(void)
{
	return initializing;
}

void
wgvk_use_device(WGPUInstance instance, WGPUAdapter adapter, WGPUDevice device)
{
	if (instance)
		wgpuInstanceAddRef(instance);
	if (adapter)
		wgpuAdapterAddRef(adapter);
	wgpuDeviceAddRef(device);
	the_instance = instance;
	the_adapter = adapter;
	the_device = device;
}

static void
str_print(const char *what, WGPUStringView s)
{
	wgvk_log("%s: %.*s\n", what, (int)(s.length == WGPU_STRLEN ? strlen(s.data ? s.data : "") : s.length),
		 s.data ? s.data : "");
}

static void
on_error(WGPUDevice const *device, WGPUErrorType type, WGPUStringView message, void *u1, void *u2)
{
	str_print(type == WGPUErrorType_Validation ? "WebGPU validation error" : "WebGPU error", message);
}

static void
on_lost(WGPUDevice const *device, WGPUDeviceLostReason reason, WGPUStringView message, void *u1, void *u2)
{
	if (reason != WGPUDeviceLostReason_Destroyed && reason != WGPUDeviceLostReason_CallbackCancelled)
		str_print("WebGPU device lost", message);
}

static void
on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void *u1, void *u2)
{
	if (status == WGPURequestAdapterStatus_Success)
		*(WGPUAdapter *)u1 = adapter;
	else
		str_print("no WebGPU adapter", message);
}

static void
on_device(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void *u1, void *u2)
{
	if (status == WGPURequestDeviceStatus_Success)
		*(WGPUDevice *)u1 = device;
	else
		str_print("no WebGPU device", message);
}

#ifndef __EMSCRIPTEN__
static VkResult
make_dawn_device(void)
{
	WGPUInstanceFeatureName feature = WGPUInstanceFeatureName_TimedWaitAny;
	WGPUInstanceDescriptor idesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
	idesc.requiredFeatureCount = 1;
	idesc.requiredFeatures = &feature;
	WGPUInstance inst = wgpuCreateInstance(&idesc);
	if (!inst)
		return VK_ERROR_INITIALIZATION_FAILED;

	WGPURequestAdapterOptions opts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
	opts.backendType = WGPUBackendType_Vulkan;
	if (getenv("WGVK_BACKEND") && !strcmp(getenv("WGVK_BACKEND"), "null"))
		opts.backendType = WGPUBackendType_Null;
	WGPUAdapter adapter = NULL;
	WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
	acb.mode = WGPUCallbackMode_WaitAnyOnly;
	acb.callback = on_adapter;
	acb.userdata1 = &adapter;
	WGPUFutureWaitInfo w = { .future = wgpuInstanceRequestAdapter(inst, &opts, acb) };
	wgpuInstanceWaitAny(inst, 1, &w, UINT64_MAX);
	if (!adapter) {
		wgpuInstanceRelease(inst);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Everything the adapter has: wgvk is a driver, not an app with a budget. */
	WGPULimits limits = WGPU_LIMITS_INIT;
	wgpuAdapterGetLimits(adapter, &limits);
	WGPUSupportedFeatures features = WGPU_SUPPORTED_FEATURES_INIT;
	wgpuAdapterGetFeatures(adapter, &features);
	WGPUDeviceDescriptor ddesc = WGPU_DEVICE_DESCRIPTOR_INIT;
	ddesc.requiredLimits = &limits;
	ddesc.requiredFeatureCount = features.featureCount;
	ddesc.requiredFeatures = features.features;
	ddesc.uncapturedErrorCallbackInfo.callback = on_error;
	ddesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
	ddesc.deviceLostCallbackInfo.callback = on_lost;
	WGPUDevice device = NULL;
	WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
	dcb.mode = WGPUCallbackMode_WaitAnyOnly;
	dcb.callback = on_device;
	dcb.userdata1 = &device;
	w.future = wgpuAdapterRequestDevice(adapter, &ddesc, dcb);
	wgpuInstanceWaitAny(inst, 1, &w, UINT64_MAX);
	wgpuSupportedFeaturesFreeMembers(features);
	if (!device) {
		wgpuAdapterRelease(adapter);
		wgpuInstanceRelease(inst);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	the_instance = inst;
	the_adapter = adapter;
	the_device = device;
	return VK_SUCCESS;
}
#endif

VkResult
wgvk_wgpu_get(WGPUInstance *inst, WGPUAdapter *adapter, WGPUDevice *dev)
{
	if (!the_device) {
#ifdef __EMSCRIPTEN__
		return VK_ERROR_INITIALIZATION_FAILED;	/* the page must call wgvk_use_device */
#else
		/* WGVK_DAWN_DRIVER_FILES: the Vulkan drivers Dawn may use (e.g. only
		 * lavapipe's json), so VK_DRIVER_FILES can name wgvk alone for the app. */
		const char *df = getenv("WGVK_DAWN_DRIVER_FILES");
		char *saved = getenv("VK_DRIVER_FILES") ? strdup(getenv("VK_DRIVER_FILES")) : NULL;
		if (df)
			setenv("VK_DRIVER_FILES", df, 1);
		initializing = true;
		VkResult r = make_dawn_device();
		initializing = false;
		if (df) {
			if (saved)
				setenv("VK_DRIVER_FILES", saved, 1);
			else
				unsetenv("VK_DRIVER_FILES");
		}
		free(saved);
		if (r != VK_SUCCESS)
			return r;
#endif
	}
	*inst = the_instance;
	*adapter = the_adapter;
	*dev = the_device;
	return VK_SUCCESS;
}

/* (diagnostics for the page's stuck report, weak so the native builds do not need rvgpu: rvgpu_where says what
 * wgvk waits for, rvgpu_debug_words[2] counts the waits in progress and [4] all waits; from gpu-t5-int 8fd8612
 * and a33c003, VK-01) */
extern char rvgpu_where[128] __attribute__((weak));
extern uint32_t rvgpu_debug_words[8] __attribute__((weak));

VkResult
wgvk_wait(struct wgvk_device *dev, const WGPUFuture *f, unsigned n, uint64_t timeout_ns)
{
	if (rvgpu_where) {
		size_t l = strlen(rvgpu_where);
		if (l < 64)
			snprintf(rvgpu_where + l, 128 - l, "; wgvk_wait %u futures, first %llu, timeout %lld", n,
				 n ? (unsigned long long)f[0].id : 0ull, (long long)timeout_ns);
	}
	/* WaitAny returns when one future completes (emdawnwebgpu: the first of a Promise.race), and a completed
	 * future passed again returns at once: wait on what is left each time (T2) */
	WGPUFutureWaitInfo w[16];
	if (n > 16)
		return VK_ERROR_UNKNOWN;
	for (unsigned i = 0; i < n; i++)
		w[i] = (WGPUFutureWaitInfo){ .future = f[i], .completed = false };
	unsigned left = n;
	while (left) {
		if (rvgpu_debug_words)
			rvgpu_debug_words[2]++, rvgpu_debug_words[4]++;
		WGPUWaitStatus s = wgpuInstanceWaitAny(dev->winst, left, w, timeout_ns);
		if (rvgpu_debug_words)
			rvgpu_debug_words[2]--;
		if (s == WGPUWaitStatus_TimedOut)
			return VK_TIMEOUT;
		if (s != WGPUWaitStatus_Success)
			return VK_ERROR_DEVICE_LOST;
		unsigned k = 0;
		for (unsigned i = 0; i < left; i++)
			if (!w[i].completed)
				w[k++] = w[i];
		left = k;
	}
	return VK_SUCCESS;
}

VkResult
wgvk_create_instance(WGPUDevice device, const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc,
		     VkInstance *instance, PFN_vkGetInstanceProcAddr *gipa)
{
	if (device && device != the_device)
		wgvk_use_device(the_instance, the_adapter, device);
	VkResult r = wgvk_CreateInstance(info, alloc, instance);
	if (r == VK_SUCCESS && gipa)
		*gipa = wgvk_GetInstanceProcAddr;
	return r;
}
