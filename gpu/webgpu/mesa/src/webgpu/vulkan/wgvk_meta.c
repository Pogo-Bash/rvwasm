/*
 * wgvk meta pipelines (T2): render pipelines of the driver's own, in WGSL
 * written here, for what WebGPU has no command for:
 *
 *  - clears inside a render pass (vkCmdClearAttachments): a full-viewport
 *    triangle whose fragment shader returns the clear value (immediates),
 *    depth from the vertex shader, stencil by replace with the reference;
 *  - blits (vkCmdBlitImage): a quad sampling the source rectangle into the
 *    destination rectangle's viewport.
 *
 * They are cached on the device by a key of what they bake in. When T3's
 * compiler can take Mesa's vk_meta NIR shaders these can move to vk_meta.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_replay.h"

#include "util/u_string.h"
#include "vk_format.h"
#define XXH_INLINE_ALL
#include "util/xxhash.h"

static WGPUShaderModule
module(struct wgvk_device *dev, const char *wgsl)
{
	struct wgvk_compiled_shader cs = { .wgsl = (char *)wgsl, .wgsl_len = strlen(wgsl) };
	cs.hash = XXH64(wgsl, cs.wgsl_len, 0x6d657461);
	return wgvk_module_get(dev, &cs);
}

static struct wgvk_meta_entry *
lookup(struct wgvk_device *dev, uint64_t key)
{
	struct hash_entry *he = _mesa_hash_table_search(dev->meta, &key);
	return he ? he->data : NULL;
}

static void
store(struct wgvk_device *dev, uint64_t key, WGPURenderPipeline p, WGPUBindGroupLayout bgl)
{
	struct wgvk_meta_entry *e = calloc(1, sizeof(*e));
	e->key = key;
	e->pipeline = p;
	e->bgl = bgl;
	_mesa_hash_table_insert(dev->meta, &e->key, e);
}

/* 'f', 'u' or 'i': the WGSL scalar type of a colour format */
static char
format_kind(WGPUTextureFormat f)
{
	switch (f) {
	case WGPUTextureFormat_R8Uint: case WGPUTextureFormat_RG8Uint: case WGPUTextureFormat_RGBA8Uint:
	case WGPUTextureFormat_R16Uint: case WGPUTextureFormat_RG16Uint: case WGPUTextureFormat_RGBA16Uint:
	case WGPUTextureFormat_R32Uint: case WGPUTextureFormat_RG32Uint: case WGPUTextureFormat_RGBA32Uint:
	case WGPUTextureFormat_RGB10A2Uint:
		return 'u';
	case WGPUTextureFormat_R8Sint: case WGPUTextureFormat_RG8Sint: case WGPUTextureFormat_RGBA8Sint:
	case WGPUTextureFormat_R16Sint: case WGPUTextureFormat_RG16Sint: case WGPUTextureFormat_RGBA16Sint:
	case WGPUTextureFormat_R32Sint: case WGPUTextureFormat_RG32Sint: case WGPUTextureFormat_RGBA32Sint:
		return 'i';
	default:
		return 'f';
	}
}

static WGPUPipelineLayout
immediates_layout(struct wgvk_device *dev, uint32_t bytes, WGPUBindGroupLayout bgl)
{
	WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	pld.bindGroupLayoutCount = bgl ? 1 : 0;
	pld.bindGroupLayouts = bgl ? &bgl : NULL;
	pld.immediateSize = bytes;
	return wgpuDeviceCreatePipelineLayout(dev->wdev, &pld);
}

