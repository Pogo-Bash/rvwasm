/*
 * wgvk index rewriting (T2): what WebGPU's primitive assembly cannot do,
 * done by making a list index buffer on the CPU.
 *
 *  - triangle fans (WebGPU has lists and strips only);
 *  - the provoking vertex "last" (GL's default; zink asks for it through
 *    VK_EXT_provoking_vertex): WebGPU's flat inputs take the first vertex,
 *    so each primitive's indices are rotated, (a, b, c) -> (c, a, b), which
 *    keeps the winding (INTERFACES.md 6.1); lines are swapped. Only when the
 *    fragment shader has flat inputs;
 *  - 8-bit indices (WebGPU has 16 and 32).
 *
 * The draw becomes an indexed draw of a list from a temporary 32-bit index
 * buffer, with Vulkan's vertex offset (or first vertex) as the base vertex.
 * It needs the indices on the CPU: a host-visible index buffer the GPU is not
 * writing, and draw arguments the CPU has (indirect draws whose arguments
 * the CPU can read are direct draws by then). Otherwise the draw goes as it
 * is and the difference is logged once (T2: a compute pre-pass).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_replay.h"

static bool gpu_rewrite(struct wgvk_replay *rp, enum wgvk_draw_kind *kind, uint32_t a[5], VkPrimitiveTopology topo,
			bool last, size_t isz, struct wgvk_buffer *ind, uint64_t ind_off, struct wgvk_index_override *o);

static bool
has_flat_inputs(const struct wgvk_shader *fs)
{
	if (!fs)
		return false;
	for (unsigned i = 0; i < 32; i++) {
		uint8_t v = fs->cs.fs_varyings[i];
		if ((v & WGVK_VARYING_USED) && WGVK_VARYING_INTERP(v) == INTERP_MODE_FLAT)
			return true;
	}
	return false;
}

static uint32_t
restart_index(VkIndexType t)
{
	return t == VK_INDEX_TYPE_UINT8_KHR ? 0xff : t == VK_INDEX_TYPE_UINT16 ? 0xffff : 0xffffffff;
}

struct emit {
	uint32_t *out;
	size_t n, cap;
};

static void
put(struct emit *e, uint32_t v)
{
	if (e->n == e->cap) {
		e->cap = MAX2(e->cap * 2, 1024);
		e->out = realloc(e->out, e->cap * sizeof(*e->out));
	}
	e->out[e->n++] = v;
}

/* One run of vertices without restarts, as list primitives. */
static void
run(struct emit *e, VkPrimitiveTopology topo, const uint32_t *v, size_t n, bool last)
{
	switch (topo) {
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		for (size_t i = 0; i + 2 < n; i += 3) {
			if (last) {
				put(e, v[i + 2]); put(e, v[i]); put(e, v[i + 1]);
			} else {
				put(e, v[i]); put(e, v[i + 1]); put(e, v[i + 2]);
			}
		}
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		for (size_t i = 0; i + 2 < n; i++) {
			/* Vulkan's strip order: even (i, i+1, i+2), odd (i+1, i, i+2); the provoking vertex is the
			 * first (i) or the last (i+2) of the triangle */
			uint32_t a = (i & 1) ? v[i + 1] : v[i], b = (i & 1) ? v[i] : v[i + 1], c = v[i + 2];
			if (last) {
				put(e, c); put(e, a); put(e, b);
			} else {
				put(e, a); put(e, b); put(e, c);
			}
		}
		break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		for (size_t i = 1; i + 1 < n; i++) {
			/* (v0, vi, vi+1); Vulkan's provoking vertex of a fan triangle is vi (first) or vi+1 (last) */
			if (last) {
				put(e, v[i + 1]); put(e, v[0]); put(e, v[i]);
			} else {
				put(e, v[i]); put(e, v[i + 1]); put(e, v[0]);
			}
		}
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		for (size_t i = 0; i + 1 < n; i += 2) {
			put(e, v[i + (last ? 1 : 0)]);
			put(e, v[i + (last ? 0 : 1)]);
		}
		break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		for (size_t i = 0; i + 1 < n; i++) {
			put(e, v[i + (last ? 1 : 0)]);
			put(e, v[i + (last ? 0 : 1)]);
		}
		break;
	default:	/* points: as they are */
		for (size_t i = 0; i < n; i++)
			put(e, v[i]);
		break;
	}
}

