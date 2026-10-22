/*
 * wgvk queries in command buffers (T2; T1 made the pools, wgvk_query.c).
 *
 * WebGPU has occlusion queries only inside a render pass that names one
 * query set, each index written once per pass, and timestamps only at pass
 * boundaries. Vulkan's queries span draws, and wgvk may split one Vulkan
 * render pass into several WebGPU passes. So wgvk counts into its own
 * places:
 *
 *  - every (pool, query) a submission begins gets an accumulator, two words
 *    in a per-submission buffer (zero at the start);
 *  - each WebGPU pass that has draws inside an occlusion query gets slots of
 *    a query set made for the Vulkan render pass; after the render pass the
 *    slots are resolved and a small compute shader adds them to their
 *    accumulators (one query over several WebGPU passes adds up);
 *  - a timestamp is an empty compute pass that writes it, resolved and set
 *    into its accumulator;
 *  - vkCmdCopyQueryPoolResults is a compute shader that writes the results
 *    (32 or 64 bits, availability) from the accumulators, or from the CPU
 *    values of queries finished in earlier submissions;
 *  - at the end of the submission the accumulators are copied to a MAP_READ
 *    buffer; when the submission completes, the ended queries get their
 *    values (wgvk_query_pool_complete), unless reset since.
 *
 * Pipeline statistics and transform feedback queries have no WebGPU form:
 * they end available with zeros (faked, as T1's pools say).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_replay.h"

#define WGVK_MAX_ACC 4096

static int
acc_find(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t q)
{
	struct wgvk_qacc *a = rp->q.accs.data;
	for (int i = (int)util_dynarray_num_elements(&rp->q.accs, struct wgvk_qacc) - 1; i >= 0; i--)
		if (a[i].pool == pool && a[i].query == q)
			return a[i].live ? a[i].acc : -1;
	return -1;
}

static void
acc_buffer(struct wgvk_replay *rp)
{
	if (!rp->q.acc) {
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.size = WGVK_MAX_ACC * 8;
		bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
		rp->q.acc = wgpuDeviceCreateBuffer(rp->dev->wdev, &bd);	/* zeroed by WebGPU */
		util_dynarray_append(&rp->sub->buffers, rp->q.acc);
	}
}

static int
acc_new(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t q)
{
	acc_buffer(rp);
	if (rp->q.nacc == WGVK_MAX_ACC - 1) {
		wgvk_warn_once("more than %u queries in one submission: the rest are not counted\n", WGVK_MAX_ACC);
		return -1;
	}
	struct wgvk_qacc *a = rp->q.accs.data;
	for (int i = (int)util_dynarray_num_elements(&rp->q.accs, struct wgvk_qacc) - 1; i >= 0; i--)
		if (a[i].pool == pool && a[i].query == q)
			a[i].live = false;
	int acc = rp->q.nacc++;
	util_dynarray_append(&rp->q.accs, ((struct wgvk_qacc){ pool, q, acc, true, false }));
	return acc;
}

static bool
is_real(struct wgvk_query_pool *pool)
{
	return pool->vk.query_type == VK_QUERY_TYPE_OCCLUSION ||
	       (pool->vk.query_type == VK_QUERY_TYPE_TIMESTAMP && pool->set);
}

void
wgvk_q_reset(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t first, uint32_t count)
{
	/* the CPU side now: a submission still in flight that wrote these finds them reset (serial 0) */
	wgvk_query_pool_reset(pool, first, count);
	struct wgvk_qacc *a = rp->q.accs.data;
	for (unsigned i = 0; i < util_dynarray_num_elements(&rp->q.accs, struct wgvk_qacc); i++)
		if (a[i].pool == pool && a[i].query >= first && a[i].query < first + count)
			a[i].live = false;
}

void
wgvk_q_begin(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t q)
{
	pool->serial[q] = rp->serial;
	pool->available[q] = 0;
	if (pool->vk.query_type != VK_QUERY_TYPE_OCCLUSION)
		return;
	rp->q.active = acc_new(rp, pool, q);
}