WGPURenderPipeline
wgvk_meta_clear_pipeline(struct wgvk_device *dev, const struct wgvk_rendering *r, uint32_t color_mask, bool depth,
			 bool stencil)
{
	struct {
		uint32_t fmt[WGVK_MAX_COLOR], ds, samples, ncolor, mask;
		uint8_t depth, stencil, pad[2];
	} k;
	memset(&k, 0, sizeof(k));
	memcpy(k.fmt, r->color_fmt, sizeof(k.fmt));
	k.ds = r->ds_fmt;
	k.samples = MAX2(r->samples, 1);
	k.ncolor = r->ncolor;
	k.mask = color_mask;
	k.depth = depth;
	k.stencil = stencil;
	uint64_t key = XXH64(&k, sizeof(k), 1);
	struct wgvk_meta_entry *me = lookup(dev, key);
	if (me)
		return me->pipeline;
	WGPURenderPipeline p;

	char wgsl[2048];
	int at = snprintf(wgsl, sizeof(wgsl),
			  "struct Imm { c: vec4<u32>, z: vec4<f32> }\n"
			  "var<immediate> imm: Imm;\n"
			  "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {\n"
			  "  let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));\n"
			  "  return vec4<f32>(p * 2.0 - 1.0, imm.z.x, 1.0);\n"
			  "}\n");
	int loc = color_mask ? ffs(color_mask) - 1 : -1;
	if (loc >= 0) {
		char t = format_kind(r->color_fmt[loc]);
		const char *ty = t == 'u' ? "vec4<u32>" : t == 'i' ? "vec4<i32>" : "vec4<f32>";
		snprintf(wgsl + at, sizeof(wgsl) - at,
			 "@fragment fn fs() -> @location(%d) %s { return bitcast<%s>(imm.c); }\n", loc, ty, ty);
	} else {
		snprintf(wgsl + at, sizeof(wgsl) - at, "@fragment fn fs() { }\n");
	}
	WGPUShaderModule m = module(dev, wgsl);
	WGPUPipelineLayout pl = immediates_layout(dev, 32, NULL);

	WGPUColorTargetState ct[WGVK_MAX_COLOR];
	for (uint32_t i = 0; i < r->ncolor; i++) {
		ct[i] = (WGPUColorTargetState)WGPU_COLOR_TARGET_STATE_INIT;
		ct[i].format = r->color_fmt[i];
		ct[i].writeMask = (color_mask & (1u << i)) ? WGPUColorWriteMask_All : WGPUColorWriteMask_None;
	}
	WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
	fs.module = m;
	fs.entryPoint = wgvk_sv("fs");
	fs.targetCount = r->ncolor;
	fs.targets = ct;
	WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
	WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	rd.layout = pl;
	rd.vertex.module = m;
	rd.vertex.entryPoint = wgvk_sv("vs");
	rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	rd.multisample.count = k.samples;
	rd.fragment = &fs;
	if (r->ds_fmt) {
		bool has_depth = r->ds_fmt != WGPUTextureFormat_Stencil8;
		ds.format = r->ds_fmt;
		ds.depthWriteEnabled = has_depth ? (depth ? WGPUOptionalBool_True : WGPUOptionalBool_False)
						 : WGPUOptionalBool_Undefined;
		ds.depthCompare = has_depth ? WGPUCompareFunction_Always : WGPUCompareFunction_Undefined;
		WGPUStencilFaceState sf = { WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep,
					    stencil ? WGPUStencilOperation_Replace : WGPUStencilOperation_Keep };
		ds.stencilFront = ds.stencilBack = sf;
		ds.stencilWriteMask = stencil ? 0xff : 0;
		rd.depthStencil = &ds;
	}
	p = wgpuDeviceCreateRenderPipeline(dev->wdev, &rd);
	wgpuPipelineLayoutRelease(pl);
	store(dev, key, p, NULL);
	return p;
}