static bool
is_rewritable(VkPrimitiveTopology t)
{
	return t <= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
}

/* ---- the same on the GPU, for indices the CPU does not have: a compute pass before the render pass */

static const char gpu_wgsl[] =
	"struct P { count: u32, first: u32, mode: u32, last: u32, isz: u32, indirect: u32, args: u32, max_prims: u32,\n"
	"           inst: u32, vo: u32, first_inst: u32, pad: u32 }\n"
	"@group(0) @binding(0) var<storage, read> src: array<u32>;\n"
	"@group(0) @binding(1) var<storage, read_write> dst: array<u32>;\n"
	"@group(0) @binding(2) var<uniform> p: P;\n"
	"@group(0) @binding(3) var<storage, read> args: array<u32>;\n"
	/* indirect: the draw's arguments from args (indexed: count, instances, first index, vertex offset, first
	 * instance; not indexed: count, instances, first vertex, first instance) */
	"fn first() -> u32 { if (p.indirect == 1u) { return select(0u, p.first, p.isz != 0u) + args[p.args + 2u]; } return p.first; }\n"
	"fn idx(i: u32) -> u32 {\n"
	"  let k = first() + i;\n"
	"  if (p.isz == 0u) { return k; }\n"
	"  if (p.isz == 4u) { return src[k]; }\n"
	"  if (p.isz == 2u) { let w = src[k >> 1u]; return select(w & 0xffffu, w >> 16u, (k & 1u) == 1u); }\n"
	"  let w = src[k >> 2u]; return (w >> ((k & 3u) * 8u)) & 0xffu;\n"
	"}\n"
	"fn prims(c: u32) -> u32 {\n"
	"  var n = 0u;\n"
	"  if (p.mode == 0u) { n = c / 3u; } else if (p.mode <= 2u) { n = select(0u, c - 2u, c >= 3u); }\n"
	"  else if (p.mode == 3u) { n = c / 2u; } else { n = select(0u, c - 1u, c >= 2u); }\n"
	"  return min(n, p.max_prims);\n"
	"}\n"
	/* mode: 0 triangle list, 1 strip, 2 fan, 3 line list, 4 line strip; one primitive per invocation; the
	 * indices from word 8 of dst, the indexed indirect draw at words 0..4 */
	"@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) g: vec3<u32>) {\n"
	"  let i = g.x + g.y * 65535u * 64u;\n"
	"  let c = select(p.count, args[p.args], p.indirect == 1u);\n"
	"  let n = prims(c);\n"
	"  let per = select(3u, 2u, p.mode >= 3u);\n"
	"  if (i == 0u) {\n"
	"    dst[0] = n * per;\n"
	"    dst[1] = select(p.inst, args[p.args + 1u], p.indirect == 1u);\n"
	"    dst[2] = 8u;\n"
	"    dst[3] = select(p.vo, select(0u, args[p.args + 3u], p.isz != 0u), p.indirect == 1u);\n"
	"    dst[4] = select(p.first_inst, args[p.args + select(3u, 4u, p.isz != 0u)], p.indirect == 1u);\n"
	"  }\n"
	"  if (i >= n) { return; }\n"
	"  let o = 8u + per * i;\n"
	"  if (p.mode <= 2u) {\n"
	"    var a: u32; var b: u32; var cc: u32;\n"
	"    if (p.mode == 0u) { a = idx(3u * i); b = idx(3u * i + 1u); cc = idx(3u * i + 2u); }\n"
	"    else if (p.mode == 1u) {\n"
	"      if ((i & 1u) == 1u) { a = idx(i + 1u); b = idx(i); } else { a = idx(i); b = idx(i + 1u); } cc = idx(i + 2u); }\n"
	"    else { a = idx(i + 1u); b = idx(i + 2u); cc = idx(0u);\n"
	"      if (p.last == 1u) { a = idx(i + 2u); b = idx(0u); cc = idx(i + 1u); } }\n"
	"    if (p.last == 1u && p.mode != 2u) { let t = cc; cc = b; b = a; a = t; }\n"
	"    dst[o] = a; dst[o + 1u] = b; dst[o + 2u] = cc;\n"
	"  } else {\n"
	"    var a: u32; var b: u32;\n"
	"    if (p.mode == 3u) { a = idx(2u * i); b = idx(2u * i + 1u); } else { a = idx(i); b = idx(i + 1u); }\n"
	"    if (p.last == 1u) { let t = a; a = b; b = t; }\n"
	"    dst[o] = a; dst[o + 1u] = b;\n"
	"  }\n"
	"}\n";