void
wgvk_q_end(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t q)
{
	int acc = pool->vk.query_type == VK_QUERY_TYPE_OCCLUSION ? acc_find(rp, pool, q) : -1;
	if (pool->vk.query_type == VK_QUERY_TYPE_OCCLUSION)
		rp->q.active = -1;
	util_dynarray_append(&rp->sub->qdone, ((struct wgvk_qdone){ pool, q, acc }));
	struct wgvk_qacc *a = rp->q.accs.data;
	for (int i = (int)util_dynarray_num_elements(&rp->q.accs, struct wgvk_qacc) - 1; i >= 0; i--)
		if (a[i].pool == pool && a[i].query == q && a[i].live) {
			a[i].ended = true;
			break;
		}
}

/* ---- slots into accumulators: resolve a query set, then add (or set) each slot into its accumulator */

static const char acc_wgsl[] =
	"@group(0) @binding(0) var<storage, read> src: array<u32>;\n"
	"@group(0) @binding(1) var<storage, read> map: array<u32>;\n"
	"@group(0) @binding(2) var<storage, read_write> acc: array<u32>;\n"
	"@compute @workgroup_size(1) fn main() {\n"
	"  let n = map[0];\n"
	"  for (var i = 0u; i < n; i++) {\n"
	"    let a = map[2u + i];\n"
	"    let lo = src[2u * i]; let hi = src[2u * i + 1u];\n"
	"    if (map[1] == 1u) { acc[2u * a] = lo; acc[2u * a + 1u] = hi; continue; }\n"
	"    let l = acc[2u * a] + lo;\n"
	"    acc[2u * a + 1u] = acc[2u * a + 1u] + hi + select(0u, 1u, l < lo);\n"
	"    acc[2u * a] = l;\n"
	"  }\n"
	"}\n";

static const char copy_wgsl[] =
	"@group(0) @binding(0) var<storage, read> acc: array<u32>;\n"
	"@group(0) @binding(1) var<storage, read> d: array<u32>;\n"
	"@group(0) @binding(2) var<storage, read_write> dst: array<u32>;\n"
	/* d: count, flags (1: 64-bit, 2: availability, 4: partial), stride in words, values, base word; then per
	 * query: mode (0 not available, 1 accumulator, 2 CPU value, 3 available zeros), acc, lo, hi */
	"@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) g: vec3<u32>) {\n"
	"  let i = g.x;\n"
	"  if (i >= d[0]) { return; }\n"
	"  let flags = d[1]; let values = d[3];\n"
	"  let q = 5u + 4u * i;\n"
	"  let mode = d[q];\n"
	"  var lo = 0u; var hi = 0u;\n"
	"  if (mode == 1u) { lo = acc[2u * d[q + 1u]]; hi = acc[2u * d[q + 1u] + 1u]; }\n"
	"  if (mode == 2u) { lo = d[q + 2u]; hi = d[q + 3u]; }\n"
	"  let avail = mode != 0u;\n"
	"  let o = d[4] + i * d[2];\n"
	"  let w64 = (flags & 1u) != 0u;\n"
	"  if (avail || (flags & 4u) != 0u) {\n"
	"    for (var k = 0u; k < values; k++) {\n"
	"      let vlo = select(0u, lo, k == 0u); let vhi = select(0u, hi, k == 0u);\n"
	"      if (w64) { dst[o + 2u * k] = vlo; dst[o + 2u * k + 1u] = vhi; } else { dst[o + k] = vlo; }\n"
	"    }\n"
	"  }\n"
	"  if ((flags & 2u) != 0u) {\n"
	"    if (w64) { dst[o + 2u * values] = select(0u, 1u, avail); dst[o + 2u * values + 1u] = 0u; }\n"
	"    else { dst[o + values] = select(0u, 1u, avail); }\n"
	"  }\n"
	"}\n";

