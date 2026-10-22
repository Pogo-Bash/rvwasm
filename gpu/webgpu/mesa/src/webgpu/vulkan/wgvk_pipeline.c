/*
 * wgvk render pipelines: the key (INTERFACES.md 5, wgvk_pipeline_key), the
 * cache, and asynchronous creation.
 *
 * WebGPU bakes into a GPURenderPipeline nearly everything Vulkan can make
 * dynamic: vertex layout, topology, cull mode and front face, depth and
 * stencil state (masks and bias included), blending, write masks, attachment
 * formats and sample count. So a Vulkan pipeline is not a WebGPU pipeline:
 * at each draw the bound shaders, the dynamic state and the formats of the
 * current rendering make a key, and the key finds (or starts) the WebGPU
 * pipeline. Creation is asynchronous (createRenderPipelineAsync, 17 ms p50
 * for a small pipeline on the M4 Max in Chrome): a draw starts it when the
 * draw is recorded into a pass, and the pass waits for it only when it
 * encodes the draw, so the pipelines of one pass are made in parallel.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wgvk_private.h"

#include "vk_format.h"
#define XXH_INLINE_ALL
#include "util/xxhash.h"

static uint8_t
blend_factor(VkBlendFactor f)
{
	static const uint8_t map[] = {
		[VK_BLEND_FACTOR_ZERO] = WGPUBlendFactor_Zero,
		[VK_BLEND_FACTOR_ONE] = WGPUBlendFactor_One,
		[VK_BLEND_FACTOR_SRC_COLOR] = WGPUBlendFactor_Src,
		[VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR] = WGPUBlendFactor_OneMinusSrc,
		[VK_BLEND_FACTOR_DST_COLOR] = WGPUBlendFactor_Dst,
		[VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR] = WGPUBlendFactor_OneMinusDst,
		[VK_BLEND_FACTOR_SRC_ALPHA] = WGPUBlendFactor_SrcAlpha,
		[VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA] = WGPUBlendFactor_OneMinusSrcAlpha,
		[VK_BLEND_FACTOR_DST_ALPHA] = WGPUBlendFactor_DstAlpha,
		[VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA] = WGPUBlendFactor_OneMinusDstAlpha,
		[VK_BLEND_FACTOR_CONSTANT_COLOR] = WGPUBlendFactor_Constant,
		[VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR] = WGPUBlendFactor_OneMinusConstant,
		/* no constant alpha in WebGPU: the colour constant (T2: the blend constant could be splatted) */
		[VK_BLEND_FACTOR_CONSTANT_ALPHA] = WGPUBlendFactor_Constant,
		[VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA] = WGPUBlendFactor_OneMinusConstant,
		[VK_BLEND_FACTOR_SRC_ALPHA_SATURATE] = WGPUBlendFactor_SrcAlphaSaturated,
		[VK_BLEND_FACTOR_SRC1_COLOR] = WGPUBlendFactor_Src1,
		[VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR] = WGPUBlendFactor_OneMinusSrc1,
		[VK_BLEND_FACTOR_SRC1_ALPHA] = WGPUBlendFactor_Src1Alpha,
		[VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA] = WGPUBlendFactor_OneMinusSrc1Alpha,
	};
	return f < ARRAY_SIZE(map) ? map[f] : WGPUBlendFactor_One;
}

static uint8_t
blend_op(VkBlendOp op)
{
	return op <= VK_BLEND_OP_MAX ? WGPUBlendOperation_Add + op : WGPUBlendOperation_Add;
}

static uint8_t
stencil_op(uint8_t op)
{
	static const uint8_t map[] = {
		[VK_STENCIL_OP_KEEP] = WGPUStencilOperation_Keep,
		[VK_STENCIL_OP_ZERO] = WGPUStencilOperation_Zero,
		[VK_STENCIL_OP_REPLACE] = WGPUStencilOperation_Replace,
		[VK_STENCIL_OP_INCREMENT_AND_CLAMP] = WGPUStencilOperation_IncrementClamp,
		[VK_STENCIL_OP_DECREMENT_AND_CLAMP] = WGPUStencilOperation_DecrementClamp,
		[VK_STENCIL_OP_INVERT] = WGPUStencilOperation_Invert,
		[VK_STENCIL_OP_INCREMENT_AND_WRAP] = WGPUStencilOperation_IncrementWrap,
		[VK_STENCIL_OP_DECREMENT_AND_WRAP] = WGPUStencilOperation_DecrementWrap,
	};
	return op < ARRAY_SIZE(map) ? map[op] : WGPUStencilOperation_Keep;
}