static WGPUComputePipeline
gpu_pipeline(struct wgvk_device *dev, WGPUBindGroupLayout *bgl_out)
{
	static WGPUComputePipeline pipe;	/* one device per process */
	static WGPUBindGroupLayout bgl;
	if (!pipe) {
		WGPUBindGroupLayoutEntry e[4];
		for (int i = 0; i < 4; i++) {
			e[i] = (WGPUBindGroupLayoutEntry)WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
			e[i].binding = i;
			e[i].visibility = WGPUShaderStage_Compute;
		}
		e[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		e[1].buffer.type = WGPUBufferBindingType_Storage;
		e[2].buffer.type = WGPUBufferBindingType_Uniform;
		e[3].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		bd.entryCount = 4;
		bd.entries = e;
		bgl = wgpuDeviceCreateBindGroupLayout(dev->wdev, &bd);
		WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pld.bindGroupLayoutCount = 1;
		pld.bindGroupLayouts = &bgl;
		WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev->wdev, &pld);
		struct wgvk_compiled_shader cs = { .wgsl = (char *)gpu_wgsl, .wgsl_len = strlen(gpu_wgsl), .hash = 0x6964787278ull };
		WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		cd.layout = pl;
		cd.compute.module = wgvk_module_get(dev, &cs);
		cd.compute.entryPoint = wgvk_sv("main");
		pipe = wgpuDeviceCreateComputePipeline(dev->wdev, &cd);
		wgpuPipelineLayoutRelease(pl);
	}
	*bgl_out = bgl;
	return pipe;
}

/* A storage window over a buffer range (aligned down): the binding entry, and the words before the range. */
static uint32_t
window(struct wgvk_replay *rp, struct wgvk_buffer *b, uint64_t off, uint64_t size, WGPUBindGroupEntry *e)
{
	uint64_t wo;
	e->buffer = wgvk_buffer_wgpu(b, off, &wo);
	uint32_t salign = MAX2(rp->dev->pdev->limits.minStorageBufferOffsetAlignment, 4);
	uint64_t base = wo / salign * salign;
	uint64_t end = MIN2(align64(wo + size, 4), b->mem->bo.offset + b->mem->bo.size);
	e->offset = base;
	e->size = MAX2(align64(end - base, 4), 4);
	wgvk_upload(rp, b->mem, base - b->mem->bo.offset, e->size);	/* a read in the pre-pass */
	return (wo - base) / 4;
}