static WGPUComputePipeline
q_pipe(struct wgvk_device *dev, const char *wgsl, uint64_t hash, WGPUBufferBindingType b2, WGPUBindGroupLayout *bgl_out)
{
	static WGPUComputePipeline pipes[2];	/* one device per process */
	static WGPUBindGroupLayout bgls[2];
	unsigned k = wgsl == acc_wgsl ? 0 : 1;
	if (!pipes[k]) {
		WGPUBindGroupLayoutEntry e[3];
		for (int i = 0; i < 3; i++) {
			e[i] = (WGPUBindGroupLayoutEntry)WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
			e[i].binding = i;
			e[i].visibility = WGPUShaderStage_Compute;
			e[i].buffer.type = i == 2 ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;
		}
		(void)b2;
		WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		bd.entryCount = 3;
		bd.entries = e;
		bgls[k] = wgpuDeviceCreateBindGroupLayout(dev->wdev, &bd);
		WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pld.bindGroupLayoutCount = 1;
		pld.bindGroupLayouts = &bgls[k];
		WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev->wdev, &pld);
		struct wgvk_compiled_shader cs = { .wgsl = (char *)wgsl, .wgsl_len = strlen(wgsl), .hash = hash };
		WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		cd.layout = pl;
		cd.compute.module = wgvk_module_get(dev, &cs);
		cd.compute.entryPoint = wgvk_sv("main");
		pipes[k] = wgpuDeviceCreateComputePipeline(dev->wdev, &cd);
		wgpuPipelineLayoutRelease(pl);
	}
	*bgl_out = bgls[k];
	return pipes[k];
}

static void
dispatch3(struct wgvk_replay *rp, WGPUComputePipeline p, WGPUBindGroupLayout bgl, const WGPUBindGroupEntry e[3],
	  uint32_t groups)
{
	WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	bd.layout = bgl;
	bd.entryCount = 3;
	bd.entries = e;
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(rp->dev->wdev, &bd);
	util_dynarray_append(&rp->sub->groups, bg);
	wgvk_end_compute(rp);
	WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(rp->enc, NULL);
	wgpuComputePassEncoderSetPipeline(cp, p);
	wgpuComputePassEncoderSetBindGroup(cp, 0, bg, 0, NULL);
	wgpuComputePassEncoderDispatchWorkgroups(cp, groups, 1, 1);
	wgpuComputePassEncoderEnd(cp);
	wgpuComputePassEncoderRelease(cp);
}

/* After the passes that wrote set's slots: resolve them and add (set_mode: set) each to map[slot]'s accumulator. */
void
wgvk_q_flush_slots(struct wgvk_replay *rp, WGPUQuerySet set, uint32_t n, const int32_t *map, bool set_mode)
{
	if (!n || !rp->q.acc)
		return;
	WGPUBuffer res = wgvk_temp_buffer(rp, align64((uint64_t)n * 8, 256),
					  WGPUBufferUsage_QueryResolve | WGPUBufferUsage_Storage);
	wgpuCommandEncoderResolveQuerySet(rp->enc, set, 0, n, res, 0);
	WGPUBuffer mb;
	uint64_t moff;
	uint32_t *m = wgvk_ring_alloc(rp, (2 + n) * 4, &mb, &moff);
	m[0] = n;
	m[1] = set_mode;
	for (uint32_t i = 0; i < n; i++)
		m[2 + i] = map[i] < 0 ? WGVK_MAX_ACC - 1 : (uint32_t)map[i];	/* (a slot nobody owns: the last) */
	WGPUBindGroupLayout bgl;
	WGPUComputePipeline p = q_pipe(rp->dev, acc_wgsl, 0x71616363ull, 0, &bgl);
	WGPUBindGroupEntry e[3] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
	e[0].binding = 0; e[0].buffer = res; e[0].size = align64((uint64_t)n * 8, 256);
	e[1].binding = 1; e[1].buffer = mb; e[1].offset = moff; e[1].size = align64((2 + n) * 4, 16);
	e[2].binding = 2; e[2].buffer = rp->q.acc; e[2].size = WGVK_MAX_ACC * 8;
	dispatch3(rp, p, bgl, e, 1);
}

/* A query set for a render pass's occlusion slots */
WGPUQuerySet
wgvk_q_occlusion_set(struct wgvk_replay *rp, uint32_t n)
{
	WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
	qd.type = WGPUQueryType_Occlusion;
	qd.count = n;
	WGPUQuerySet s = wgpuDeviceCreateQuerySet(rp->dev->wdev, &qd);
	util_dynarray_append(&rp->sub->qsets, s);
	return s;
}