static uint8_t
compare_op(uint8_t op)
{
	return WGPUCompareFunction_Never + op;	/* the same order */
}

static bool
format_has_depth(WGPUTextureFormat f)
{
	return f == WGPUTextureFormat_Depth16Unorm || f == WGPUTextureFormat_Depth24Plus ||
	       f == WGPUTextureFormat_Depth24PlusStencil8 || f == WGPUTextureFormat_Depth32Float ||
	       f == WGPUTextureFormat_Depth32FloatStencil8;
}

static bool
format_has_stencil(WGPUTextureFormat f)
{
	return f == WGPUTextureFormat_Stencil8 || f == WGPUTextureFormat_Depth24PlusStencil8 ||
	       f == WGPUTextureFormat_Depth32FloatStencil8;
}

/* The key from the draw's state. Zeroed first: it is hashed and compared as bytes. */
void
wgvk_pipeline_key_build(const struct wgvk_key_inputs *in, struct wgvk_pipeline_key *k)
{
	const struct vk_dynamic_graphics_state *d = in->dyn;
	memset(k, 0, sizeof(*k));
	k->vs = in->vcs->hash;
	const struct wgvk_compiled_shader *fcs = in->fcs ? in->fcs : in->fs ? &in->fs->cs : NULL;
	k->fs = fcs ? fcs->hash : 0;
	k->layout = in->vs->layout_hash ^ (in->fs ? (in->fs->layout_hash * 0x9E3779B97F4A7C15ull) : 0);

	uint8_t topo = in->topology >= 0 ? in->topology : d->ia.primitive_topology;
	switch (topo) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: k->topology = WGPUPrimitiveTopology_PointList; break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: k->topology = WGPUPrimitiveTopology_LineList; break;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: k->topology = WGPUPrimitiveTopology_LineStrip; break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: k->topology = WGPUPrimitiveTopology_TriangleStrip; break;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: k->topology = WGPUPrimitiveTopology_TriangleList; break;
	default:
		wgvk_warn_once("topology %d needs index rewriting (T2: fans, adjacency, patches); drawn as a list\n", topo);
		k->topology = WGPUPrimitiveTopology_TriangleList;
		break;
	}
	bool strip = k->topology == WGPUPrimitiveTopology_LineStrip || k->topology == WGPUPrimitiveTopology_TriangleStrip;
	k->strip_index_format = strip ? in->strip_index : WGPUIndexFormat_Undefined;
	if (in->vcs->legal.points_expanded) {
		/* points as 2 triangles each (T3b), culling off */
		k->topology = WGPUPrimitiveTopology_TriangleList;
		k->strip_index_format = WGPUIndexFormat_Undefined;
	}
	k->front_face = d->rs.front_face == VK_FRONT_FACE_CLOCKWISE ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
	k->cull_mode = (d->rs.cull_mode & VK_CULL_MODE_FRONT_AND_BACK) == VK_CULL_MODE_FRONT_BIT ? WGPUCullMode_Front :
		       (d->rs.cull_mode & VK_CULL_MODE_FRONT_AND_BACK) == VK_CULL_MODE_BACK_BIT ? WGPUCullMode_Back :
												 WGPUCullMode_None;
	if (in->vcs->legal.points_expanded)
		k->cull_mode = WGPUCullMode_None;
	k->unclipped_depth = d->rs.depth_clamp_enable;
	k->sample_count = MAX2(in->samples, 1);
	k->alpha_to_coverage = k->sample_count > 1 && d->ms.alpha_to_coverage_enable;
	k->sample_mask = k->sample_count > 1 ? (d->ms.sample_mask | 0xffff0000u) : 0xffffffffu;

	/* Vertex buffers: WebGPU slots in the order of the Vulkan bindings the attributes use. */
	if (d->vi && !in->no_vertex_input) {
		uint32_t used = 0;
		uint32_t pulled = in->vcs->legal.pulled_locations;
		u_foreach_bit(a, d->vi->attributes_valid) {
			if (!(in->vs->vs_inputs & (1u << a)) || (pulled & (1u << a)))
				continue;	/* WebGPU accepts extra attributes, but there are only 16 */
			used |= 1u << d->vi->attributes[a].binding;
		}
		int8_t slot_of[MESA_VK_MAX_VERTEX_BINDINGS];
		memset(slot_of, -1, sizeof(slot_of));
		u_foreach_bit(b, used) {
			if (k->vb_count == WGVK_MAX_VBS) {
				wgvk_warn_once("more than %d vertex buffers (T2: vertex pulling)\n", WGVK_MAX_VBS);
				break;
			}
			const struct vk_vertex_binding_state *vb = &d->vi->bindings[b];
			unsigned s = k->vb_count++;
			slot_of[b] = s;
			k->vb[s].binding = b;
			k->vb[s].stride = d->vi_binding_strides[b];
			k->vb[s].step_mode = vb->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE ? WGPUVertexStepMode_Instance
											    : WGPUVertexStepMode_Vertex;
			if ((vb->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE && vb->divisor != 1) || k->vb[s].stride % 4)
				wgvk_warn_once("binding %u needs vertex pulling but is in the vertex state\n", b);
		}
		u_foreach_bit(a, d->vi->attributes_valid) {
			const struct vk_vertex_attribute_state *at = &d->vi->attributes[a];
			if (!(in->vs->vs_inputs & (1u << a)) || (pulled & (1u << a)) || slot_of[at->binding] < 0 ||
			    k->attr_count == WGVK_MAX_ATTRS)
				continue;
			const struct wgvk_format *af = wgvk_format_get(in->pdev, at->format);
			WGPUVertexFormat vf = af ? af->vf : (WGPUVertexFormat)0;
			if (!vf) {
				wgvk_warn_once("vertex format %d needs vertex pulling but is in the vertex state\n", at->format);
				vf = WGPUVertexFormat_Float32x4;
			}
			unsigned i = k->attr_count++;
			k->attr[i].format = vf;
			k->attr[i].location = a;
			k->attr[i].slot = slot_of[at->binding];
			k->attr[i].offset = at->offset;
		}
	}

	k->color_count = in->color_count;
	static int trace = -1;
	if (trace < 0)
		trace = getenv("WGVK_TRACE") != NULL;
	if (trace)
		wgvk_log("trace: key cb: %u attachments, write enables %x, masks %x %x %x %x, blend %d; ds: test %d "
			 "write %d op %d; stencil test %d front %d/%d/%d/%d cmp %x wr %x ref %u\n", d->cb.attachment_count,
			 d->cb.color_write_enables, d->cb.attachments[0].write_mask, d->cb.attachments[1].write_mask,
			 d->cb.attachments[2].write_mask, d->cb.attachments[3].write_mask, d->cb.attachments[0].blend_enable,
			 d->ds.depth.test_enable, d->ds.depth.write_enable, d->ds.depth.compare_op, d->ds.stencil.test_enable,
			 d->ds.stencil.front.op.compare, d->ds.stencil.front.op.fail, d->ds.stencil.front.op.pass,
			 d->ds.stencil.front.op.depth_fail, d->ds.stencil.front.compare_mask, d->ds.stencil.front.write_mask,
			 d->ds.stencil.front.reference);
	for (unsigned i = 0; i < in->color_count; i++) {
		if (in->color[i] == WGPUTextureFormat_Undefined)
			continue;
		const struct vk_color_blend_attachment_state *a = &d->cb.attachments[i];
		k->color[i].format = in->color[i];
		if (in->dummy_color == i + 1)
			continue;	/* the stand-in target of a pass without attachments: never written */
		/* (cb.attachment_count is not consulted: with the blend enables, equations and write masks dynamic
		 * (zink, EXT_extended_dynamic_state3) a pipeline's attachmentCount is ignored and may be 0) */
		k->color[i].write_mask = (d->cb.color_write_enables & (1u << i)) ? a->write_mask : 0;
		/* WebGPU wants writeMask 0 on targets the fragment shader does not write (Vulkan ignores them) */
		if (in->fs && !(in->fs->fs_outputs & (1u << i)))
			k->color[i].write_mask = 0;
		if (a->blend_enable) {
			k->color[i].blend_enable = 1;
			k->color[i].color_op = blend_op(a->color_blend_op);
			k->color[i].alpha_op = blend_op(a->alpha_blend_op);
			bool cminmax = a->color_blend_op == VK_BLEND_OP_MIN || a->color_blend_op == VK_BLEND_OP_MAX;
			bool aminmax = a->alpha_blend_op == VK_BLEND_OP_MIN || a->alpha_blend_op == VK_BLEND_OP_MAX;
			/* WebGPU: min and max take the factors one, one (Vulkan ignores them) */
			k->color[i].color_src = cminmax ? WGPUBlendFactor_One : blend_factor(a->src_color_blend_factor);
			k->color[i].color_dst = cminmax ? WGPUBlendFactor_One : blend_factor(a->dst_color_blend_factor);
			k->color[i].alpha_src = aminmax ? WGPUBlendFactor_One : blend_factor(a->src_alpha_blend_factor);
			k->color[i].alpha_dst = aminmax ? WGPUBlendFactor_One : blend_factor(a->dst_alpha_blend_factor);
		}
	}
	if (d->cb.logic_op_enable)
		wgvk_warn_once("logic ops are not in WebGPU (design: fake first)\n");

	if (in->ds != WGPUTextureFormat_Undefined) {
		k->ds.format = in->ds;
		bool depth = format_has_depth(in->ds), stencil = format_has_stencil(in->ds);
		k->ds.depth_write = depth && d->ds.depth.test_enable && d->ds.depth.write_enable;
		k->ds.depth_compare = depth && d->ds.depth.test_enable ? compare_op(d->ds.depth.compare_op)
								       : WGPUCompareFunction_Always;
		if (!depth)
			k->ds.depth_compare = WGPUCompareFunction_Undefined;
		const struct vk_stencil_test_face_state *f[2] = { &d->ds.stencil.front, &d->ds.stencil.back };
		uint8_t *o[2] = { k->ds.stencil_front, k->ds.stencil_back };
		for (unsigned s = 0; s < 2; s++) {
			if (stencil && d->ds.stencil.test_enable) {
				o[s][0] = compare_op(f[s]->op.compare);
				o[s][1] = stencil_op(f[s]->op.fail);
				o[s][2] = stencil_op(f[s]->op.depth_fail);
				o[s][3] = stencil_op(f[s]->op.pass);
			} else {
				o[s][0] = WGPUCompareFunction_Always;
				o[s][1] = o[s][2] = o[s][3] = WGPUStencilOperation_Keep;
			}
		}
		/* one mask for both faces in WebGPU */
		k->ds.stencil_read_mask = stencil && d->ds.stencil.test_enable ? d->ds.stencil.front.compare_mask : 0xff;
		k->ds.stencil_write_mask = stencil && d->ds.stencil.test_enable ? d->ds.stencil.front.write_mask : 0;
		if (stencil && d->ds.stencil.test_enable &&
		    (d->ds.stencil.front.compare_mask != d->ds.stencil.back.compare_mask ||
		     d->ds.stencil.front.write_mask != d->ds.stencil.back.write_mask))
			wgvk_warn_once("different front and back stencil masks: WebGPU has one (front used)\n");
		bool tris = k->topology == WGPUPrimitiveTopology_TriangleList ||
			    k->topology == WGPUPrimitiveTopology_TriangleStrip;
		if (depth && tris && d->rs.depth_bias.enable) {
			k->ds.depth_bias = (int32_t)d->rs.depth_bias.constant_factor;
			k->ds.depth_bias_slope = d->rs.depth_bias.slope_factor;
			k->ds.depth_bias_clamp = d->rs.depth_bias.clamp;
		}
	}
	if (d->ds.depth.bounds_test.enable)
		wgvk_warn_once("depth bounds test is not in WebGPU, ignored\n");
}

