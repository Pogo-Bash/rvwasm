/*
 * Spike S5: webgpu.h on Dawn native in the build VM (Vulkan backend on lavapipe): adapter, device, a WGSL compute
 * shader, a readback. The same C API is what emdawnwebgpu gives a wasm module in the page, so a driver written
 * against it can be tested here without a browser.
 */
#include <stdio.h>
#include <string.h>
#include <webgpu/webgpu.h>

static WGPUAdapter adapter; static WGPUDevice device; static int mapped;
static WGPUStringView sv(const char *s) { return (WGPUStringView){ s, strlen(s) }; }
static void on_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView msg, void *u1, void *u2)
{ if (st == WGPURequestAdapterStatus_Success) adapter = a; else fprintf(stderr, "adapter: %.*s\n", (int)msg.length, msg.data); }
static void on_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView msg, void *u1, void *u2)
{ if (st == WGPURequestDeviceStatus_Success) device = d; else fprintf(stderr, "device: %.*s\n", (int)msg.length, msg.data); }
static void on_map(WGPUMapAsyncStatus st, WGPUStringView msg, void *u1, void *u2) { mapped = st == WGPUMapAsyncStatus_Success ? 1 : -1; }

int main(void)
{
	WGPUInstance inst = wgpuCreateInstance(NULL);
	WGPURequestAdapterOptions ao = { .backendType = WGPUBackendType_Vulkan, .featureLevel = WGPUFeatureLevel_Core };
	for (int fb = 0; fb < 2 && !adapter; fb++) {
		ao.forceFallbackAdapter = fb;
		wgpuInstanceRequestAdapter(inst, &ao, (WGPURequestAdapterCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_adapter });
		for (int i = 0; i < 100 && !adapter; i++) wgpuInstanceProcessEvents(inst);
	}
	if (!adapter) { fprintf(stderr, "no adapter\n"); return 1; }
	WGPUAdapterInfo info = { 0 };
	wgpuAdapterGetInfo(adapter, &info);
	printf("adapter: %.*s / %.*s, backend %d, type %d\n", (int)info.device.length, info.device.data,
		(int)info.description.length, info.description.data, info.backendType, info.adapterType);
	wgpuAdapterRequestDevice(adapter, NULL, (WGPURequestDeviceCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_device });
	for (int i = 0; i < 100 && !device; i++) wgpuInstanceProcessEvents(inst);
	if (!device) return 1;
	const char *code = "@group(0) @binding(0) var<storage, read_write> b: array<u32>;\n"
		"@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) g: vec3u) { b[g.x] = g.x * 2u + 1u; }\n";
	WGPUShaderSourceWGSL wgsl = { .chain = { .sType = WGPUSType_ShaderSourceWGSL }, .code = sv(code) };
	WGPUShaderModuleDescriptor smd = { .nextInChain = &wgsl.chain };
	WGPUShaderModule sm = wgpuDeviceCreateShaderModule(device, &smd);
	WGPUComputePipelineDescriptor cpd = { .compute = { .module = sm, .entryPoint = sv("main") } };
	WGPUComputePipeline cp = wgpuDeviceCreateComputePipeline(device, &cpd);
	WGPUBufferDescriptor bd = { .size = 4096, .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc };
	WGPUBuffer buf = wgpuDeviceCreateBuffer(device, &bd);
	WGPUBufferDescriptor rd = { .size = 4096, .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst };
	WGPUBuffer rb = wgpuDeviceCreateBuffer(device, &rd);
	WGPUBindGroupEntry be = { .binding = 0, .buffer = buf, .size = 4096 };
	WGPUBindGroupDescriptor bgd = { .layout = wgpuComputePipelineGetBindGroupLayout(cp, 0), .entryCount = 1, .entries = &be };
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(device, &bgd);
	WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, NULL);
	WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, NULL);
	wgpuComputePassEncoderSetPipeline(pass, cp);
	wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, NULL);
	wgpuComputePassEncoderDispatchWorkgroups(pass, 16, 1, 1);
	wgpuComputePassEncoderEnd(pass);
	wgpuCommandEncoderCopyBufferToBuffer(enc, buf, 0, rb, 0, 4096);
	WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
	wgpuQueueSubmit(wgpuDeviceGetQueue(device), 1, &cb);
	wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, 4096, (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_map });
	for (int i = 0; i < 100000 && !mapped; i++) wgpuInstanceProcessEvents(inst);
	const uint32_t *v = wgpuBufferGetConstMappedRange(rb, 0, 4096);
	printf("mapped %d: b[0]=%u b[1]=%u b[1023]=%u (expect 1 3 2047)\n", mapped, v ? v[0] : 0, v ? v[1] : 0, v ? v[1023] : 0);
	return !(v && v[1023] == 2047);
}