void
wgvk_q_timestamp(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t q)
{
	pool->serial[q] = rp->serial;
	pool->available[q] = 0;
	if (!pool->set) {	/* no timestamp feature: zero, available */
		util_dynarray_append(&rp->sub->qdone, ((struct wgvk_qdone){ pool, q, -1 }));
		return;
	}
	if (rp->r.active)
		wgvk_warn_once("a timestamp inside a render pass: written after it (WebGPU writes them at pass "
			       "boundaries)\n");
	int acc = acc_new(rp, pool, q);
	if (acc < 0)
		return;
	WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
	qd.type = WGPUQueryType_Timestamp;
	qd.count = 1;
	WGPUQuerySet s = wgpuDeviceCreateQuerySet(rp->dev->wdev, &qd);
	util_dynarray_append(&rp->sub->qsets, s);
	WGPUPassTimestampWrites tw = WGPU_PASS_TIMESTAMP_WRITES_INIT;
	tw.querySet = s;
	tw.beginningOfPassWriteIndex = 0;
	tw.endOfPassWriteIndex = WGPU_QUERY_SET_INDEX_UNDEFINED;
	WGPUComputePassDescriptor cpd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
	cpd.timestampWrites = &tw;
	if (rp->r.active) {
		/* deferred to the end of the render pass */
		util_dynarray_append(&rp->q.pending_ts, ((struct wgvk_qts){ s, acc }));
	} else {
		wgvk_end_compute(rp);
		WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(rp->enc, &cpd);
		wgpuComputePassEncoderEnd(cp);
		wgpuComputePassEncoderRelease(cp);
		int32_t map = acc;
		wgvk_q_flush_slots(rp, s, 1, &map, true);
	}
	util_dynarray_append(&rp->sub->qdone, ((struct wgvk_qdone){ pool, q, acc }));
	struct wgvk_qacc *a = rp->q.accs.data;
	for (int i = (int)util_dynarray_num_elements(&rp->q.accs, struct wgvk_qacc) - 1; i >= 0; i--)
		if (a[i].pool == pool && a[i].query == q && a[i].live) {
			a[i].ended = true;
			break;
		}
}

/* timestamps that came inside a render pass, after it */
void
wgvk_q_after_pass(struct wgvk_replay *rp)
{
	util_dynarray_foreach(&rp->q.pending_ts, struct wgvk_qts, t) {
		WGPUPassTimestampWrites tw = WGPU_PASS_TIMESTAMP_WRITES_INIT;
		tw.querySet = t->set;
		tw.beginningOfPassWriteIndex = 0;
		tw.endOfPassWriteIndex = WGPU_QUERY_SET_INDEX_UNDEFINED;
		WGPUComputePassDescriptor cpd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
		cpd.timestampWrites = &tw;
		WGPUComputePassEncoder cp = wgpuCommandEncoderBeginComputePass(rp->enc, &cpd);
		wgpuComputePassEncoderEnd(cp);
		wgpuComputePassEncoderRelease(cp);
		int32_t map = t->acc;
		wgvk_q_flush_slots(rp, t->set, 1, &map, true);
	}
	util_dynarray_clear(&rp->q.pending_ts);
}