/* ---- the cache */

static uint32_t
key_hash(const void *key)
{
	return (uint32_t)XXH64(key, sizeof(struct wgvk_pipeline_key), 0);
}

static bool
key_equal(const void *a, const void *b)
{
	return !memcmp(a, b, sizeof(struct wgvk_pipeline_key));
}

struct hash_table *wgvk_pipeline_table_create(void);
struct hash_table *
wgvk_pipeline_table_create(void)
{
	return _mesa_hash_table_create(NULL, key_hash, key_equal);
}

static void
on_pipeline(WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline, WGPUStringView message, void *u1,
	    void *u2)
{
	struct wgvk_rpipe *p = u1;
	p->pending = false;
	if (status == WGPUCreatePipelineAsyncStatus_Success) {
		p->pipeline = pipeline;
	} else {
		p->failed = true;
		wgvk_log("render pipeline not made: %.*s\n",
			 (int)(message.length == WGPU_STRLEN ? strlen(message.data ? message.data : "") : message.length),
			 message.data ? message.data : "");
	}
}

static void cache_store(struct wgvk_device *dev, const struct wgvk_rpipe *p, const struct wgvk_compiled_shader *vcs,
			const struct wgvk_compiled_shader *fcs);

/* Start creating the GPURenderPipeline of p (key, bgl, ngroups, immediate_bytes set) from the WGSL of its
 * stages (fcs NULL: no fragment stage). */