/* sample_kind: 0 float (filtered), 1 uint, 2 sint, 3 depth. sdim: 2D or 2DArray. */
WGPURenderPipeline
wgvk_meta_blit_pipeline(struct wgvk_device *dev, WGPUTextureFormat dst, WGPUTextureViewDimension sdim, int sample_kind,
			WGPUBindGroupLayout *bgl_out)
{
	struct {
		uint32_t dst, sdim, kind;
	} k = { dst, sdim, sample_kind };
	uint64_t key = XXH64(&k, sizeof(k), 2);
	struct wgvk_meta_entry *me = lookup(dev, key);
	if (me) {
		*bgl_out = me->bgl;
		return me->pipeline;
	}
	WGPURenderPipeline p;
	bool arr = sdim == WGPUTextureViewDimension_2DArray;
	bool depth_dst = dst == WGPUTextureFormat_Depth16Unorm || dst == WGPUTextureFormat_Depth32Float ||
			 dst == WGPUTextureFormat_Depth24Plus || dst == WGPUTextureFormat_Depth24PlusStencil8 ||
			 dst == WGPUTextureFormat_Depth32FloatStencil8;
	bool d3 = sdim == WGPUTextureViewDimension_3D;	/* imm.lod.y: the normalised z */
	const char *tex = sample_kind == 3 ? (arr ? "texture_depth_2d_array" : "texture_depth_2d") :
			  sample_kind == 1 ? (d3 ? "texture_3d<u32>" : arr ? "texture_2d_array<u32>" : "texture_2d<u32>") :
			  sample_kind == 2 ? (d3 ? "texture_3d<i32>" : arr ? "texture_2d_array<i32>" : "texture_2d<i32>") :
					     (d3 ? "texture_3d<f32>" : arr ? "texture_2d_array<f32>" : "texture_2d<f32>");
	const char *layer = arr ? ", i32(imm.lod.y)" : "";
	char fetch[512];
	if (sample_kind == 0 && d3)
		snprintf(fetch, sizeof(fetch), "textureSampleLevel(t, s, vec3<f32>(v.uv, imm.lod.y), imm.lod.x)");
	else if (sample_kind == 0)
		snprintf(fetch, sizeof(fetch), "textureSampleLevel(t, s, v.uv%s, imm.lod.x)", layer);
	else if (d3)
		snprintf(fetch, sizeof(fetch),
			 "textureLoad(t, vec3<i32>(floor(vec3<f32>(v.uv, imm.lod.y) * vec3<f32>(textureDimensions(t, "
			 "i32(imm.lod.x))))), i32(imm.lod.x))");
	else
		snprintf(fetch, sizeof(fetch),
			 "textureLoad(t, vec2<i32>(floor(v.uv * vec2<f32>(textureDimensions(t, i32(imm.lod.x))))), "
			 "%s%si32(imm.lod.x))", arr ? "i32(imm.lod.y)" : "", arr ? ", " : "");
	char t = format_kind(dst);
	const char *oty = t == 'u' ? "vec4<u32>" : t == 'i' ? "vec4<i32>" : "vec4<f32>";
	char out[512];
	if (depth_dst)
		snprintf(out, sizeof(out), "@fragment fn fs(v: V) -> @builtin(frag_depth) f32 { return %s%s; }\n", fetch,
			 sample_kind == 3 ? "" : ".x");
	else if (sample_kind == 3)
		snprintf(out, sizeof(out), "@fragment fn fs(v: V) -> @location(0) %s { return %s(%s); }\n", oty, oty, fetch);
	else
		snprintf(out, sizeof(out), "@fragment fn fs(v: V) -> @location(0) %s { return %s(%s); }\n", oty, oty, fetch);
	char wgsl[2048];
	snprintf(wgsl, sizeof(wgsl),
		 "struct Imm { src: vec4<f32>, lod: vec4<f32> }\n"
		 "var<immediate> imm: Imm;\n"
		 "@group(0) @binding(0) var t: %s;\n"
		 "%s"
		 "struct V { @builtin(position) p: vec4<f32>, @location(0) uv: vec2<f32> }\n"
		 "@vertex fn vs(@builtin(vertex_index) i: u32) -> V {\n"
		 "  let q = vec2<f32>(f32(i & 1u), f32((i >> 1u) & 1u));\n"
		 "  var o: V;\n"
		 "  o.p = vec4<f32>(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);\n"
		 "  o.uv = mix(imm.src.xy, imm.src.zw, q);\n"
		 "  return o;\n"
		 "}\n"
		 "%s",
		 tex, sample_kind == 0 ? "@group(0) @binding(1) var s: sampler;\n" : "", out);
	WGPUShaderModule m = module(dev, wgsl);

	WGPUBindGroupLayoutEntry e[2] = { WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT };
	e[0].binding = 0;
	e[0].visibility = WGPUShaderStage_Fragment;
	e[0].texture.viewDimension = sdim;
	e[0].texture.sampleType = sample_kind == 1 ? WGPUTextureSampleType_Uint :
				  sample_kind == 2 ? WGPUTextureSampleType_Sint :
				  sample_kind == 3 ? WGPUTextureSampleType_Depth : WGPUTextureSampleType_Float;
	e[1].binding = 1;
	e[1].visibility = WGPUShaderStage_Fragment;
	e[1].sampler.type = WGPUSamplerBindingType_Filtering;
	WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	bd.entryCount = sample_kind == 0 ? 2 : 1;
	bd.entries = e;
	WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(dev->wdev, &bd);
	WGPUPipelineLayout pl = immediates_layout(dev, 32, bgl);

	WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	rd.layout = pl;
	rd.vertex.module = m;
	rd.vertex.entryPoint = wgvk_sv("vs");
	rd.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
	WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
	ct.format = dst;
	WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
	fs.module = m;
	fs.entryPoint = wgvk_sv("fs");
	WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
	if (depth_dst) {
		ds.format = dst;
		ds.depthWriteEnabled = WGPUOptionalBool_True;
		ds.depthCompare = WGPUCompareFunction_Always;
		rd.depthStencil = &ds;
	} else {
		fs.targetCount = 1;
		fs.targets = &ct;
	}
	rd.fragment = &fs;
	p = wgpuDeviceCreateRenderPipeline(dev->wdev, &rd);
	wgpuPipelineLayoutRelease(pl);
	store(dev, key, p, bgl);
	*bgl_out = bgl;
	return p;
}