void
wgvk_q_copy(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t first, uint32_t count,
	    struct wgvk_buffer *dst, uint64_t off, uint64_t stride, VkQueryResultFlags flags)
{
	if (!count || !dst || !dst->mem || !dst->mem->bo.buffer)
		return;
	if (off % 4 || stride % 4) {
		wgvk_warn_once("vkCmdCopyQueryPoolResults to an offset or stride not in words\n");
		return;
	}
	bool w64 = flags & VK_QUERY_RESULT_64_BIT;
	uint32_t values = pool->values;
	uint32_t *d;
	WGPUBuffer db;
	uint64_t doff;
	d = wgvk_ring_alloc(rp, (5 + 4 * count) * 4, &db, &doff);
	uint64_t woff;
	WGPUBuffer wb = wgvk_buffer_wgpu(dst, off, &woff);
	uint32_t salign = MAX2(rp->dev->pdev->limits.minStorageBufferOffsetAlignment, 4);
	uint64_t base = woff / salign * salign;
	uint64_t span = (uint64_t)(count - 1) * stride + (w64 ? 8 : 4) * (values + !!(flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
	d[0] = count;
	d[1] = (w64 ? 1 : 0) | ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ? 2 : 0) |
	       ((flags & VK_QUERY_RESULT_PARTIAL_BIT) ? 4 : 0);
	d[2] = stride / 4;
	d[3] = values;
	d[4] = (woff - base) / 4;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t q = first + i, *e = &d[5 + 4 * i];
		e[0] = e[1] = e[2] = e[3] = 0;
		struct wgvk_qacc *a = rp->q.accs.data;
		bool found = false;
		for (int k = (int)util_dynarray_num_elements(&rp->q.accs, struct wgvk_qacc) - 1; k >= 0 && !found; k--) {
			if (a[k].pool != pool || a[k].query != q)
				continue;
			found = true;
			if (a[k].live && a[k].ended) {
				e[0] = 1;
				e[1] = a[k].acc;
			}
		}
		if (found)
			continue;
		if (!is_real(pool)) {
			/* faked: available once ended, zeros */
			util_dynarray_foreach(&rp->sub->qdone, struct wgvk_qdone, x)
				if (x->pool == pool && x->query == q)
					e[0] = 3;
			if (pool->available[q])
				e[0] = 3;
			continue;
		}
		if (pool->available[q]) {
			uint64_t v = pool->results[(uint64_t)q * values];
			e[0] = 2;
			e[2] = (uint32_t)v;
			e[3] = (uint32_t)(v >> 32);
		}
	}
	acc_buffer(rp);	/* (bound even when no query of this submission is read) */
	WGPUBindGroupLayout bgl;
	WGPUComputePipeline p = q_pipe(rp->dev, copy_wgsl, 0x71636f7079ull, 0, &bgl);
	WGPUBindGroupEntry e[3] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
	e[0].binding = 0; e[0].buffer = rp->q.acc; e[0].size = WGVK_MAX_ACC * 8;
	e[1].binding = 1; e[1].buffer = db; e[1].offset = doff; e[1].size = align64((5 + 4 * count) * 4, 16);
	e[2].binding = 2; e[2].buffer = wb; e[2].offset = base; e[2].size = align64(woff - base + span, 4);
	wgvk_upload(rp, dst->mem, base - dst->mem->bo.offset, e[2].size);	/* words not written keep the window's */
	dispatch3(rp, p, bgl, e, DIV_ROUND_UP(count, 64));
	wgvk_wrote(rp, dst->mem, base - dst->mem->bo.offset, e[2].size);
}

/* End of the submission: what the queries ended in it counted, to a MAP_READ buffer */
void
wgvk_q_finish(struct wgvk_replay *rp)
{
	if (!util_dynarray_num_elements(&rp->sub->qdone, struct wgvk_qdone) || !rp->q.acc)
		return;
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	bd.size = (uint64_t)MAX2(rp->q.nacc, 1) * 8;
	bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
	rp->sub->qstaging = wgpuDeviceCreateBuffer(rp->dev->wdev, &bd);
	rp->sub->qstaging_size = bd.size;
	wgpuCommandEncoderCopyBufferToBuffer(rp->enc, rp->q.acc, 0, rp->sub->qstaging, 0, bd.size);
}

/* At completion (in serial order): the ended queries' values, unless they were reset or begun again since */
void
wgvk_q_retire(struct wgvk_device *dev, struct wgvk_submission *sub)
{
	const uint64_t *v = NULL;
	if (sub->qstaging && !sub->failed)
		v = wgpuBufferGetConstMappedRange(sub->qstaging, 0, sub->qstaging_size);
	util_dynarray_foreach(&sub->qdone, struct wgvk_qdone, x) {
		if (x->pool->serial[x->query] != sub->serial)
			continue;
		uint64_t val[16] = { 0 };
		if (v && x->acc >= 0 && (uint64_t)x->acc * 8 < sub->qstaging_size)
			val[0] = v[x->acc];
		wgvk_query_pool_complete(x->pool, x->query, val);
	}
	if (sub->qstaging) {
		if (v)
			wgpuBufferUnmap(sub->qstaging);
		wgpuBufferRelease(sub->qstaging);
	}
}