static void
rpipe_create(struct wgvk_device *dev, struct wgvk_rpipe *p, const struct wgvk_compiled_shader *vcs,
	     const struct wgvk_compiled_shader *fcs)
{
	const struct wgvk_pipeline_key *key = &p->key;

	WGPUBindGroupLayout bgls[4];
	for (unsigned g = 0; g < p->ngroups; g++)
		bgls[g] = p->bgl[g]->layout;
	WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	pld.bindGroupLayoutCount = p->ngroups;
	pld.bindGroupLayouts = bgls;
	pld.immediateSize = align(p->immediate_bytes, 4);
	WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev->wdev, &pld);

	WGPUVertexBufferLayout vbl[WGVK_MAX_VBS];
	WGPUVertexAttribute attrs[WGVK_MAX_VBS][WGVK_MAX_ATTRS];
	for (unsigned s = 0; s < key->vb_count; s++) {
		vbl[s] = (WGPUVertexBufferLayout)WGPU_VERTEX_BUFFER_LAYOUT_INIT;
		vbl[s].arrayStride = key->vb[s].stride;
		vbl[s].stepMode = key->vb[s].step_mode;
		vbl[s].attributes = attrs[s];
		for (unsigned a = 0; a < key->attr_count; a++) {
			if (key->attr[a].slot != s)
				continue;
			attrs[s][vbl[s].attributeCount++] = (WGPUVertexAttribute){
				.format = key->attr[a].format, .offset = key->attr[a].offset,
				.shaderLocation = key->attr[a].location,
			};
		}
	}

	WGPURenderPipelineDescriptor rd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	rd.layout = pl;
	rd.vertex.module = wgvk_module_get(dev, vcs);
	rd.vertex.entryPoint = wgvk_sv("main");
	rd.vertex.bufferCount = key->vb_count;
	rd.vertex.buffers = vbl;
	rd.primitive.topology = key->topology;
	rd.primitive.stripIndexFormat = key->strip_index_format;
	rd.primitive.frontFace = key->front_face;
	rd.primitive.cullMode = key->cull_mode;
	rd.primitive.unclippedDepth = key->unclipped_depth;
	rd.multisample.count = key->sample_count;
	rd.multisample.mask = key->sample_mask;
	rd.multisample.alphaToCoverageEnabled = key->alpha_to_coverage;

	WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
	if (key->ds.format) {
		ds.format = key->ds.format;
		ds.depthWriteEnabled = format_has_depth(key->ds.format) ? (key->ds.depth_write ? WGPUOptionalBool_True
											       : WGPUOptionalBool_False)
									 : WGPUOptionalBool_Undefined;
		ds.depthCompare = key->ds.depth_compare;
		ds.stencilFront = (WGPUStencilFaceState){ key->ds.stencil_front[0], key->ds.stencil_front[1],
							  key->ds.stencil_front[2], key->ds.stencil_front[3] };
		ds.stencilBack = (WGPUStencilFaceState){ key->ds.stencil_back[0], key->ds.stencil_back[1],
							 key->ds.stencil_back[2], key->ds.stencil_back[3] };
		ds.stencilReadMask = key->ds.stencil_read_mask;
		ds.stencilWriteMask = key->ds.stencil_write_mask;
		ds.depthBias = key->ds.depth_bias;
		ds.depthBiasSlopeScale = key->ds.depth_bias_slope;
		ds.depthBiasClamp = key->ds.depth_bias_clamp;
		rd.depthStencil = &ds;
	}

	WGPUColorTargetState ct[WGVK_MAX_COLOR];
	WGPUBlendState bs[WGVK_MAX_COLOR];
	WGPUFragmentState fsd = WGPU_FRAGMENT_STATE_INIT;
	if (fcs || key->color_count) {
		for (unsigned i = 0; i < key->color_count; i++) {
			ct[i] = (WGPUColorTargetState)WGPU_COLOR_TARGET_STATE_INIT;
			ct[i].format = key->color[i].format;
			ct[i].writeMask = key->color[i].write_mask;
			if (key->color[i].blend_enable) {
				bs[i] = (WGPUBlendState){
					.color = { key->color[i].color_op, key->color[i].color_src, key->color[i].color_dst },
					.alpha = { key->color[i].alpha_op, key->color[i].alpha_src, key->color[i].alpha_dst },
				};
				ct[i].blend = &bs[i];
			}
		}
		/* no fragment shader but colour targets (the pass's attachments must match the pipeline's
		 * targets in WebGPU): an empty one, its targets write-masked */
		static const char empty_fs[] = "@fragment fn main() { }\n";
		struct wgvk_compiled_shader empty = { .wgsl = (char *)empty_fs, .wgsl_len = sizeof(empty_fs) - 1,
						      .hash = 0x77677666736e6f6eull };
		if (!fcs)
			for (unsigned i = 0; i < key->color_count; i++)
				ct[i].writeMask = WGPUColorWriteMask_None, ct[i].blend = NULL;
		fsd.module = wgvk_module_get(dev, fcs ? fcs : &empty);
		fsd.entryPoint = wgvk_sv("main");
		fsd.targetCount = key->color_count;
		fsd.targets = ct;
		rd.fragment = &fsd;
	}

	WGPUCreateRenderPipelineAsyncCallbackInfo cb = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
	cb.mode = WGPUCallbackMode_AllowProcessEvents;
	cb.callback = on_pipeline;
	cb.userdata1 = p;
	p->pending = true;
	p->future = wgpuDeviceCreateRenderPipelineAsync(dev->wdev, &rd, cb);
	wgpuPipelineLayoutRelease(pl);
	dev->stats.pipelines_created++;
	dev->stats.pipelines_async++;
	_mesa_hash_table_insert(dev->pipelines, &p->key, p);
}