/* ---- depth copies WebGPU does not do (T1's WGVK_FMT_DEPTH_NO_COPY / _NO_COPY_DST): a buffer into a depth
 * aspect by drawing it (frag_depth), a depth24plus aspect into a buffer by a compute shader */

/* Immediates of the upload: origin x, y; row length and image height in texels; first element; format (0 D16,
 * 1 D24 in 32-bit words, 2 D32 float); layer (relative) */
WGPURenderPipeline
wgvk_meta_depth_upload_pipeline(struct wgvk_device *dev, WGPUTextureFormat ds, WGPUBindGroupLayout *bgl_out)
{
	uint64_t key = XXH64(&ds, sizeof(ds), 3);
	struct wgvk_meta_entry *me = lookup(dev, key);
	if (me) {
		*bgl_out = me->bgl;
		return me->pipeline;
	}
	static const char wgsl[] =
		"struct Imm { ox: u32, oy: u32, row: u32, height: u32, first: u32, fmt: u32, layer: u32, pad: u32 }\n"
		"var<immediate> imm: Imm;\n"
		"@group(0) @binding(0) var<storage, read> src: array<u32>;\n"
		"@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {\n"
		"  let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));\n"
		"  return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n"
		"@fragment fn fs(@builtin(position) pos: vec4<f32>) -> @builtin(frag_depth) f32 {\n"
		"  let x = u32(pos.x) - imm.ox; let y = u32(pos.y) - imm.oy;\n"
		"  let e = imm.first + (imm.layer * imm.height + y) * imm.row + x;\n"
		"  if (imm.fmt == 0u) { let w = src[e >> 1u]; return f32((w >> ((e & 1u) * 16u)) & 0xffffu) / 65535.0; }\n"
		"  if (imm.fmt == 1u) { return f32(src[e] & 0xffffffu) / 16777215.0; }\n"
		"  return bitcast<f32>(src[e]);\n"
		"}\n";
	WGPUShaderModule m = module(dev, wgsl);
	WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
	e.binding = 0;
	e.visibility = WGPUShaderStage_Fragment;
	e.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
	WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
	bd.entryCount = 1;
	bd.entries = &e;
	WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(dev->wdev, &bd);
	WGPUPipelineLayout pl = immediates_layout(dev, 32, bgl);
	WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
	fs.module = m;
	fs.entryPoint = wgvk_sv("fs");
	WGPUDepthStencilState dss = WGPU_DEPTH_STENCIL_STATE_INIT;
	dss.format = ds;
	dss.depthWriteEnabled = WGPUOptionalBool_True;
	dss.depthCompare = WGPUCompareFunction_Always;
	WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	rd.layout = pl;
	rd.vertex.module = m;
	rd.vertex.entryPoint = wgvk_sv("vs");
	rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
	rd.depthStencil = &dss;
	rd.fragment = &fs;
	WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(dev->wdev, &rd);
	wgpuPipelineLayoutRelease(pl);
	store(dev, key, p, bgl);
	*bgl_out = bgl;
	return p;
}

/* Immediates of the readback: origin x, y; width, height (texels); row length and image height; first word;
 * layer count; mip level. One invocation per texel; the result is Vulkan's D24 layout (24 bits in a word). */