static bool
gpu_rewrite(struct wgvk_replay *rp, enum wgvk_draw_kind *kind, uint32_t a[5], VkPrimitiveTopology topo, bool last,
	    size_t isz, struct wgvk_buffer *ind, uint64_t ind_off, struct wgvk_index_override *o)
{
	struct wgvk_device *dev = rp->dev;
	const struct vk_dynamic_graphics_state *dyn = &rp->cmd->vk.dynamic_graphics_state;
	bool indexed = isz != 0;
	if (dyn->ia.primitive_restart_enable && topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST &&
	    topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
		wgvk_warn_once("primitive restart in a GPU-side index rewrite: ignored (T2)\n");
	uint32_t mode, per;
	switch (topo) {
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: mode = 0; per = 3; break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: mode = 1; per = 3; break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: mode = 2; per = 3; break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: mode = 3; per = 2; break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: mode = 4; per = 2; break;
	default:
		wgvk_warn_once("topology %d: no GPU index rewrite\n", topo);
		return false;
	}
	/* how many vertices the draw can have: its count, or for an indirect draw what its buffers hold */
	uint64_t max_count = a[0];
	if (ind) {
		if (indexed) {
			max_count = rp->ib.size / isz;
		} else {
			max_count = 1u << 20;
			if (dyn->vi)
				u_foreach_bit(b, dyn->vi->bindings_valid) {
					if (dyn->vi->bindings[b].input_rate != VK_VERTEX_INPUT_RATE_VERTEX || !dyn->vi_binding_strides[b])
						continue;
					max_count = MIN2(max_count, rp->vb[b].size / dyn->vi_binding_strides[b] + 1);
				}
		}
	}
	uint32_t max_prims = mode == 0 ? max_count / 3 : mode <= 2 ? (max_count >= 3 ? max_count - 2 : 0) :
			     mode == 3 ? max_count / 2 : (max_count >= 2 ? max_count - 1 : 0);
	memset(o, 0, sizeof(*o));
	if (!max_prims) {
		o->empty = true;
		return true;
	}
	WGPUBindGroupEntry be[4];
	for (int i = 0; i < 4; i++) {
		be[i] = (WGPUBindGroupEntry)WGPU_BIND_GROUP_ENTRY_INIT;
		be[i].binding = i;
	}
	uint32_t src_first = 0;
	if (indexed) {
		struct wgvk_buffer *ib = rp->ib.buf;
		if (!ib || !ib->mem || !ib->mem->bo.buffer)
			return false;
		uint32_t w0 = window(rp, ib, rp->ib.off, rp->ib.size, &be[0]);
		src_first = w0 * 4 / isz + (ind ? 0 : a[2]);
	} else {
		be[0].buffer = wgvk_dummy_buffer(dev);
		be[0].size = 256;
	}
	uint32_t args_word = 0;
	if (ind) {
		args_word = window(rp, ind, ind_off, indexed ? 20 : 16, &be[3]);
	} else {
		be[3].buffer = wgvk_dummy_buffer(dev);
		be[3].size = 256;
	}
	uint32_t params[12] = { a[0], ind ? src_first : (indexed ? src_first : a[2]), mode, last, (uint32_t)isz, ind != NULL,
				args_word, max_prims, a[1], indexed ? a[3] : 0, indexed ? a[4] : a[3], 0 };
	memcpy(wgvk_ring_alloc(rp, sizeof(params), &be[2].buffer, &be[2].offset), params, sizeof(params));
	be[2].size = 64;
	uint64_t out_size = 32 + align64((uint64_t)max_prims * per * 4, 4);
	WGPUBuffer dst = wgvk_temp_buffer(rp, out_size, WGPUBufferUsage_Storage | WGPUBufferUsage_Index |
						      WGPUBufferUsage_Indirect);
	be[1].buffer = dst;
	be[1].size = out_size;
	WGPUBindGroupLayout bgl;
	struct wgvk_prepass pp = { .pipe = gpu_pipeline(dev, &bgl), .ngroups = 1 };
	WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	bd.layout = bgl;
	bd.entryCount = 4;
	bd.entries = be;
	pp.bg[0] = wgpuDeviceCreateBindGroup(dev->wdev, &bd);
	util_dynarray_append(&rp->sub->groups, pp.bg[0]);
	uint32_t groups = DIV_ROUND_UP(max_prims, 64);
	pp.groups[0] = MIN2(groups, 65535);
	pp.groups[1] = DIV_ROUND_UP(groups, 65535);
	pp.groups[2] = 1;
	o->prepass = pp;
	o->has_prepass = true;
	o->active = true;
	o->indirect = true;
	o->buf = dst;
	o->size = out_size;
	o->topology = per == 3 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	*kind = WGVK_DRAW_INDEXED_INDIRECT;
	dev->stats.index_rewrites++;
	return true;
}

bool
wgvk_index_rewrite(struct wgvk_replay *rp, enum wgvk_draw_kind *kind, uint32_t a[5], struct wgvk_buffer *ind,
		   uint64_t ind_off, struct wgvk_index_override *o)
{
	const struct vk_dynamic_graphics_state *dyn = &rp->cmd->vk.dynamic_graphics_state;
	VkPrimitiveTopology topo = dyn->ia.primitive_topology;
	bool indexed = *kind == WGVK_DRAW_INDEXED || *kind == WGVK_DRAW_INDEXED_INDIRECT;
	bool fan = topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	bool u8 = indexed && rp->ib.type == VK_INDEX_TYPE_UINT8_KHR;
	bool last = dyn->rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT &&
		    topo != VK_PRIMITIVE_TOPOLOGY_POINT_LIST && has_flat_inputs(rp->fs);
	memset(o, 0, sizeof(*o));
	if (!fan && !u8 && !last)
		return false;
	if (!is_rewritable(topo)) {
		wgvk_warn_once("topology %d with a provoking vertex or 8-bit indices: not rewritten\n", topo);
		return false;
	}
	if (ind) {
		size_t isz = !indexed ? 0 : rp->ib.type == VK_INDEX_TYPE_UINT8_KHR ? 1 : rp->ib.type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
		return gpu_rewrite(rp, kind, a, topo, last, isz, ind, ind_off, o);
	}
	uint32_t count = a[0];
	size_t isz = 0;
	void *src = NULL;
	if (indexed) {
		isz = rp->ib.type == VK_INDEX_TYPE_UINT8_KHR ? 1 : rp->ib.type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
		src = malloc((size_t)count * isz + 4);
		if (!rp->ib.buf || !wgvk_cpu_bytes(rp, rp->ib.buf, rp->ib.off + (uint64_t)a[2] * isz, (uint64_t)count * isz, src)) {
			free(src);
			return gpu_rewrite(rp, kind, a, topo, last, isz, NULL, 0, o);
		}
	}
	uint32_t *v = malloc((size_t)MAX2(count, 1) * sizeof(*v));
	struct emit e = { 0 };
	bool restart = indexed && dyn->ia.primitive_restart_enable;
	uint32_t ri = indexed ? restart_index(rp->ib.type) : 0;
	size_t start = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t x = !indexed ? a[2] + i :
			     isz == 1 ? ((uint8_t *)src)[i] : isz == 2 ? ((uint16_t *)src)[i] : ((uint32_t *)src)[i];
		if (restart && x == ri) {
			run(&e, topo, v + start, i - start, last);
			start = i + 1;
			continue;
		}
		v[i] = x;
	}
	run(&e, topo, v + start, count - start, last);
	free(src);
	free(v);
	if (!e.n) {
		o->empty = true;
		return true;
	}
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	bd.size = align64(e.n * 4, 4);
	bd.usage = WGPUBufferUsage_Index;
	bd.mappedAtCreation = true;
	o->buf = wgpuDeviceCreateBuffer(rp->dev->wdev, &bd);
	void *p = wgpuBufferGetMappedRange(o->buf, 0, bd.size);
	if (p)
		memcpy(p, e.out, e.n * 4);
	wgpuBufferUnmap(o->buf);
	util_dynarray_append(&rp->sub->buffers, o->buf);
	free(e.out);
	o->active = true;
	o->size = bd.size;
	o->topology = topo >= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
		      topo >= VK_PRIMITIVE_TOPOLOGY_LINE_LIST ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
							       : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	/* an indexed draw of the new list: base vertex = Vulkan's vertex offset (non-indexed: the indices hold
	 * the vertex numbers) */
	uint32_t inst = a[1], first_inst = indexed ? a[4] : a[3];
	int32_t base = indexed ? (int32_t)a[3] : 0;
	a[0] = e.n;
	a[1] = inst;
	a[2] = 0;
	a[3] = (uint32_t)base;
	a[4] = first_inst;
	*kind = WGVK_DRAW_INDEXED;
	rp->dev->stats.index_rewrites++;
	return true;
}