struct wgvk_rpipe *
wgvk_rpipe_get(struct wgvk_device *dev, const struct wgvk_pipeline_key *key, struct wgvk_shader *vs,
	       const struct wgvk_compiled_shader *vcs, struct wgvk_shader *fs, const struct wgvk_compiled_shader *fcs,
	       const struct wgvk_unfilterable *uf)
{
	if (fs && !fcs)
		fcs = &fs->cs;
	struct hash_entry *he = _mesa_hash_table_search(dev->pipelines, key);
	if (he) {
		dev->stats.pipeline_hits++;
		return he->data;
	}
	struct wgvk_rpipe *p = calloc(1, sizeof(*p));
	p->key = *key;
	struct wgvk_shader *sh[2] = { vs, fs };
	const struct wgvk_compiled_shader *cs[2] = { vcs, fcs };
	p->ngroups = wgvk_bgls_for_shaders(dev, sh, cs, fs ? 2 : 1, uf, p->bgl);
	p->immediate_bytes = MAX2(vcs->immediate_bytes, fcs ? fcs->immediate_bytes : 0);
	p->draw_params_offset = vcs->legal.draw_params_offset;
	rpipe_create(dev, p, vcs, fcs);
	if (dev->cache_store)
		cache_store(dev, p, vcs, fcs);
	return p;
}