WGPUComputePipeline
wgvk_meta_depth_readback_pipeline(struct wgvk_device *dev, WGPUBindGroupLayout *bgl_out)
{
	static WGPUComputePipeline pipe;	/* one device per process */
	static WGPUBindGroupLayout bgl;
	if (!pipe) {
		static const char wgsl[] =
			"struct Imm { ox: u32, oy: u32, w: u32, h: u32, row: u32, height: u32, first: u32, mip: u32 }\n"
			"var<immediate> imm: Imm;\n"
			"@group(0) @binding(0) var t: texture_depth_2d_array;\n"
			"@group(0) @binding(1) var<storage, read_write> o: array<u32>;\n"
			"@compute @workgroup_size(8, 8) fn main(@builtin(global_invocation_id) g: vec3<u32>) {\n"
			"  if (g.x >= imm.w || g.y >= imm.h) { return; }\n"
			"  let d = textureLoad(t, vec2<i32>(i32(imm.ox + g.x), i32(imm.oy + g.y)), i32(g.z), i32(imm.mip));\n"
			"  o[imm.first + (g.z * imm.height + g.y) * imm.row + g.x] = u32(round(clamp(d, 0.0, 1.0) * 16777215.0));\n"
			"}\n";
		WGPUBindGroupLayoutEntry e[2] = { WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT };
		e[0].binding = 0;
		e[0].visibility = WGPUShaderStage_Compute;
		e[0].texture.sampleType = WGPUTextureSampleType_Depth;
		e[0].texture.viewDimension = WGPUTextureViewDimension_2DArray;
		e[1].binding = 1;
		e[1].visibility = WGPUShaderStage_Compute;
		e[1].buffer.type = WGPUBufferBindingType_Storage;
		WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		bd.entryCount = 2;
		bd.entries = e;
		bgl = wgpuDeviceCreateBindGroupLayout(dev->wdev, &bd);
		WGPUPipelineLayout pl = immediates_layout(dev, 32, bgl);
		WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		cd.layout = pl;
		cd.compute.module = module(dev, wgsl);
		cd.compute.entryPoint = wgvk_sv("main");
		pipe = wgpuDeviceCreateComputePipeline(dev->wdev, &cd);
		wgpuPipelineLayoutRelease(pl);
	}
	*bgl_out = bgl;
	return pipe;
}

/* The last 1-3 bytes of a buffer copy whose size is not a whole number of words (WebGPU copies whole words):
 * binding 0 holds the source word (a 4-byte copy of the bytes at the end of the range), binding 1 is the
 * destination; the immediates are the destination word's index and the mask of the bytes to take. One
 * invocation. VK-16 (a 34-byte update of Blender's). */
WGPUComputePipeline
wgvk_meta_tail_copy_pipeline(struct wgvk_device *dev, WGPUBindGroupLayout *bgl_out)
{
	static WGPUComputePipeline pipe;	/* one device per process */
	static WGPUBindGroupLayout bgl;
	if (!pipe) {
		static const char wgsl[] =
			"struct Imm { word: u32, mask: u32 }\n"
			"var<immediate> imm: Imm;\n"
			"@group(0) @binding(0) var<storage, read> s: array<u32>;\n"
			"@group(0) @binding(1) var<storage, read_write> o: array<u32>;\n"
			"@compute @workgroup_size(1) fn main() {\n"
			"  o[imm.word] = (o[imm.word] & ~imm.mask) | (s[0] & imm.mask);\n"
			"}\n";
		WGPUBindGroupLayoutEntry e[2] = { WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT };
		e[0].binding = 0;
		e[0].visibility = WGPUShaderStage_Compute;
		e[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		e[1].binding = 1;
		e[1].visibility = WGPUShaderStage_Compute;
		e[1].buffer.type = WGPUBufferBindingType_Storage;
		WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		bd.entryCount = 2;
		bd.entries = e;
		bgl = wgpuDeviceCreateBindGroupLayout(dev->wdev, &bd);
		WGPUPipelineLayout pl = immediates_layout(dev, 8, bgl);
		WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		cd.layout = pl;
		cd.compute.module = module(dev, wgsl);
		cd.compute.entryPoint = wgvk_sv("main");
		pipe = wgpuDeviceCreateComputePipeline(dev->wdev, &cd);
		wgpuPipelineLayoutRelease(pl);
	}
	*bgl_out = bgl;
	return pipe;
}