/* ---- the persistent pipeline cache (INTERFACES.md 5): a record per pipeline, for the page to keep
 * (IndexedDB) and hand back at the next start, so the pipelines are made before the first draw needs them.
 * Dawn and the browser cache compiled pipelines themselves; this saves wgvk's compile, the WGSL and the
 * pipeline creation on the critical path. A record is only valid for the same wgvk (version word). */

#define WGVK_CACHE_MAGIC 0x6b766777	/* "wgvk" */
#define WGVK_CACHE_VERSION (1u << 16 | (uint32_t)sizeof(struct wgvk_pipeline_key))

struct cache_head {
	uint32_t magic, version, size;
	uint32_t ngroups, immediate_bytes, draw_params_offset, has_fs;
	uint32_t vs_len, fs_len, n_entries[4];
	uint64_t vs_hash, fs_hash;
	struct wgvk_pipeline_key key;
};

static void
cache_store(struct wgvk_device *dev, const struct wgvk_rpipe *p, const struct wgvk_compiled_shader *vcs,
	    const struct wgvk_compiled_shader *fcs)
{
	struct cache_head h = {
		.magic = WGVK_CACHE_MAGIC, .version = WGVK_CACHE_VERSION, .ngroups = p->ngroups,
		.immediate_bytes = p->immediate_bytes, .draw_params_offset = p->draw_params_offset, .has_fs = fcs != NULL,
		.vs_len = vcs->wgsl_len, .fs_len = fcs ? fcs->wgsl_len : 0, .vs_hash = vcs->hash,
		.fs_hash = fcs ? fcs->hash : 0, .key = p->key,
	};
	size_t size = sizeof(h) + h.vs_len + h.fs_len;
	for (unsigned g = 0; g < p->ngroups; g++) {
		h.n_entries[g] = p->bgl[g]->n;
		size += p->bgl[g]->n * sizeof(WGPUBindGroupLayoutEntry);
	}
	h.size = size;
	char *b = malloc(size), *at = b;
	memcpy(at, &h, sizeof(h));
	at += sizeof(h);
	memcpy(at, vcs->wgsl, h.vs_len);
	at += h.vs_len;
	if (fcs)
		memcpy(at, fcs->wgsl, h.fs_len);
	at += h.fs_len;
	for (unsigned g = 0; g < p->ngroups; g++) {
		memcpy(at, p->bgl[g]->entries, p->bgl[g]->n * sizeof(WGPUBindGroupLayoutEntry));
		at += p->bgl[g]->n * sizeof(WGPUBindGroupLayoutEntry);
	}
	dev->cache_store(dev->cache_store_data, b, size);
	free(b);
}

/* One record back: the pipeline is created (asynchronously) and in the key table before any draw. Returns
 * the record's size, or 0 if it is not one (another version: skip the rest). */
size_t
wgvk_pipeline_cache_preload(struct wgvk_device *dev, const void *data, size_t size)
{
	struct cache_head h;
	if (size < sizeof(h))
		return 0;
	memcpy(&h, data, sizeof(h));
	if (h.magic != WGVK_CACHE_MAGIC || h.version != WGVK_CACHE_VERSION || h.size > size || h.ngroups > 4)
		return 0;
	if (_mesa_hash_table_search(dev->pipelines, &h.key))
		return h.size;
	const char *at = (const char *)data + sizeof(h);
	struct wgvk_compiled_shader vcs = { .wgsl = (char *)at, .wgsl_len = h.vs_len, .hash = h.vs_hash };
	at += h.vs_len;
	struct wgvk_compiled_shader fcs = { .wgsl = (char *)at, .wgsl_len = h.fs_len, .hash = h.fs_hash };
	at += h.fs_len;
	struct wgvk_rpipe *p = calloc(1, sizeof(*p));
	p->key = h.key;
	p->ngroups = h.ngroups;
	p->immediate_bytes = h.immediate_bytes;
	p->draw_params_offset = h.draw_params_offset;
	for (unsigned g = 0; g < h.ngroups; g++) {
		WGPUBindGroupLayoutEntry *e = malloc(MAX2(h.n_entries[g], 1) * sizeof(*e));
		memcpy(e, at, h.n_entries[g] * sizeof(*e));
		for (unsigned i = 0; i < h.n_entries[g]; i++)
			e[i].nextInChain = NULL;
		at += h.n_entries[g] * sizeof(*e);
		p->bgl[g] = wgvk_bgl_get(dev, e, h.n_entries[g]);
		free(e);
	}
	/* the modules: by hash, as the draws will ask for them (wgvk_module_get keeps them) */
	rpipe_create(dev, p, &vcs, h.has_fs ? &fcs : NULL);
	dev->stats.pipelines_preloaded++;
	return h.size;
}

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

WGPURenderPipeline
wgvk_rpipe_wait(struct wgvk_device *dev, struct wgvk_rpipe *p)
{
	if (p->pending) {
		double t = now_ms();
		wgvk_wait(dev, &p->future, 1, UINT64_MAX);
		dev->stats.pipeline_waits++;
		dev->stats.pipeline_wait_ms += now_ms() - t;
	}
	return p->pipeline;
}
