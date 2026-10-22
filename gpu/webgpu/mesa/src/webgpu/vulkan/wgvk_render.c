/*
 * wgvk render passes (T2): Vulkan's dynamic rendering (render passes reach
 * here through Mesa's vk_render_pass.c) as WebGPU render passes, and draws.
 *
 * WebGPU checks resource usage per render pass (a "usage scope"): within one
 * pass, a buffer or texture subresource that is written (storage, attachment)
 * may not be used in any other way. Vulkan only asks for barriers. And in
 * wgvk a VkDeviceMemory is one GPUBuffer, so two VkBuffers suballocated from
 * one memory are one buffer for WebGPU's rules. So a pass is not encoded as
 * its draws are replayed. The draws are collected (struct wgvk_draw: the
 * pipeline key resolved, bind groups made, every buffer range a draw uses
 * listed) and when the pass ends:
 *
 *  - a draw that conflicts with what earlier draws of the pass used (a write
 *    after a read, a read after a write) starts a new WebGPU pass, which
 *    loads what the previous one stored ("splits");
 *  - a draw that conflicts with itself (reads a range of a GPUBuffer it
 *    writes elsewhere) reads a copy of those ranges made just before its
 *    pass begins ("snapshots");
 *  - a draw that samples a texture subresource the pass renders to (a
 *    feedback loop) starts a WebGPU pass of its own and samples a copy of
 *    the subresource made just before it (as if a texture barrier came
 *    before each such draw); textures written as storage and used
 *    otherwise in the pass split it too.
 *
 * WGVK_STATS=1 prints the counts at vkDestroyDevice (design risk 1).
 *
 * Pipelines are looked up (and their creation started) when a draw is
 * collected and waited for when it is encoded, so the new pipelines of a
 * pass compile in parallel.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wgvk_replay.h"

#include "vk_format.h"
#include "poly/geometry.h"
#include "poly/prim.h"
#include "../poly/wgvk_poly_args.h"
#define XXH_INLINE_ALL
#include "util/xxhash.h"

/* ---- begin and end */

static uint32_t
mip_size(uint32_t s, uint32_t level)
{
	return MAX2(s >> level, 1);
}

static void
set_att(struct wgvk_att *a, const VkRenderingAttachmentInfo *ai)
{
	memset(a, 0, sizeof(*a));
	if (!ai || !ai->imageView)
		return;
	a->iv = wgvk_image_view_from_handle(ai->imageView);
	a->load = ai->loadOp;
	a->store = ai->storeOp;
	a->clear = ai->clearValue;
	a->resolve_mode = ai->resolveMode;
	if (ai->resolveMode != VK_RESOLVE_MODE_NONE && ai->resolveImageView)
		a->resolve = wgvk_image_view_from_handle(ai->resolveImageView);
}

void
wgvk_cmd_begin_rendering(struct wgvk_replay *rp, const VkRenderingInfo *info)
{
	struct wgvk_rendering *r = &rp->r;
	if (r->active)
		wgvk_cmd_end_rendering(rp);
	memset(r, 0, sizeof(*r));
	r->active = true;
	r->area = info->renderArea;
	r->layers = info->layerCount;
	r->view_mask = info->viewMask;
	if (r->view_mask)
		wgvk_warn_once("multiview rendering is not in WebGPU (T2: one pass per view)\n");
	r->ncolor = MIN2(info->colorAttachmentCount, WGVK_MAX_COLOR);
	struct wgvk_image_view *any = NULL;
	for (uint32_t i = 0; i < r->ncolor; i++) {
		set_att(&r->color[i], &info->pColorAttachments[i]);
		r->color_fmt[i] = r->color[i].iv ? r->color[i].iv->wformat : WGPUTextureFormat_Undefined;
		any = any ? any : r->color[i].iv;
	}
	set_att(&r->depth, info->pDepthAttachment);
	set_att(&r->stencil, info->pStencilAttachment);
	struct wgvk_image_view *ds = r->depth.iv ? r->depth.iv : r->stencil.iv;
	if (ds) {
		struct wgvk_image *img = container_of(ds->vk.image, struct wgvk_image, vk);
		r->ds_fmt = img->wformat;
		any = any ? any : ds;
	}
	if (any) {
		r->samples = any->vk.image->samples;
		r->width = mip_size(any->vk.image->extent.width, any->vk.base_mip_level);
		r->height = mip_size(any->vk.image->extent.height, any->vk.base_mip_level);
	} else if (r->ncolor < WGVK_MAX_COLOR) {
		r->dummy = r->ncolor + 1;
		r->color_fmt[r->ncolor++] = WGPUTextureFormat_R8Unorm;
		r->width = MAX2(r->area.offset.x + r->area.extent.width, 1);
		r->height = MAX2(r->area.offset.y + r->area.extent.height, 1);
		rp->dev->stats.dummy_attachments++;
	}
	/* a resumed pass: what the suspended one stored is loaded (wgvk ends a pass at every vkCmdEndRendering) */
	if (info->flags & VK_RENDERING_RESUMING_BIT) {
		for (uint32_t i = 0; i < r->ncolor; i++)
			r->color[i].load = VK_ATTACHMENT_LOAD_OP_LOAD;
		r->depth.load = r->stencil.load = VK_ATTACHMENT_LOAD_OP_LOAD;
	}
	if ((r->area.offset.x || r->area.offset.y || r->area.extent.width < r->width ||
	     r->area.extent.height < r->height)) {
		bool clears = false;
		for (uint32_t i = 0; i < r->ncolor; i++)
			clears |= r->color[i].iv && r->color[i].load == VK_ATTACHMENT_LOAD_OP_CLEAR;
		clears |= r->depth.iv && r->depth.load == VK_ATTACHMENT_LOAD_OP_CLEAR;
		clears |= r->stencil.iv && r->stencil.load == VK_ATTACHMENT_LOAD_OP_CLEAR;
		if (clears)
			wgvk_warn_once("a clear inside a partial render area clears the whole attachment (T2: meta clear)\n");
	}
	util_dynarray_clear(&rp->draws);
	util_dynarray_clear(&rp->uses);
	util_dynarray_clear(&rp->tuses);
	rp->dev->stats.render_passes++;
	/* WebGPU refuses NaN and infinite clear values, which GL clears with (Blender) and float targets keep: the
	 * attachment is cleared to 0 by the pass and then a driver quad writes the exact bits (immediates go
	 * unchecked) */
	for (uint32_t i = 0; i < r->ncolor; i++) {
		struct wgvk_att *a = &r->color[i];
		if (!a->iv || a->load != VK_ATTACHMENT_LOAD_OP_CLEAR || vk_format_is_int(a->iv->vk.format) ||
		    wgvk_color_is_finite(a->clear.color.float32))
			continue;
		VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = i, .clearValue = a->clear };
		VkClearRect cr = { .rect = r->area, .baseArrayLayer = 0, .layerCount = 1 };
		struct vk_cmd_clear_attachments c = { .attachment_count = 1, .attachments = &ca, .rect_count = 1, .rects = &cr };
		wgvk_cmd_clear_attachments(rp, &c);
		memset(&a->clear, 0, sizeof(a->clear));
		rp->dev->stats.nonfinite_clears++;
	}
}

static WGPULoadOp
load_op(VkAttachmentLoadOp op)
{
	switch (op) {
	case VK_ATTACHMENT_LOAD_OP_CLEAR: return WGPULoadOp_Clear;
	case VK_ATTACHMENT_LOAD_OP_DONT_CARE: return WGPULoadOp_Clear;	/* cheaper than a load; any value will do */
	default: return WGPULoadOp_Load;
	}
}

static WGPUStoreOp
store_op(VkAttachmentStoreOp op)
{
	return op == VK_ATTACHMENT_STORE_OP_DONT_CARE ? WGPUStoreOp_Discard : WGPUStoreOp_Store;
}

static bool
wgpu_has_depth(WGPUTextureFormat f)
{
	return f == WGPUTextureFormat_Depth16Unorm || f == WGPUTextureFormat_Depth24Plus ||
	       f == WGPUTextureFormat_Depth24PlusStencil8 || f == WGPUTextureFormat_Depth32Float ||
	       f == WGPUTextureFormat_Depth32FloatStencil8;
}

static bool
wgpu_has_stencil(WGPUTextureFormat f)
{
	return f == WGPUTextureFormat_Stencil8 || f == WGPUTextureFormat_Depth24PlusStencil8 ||
	       f == WGPUTextureFormat_Depth32FloatStencil8;
}

/* ---- encoding a run of draws as one WebGPU pass */

struct snap {
	unsigned draw, use;	/* indices: the draw, its use (relative) or texture use */
	WGPUBuffer buf;		/* a buffer snapshot, or */
	WGPUTexture tex;	/* a texture snapshot (feedback loops) */
};

struct occl_slots {
	WGPUQuerySet set;
	uint32_t n;
	int32_t map[1024];	/* slot -> accumulator */
};

/* this segment's slot for accumulator occl1 - 1 (slots of a segment from base) */
static uint32_t
occl_slot(struct occl_slots *os, uint32_t base, int32_t acc)
{
	for (uint32_t i = base; i < os->n; i++)
		if (os->map[i] == acc)
			return i;
	if (os->n == ARRAY_SIZE(os->map))
		return os->n - 1;
	os->map[os->n] = acc;
	return os->n++;
}

static void
encode_segment(struct wgvk_replay *rp, unsigned from, unsigned to, bool first, bool last, const struct snap *snaps,
	       unsigned nsnaps, struct occl_slots *os)
{
	struct wgvk_device *dev = rp->dev;
	struct wgvk_rendering *r = &rp->r;
	struct wgvk_draw *draws = rp->draws.data;
	struct wgvk_use *uses = rp->uses.data;

	/* image buffers, alias textures: brought up to date before the pass (and before snapshots copy them) */
	for (unsigned i = from; i < to; i++)
		wgvk_tuses_before(rp, (const struct wgvk_tuse *)rp->tuses.data + draws[i].tuse0, draws[i].ntuse);
	for (uint32_t i = 0; i < r->ncolor; i++) {
		const struct wgvk_att *a = &r->color[i];
		if (a->iv)
			wgvk_image_before(rp, container_of(a->iv->vk.image, struct wgvk_image, vk), a->iv->alias, true);
		if (a->resolve)
			wgvk_image_before(rp, container_of(a->resolve->vk.image, struct wgvk_image, vk), a->resolve->alias,
					  true);
	}
	struct wgvk_image_view *dsv0 = r->depth.iv ? r->depth.iv : r->stencil.iv;
	if (dsv0)
		wgvk_image_before(rp, container_of(dsv0->vk.image, struct wgvk_image, vk), dsv0->alias, true);

	/* snapshots of this segment's draws, copied before the pass begins */
	for (unsigned s = 0; s < nsnaps; s++) {
		if (snaps[s].draw < from || snaps[s].draw >= to)
			continue;
		if (snaps[s].tex) {
			const struct wgvk_tuse *t = (const struct wgvk_tuse *)rp->tuses.data + draws[snaps[s].draw].tuse0 +
						    snaps[s].use;
			struct wgvk_image *img = container_of(t->iv->vk.image, struct wgvk_image, vk);
			for (unsigned m = 0; m < t->nmip; m++) {
				WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
				src.texture = t->tex;
				src.mipLevel = t->mip0 + m;
				src.origin.z = img->vk.image_type == VK_IMAGE_TYPE_3D ? 0 : t->layer0;
				dst.texture = snaps[s].tex;
				dst.mipLevel = m;
				WGPUExtent3D ext = { MAX2(img->vk.extent.width >> (t->mip0 + m), 1),
						     MAX2(img->vk.extent.height >> (t->mip0 + m), 1),
						     img->vk.image_type == VK_IMAGE_TYPE_3D ? MAX2(img->vk.extent.depth >> (t->mip0 + m), 1)
											    : t->nlayer };
				wgpuCommandEncoderCopyTextureToTexture(rp->enc, &src, &dst, &ext);
			}
			dev->stats.texture_snapshots++;
			continue;
		}
		const struct wgvk_use *u = &uses[draws[snaps[s].draw].use0 + snaps[s].use];
		wgpuCommandEncoderCopyBufferToBuffer(rp->enc, u->buf, u->off, snaps[s].buf, 0, align64(u->size, 4));
		dev->stats.snapshots++;
		dev->stats.snapshot_bytes += u->size;
	}

	/* the compute pre-passes of this segment's draws (index rewriting, geometry shaders), ahead of it */
	WGPUComputePassEncoder cp = NULL;
	for (unsigned i = from; i < to; i++) {
		const struct wgvk_prepass *pp = (const struct wgvk_prepass *)rp->prepasses.data + draws[i].prepass0;
		for (unsigned k = 0; k < draws[i].nprepass; k++) {
			if (pp[k].copy_src) {
				if (cp) {
					wgpuComputePassEncoderEnd(cp);
					wgpuComputePassEncoderRelease(cp);
					cp = NULL;
				}
				wgpuCommandEncoderCopyBufferToBuffer(rp->enc, pp[k].copy_src, pp[k].copy_src_off, pp[k].copy_dst,
								     pp[k].copy_dst_off, pp[k].copy_size);
				continue;
			}
			if (!cp)
				cp = wgpuCommandEncoderBeginComputePass(rp->enc, NULL);
			wgpuComputePassEncoderSetPipeline(cp, pp[k].pipe);
			for (unsigned g = 0; g < pp[k].ngroups; g++)
				if (pp[k].bg[g])
					wgpuComputePassEncoderSetBindGroup(cp, g, pp[k].bg[g], pp[k].ndyn[g], pp[k].dyn[g]);
			if (pp[k].imm_size)
				wgpuComputePassEncoderSetImmediates(cp, 0, pp[k].imm, pp[k].imm_size);
			if (pp[k].ind)
				wgpuComputePassEncoderDispatchWorkgroupsIndirect(cp, pp[k].ind, pp[k].ind_off);
			else
				wgpuComputePassEncoderDispatchWorkgroups(cp, pp[k].groups[0], pp[k].groups[1], pp[k].groups[2]);
		}
	}
	if (cp) {
		wgpuComputePassEncoderEnd(cp);
		wgpuComputePassEncoderRelease(cp);
		dev->stats.compute_passes++;
	}

	WGPURenderPassColorAttachment ca[WGVK_MAX_COLOR];
	WGPUTexture dummy_tex = NULL;
	WGPUTextureView dummy_view = NULL;
	if (r->dummy) {
		/* the stand-in target of a pass without attachments: never written (write mask 0), discarded */
		WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
		td.usage = WGPUTextureUsage_RenderAttachment;
		td.size = (WGPUExtent3D){ r->width, r->height, 1 };
		td.format = WGPUTextureFormat_R8Unorm;
		td.sampleCount = MAX2(r->samples, 1);
		dummy_tex = wgpuDeviceCreateTexture(dev->wdev, &td);
		dummy_view = wgpuTextureCreateView(dummy_tex, NULL);
	}
	for (uint32_t i = 0; i < r->ncolor; i++) {
		const struct wgvk_att *a = &r->color[i];
		ca[i] = (WGPURenderPassColorAttachment)WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		if (r->dummy == i + 1) {
			ca[i].view = dummy_view;
			ca[i].loadOp = WGPULoadOp_Clear;
			ca[i].storeOp = WGPUStoreOp_Discard;
			continue;
		}
		if (!a->iv)
			continue;
		ca[i].view = a->iv->attachment;
		ca[i].depthSlice = a->iv->depth_slice;
		ca[i].loadOp = first ? load_op(a->load) : WGPULoadOp_Load;
		ca[i].storeOp = last && !a->resolve ? store_op(a->store) : WGPUStoreOp_Store;
		const VkClearColorValue *c = &a->clear.color;
		if (vk_format_is_int(a->iv->vk.format)) {
			bool sint = vk_format_is_sint(a->iv->vk.format);
			ca[i].clearValue = (WGPUColor){ sint ? c->int32[0] : c->uint32[0], sint ? c->int32[1] : c->uint32[1],
							sint ? c->int32[2] : c->uint32[2], sint ? c->int32[3] : c->uint32[3] };
		} else {
			/* (non-finite clears became quads at the pass's start; this only keeps WebGPU from refusing
			 * the pass should one get here) */
			double v[4] = { c->float32[0], c->float32[1], c->float32[2], c->float32[3] };
			for (unsigned k = 0; k < 4; k++)
				v[k] = isfinite(v[k]) ? v[k] : 0;
			ca[i].clearValue = (WGPUColor){ v[0], v[1], v[2], v[3] };
		}
		if (last && a->resolve) {
			if (a->resolve_mode != VK_RESOLVE_MODE_AVERAGE_BIT)
				wgvk_warn_once("resolve mode %d is averaged by WebGPU\n", a->resolve_mode);
			ca[i].resolveTarget = a->resolve->attachment;
			ca[i].storeOp = store_op(a->store);
		}
	}
	WGPURenderPassDepthStencilAttachment dsa = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	dsa.depthClearValue = 0.0f;	/* (webgpu.h's undefined is NaN, which the browser refuses: VK-16) */
	struct wgvk_image_view *dsv = r->depth.iv ? r->depth.iv : r->stencil.iv;
	/* fragment shaders testing depth themselves (INTERFACES 6.1) sample the depth attachment: read-only in
	 * this pass (end_rendering keeps their draws apart from draws that write depth) */
	bool depth_ro = false;
	for (unsigned i = from; i < to; i++)
		depth_ro |= draws[i].early;
	if (dsv && depth_ro && first && r->depth.iv && wgpu_has_depth(r->ds_fmt) &&
	    load_op(r->depth.load) == WGPULoadOp_Clear) {
		/* the depth clear first, in a pass of its own (a read-only attachment cannot be cleared) */
		WGPURenderPassDepthStencilAttachment cd = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
		cd.depthClearValue = 0.0f;	/* (webgpu.h's undefined is NaN, which the browser refuses: VK-16) */
		cd.view = dsv->attachment;
		cd.depthLoadOp = WGPULoadOp_Clear;
		cd.depthStoreOp = WGPUStoreOp_Store;
		cd.depthClearValue = CLAMP(r->depth.clear.depthStencil.depth, 0.0f, 1.0f);
		cd.stencilReadOnly = wgpu_has_stencil(r->ds_fmt);
		WGPURenderPassDescriptor crd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		crd.depthStencilAttachment = &cd;
		WGPURenderPassEncoder cpass = wgpuCommandEncoderBeginRenderPass(rp->enc, &crd);
		wgpuRenderPassEncoderEnd(cpass);
		wgpuRenderPassEncoderRelease(cpass);
	}
	if (dsv) {
		dsa.view = dsv->attachment;
		if (wgpu_has_depth(r->ds_fmt) && depth_ro) {
			dsa.depthReadOnly = true;
			dev->stats.early_depth_passes++;
		} else if (wgpu_has_depth(r->ds_fmt)) {
			const struct wgvk_att *a = r->depth.iv ? &r->depth : NULL;
			dsa.depthLoadOp = first && a ? load_op(a->load) : WGPULoadOp_Load;
			dsa.depthStoreOp = last && a ? store_op(a->store) : WGPUStoreOp_Store;
			dsa.depthClearValue = a ? CLAMP(a->clear.depthStencil.depth, 0.0f, 1.0f) : 0.0f;
		}
		if (wgpu_has_stencil(r->ds_fmt)) {
			const struct wgvk_att *a = r->stencil.iv ? &r->stencil : NULL;
			dsa.stencilLoadOp = first && a ? load_op(a->load) : WGPULoadOp_Load;
			dsa.stencilStoreOp = last && a ? store_op(a->store) : WGPUStoreOp_Store;
			dsa.stencilClearValue = a ? a->clear.depthStencil.stencil : 0;
		}
		if (r->depth.resolve || r->stencil.resolve)
			wgvk_warn_once("depth/stencil resolves are not in WebGPU (T2: meta resolve)\n");
	}
	WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	rpd.colorAttachmentCount = r->ncolor;
	rpd.colorAttachments = ca;
	rpd.depthStencilAttachment = dsv ? &dsa : NULL;
	bool occl = false;
	for (unsigned i = from; i < to; i++)
		occl |= draws[i].occl1 != 0;
	if (occl)
		rpd.occlusionQuerySet = os->set;
	uint32_t slot_base = os->n;
	int32_t cur_occl = 0;
	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(rp->enc, &rpd);
	dev->stats.pass_segments++;

	WGPURenderPipeline cur = NULL;
	static int trace = -1;
	if (trace < 0)
		trace = getenv("WGVK_TRACE") != NULL;
	if (trace)
		wgvk_log("trace: pass %ux%u, %u colour (load %d), ds %d (load %d %d, clear %.2f), draws %u..%u\n", r->width,
			 r->height, r->ncolor, r->ncolor ? r->color[0].load : -1, r->ds_fmt, r->depth.iv ? r->depth.load : -1,
			 r->stencil.iv ? r->stencil.load : -1, r->depth.clear.depthStencil.depth, from, to);
	for (unsigned i = from; i < to; i++) {
		struct wgvk_draw *d = &draws[i];
		if (trace) {
			const struct wgvk_pipeline_key *k = d->pipe ? &d->pipe->key : NULL;
			wgvk_log("trace: draw kind %u a %u %u %u %u %u vp %.1f %.1f %.1f %.1f %.2f %.2f sc %u %u %u %u "
				 "vbs %u ib %p imm %u pipe %p topo %u cull %u ff %u wmask %u ds %u dw %u dc %u vb0 stride %u "
				 "attrs %u\n", d->kind, d->a[0], d->a[1], d->a[2], d->a[3], d->a[4], d->vp[0], d->vp[1],
				 d->vp[2], d->vp[3], d->vp[4], d->vp[5], d->sc[0], d->sc[1], d->sc[2], d->sc[3], d->nvb,
				 (void *)d->ib, d->imm_size, d->pipe ? (void *)d->pipe->pipeline : NULL, k ? k->topology : 0,
				 k ? k->cull_mode : 0, k ? k->front_face : 0, k ? k->color[0].write_mask : 0,
				 k ? k->ds.format : 0, k ? k->ds.depth_write : 0, k ? k->ds.depth_compare : 0,
				 k ? k->vb[0].stride : 0, k ? k->attr_count : 0);
			for (unsigned v = 0; v < d->nvb; v++)
				wgvk_log("trace:   vb %u: %p +%llu size %llu stride %u step %u\n", v, (void *)d->vb[v].buf,
					 (unsigned long long)d->vb[v].off, (unsigned long long)d->vb[v].size,
					 k ? k->vb[v].stride : 0, k ? k->vb[v].step_mode : 0);
			for (unsigned a = 0; k && a < k->attr_count; a++)
				wgvk_log("trace:   attr loc %u slot %u fmt %u off %u\n", k->attr[a].location, k->attr[a].slot,
					 k->attr[a].format, k->attr[a].offset);
			if (d->ib)
				wgvk_log("trace:   ib %p +%llu size %llu fmt %u\n", (void *)d->ib, (unsigned long long)d->ib_off,
					 (unsigned long long)d->ib_size, d->ib_fmt);
		}
		WGPURenderPipeline p = d->kind == WGVK_DRAW_META ? d->meta : wgvk_rpipe_wait(dev, d->pipe);
		if (!p)
			continue;
		if (occl && d->occl1 != cur_occl) {
			if (cur_occl)
				wgpuRenderPassEncoderEndOcclusionQuery(pass);
			if (d->occl1)
				wgpuRenderPassEncoderBeginOcclusionQuery(pass, occl_slot(os, slot_base, d->occl1 - 1));
			cur_occl = d->occl1;
		}
		if (p != cur)
			wgpuRenderPassEncoderSetPipeline(pass, p);
		cur = p;
		for (uint32_t g = 0; g < d->ngroups; g++) {
			if (d->bg_snap[g])
				wgpuRenderPassEncoderSetBindGroup(pass, g, d->bg_snap[g], d->ndyn[g], d->dyn[g]);
			else if (d->bg[g])
				wgpuRenderPassEncoderSetBindGroup(pass, g, d->bg[g]->bg, d->ndyn[g], d->dyn[g]);

		}
		for (unsigned s = 0; s < d->nvb; s++)
			if (d->vb[s].buf)
				wgpuRenderPassEncoderSetVertexBuffer(pass, s, d->vb[s].buf, d->vb[s].off, d->vb[s].size);
		if (d->ib)
			wgpuRenderPassEncoderSetIndexBuffer(pass, d->ib, d->ib_fmt, d->ib_off, d->ib_size);
		wgpuRenderPassEncoderSetViewport(pass, d->vp[0], d->vp[1], d->vp[2], d->vp[3], d->vp[4], d->vp[5]);
		wgpuRenderPassEncoderSetScissorRect(pass, d->sc[0], d->sc[1], d->sc[2], d->sc[3]);
		wgpuRenderPassEncoderSetBlendConstant(pass, &(WGPUColor){ d->blend[0], d->blend[1], d->blend[2], d->blend[3] });
		wgpuRenderPassEncoderSetStencilReference(pass, d->stencil_ref);
		if (d->imm_size)
			wgpuRenderPassEncoderSetImmediates(pass, 0, d->imm, d->imm_size);
		switch (d->kind) {
		case WGVK_DRAW:
		case WGVK_DRAW_META:
			wgpuRenderPassEncoderDraw(pass, d->a[0], d->a[1], d->a[2], d->a[3]);
			break;
		case WGVK_DRAW_INDEXED:
			wgpuRenderPassEncoderDrawIndexed(pass, d->a[0], d->a[1], d->a[2], (int32_t)d->a[3], d->a[4]);
			break;
		case WGVK_DRAW_INDIRECT:
			wgpuRenderPassEncoderDrawIndirect(pass, d->ind, d->ind_off);
			break;
		case WGVK_DRAW_INDEXED_INDIRECT:
			wgpuRenderPassEncoderDrawIndexedIndirect(pass, d->ind, d->ind_off);
			break;
		}
	}
	if (cur_occl)
		wgpuRenderPassEncoderEndOcclusionQuery(pass);
	wgpuRenderPassEncoderEnd(pass);
	wgpuRenderPassEncoderRelease(pass);
	if (dummy_tex) {
		wgpuTextureViewRelease(dummy_view);
		wgpuTextureRelease(dummy_tex);
	}
}

/* the usage scope of the WebGPU pass being built: GPUBuffer -> 1 read, 2 written */
#define SCOPE_R 1
#define SCOPE_W 2

static bool
tuse_overlap(const struct wgvk_tuse *a, const struct wgvk_tuse *b)
{
	return a->tex && a->tex == b->tex && a->mip0 < b->mip0 + b->nmip && b->mip0 < a->mip0 + a->nmip &&
	       a->layer0 < b->layer0 + b->nlayer && b->layer0 < a->layer0 + a->nlayer;
}

void
wgvk_cmd_end_rendering(struct wgvk_replay *rp)
{
	struct wgvk_device *dev = rp->dev;
	struct wgvk_rendering *r = &rp->r;
	if (!r->active)
		return;
	unsigned n = util_dynarray_num_elements(&rp->draws, struct wgvk_draw);
	struct wgvk_draw *draws = rp->draws.data;
	struct wgvk_use *uses = rp->uses.data;
	struct wgvk_tuse *tuses = rp->tuses.data;

	/* what the pass renders to */
	struct wgvk_tuse att[WGVK_MAX_COLOR + 1];
	unsigned natt = 0;
	struct wgvk_image_view *views[WGVK_MAX_COLOR + 1];
	for (uint32_t i = 0; i < r->ncolor; i++)
		views[natt++] = r->color[i].iv;
	views[natt++] = r->depth.iv ? r->depth.iv : r->stencil.iv;
	unsigned k = 0;
	for (unsigned i = 0; i < natt; i++) {
		if (!views[i])
			continue;
		struct wgvk_image *img = container_of(views[i]->vk.image, struct wgvk_image, vk);
		att[k++] = (struct wgvk_tuse){ img->tex, views[i]->vk.base_mip_level, 1, views[i]->vk.base_array_layer,
					       MAX2(r->layers, 1), 1 };
	}
	natt = k;

	struct hash_table *scope = _mesa_pointer_hash_table_create(NULL);
	struct util_dynarray snaps, tscope;
	util_dynarray_init(&snaps, NULL);
	util_dynarray_init(&tscope, NULL);
	unsigned seg = 0;
	struct seg {
		unsigned from, to;
	} segs[1024];
	unsigned nsegs = 0;
	bool seg_dw = false, seg_early = false;	/* the segment has draws writing depth / testing it in the shader */

	for (unsigned i = 0; i < n; i++) {
		struct wgvk_draw *d = &draws[i];
		struct wgvk_use *du = uses + d->use0;
		bool snapped[256] = { false };
		/* reads of a GPUBuffer this same draw writes: snapshot */
		for (unsigned a = 0; a < d->nuse && a < ARRAY_SIZE(snapped); a++) {
			if (du[a].write)
				continue;
			for (unsigned b = 0; b < d->nuse; b++)
				if (du[b].write && du[b].buf == du[a].buf)
					snapped[a] = true;
		}
		/* feedback loops: sampling a subresource the pass renders to. A snapshot of it, copied just before this
		 * draw's own WebGPU pass (as if a texture barrier came before each such draw) */
		bool fb[64] = { false }, any_fb = false;
		struct wgvk_tuse *dt = tuses + d->tuse0;
		for (unsigned t = 0; t < d->ntuse && t < ARRAY_SIZE(fb); t++)
			for (unsigned a = 0; a < natt; a++)
				if (!dt[t].write && tuse_overlap(&dt[t], &att[a]))
					fb[t] = any_fb = true;
		/* against the scope so far */
		bool conflict = any_fb;
		unsigned nst = util_dynarray_num_elements(&tscope, struct wgvk_tuse);
		for (unsigned t = 0; t < d->ntuse && !conflict; t++) {
			if (t < ARRAY_SIZE(fb) && fb[t])
				continue;
			for (unsigned x = 0; x < nst; x++) {
				const struct wgvk_tuse *o = (const struct wgvk_tuse *)tscope.data + x;
				if ((o->write || dt[t].write) && tuse_overlap(o, &dt[t]))
					conflict = true;
			}
		}
		for (unsigned a = 0; a < d->nuse; a++) {
			struct hash_entry *he = _mesa_hash_table_search(scope, du[a].buf);
			uintptr_t bits = he ? (uintptr_t)he->data : 0;
			if (snapped[a])
				conflict |= (bits & SCOPE_W) != 0;	/* the copy must come after those writes */
			else
				conflict |= du[a].write ? (bits & SCOPE_R) != 0 : (bits & SCOPE_W) != 0;
		}
		/* a draw testing depth in its shader needs the depth read-only: not with a draw writing it */
		bool depth_split = (d->early && seg_dw) || (d->writes_depth && seg_early);
		conflict |= depth_split;
		if (conflict && i > seg) {
			if (nsegs < ARRAY_SIZE(segs))
				segs[nsegs++] = (struct seg){ seg, i };
			seg = i;
			_mesa_hash_table_clear(scope, NULL);
			util_dynarray_clear(&tscope);
			seg_dw = seg_early = false;
			if (any_fb)
				dev->stats.splits_feedback++;
			else if (depth_split)
				dev->stats.splits_depth++;
			else
				dev->stats.splits_buffer++;
		}
		seg_dw |= d->writes_depth;
		seg_early |= d->early;
		for (unsigned t = 0; t < d->ntuse; t++)
			if (!(t < ARRAY_SIZE(fb) && fb[t]))
				util_dynarray_append(&tscope, dt[t]);
		if (any_fb) {
			WGPUTextureView views[WGVK_MAX_SETS][128] = { 0 };
			bool tv[WGVK_MAX_SETS] = { false };
			for (unsigned t = 0; t < d->ntuse && t < ARRAY_SIZE(fb); t++) {
				if (!fb[t] || !dt[t].iv || dt[t].slot >= WGVK_MAX_SETS || dt[t].idx >= 128)
					continue;
				struct wgvk_image_view *iv = dt[t].iv;
				struct wgvk_image *img = container_of(iv->vk.image, struct wgvk_image, vk);
				if (vk_format_is_depth_or_stencil(img->vk.format)) {
					wgvk_warn_once("a feedback loop on a depth/stencil image: not snapshotted (T2: copies "
						       "through a shader)\n");
					continue;
				}
				WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
				td.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
				td.dimension = img->wdim;
				td.size = (WGPUExtent3D){ MAX2(img->vk.extent.width >> dt[t].mip0, 1),
							  MAX2(img->vk.extent.height >> dt[t].mip0, 1),
							  img->vk.image_type == VK_IMAGE_TYPE_3D ?
								  MAX2(img->vk.extent.depth >> dt[t].mip0, 1) : dt[t].nlayer };
				td.format = img->wformat;
				td.mipLevelCount = dt[t].nmip;
				WGPUTexture tmp = wgvk_temp_texture(rp, &td);
				WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
				vd.format = iv->wformat;
				vd.dimension = iv->wdim;
				vd.aspect = iv->aspect;
				views[dt[t].slot][dt[t].idx] = wgpuTextureCreateView(tmp, &vd);
				tv[dt[t].slot] = true;
				util_dynarray_append(&snaps, ((struct snap){ i, t, NULL, tmp }));
				dev->stats.feedback_loops++;
			}
			for (uint32_t g = 0; g < d->ngroups; g++) {
				if (!tv[g] || !d->bg[g])
					continue;
				if (d->bg_snap[g]) {
					wgvk_warn_once("a draw with both buffer and texture snapshots: textures win (T2)\n");
					wgpuBindGroupRelease(d->bg_snap[g]);
				}
				d->bg_snap[g] = wgvk_bind_group_rebuild(rp, d->bg[g], NULL, NULL, views[g]);
				for (unsigned x = 0; x < 128; x++)
					if (views[g][x])
						wgpuTextureViewRelease(views[g][x]);
			}
		}
		for (unsigned a = 0; a < d->nuse; a++) {
			if (snapped[a])
				continue;
			struct hash_entry *he = _mesa_hash_table_search(scope, du[a].buf);
			uintptr_t bits = (he ? (uintptr_t)he->data : 0) | (du[a].write ? SCOPE_W : SCOPE_R);
			_mesa_hash_table_insert(scope, du[a].buf, (void *)bits);
		}
		/* the snapshots: copies in temporary buffers, and the draw's bind groups / buffers pointed at them */
		bool any = false;
		for (unsigned a = 0; a < d->nuse && a < ARRAY_SIZE(snapped); a++)
			any |= snapped[a];
		if (any) {
			WGPUBuffer gbuf[WGVK_MAX_SETS][64] = { 0 };
			uint64_t goff[WGVK_MAX_SETS][64] = { 0 };
			for (unsigned a = 0; a < d->nuse && a < ARRAY_SIZE(snapped); a++) {
				if (!snapped[a])
					continue;
				/* (as large as the binding the shader declares: a uniform block may be larger than what
				 * is bound, and WebGPU checks the bound size against it) */
				uint64_t tsize = du[a].size;
				if (du[a].slot < WGVK_MAX_SETS && d->bg[du[a].slot] &&
				    du[a].idx < d->bg[du[a].slot]->nuse) {
					const struct wgvk_bg *sbg = d->bg[du[a].slot];
					tsize = MAX2(tsize, sbg->bgl->entries[sbg->use[du[a].idx].idx].buffer.minBindingSize);
				}
				WGPUBuffer t = wgvk_temp_buffer(rp, tsize,
								WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform |
								WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex |
								WGPUBufferUsage_Index | WGPUBufferUsage_Indirect);
				util_dynarray_append(&snaps, ((struct snap){ i, a, t }));
				if (du[a].slot < WGVK_MAX_SETS) {
					if (du[a].idx >= 64) {
						wgvk_warn_once("snapshot of bind group entry %u: over 64 buffers\n", du[a].idx);
						continue;
					}
					gbuf[du[a].slot][du[a].idx] = t;
					if (du[a].dyn >= 0)
						d->dyn[du[a].slot][du[a].dyn] = 0;
				} else if (du[a].slot == WGVK_USE_DRIVER) {
					wgvk_warn_once("a snapshot of a driver binding (pulled vertices): not done (T2)\n");
					continue;
				} else if (du[a].slot == WGVK_USE_VB) {
					d->vb[du[a].idx].buf = t;
					d->vb[du[a].idx].off = 0;
				} else if (du[a].slot == WGVK_USE_IB) {
					d->ib = t;
					d->ib_off = 0;
				} else {
					d->ind = t;
					d->ind_off = 0;
				}
			}
			for (uint32_t g = 0; g < d->ngroups; g++) {
				bool touched = false;
				for (unsigned x = 0; x < 64; x++)
					touched |= gbuf[g][x] != NULL;
				if (touched && d->bg[g]) {
					d->bg_snap[g] = wgvk_bind_group_rebuild(rp, d->bg[g], gbuf[g], goff[g], NULL);
				}
			}
		}
	}
	if (nsegs < ARRAY_SIZE(segs))
		segs[nsegs++] = (struct seg){ seg, n };
	else
		segs[nsegs - 1].to = n;

	/* occlusion queries: slots of one query set for the whole Vulkan render pass (at most one per WebGPU pass
	 * and query), added to the queries' accumulators after it */
	static struct occl_slots os;
	os.n = 0;
	os.set = NULL;
	unsigned with_q = 0;
	for (unsigned i = 0; i < n; i++)
		with_q += draws[i].occl1 != 0;
	if (with_q)
		os.set = wgvk_q_occlusion_set(rp, MIN2(with_q, ARRAY_SIZE(os.map)));
	for (unsigned s = 0; s < nsegs; s++)
		encode_segment(rp, segs[s].from, segs[s].to, s == 0, s == nsegs - 1, snaps.data,
			       util_dynarray_num_elements(&snaps, struct snap), &os);
	if (os.n)
		wgvk_q_flush_slots(rp, os.set, os.n, os.map, false);
	wgvk_q_after_pass(rp);

	for (unsigned i = 0; i < n; i++)
		for (uint32_t g = 0; g < draws[i].ngroups; g++)
			if (draws[i].bg_snap[g])
				wgpuBindGroupRelease(draws[i].bg_snap[g]);
	util_dynarray_fini(&snaps);
	util_dynarray_fini(&tscope);
	_mesa_hash_table_destroy(scope, NULL);
	util_dynarray_clear(&rp->draws);
	util_dynarray_clear(&rp->uses);
	util_dynarray_clear(&rp->tuses);
	r->active = false;
}

/* ---- draws */

static WGPUIndexFormat
index_format(VkIndexType t)
{
	switch (t) {
	case VK_INDEX_TYPE_UINT16: return WGPUIndexFormat_Uint16;
	case VK_INDEX_TYPE_UINT32: return WGPUIndexFormat_Uint32;
	default:
		wgvk_warn_once("index type %d needs conversion to 16 bits (T2)\n", t);
		return WGPUIndexFormat_Uint16;
	}
}

static void
add_use(struct wgvk_replay *rp, struct wgvk_draw *d, const struct wgvk_use *u)
{
	wgvk_use_buffer(rp, u);
	util_dynarray_append(&rp->uses, *u);
	d->nuse++;
}

static void
viewport_scissor(struct wgvk_replay *rp, const struct vk_dynamic_graphics_state *dyn, struct wgvk_draw *d, bool *flip)
{
	const struct wgvk_rendering *r = &rp->r;
	if (d->viewports) {
		/* gl_ViewportIndex emulated (INTERFACES 6.6): the WebGPU viewport and scissor are the render area,
		 * the shaders map into it and discard outside viewport i and scissor i */
		*flip = false;
		int64_t x0 = CLAMP(r->area.offset.x, 0, r->width), y0 = CLAMP(r->area.offset.y, 0, r->height);
		int64_t x1 = CLAMP((int64_t)r->area.offset.x + r->area.extent.width, x0, r->width);
		int64_t y1 = CLAMP((int64_t)r->area.offset.y + r->area.extent.height, y0, r->height);
		d->vp[0] = x0, d->vp[1] = y0, d->vp[2] = x1 - x0, d->vp[3] = y1 - y0, d->vp[4] = 0, d->vp[5] = 1;
		d->sc[0] = x0, d->sc[1] = y0, d->sc[2] = x1 - x0, d->sc[3] = y1 - y0;
		return;
	}
	VkViewport vp = dyn->vp.viewport_count ? dyn->vp.viewports[0]
					       : (VkViewport){ 0, 0, (float)r->width, (float)r->height, 0, 1 };

	*flip = vp.height < 0;
	if (*flip) {
		/* Vulkan's y-flip by a negative height: the same pixels as a positive-height viewport one height
		 * up, with the vertex shader keeping clip-space y (keep_clip_y, INTERFACES 6) */
		vp.y += vp.height;
		vp.height = -vp.height;
	}
	float mind = vp.minDepth, maxd = vp.maxDepth;
	if (mind > maxd) {
		wgvk_warn_once("an inverted depth range is not in WebGPU (T3b: z in the vertex shader)\n");
		float t = mind;
		mind = maxd;
		maxd = t;
	}
	d->vp[0] = vp.x;
	d->vp[1] = vp.y;
	d->vp[2] = MAX2(vp.width, 0.0f);
	d->vp[3] = MAX2(vp.height, 0.0f);
	d->vp[4] = CLAMP(mind, 0.0f, 1.0f);
	d->vp[5] = CLAMP(maxd, 0.0f, 1.0f);
	/* WebGPU wants the scissor inside the attachment */
	int64_t x0 = r->area.offset.x, y0 = r->area.offset.y;
	int64_t x1 = x0 + r->area.extent.width, y1 = y0 + r->area.extent.height;
	if (dyn->vp.scissor_count) {
		const VkRect2D *s = &dyn->vp.scissors[0];
		x0 = MAX2(x0, s->offset.x);
		y0 = MAX2(y0, s->offset.y);
		x1 = MIN2(x1, (int64_t)s->offset.x + s->extent.width);
		y1 = MIN2(y1, (int64_t)s->offset.y + s->extent.height);
	}
	x0 = CLAMP(x0, 0, r->width);
	y0 = CLAMP(y0, 0, r->height);
	x1 = CLAMP(x1, x0, r->width);
	y1 = CLAMP(y1, y0, r->height);
	d->sc[0] = x0;
	d->sc[1] = y0;
	d->sc[2] = x1 - x0;
	d->sc[3] = y1 - y0;
}

/* Textures bound where the shaders sample float that WebGPU cannot filter (depth, unfilterable formats), and
 * the samplers of the same combined image samplers. */
void
wgvk_unfilterable(const struct wgvk_bound_sets *bind, const struct wgvk_compiled_shader *const *cs, unsigned n,
		  struct wgvk_unfilterable *uf)
{
	uf->n = 0;
	for (unsigned s = 0; s < n; s++) {
		if (!cs[s])
			continue;
		for (unsigned i = 0; i < cs[s]->n_bindings; i++) {
			const struct wgvk_shader_binding *b = &cs[s]->bindings[i];
			if (b->kind != WGSL_BINDING_TEXTURE || b->sample_type != WGSL_SAMPLE_FLOAT || b->group >= WGVK_MAX_SETS)
				continue;
			struct wgvk_descriptor_set *set = bind->set[b->group];
			if (!set || b->binding >= set->count || set->desc[b->binding].kind != WGVK_DESC_IMAGE ||
			    !set->desc[b->binding].img.view)
				continue;
			const struct wgvk_image_view *iv = set->desc[b->binding].img.view;
			bool depth = iv->aspect == WGPUTextureAspect_DepthOnly || vk_format_is_depth_or_stencil(iv->vk.format);
			if (!depth && iv->fmt && (iv->fmt->caps & WGVK_CAP_FILTER))
				continue;
			if (uf->n < ARRAY_SIZE(uf->e))
				uf->e[uf->n++] = (struct wgvk_uf_entry){ b->group, b->binding };
			/* the sampler of the same combined image sampler */
			for (unsigned t = 0; t < n; t++)
				for (unsigned j = 0; cs[t] && j < cs[t]->n_bindings; j++) {
					const struct wgvk_shader_binding *c = &cs[t]->bindings[j];
					if (c->sampler_of_combined && c->vk_set == b->vk_set && c->vk_binding == b->vk_binding &&
					    c->vk_index == b->vk_index && uf->n < ARRAY_SIZE(uf->e))
						uf->e[uf->n++] = (struct wgvk_uf_entry){ c->group, c->binding };
				}
		}
	}
}

/* The fragment shader's variant for this draw (INTERFACES 6.1, 6.6): viewport / layer emulation when the last
 * vertex stage writes gl_ViewportIndex or gl_Layer, the early depth test when the shader writes memory under
 * early_fragment_tests and the draw tests depth (the pass then binds its depth read-only). Sets d->viewports,
 * d->early, d->early_view and *vp_varying (the location + 1 the last vertex stage writes (i, layer) to). */
static const struct wgvk_compiled_shader *
fs_variant(struct wgvk_replay *rp, const struct vk_dynamic_graphics_state *dyn, struct wgvk_draw *d, uint8_t *vp_varying)
{
	const struct wgvk_rendering *r = &rp->r;
	*vp_varying = 0;
	struct wgvk_shader *last = rp->gs ? rp->gs : rp->vs;
	if (!rp->fs) {
		if (last && last->writes_viewport_index)
			wgvk_warn_once("gl_ViewportIndex without a fragment shader: viewport 0 (T2)\n");
		return NULL;
	}
	struct wgvk_fs_key fk = { 0 };
	fk.viewports = last && last->writes_viewport_index;
	/* a colour output whose type is not the attachment's (float bits written to an integer attachment, which
	 * Vulkan allows and WebGPU's pipeline creation refuses: Blender's outline ids, R16UI, VK-16): declared as
	 * the attachment's type, the bits kept */
	for (unsigned i = 0; i < r->ncolor && i < 8; i++) {
		const struct wgvk_image_view *cv = r->color[i].iv;
		if (!cv || !(rp->fs->fs_outputs & (1u << i)))
			continue;
		bool fmt_int = vk_format_is_int(cv->vk.format), sh_float = rp->fs->fs_float_outputs & (1u << i);
		if (fmt_int && sh_float)
			fk.out_type[i] = vk_format_is_sint(cv->vk.format) ? 2 : 3;
		else if (!fmt_int && !sh_float)
			fk.out_type[i] = 1;
	}
	struct wgvk_image_view *dv = r->depth.iv;
	if (rp->fs->fs_early && dyn->ds.depth.test_enable && dv && wgpu_has_depth(r->ds_fmt)) {
		if (dyn->ds.depth.write_enable)
			wgvk_warn_once("early fragment tests in a draw that writes depth: not emulated (T2)\n");
		else
			fk.early_depth = r->samples > 1 ? 2 : 1;
	}
	const struct wgvk_compiled_shader *fcs = wgvk_shader_fs_variant(rp->dev, rp->fs, &fk);
	if (fk.viewports && fcs->legal.viewport_index_location >= 0) {
		*vp_varying = fcs->legal.viewport_index_location + 1;
		d->viewports = 1;
	}
	if (fk.early_depth && fcs->legal.early_depth_test) {
		d->early = 1;
		if (!dv->depth_ro) {
			struct wgvk_image *img = container_of(dv->vk.image, struct wgvk_image, vk);
			WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
			vd.dimension = WGPUTextureViewDimension_2D;
			vd.baseMipLevel = dv->vk.base_mip_level;
			vd.mipLevelCount = 1;
			vd.baseArrayLayer = dv->vk.base_array_layer;
			vd.arrayLayerCount = 1;
			vd.aspect = WGPUTextureAspect_DepthOnly;
			vd.usage = WGPUTextureUsage_TextureBinding;
			dv->depth_ro = wgpuTextureCreateView(img->tex, &vd);
		}
		d->early_view = dv->depth_ro;
	}
	return fcs;
}

/* The vertex shader's variant key for this draw: the fragment shader's inputs, viewport sign, depth range,
 * vertex pulling (bindings WebGPU's vertex state cannot express) and point expansion. */
static void
vs_key(struct wgvk_replay *rp, const struct vk_dynamic_graphics_state *dyn, bool flip, bool indexed,
       const struct wgvk_compiled_shader *fcs, uint8_t vp_varying, struct wgvk_vs_key *k)
{
	bool gs = rp->gs != NULL;
	memset(k, 0, sizeof(*k));
	if (fcs)
		memcpy(k->fs_varyings, fcs->fs_varyings, 32);
	if (!gs)
		k->viewport_index_varying = vp_varying;
	k->keep_clip_y = flip;
	k->z_minus_one = dyn->vp.depth_clip_negative_one_to_one;
	if (!dyn->vi)
		return;
	struct wgvk_vertex_input vi;
	memset(&vi, 0, sizeof(vi));
	uint32_t used = 0;
	u_foreach_bit(l, dyn->vi->attributes_valid) {
		if (l >= 32 || !(rp->vs->vs_inputs & (1u << l)))
			continue;
		const struct vk_vertex_attribute_state *at = &dyn->vi->attributes[l];
		vi.attr[l] = (struct wgvk_vertex_attr){ at->binding, vk_format_to_pipe_format(at->format), at->offset };
		used |= 1u << at->binding;
	}
	u_foreach_bit(b, used) {
		if (b >= 16)
			continue;
		const struct vk_vertex_binding_state *bs = &dyn->vi->bindings[b];
		vi.binding[b] = (struct wgvk_vertex_binding){
			dyn->vi_binding_strides[b], bs->input_rate == VK_VERTEX_INPUT_RATE_INSTANCE, bs->divisor };
	}
	uint16_t pull = 0;
	u_foreach_bit(b, used) {
		if (b >= 16)
			continue;
		bool unaligned = rp->vb[b].buf && (rp->vb[b].buf->offset + rp->vb[b].off) % 4;
		/* an integer format read as a float input (zink's way to feed Blender's text and overlays their
		 * attributes: 32-bit integers whose bits the shader takes as floats, VK-16): WebGPU wants the
		 * format's base type to be the shader's, so the shader fetches it (the bits stay as they are) */
		bool convert = false;
		u_foreach_bit(l, rp->vs->vs_float_inputs) {
			enum pipe_format f = vi.attr[l].format;
			convert |= vi.attr[l].binding == b && f != PIPE_FORMAT_NONE && util_format_is_pure_integer(f);
		}
		if (unaligned || convert || wgvk_vertex_binding_needs_pulling(&vi, b))
			pull |= 1u << b;
	}
	if (gs) {
		/* before a geometry shader: a compute shader, every attribute pulled (INTERFACES 6.2) */
		memset(k->fs_varyings, 0, sizeof(k->fs_varyings));
		k->keep_clip_y = k->z_minus_one = 0;
		k->viewport_index_varying = 0;
		k->vs_before_gs = 1;
		k->pull_mask = used & 0xffff;
		k->vi = vi;
		return;
	}
	if (dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST && rp->vs->vs_writes_psiz) {
		k->point_mode = !indexed ? WGVK_POINTS_EXPAND :
				rp->ib.type == VK_INDEX_TYPE_UINT16 ? WGVK_POINTS_EXPAND_INDEXED16 : WGVK_POINTS_EXPAND_INDEXED32;
		if (indexed && rp->ib.type == VK_INDEX_TYPE_UINT8_KHR) {
			wgvk_warn_once("large points with 8-bit indices: 1-pixel points (T2)\n");
			k->point_mode = 0;
		}
		if (fcs && fcs->legal.point_coord_location >= 0)
			k->point_coord_varying = fcs->legal.point_coord_location + 1;
	}
	if (k->point_mode)
		pull = used & 0xffff;	/* every attribute is pulled */
	k->pull_mask = pull;
	if (pull || k->point_mode)
		k->vi = vi;
}

/* libpoly's memory for one geometry shader draw (INTERFACES 6.2): params, a sink, counts, vertex outputs,
 * the output index buffer; addresses in it are byte offsets (segment 0) */
#define POLY_OFF_VP 0
#define POLY_OFF_GP 128
#define POLY_OFF_SINK 512
#define POLY_OFF_SETUP 640	/* indirect draws: struct wgvk_gs_indirect_args */
#define POLY_OFF_HEAP 704	/* ... struct poly_heap (base 0) */
#define POLY_OFF_DRAW_PARAMS 752	/* ... struct wgvk_draw_params, from the command */
#define POLY_OFF_REST 1024
/* indirect draws: the count buffer (no prefix sums), the static topology, then the heap the setup allocates
 * the vertex outputs and the output index buffer from */
#define POLY_OFF_I_COUNT 1024
#define POLY_OFF_I_TOPOLOGY 2048
#define POLY_OFF_I_HEAP 4096
/* the poly indirect buffer (937): the command, then the setup's 9 dispatch words */
#define POLY_IND_DRAW 0
#define POLY_IND_DISPATCH 32
#define POLY_IND_SIZE 128

/* The driver's entries (bindings 900..) of a draw's bind group layout. */
static unsigned
driver_entries(struct wgvk_replay *rp, struct wgvk_draw *d, const struct wgvk_bgl *bgl, const struct wgvk_vs_key *k,
	       const struct wgvk_compiled_shader *vcs, const struct vk_dynamic_graphics_state *dyn, bool indexed,
	       const uint32_t a[5], int32_t base_vertex, uint32_t base_instance, WGPUBindGroupEntry *out)
{
	struct wgvk_device *dev = rp->dev;
	unsigned n = 0;
	struct wgvk_driver_uniforms *du = NULL;
	WGPUBindGroupEntry *du_entry = NULL;
	struct wgvk_heap_state hs = { 0 };
	uint32_t salign = MAX2(dev->pdev->limits.minStorageBufferOffsetAlignment, 4);
	for (unsigned i = 0; i < bgl->n && n < 24; i++) {
		uint32_t b = bgl->entries[i].binding;
		if (b < WGVK_DRIVER_BINDING_BASE)
			continue;
		WGPUBindGroupEntry *e = &out[n++];
		*e = (WGPUBindGroupEntry)WGPU_BIND_GROUP_ENTRY_INIT;
		e->binding = b;
		if (b == WGVK_DRIVER_BINDING_PUSH_CONSTANTS) {
			void *p = wgvk_ring_alloc(rp, sizeof(rp->push), &e->buffer, &e->offset);
			memcpy(p, rp->push, sizeof(rp->push));
			e->size = sizeof(rp->push);
		} else if (b == WGVK_DRIVER_BINDING_UNIFORMS) {
			du = wgvk_ring_alloc(rp, sizeof(*du), &e->buffer, &e->offset);
			e->size = align64(sizeof(*du), 16);
			du_entry = e;
		} else if (b >= WGVK_DRIVER_BINDING_VERTEX_BUFFER && b < WGVK_DRIVER_BINDING_VERTEX_BUFFER + 16) {
			unsigned vb = b - WGVK_DRIVER_BINDING_VERTEX_BUFFER;
			const struct wgvk_vb *v = &rp->vb[vb];
			if (!v->buf || !v->buf->mem || !v->buf->mem->bo.buffer) {
				e->buffer = wgvk_dummy_buffer(dev);
				e->size = 256;
				continue;
			}
			uint64_t off;
			WGPUBuffer buf = wgvk_buffer_wgpu(v->buf, v->off, &off);
			uint64_t base = off / salign * salign, end = MIN2(align64(off + v->size, 4),
									  v->buf->mem->bo.offset + v->buf->mem->bo.size);
			e->buffer = buf;
			e->offset = base;
			e->size = MAX2(end - base, 4);
			d->pull_off[vb] = off - base;
			add_use(rp, d, &(struct wgvk_use){ v->buf->mem, buf, base, e->size, 0, WGVK_USE_DRIVER, vb, -1 });
		} else if (b == WGVK_DRIVER_BINDING_INDEX_BUFFER) {
			if (!indexed || !rp->ib.buf || !rp->ib.buf->mem) {
				e->buffer = wgvk_dummy_buffer(dev);
				e->size = 256;
				continue;
			}
			uint64_t off;
			WGPUBuffer buf = wgvk_buffer_wgpu(rp->ib.buf, rp->ib.off, &off);
			uint64_t base = off / salign * salign;
			uint64_t end = MIN2(align64(off + rp->ib.size, 4), rp->ib.buf->mem->bo.offset + rp->ib.buf->mem->bo.size);
			e->buffer = buf;
			e->offset = base;
			e->size = MAX2(end - base, 4);
			d->ib_pull_off = off - base;
			add_use(rp, d, &(struct wgvk_use){ rp->ib.buf->mem, buf, base, e->size, 0, WGVK_USE_DRIVER, 0, -1 });
		} else if (b == WGVK_DRIVER_BINDING_HEAP_TABLE ||
			   (b >= WGVK_DRIVER_BINDING_HEAP && b < WGVK_DRIVER_BINDING_HEAP + WGVK_HEAP_ARENAS)) {
			if (!hs.ready) {
				/* the stage whose storage buffers went to the heap (one per draw: T3b numbers slots per
				 * shader, and the stages share binding 902) */
				const struct wgvk_compiled_shader *hcs = vcs;
				const struct wgvk_layout_remap *remap = &rp->vs->remap;
				if (rp->fs && rp->fs->cs.legal.heap_slots) {
					if (vcs && vcs->legal.heap_slots)
						wgvk_warn_once("buffer heap in two stages of one draw: the fragment stage's used\n");
					hcs = &rp->fs->cs;
					remap = &rp->fs->remap;
				}
				wgvk_heap_prepare(rp, hcs, remap, &rp->bind[0], &hs);
			}
			struct wgvk_use u;
			if (wgvk_heap_entry(&hs, b, e, &u))
				add_use(rp, d, &u);
		} else if (b == WGVK_DRIVER_BINDING_POLY && d->poly) {
			e->buffer = d->poly;
			e->size = d->poly_size;
		} else if (b == WGVK_DRIVER_BINDING_POLY_INDIRECT && d->poly_ind) {
			e->buffer = d->poly_ind;
			e->size = POLY_IND_SIZE;
		} else if (b == WGVK_DRIVER_BINDING_DEPTH && d->early_view) {
			e->textureView = d->early_view;
		} else {
			wgvk_warn_once("driver binding %u not filled yet (T2: buffer heap 902/910+)\n", b);
			e->buffer = wgvk_dummy_buffer(dev);
			e->size = 256;
		}
	}
	if (du) {
		memset(du, 0, sizeof(*du));
		du->draw = (struct wgvk_draw_params){ base_vertex, base_instance, rp->draw_id };
		du->first_index = indexed ? a[2] : 0;
		du->viewport_inv_size[0] = d->vp[2] > 0 ? 1.0f / d->vp[2] : 0;
		du->viewport_inv_size[1] = d->vp[3] > 0 ? 1.0f / d->vp[3] : 0;
		du->point_size_range[0] = dev->pdev->vk.properties.pointSizeRange[0];
		du->point_size_range[1] = dev->pdev->vk.properties.pointSizeRange[1];
		for (unsigned b = 0; b < 16; b++)
			du->vertex_buffer_offset[b] = d->pull_off[b];
		du->index_buffer_offset = d->ib_pull_off;
		if (d->poly) {
			du->poly_vertex_params = POLY_OFF_VP;
			du->poly_geometry_params = POLY_OFF_GP;
			du->poly_sink = POLY_OFF_SINK;
		}
		if (d->poly_ind) {
			du->poly_setup_args = POLY_OFF_SETUP;
			du->poly_heap = POLY_OFF_HEAP;
			du->poly_indirect_draw = POLY_IND_DRAW;
			du->poly_indirect_dispatch = POLY_IND_DISPATCH;
			du->poly_draw_params = POLY_OFF_DRAW_PARAMS;
		}
		if (d->viewports) {
			/* (INTERFACES 6.6) the WebGPU viewport (the render area), Vulkan's viewports and scissors */
			du->layer = 0;
			for (unsigned c = 0; c < 4; c++)
				du->render_area[c] = d->vp[c];
			for (unsigned v = 0; v < MIN2(dyn->vp.viewport_count, 16); v++) {
				const VkViewport *w = &dyn->vp.viewports[v];
				float f[6] = { w->x, w->y, w->width, w->height, w->minDepth, w->maxDepth };
				memcpy(du->viewports[v], f, sizeof(f));
			}
			for (unsigned v = 0; v < MIN2(dyn->vp.scissor_count, 16); v++) {
				const VkRect2D *sc = &dyn->vp.scissors[v];
				du->scissors[v][0] = sc->offset.x;
				du->scissors[v][1] = sc->offset.y;
				du->scissors[v][2] = MIN2(sc->extent.width, INT32_MAX);
				du->scissors[v][3] = MIN2(sc->extent.height, INT32_MAX);
			}
		}
		/* the early depth test (INTERFACES 6.1): the pipeline's compare op, depths as the format stores them
		 * (depth24plus: 24-bit unorm natively, Dawn's Vulkan backend on lavapipe; it may be float elsewhere) */
		du->depth_compare = dyn->ds.depth.test_enable ? dyn->ds.depth.compare_op : VK_COMPARE_OP_ALWAYS;
		WGPUTextureFormat df = rp->r.ds_fmt;
		du->depth_unorm_max = df == WGPUTextureFormat_Depth16Unorm ? 65535.0f :
				      df == WGPUTextureFormat_Depth24Plus || df == WGPUTextureFormat_Depth24PlusStencil8
					      ? 16777215.0f : 0.0f;
		(void)du_entry;
	}
	(void)k;
	(void)vcs;
	(void)dyn;
	return n;
}

/* ---- geometry shader draws (T4's libpoly, INTERFACES 6.2): the vertex shader and the GS's compute parts as
 * pre-passes ahead of the render pass, then the rast part drawn indirectly from libpoly's draw command */

static enum mesa_prim
vk_to_mesa_prim(VkPrimitiveTopology t)
{
	switch (t) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return MESA_PRIM_POINTS;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return MESA_PRIM_LINES;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return MESA_PRIM_LINE_STRIP;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return MESA_PRIM_TRIANGLES;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return MESA_PRIM_TRIANGLE_STRIP;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return MESA_PRIM_TRIANGLE_FAN;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: return MESA_PRIM_LINES_ADJACENCY;
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return MESA_PRIM_LINE_STRIP_ADJACENCY;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY: return MESA_PRIM_TRIANGLES_ADJACENCY;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: return MESA_PRIM_TRIANGLE_STRIP_ADJACENCY;
	default: return MESA_PRIM_TRIANGLES;
	}
}

static VkPrimitiveTopology
mesa_to_vk_list(uint8_t mode)
{
	switch (mode) {
	case MESA_PRIM_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	case MESA_PRIM_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
	case MESA_PRIM_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	case MESA_PRIM_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}
}

/* One compute part as a pre-pass of draw d: its bind groups (the application's sets and the driver's), its
 * immediates (push constants, draw parameters at the part's own offset). */
static void
gs_prepass(struct wgvk_replay *rp, struct wgvk_draw *d, const struct wgvk_compiled_shader *cs,
	   const struct wgvk_layout_remap *remap, const struct wgvk_vs_key *k, const struct vk_dynamic_graphics_state *dyn,
	   bool indexed, const uint32_t a[5], int32_t base_vertex, uint32_t base_instance, uint32_t gx, uint32_t gy)
{
	if (!cs->wgsl || !gx || !gy)
		return;
	const struct wgvk_cpipe *cp = wgvk_cpipe_get(rp->dev, cs, remap, NULL);
	struct wgvk_prepass pp = { .pipe = cp->pipe, .ngroups = cp->ngroups, .groups = { MIN2(gx, 65535), gy, 1 } };
	if (gx == ~0u) {
		/* (an indirect GS draw: the workgroup counts the setup part wrote, at gy in the poly indirect buffer) */
		pp.ind = d->poly_ind;
		pp.ind_off = gy;
	} else if (gx > 65535)
		wgvk_warn_once("a geometry shader draw over 65535 workgroups wide: cut (T2)\n");
	for (uint32_t g = 0; g < cp->ngroups; g++) {
		WGPUBindGroupEntry drv[24];
		unsigned nd = driver_entries(rp, d, cp->bgl[g], k, cs, dyn, indexed, a, base_vertex, base_instance, drv);
		struct wgvk_bg *bg = wgvk_replay_bind_group(rp, rp->bind[0].set[g], cp->bgl[g], drv, nd);
		pp.bg[g] = bg->bg;
		pp.ndyn[g] = wgvk_bind_group_dyn(bg, rp->bind[0].dyn[g], pp.dyn[g]);
		for (unsigned i = 0; i < bg->nuse; i++) {
			struct wgvk_use u = bg->use[i];
			if (u.dyn >= 0)
				u.off += pp.dyn[g][u.dyn];
			wgvk_use_buffer(rp, &u);	/* uploads; read in the compute pass, not the render pass */
		}
	}
	pp.imm_size = align(MIN2(cp->immediate_bytes, WGVK_MAX_IMMEDIATES), 4);
	memcpy(pp.imm, rp->push, pp.imm_size);
	if (cp->draw_params_offset != 0xffff && cp->draw_params_offset + 12 <= WGVK_MAX_IMMEDIATES) {
		struct wgvk_draw_params dp = { base_vertex, base_instance, rp->draw_id };
		memcpy(pp.imm + cp->draw_params_offset, &dp, sizeof(dp));
		pp.imm_size = MAX2(pp.imm_size, cp->draw_params_offset + 12u);
	}
	if (!d->nprepass)
		d->prepass0 = util_dynarray_num_elements(&rp->prepasses, struct wgvk_prepass);
	util_dynarray_append(&rp->prepasses, pp);
	d->nprepass++;
}

static void
gs_draw(struct wgvk_replay *rp, enum wgvk_draw_kind kind, const uint32_t a[5], struct wgvk_buffer *ind, uint64_t ind_off)
{
	struct wgvk_device *dev = rp->dev;
	struct wgvk_rendering *r = &rp->r;
	const struct vk_dynamic_graphics_state *dyn = &rp->cmd->vk.dynamic_graphics_state;
	bool indexed = kind == WGVK_DRAW_INDEXED || kind == WGVK_DRAW_INDEXED_INDIRECT;
	uint32_t count = ind ? 0 : a[0], inst = ind ? 0 : a[1];
	int32_t base_vertex = ind ? 0 : indexed ? (int32_t)a[3] : (int32_t)a[2];
	uint32_t base_instance = ind ? 0 : indexed ? a[4] : a[3];

	struct wgvk_draw d;
	memset(&d, 0, sizeof(d));
	uint8_t vpv;
	const struct wgvk_compiled_shader *fcs = fs_variant(rp, dyn, &d, &vpv);
	bool flip;
	viewport_scissor(rp, dyn, &d, &flip);
	struct wgvk_vs_key vk;
	vs_key(rp, dyn, false, indexed, fcs, 0, &vk);
	const struct wgvk_compiled_shader *vcs = wgvk_shader_vs_variant(dev, rp->vs, &vk);
	struct wgvk_gs_key gk;
	memset(&gk, 0, sizeof(gk));
	if (fcs)
		memcpy(gk.fs_varyings, fcs->fs_varyings, 32);
	gk.keep_clip_y = flip;
	gk.z_minus_one = dyn->vp.depth_clip_negative_one_to_one;
	gk.viewport_index_varying = vpv;
	const struct wgvk_gs_variant *gv = wgvk_shader_gs_variant(dev, rp->gs, &gk);
	if (!gv || !vcs->legal.vs_outputs) {
		wgvk_warn_once("a geometry shader draw without its parts (compile failed): skipped\n");
		return;
	}
	const struct wgvk_gs_program *g = &gv->prog;
	if (ind) {
		/* the setup part (INTERFACES 6.2, "Indirect draws through a GS"), compiled once per device */
		if (!dev->gs_setup_state)
			dev->gs_setup_state = wgvk_compile_gs_setup(wgvk_device_compile_caps(dev), &dev->gs_setup) ==
							      VK_SUCCESS ? 1 : -1;
		if (dev->gs_setup_state < 0) {
			wgvk_warn_once("an indirect geometry shader draw without the setup part (not compiled): skipped\n");
			return;
		}
	}

	/* libpoly's parameters, written on the CPU into this draw's poly buffer: all of them for a direct draw;
	 * for an indirect one what does not depend on the command (the setup part writes the rest) */
	enum mesa_prim prim = vk_to_mesa_prim(dyn->ia.primitive_topology);
	static const uint32_t wg[3] = { 64, 1, 1 };
	struct poly_vertex_params vp;
	struct poly_geometry_params gp;
	poly_vertex_params_init(&vp, vcs->legal.vs_outputs, wg);
	poly_geometry_params_init(&gp, prim, wg);
	uint32_t prims = 0, isz = 0;
	if (indexed) {
		if (!rp->ib.buf || !rp->ib.buf->mem) {
			wgvk_warn_once("an indexed draw without an index buffer\n");
			return;
		}
		isz = rp->ib.type == VK_INDEX_TYPE_UINT8_KHR ? 1 : rp->ib.type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
	}
	uint64_t at, count_off = 0, index_off = 0, vs_out = 0;
	if (!ind) {
		poly_vertex_params_set_draw(&vp, count, inst);
		poly_geometry_params_set_draw(&gp, prim, g->shape, g->max_indices, count, inst);
		prims = gp.grid[0];
		if (!prims || !inst)
			return;
		at = POLY_OFF_REST;
		if (g->count_words) {
			count_off = at;
			at = align64(at + (uint64_t)gp.input_primitives * g->count_words * 4, 256);
		}
		vs_out = at;
		at = align64(at + poly_tcs_in_size(count * inst, vcs->legal.vs_outputs), 256);
		if (g->shape == POLY_GS_SHAPE_DYNAMIC_INDEXED) {
			index_off = at;
			at = align64(at + (uint64_t)gp.draw.index_count * 4, 256);
		} else if (g->shape == POLY_GS_SHAPE_STATIC_INDEXED) {
			index_off = at;
			at = align64(at + (uint64_t)g->max_indices * 4, 256);
		}
		vp.output_buffer = vs_out;
	} else {
		/* the heap the setup allocates the vertex outputs and the output index buffer from: the counts are
		 * on the GPU, so a fixed size (WGVK_POLY_HEAP_MB, default 8) */
		static uint64_t heap_mb;
		if (!heap_mb) {
			const char *e = getenv("WGVK_POLY_HEAP_MB");
			heap_mb = e && atoi(e) > 0 ? atoi(e) : 8;
		}
		count_off = g->count_words && !g->prefix_sum ? POLY_OFF_I_COUNT : 0;
		index_off = g->shape == POLY_GS_SHAPE_STATIC_INDEXED ? POLY_OFF_I_TOPOLOGY : 0;
		at = POLY_OFF_I_HEAP + (heap_mb << 20);
	}
	if (indexed) {
		uint64_t off;
		wgvk_buffer_wgpu(rp->ib.buf, rp->ib.off, &off);
		uint32_t salign = MAX2(dev->pdev->limits.minStorageBufferOffsetAlignment, 4);
		d.ib_pull_off = off - off / salign * salign;	/* as driver_entries binds 936 */
		vp.index_size_B = isz;
		if (!ind) {
			vp.index_buffer = (1ull << 32) | (d.ib_pull_off + (uint64_t)a[2] * isz);
			vp.index_buffer_range_el = poly_index_buffer_range_el(rp->ib.size / isz, a[2]);
		}
	}
	for (unsigned i = 0; i < POLY_MAX_VERTEX_STREAMS; i++) {
		gp.prims_generated_counter[i] = POLY_OFF_SINK;
		gp.xfb_prims_generated_counter[i] = POLY_OFF_SINK;
		gp.xfb_overflow[i] = POLY_OFF_SINK;
	}
	gp.xfb_any_overflow = POLY_OFF_SINK;
	if (g->count_words) {
		gp.count_buffer = count_off;
		gp.count_buffer_stride = g->count_words * 4;
	}
	if (g->shape == POLY_GS_SHAPE_DYNAMIC_INDEXED && !ind) {
		gp.output_index_buffer = index_off;
		gp.draw.first_index = index_off / 4;
	} else if (g->shape == POLY_GS_SHAPE_STATIC_INDEXED) {
		gp.draw.first_index = index_off / 4;
	}
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	bd.size = at;
	bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Index | WGPUBufferUsage_Indirect | WGPUBufferUsage_CopyDst;
	bd.mappedAtCreation = true;
	d.poly = wgpuDeviceCreateBuffer(dev->wdev, &bd);
	d.poly_size = at;
	uint64_t head = ind ? POLY_OFF_I_HEAP : at;	/* (the rest is zero, as WebGPU makes it) */
	uint8_t *p = wgpuBufferGetMappedRange(d.poly, 0, head);
	if (p) {
		memset(p, 0, head);
		memcpy(p + POLY_OFF_VP, &vp, sizeof(vp));
		memcpy(p + POLY_OFF_GP, &gp, sizeof(gp));
		if (g->shape == POLY_GS_SHAPE_STATIC_INDEXED)
			for (unsigned i = 0; i < g->max_indices; i++)
				((uint32_t *)(p + index_off))[i] = g->topology[i] == 0xff ? ~0u : g->topology[i];
		if (ind) {
			struct wgvk_gs_indirect_args ia = {
				.vs_outputs = vcs->legal.vs_outputs, .index_size_B = isz,
				.index_buffer_range_el = isz ? rp->ib.size / isz : 0, .prim = prim, .prefix_sum = g->prefix_sum,
				.max_indices = g->max_indices, .shape = g->shape, .draw_id = rp->draw_id,
			};
			memcpy(p + POLY_OFF_SETUP, &ia, sizeof(ia));
			/* base 0: libpoly gives the GS index buffer's first index relative to the heap's base, and the
			 * rast draw binds the whole poly buffer as its index buffer */
			struct poly_heap heap = { .base = 0, .bottom = POLY_OFF_I_HEAP, .size = (uint32_t)at };
			memcpy(p + POLY_OFF_HEAP, &heap, sizeof(heap));
		}
	}
	wgpuBufferUnmap(d.poly);
	util_dynarray_append(&rp->sub->buffers, d.poly);

	d.use0 = util_dynarray_num_elements(&rp->uses, struct wgvk_use);
	d.tuse0 = util_dynarray_num_elements(&rp->tuses, struct wgvk_tuse);
	if (ind) {
		/* the command into the poly indirect buffer (a dispatch may not read its indirect arguments from a
		 * buffer it binds writable), then the setup part, which writes the parts' workgroup counts there */
		WGPUBufferDescriptor ibd = WGPU_BUFFER_DESCRIPTOR_INIT;
		ibd.size = POLY_IND_SIZE;
		ibd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect | WGPUBufferUsage_CopyDst;
		d.poly_ind = wgpuDeviceCreateBuffer(dev->wdev, &ibd);
		util_dynarray_append(&rp->sub->buffers, d.poly_ind);
		uint64_t src_off;
		WGPUBuffer src = wgvk_buffer_wgpu(ind, ind_off, &src_off);
		uint32_t cmd_size = indexed ? 20 : 16;
		struct wgvk_use u = { ind->mem, src, src_off, 20, 0, WGVK_USE_INDIRECT, 0, -1 };
		u.size = cmd_size;
		wgvk_use_buffer(rp, &u);	/* (read by a copy before the pass, not in it) */
		struct wgvk_prepass cp = { .copy_src = src, .copy_src_off = src_off, .copy_dst = d.poly_ind,
					   .copy_dst_off = POLY_IND_DRAW, .copy_size = align(cmd_size, 4) };
		if (!d.nprepass)
			d.prepass0 = util_dynarray_num_elements(&rp->prepasses, struct wgvk_prepass);
		util_dynarray_append(&rp->prepasses, cp);
		d.nprepass++;
		static const struct wgvk_layout_remap no_remap;
		gs_prepass(rp, &d, &dev->gs_setup, &no_remap, &vk, dyn, indexed, a, 0, 0, 1, 1);
		gs_prepass(rp, &d, vcs, &rp->vs->remap, &vk, dyn, indexed, a, 0, 0, ~0u, POLY_IND_DISPATCH);
		gs_prepass(rp, &d, &g->part[WGVK_GS_COUNT], &rp->gs->remap, &vk, dyn, indexed, a, 0, 0, ~0u,
			   POLY_IND_DISPATCH + 12);
		gs_prepass(rp, &d, &g->part[WGVK_GS_PREFIX], &rp->gs->remap, &vk, dyn, indexed, a, 0, 0, 1, 1);
		gs_prepass(rp, &d, &g->part[WGVK_GS_PRE], &rp->gs->remap, &vk, dyn, indexed, a, 0, 0, 1, 1);
		gs_prepass(rp, &d, &g->part[WGVK_GS_MAIN], &rp->gs->remap, &vk, dyn, indexed, a, 0, 0, ~0u,
			   POLY_IND_DISPATCH + 12);
		dev->stats.gs_indirect_draws++;
	} else {
		/* the pre-passes: VS, count, pre, main (scene-run.c's order) */
		gs_prepass(rp, &d, vcs, &rp->vs->remap, &vk, dyn, indexed, a, base_vertex, base_instance,
			   DIV_ROUND_UP(count, 64), inst);
		gs_prepass(rp, &d, &g->part[WGVK_GS_COUNT], &rp->gs->remap, &vk, dyn, indexed, a, base_vertex, base_instance,
			   DIV_ROUND_UP(prims, 64), inst);
		gs_prepass(rp, &d, &g->part[WGVK_GS_PRE], &rp->gs->remap, &vk, dyn, indexed, a, base_vertex, base_instance, 1, 1);
		gs_prepass(rp, &d, &g->part[WGVK_GS_MAIN], &rp->gs->remap, &vk, dyn, indexed, a, base_vertex, base_instance,
			   DIV_ROUND_UP(prims, 64), inst);
	}

	/* the rast part: a vertex shader with no vertex buffers, drawn from libpoly's draw command */
	bool gs_indexed = poly_gs_indexed(g->shape);
	const struct wgvk_compiled_shader *rcs = &g->part[WGVK_GS_RAST];
	/* (the rast part stands in for the vertex shader: its hashes, its bindings) */
	struct wgvk_shader *gsh = rp->gs;
	gsh->layout_hash = gv->layout_hash;
	if (r->dummy && !r->samples)
		r->samples = dyn->ms.rasterization_samples;
	struct wgvk_key_inputs in = {
		.pdev = dev->pdev, .vs = gsh, .fs = rp->fs, .vcs = rcs, .fcs = fcs, .dyn = dyn, .color_count = r->ncolor,
		.samples = r->samples, .ds = r->ds_fmt, .dummy_color = r->dummy, .strip_index = gs_indexed ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Undefined,
		.topology = mesa_to_vk_list(g->mode), .no_vertex_input = true,
	};
	memcpy(in.color, r->color_fmt, sizeof(in.color));
	struct wgvk_pipeline_key key;
	wgvk_pipeline_key_build(&in, &key);
	struct wgvk_unfilterable uf;
	const struct wgvk_compiled_shader *both[2] = { rcs, fcs };
	wgvk_unfilterable(&rp->bind[0], both, 2, &uf);
	if (uf.n)
		key.unfilterable = XXH64(uf.e, uf.n * sizeof(uf.e[0]), uf.n);
	d.pipe = wgvk_rpipe_get(dev, &key, gsh, rcs, rp->fs, fcs, &uf);
	d.writes_depth = key.ds.depth_write;
	d.ngroups = d.pipe->ngroups;
	for (uint32_t gi = 0; gi < d.ngroups; gi++) {
		WGPUBindGroupEntry drv[24];
		unsigned nd = driver_entries(rp, &d, d.pipe->bgl[gi], &vk, rcs, dyn, indexed, a, base_vertex, base_instance, drv);
		d.bg[gi] = wgvk_replay_bind_group(rp, rp->bind[0].set[gi], d.pipe->bgl[gi], drv, nd);
		d.ndyn[gi] = wgvk_bind_group_dyn(d.bg[gi], rp->bind[0].dyn[gi], d.dyn[gi]);
		for (unsigned i = 0; i < d.bg[gi]->nuse; i++) {
			struct wgvk_use u = d.bg[gi]->use[i];
			if (u.dyn >= 0)
				u.off += d.dyn[gi][u.dyn];
			u.slot = gi;
			u.idx = i;
			add_use(rp, &d, &u);
		}
		for (unsigned i = 0; i < d.bg[gi]->ntuse; i++) {
			struct wgvk_tuse t = d.bg[gi]->tuse[i];
			t.slot = gi;
			util_dynarray_append(&rp->tuses, t);
			d.ntuse++;
		}
	}
	d.kind = gs_indexed ? WGVK_DRAW_INDEXED_INDIRECT : WGVK_DRAW_INDIRECT;
	d.ind = d.poly;
	d.ind_off = POLY_OFF_GP + offsetof(struct poly_geometry_params, draw);
	if (gs_indexed) {
		d.ib = d.poly;
		d.ib_off = 0;
		d.ib_size = d.poly_size;
		d.ib_fmt = WGPUIndexFormat_Uint32;
	}
	memcpy(d.blend, dyn->cb.blend_constants, sizeof(d.blend));
	d.stencil_ref = dyn->ds.stencil.front.reference;
	d.imm_size = align(MIN2(d.pipe->immediate_bytes, WGVK_MAX_IMMEDIATES), 4);
	memcpy(d.imm, rp->push, d.imm_size);
	uint16_t dpo = d.pipe->draw_params_offset;
	if (dpo != 0xffff && dpo + 12 <= WGVK_MAX_IMMEDIATES) {
		struct wgvk_draw_params dp = { base_vertex, base_instance, rp->draw_id };
		memcpy(d.imm + dpo, &dp, sizeof(dp));
		d.imm_size = MAX2(d.imm_size, dpo + 12u);
	}
	d.occl1 = rp->q.active + 1;
	util_dynarray_append(&rp->draws, d);
	dev->stats.draws++;
	dev->stats.gs_draws++;
}

void
wgvk_cmd_draw(struct wgvk_replay *rp, enum wgvk_draw_kind kind, const uint32_t a[5], struct wgvk_buffer *ind,
	      uint64_t ind_off)
{
	struct wgvk_device *dev = rp->dev;
	struct wgvk_rendering *r = &rp->r;
	if (!r->active) {
		wgvk_warn_once("a draw outside a render pass\n");
		return;
	}
	if (!rp->vs) {
		wgvk_warn_once("a draw without a vertex shader\n");
		return;
	}
	const struct vk_dynamic_graphics_state *dyn = &rp->cmd->vk.dynamic_graphics_state;
	if (dyn->rs.rasterizer_discard_enable)
		return;	/* nothing reaches the attachments; vertex stores are not in WebGPU */
	if ((kind == WGVK_DRAW || kind == WGVK_DRAW_INDEXED) && (!a[0] || !a[1]))
		return;
	if (rp->gs) {
		gs_draw(rp, kind, a, ind, ind_off);
		return;
	}

	/* the draw parameters as Vulkan gave them, before any index rewriting */
	bool vk_indexed = kind == WGVK_DRAW_INDEXED || kind == WGVK_DRAW_INDEXED_INDIRECT;
	int32_t dp_base_vertex = ind ? 0 : vk_indexed ? (int32_t)a[3] : (int32_t)a[2];
	uint32_t dp_base_instance = ind ? 0 : vk_indexed ? a[4] : a[3];
	uint32_t ra[5];
	memcpy(ra, a, sizeof(ra));
	a = ra;
	struct wgvk_index_override io;
	if (wgvk_index_rewrite(rp, &kind, ra, ind, ind_off, &io)) {
		if (io.empty)
			return;
		ind = NULL;
	}

	struct wgvk_draw d;
	memset(&d, 0, sizeof(d));
	d.kind = kind;
	uint8_t vpv;
	const struct wgvk_compiled_shader *fcs = fs_variant(rp, dyn, &d, &vpv);
	bool flip;
	viewport_scissor(rp, dyn, &d, &flip);

	bool indexed = kind == WGVK_DRAW_INDEXED || kind == WGVK_DRAW_INDEXED_INDIRECT;
	struct wgvk_vs_key vk;
	vs_key(rp, dyn, flip, indexed, fcs, vpv, &vk);
	if (vk.point_mode && ind) {
		wgvk_warn_once("an indirect draw of large points: drawn as 1-pixel points (T2)\n");
		vk.point_mode = 0;
		vk.point_coord_varying = 0;
		if (!vk.pull_mask)
			memset(&vk.vi, 0, sizeof(vk.vi));
	}
	const struct wgvk_compiled_shader *vcs = wgvk_shader_vs_variant(dev, rp->vs, &vk);
	if (r->dummy && !r->samples)
		r->samples = dyn->ms.rasterization_samples;
	struct wgvk_key_inputs in = {
		.pdev = dev->pdev, .vs = rp->vs, .fs = rp->fs, .vcs = vcs, .fcs = fcs, .dyn = dyn, .color_count = r->ncolor,
		.samples = r->samples, .ds = r->ds_fmt, .dummy_color = r->dummy,
		.strip_index = indexed ? (io.active ? WGPUIndexFormat_Uint32 : index_format(rp->ib.type))
				       : WGPUIndexFormat_Undefined,
		.topology = io.active ? (int)io.topology : -1,
	};
	memcpy(in.color, r->color_fmt, sizeof(in.color));
	struct wgvk_pipeline_key key;
	wgvk_pipeline_key_build(&in, &key);
	struct wgvk_unfilterable uf;
	const struct wgvk_compiled_shader *both[2] = { vcs, fcs };
	wgvk_unfilterable(&rp->bind[0], both, 2, &uf);
	if (uf.n)
		key.unfilterable = XXH64(uf.e, uf.n * sizeof(uf.e[0]), uf.n);
	if (key.cull_mode == WGPUCullMode_None && (dyn->rs.cull_mode & VK_CULL_MODE_FRONT_AND_BACK) ==
						     VK_CULL_MODE_FRONT_AND_BACK &&
	    (key.topology == WGPUPrimitiveTopology_TriangleList || key.topology == WGPUPrimitiveTopology_TriangleStrip))
		return;	/* every triangle culled */
	d.pipe = wgvk_rpipe_get(dev, &key, rp->vs, vcs, rp->fs, fcs, &uf);
	d.writes_depth = key.ds.depth_write;

	d.use0 = util_dynarray_num_elements(&rp->uses, struct wgvk_use);
	d.tuse0 = util_dynarray_num_elements(&rp->tuses, struct wgvk_tuse);
	d.ngroups = d.pipe->ngroups;
	for (uint32_t g = 0; g < d.ngroups; g++) {
		struct wgvk_descriptor_set *set = rp->bind[0].set[g];
		const struct wgvk_bgl *bgl = d.pipe->bgl[g];
		bool app_entries = bgl->n && bgl->entries[0].binding < WGVK_DRIVER_BINDING_BASE;
		if (!set && app_entries)
			wgvk_warn_once("a draw without the descriptor set %u its shaders use: dummies stand in\n", g);
		WGPUBindGroupEntry drv[24];
		unsigned nd = driver_entries(rp, &d, bgl, &vk, vcs, dyn, indexed, a, dp_base_vertex, dp_base_instance, drv);
		d.bg[g] = wgvk_replay_bind_group(rp, set, bgl, drv, nd);
		d.ndyn[g] = wgvk_bind_group_dyn(d.bg[g], rp->bind[0].dyn[g], d.dyn[g]);
		for (unsigned i = 0; i < d.bg[g]->nuse; i++) {
			struct wgvk_use u = d.bg[g]->use[i];
			if (u.dyn >= 0)
				u.off += d.dyn[g][u.dyn];
			u.slot = g;
			u.idx = i;
			add_use(rp, &d, &u);
		}
		for (unsigned i = 0; i < d.bg[g]->ntuse; i++) {
			struct wgvk_tuse t = d.bg[g]->tuse[i];
			t.slot = g;
			util_dynarray_append(&rp->tuses, t);
			d.ntuse++;
		}
	}
	d.nvb = key.vb_count;
	for (unsigned s = 0; s < key.vb_count; s++) {
		const struct wgvk_vb *vb = &rp->vb[key.vb[s].binding];
		if (!vb->buf || !vb->buf->mem) {
			d.vb[s].buf = wgvk_dummy_buffer(dev);	/* a null binding reads zeros */
			d.vb[s].off = 0;
			d.vb[s].size = 256;
			continue;
		}
		d.vb[s].buf = wgvk_buffer_wgpu(vb->buf, vb->off, &d.vb[s].off);
		d.vb[s].size = vb->size;
		if (d.vb[s].off % 4)
			wgvk_warn_once("vertex buffer offset not a multiple of 4 (T2/T3b: vertex pulling)\n");
		add_use(rp, &d, &(struct wgvk_use){ vb->buf->mem, d.vb[s].buf, d.vb[s].off, d.vb[s].size, 0, WGVK_USE_VB, s,
						    -1 });
	}
	if (io.has_prepass) {
		d.prepass0 = util_dynarray_num_elements(&rp->prepasses, struct wgvk_prepass);
		d.nprepass = 1;
		util_dynarray_append(&rp->prepasses, io.prepass);
	}
	if (io.active) {
		d.ib = io.buf;
		d.ib_off = 0;
		d.ib_size = io.size;
		d.ib_fmt = WGPUIndexFormat_Uint32;
		if (io.indirect) {
			d.ind = io.buf;
			d.ind_off = 0;
		}
	} else if (indexed) {
		if (!rp->ib.buf) {
			wgvk_warn_once("an indexed draw without an index buffer\n");
			rp->uses.size = d.use0 * sizeof(struct wgvk_use);
			return;
		}
		d.ib = wgvk_buffer_wgpu(rp->ib.buf, rp->ib.off, &d.ib_off);
		d.ib_size = rp->ib.size;
		d.ib_fmt = index_format(rp->ib.type);
		add_use(rp, &d, &(struct wgvk_use){ rp->ib.buf->mem, d.ib, d.ib_off, d.ib_size, 0, WGVK_USE_IB, 0, -1 });
	}
	if (ind) {
		d.ind = wgvk_buffer_wgpu(ind, ind_off, &d.ind_off);
		add_use(rp, &d, &(struct wgvk_use){ ind->mem, d.ind, d.ind_off, indexed ? 20 : 16, 0, WGVK_USE_INDIRECT, 0,
						    -1 });
	}
	memcpy(d.a, a, sizeof(d.a));
	if (vcs->legal.points_expanded) {
		/* 6 vertices per point, as triangles (INTERFACES 6.1): draw(6 count, instances, 6 first, firstInstance),
		 * first = firstIndex for indexed draws (the shader reads the index buffer at 936) */
		uint32_t count = a[0], inst = a[1], first = a[2], first_inst = indexed ? a[4] : a[3];
		d.kind = WGVK_DRAW;
		d.a[0] = 6 * count;
		d.a[1] = inst;
		d.a[2] = 6 * first;
		d.a[3] = first_inst;
		d.ib = NULL;
	}
	memcpy(d.blend, dyn->cb.blend_constants, sizeof(d.blend));
	d.stencil_ref = dyn->ds.stencil.front.reference;
	if (dyn->ds.stencil.front.reference != dyn->ds.stencil.back.reference)
		wgvk_warn_once("different front and back stencil references: WebGPU has one (front used)\n");
	d.imm_size = align(MIN2(d.pipe->immediate_bytes, WGVK_MAX_IMMEDIATES), 4);
	memcpy(d.imm, rp->push, d.imm_size);
	/* draw parameters (INTERFACES 6.1): BaseVertex, BaseInstance, DrawIndex after the push constants */
	uint16_t dpo = d.pipe->draw_params_offset;
	if (dpo != 0xffff && dpo + 12 <= WGVK_MAX_IMMEDIATES) {
		if (ind)
			wgvk_warn_once("draw parameters of an indirect draw need the arguments on the CPU or a driver "
				       "UBO copy (T2): zeros\n");
		struct wgvk_draw_params dp = {
			.base_vertex = dp_base_vertex,
			.base_instance = dp_base_instance,
			.draw_id = rp->draw_id,
		};
		memcpy(d.imm + dpo, &dp, sizeof(dp));
		d.imm_size = MAX2(d.imm_size, dpo + 12);
	}
	d.occl1 = rp->q.active + 1;
	util_dynarray_append(&rp->draws, d);
	dev->stats.draws++;
}

/* vkCmdClearAttachments: WebGPU clears only at a pass's start, so a quad per rectangle with a driver
 * pipeline that writes the clear value (wgvk_meta.c). */
void
wgvk_cmd_clear_attachments(struct wgvk_replay *rp, const struct vk_cmd_clear_attachments *c)
{
	struct wgvk_device *dev = rp->dev;
	struct wgvk_rendering *r = &rp->r;
	if (!r->active)
		return;
	for (uint32_t i = 0; i < c->attachment_count; i++) {
		const VkClearAttachment *ca = &c->attachments[i];
		uint32_t color_mask = 0;
		bool depth = ca->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT, stencil = ca->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT;
		if (ca->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
			if (ca->colorAttachment >= r->ncolor || !r->color[ca->colorAttachment].iv)
				continue;
			color_mask = 1u << ca->colorAttachment;
		}
		depth &= wgpu_has_depth(r->ds_fmt);
		stencil &= wgpu_has_stencil(r->ds_fmt);
		if (!color_mask && !depth && !stencil)
			continue;
		WGPURenderPipeline p = wgvk_meta_clear_pipeline(dev, r, color_mask, depth, stencil);
		if (!p)
			continue;
		for (uint32_t j = 0; j < c->rect_count; j++) {
			const VkClearRect *cr = &c->rects[j];
			struct wgvk_draw d;
			memset(&d, 0, sizeof(d));
			d.kind = WGVK_DRAW_META;
			d.meta = p;
			d.writes_depth = depth;
			int64_t x0 = CLAMP(cr->rect.offset.x, 0, r->width), y0 = CLAMP(cr->rect.offset.y, 0, r->height);
			int64_t x1 = CLAMP((int64_t)cr->rect.offset.x + cr->rect.extent.width, x0, r->width);
			int64_t y1 = CLAMP((int64_t)cr->rect.offset.y + cr->rect.extent.height, y0, r->height);
			if (x1 <= x0 || y1 <= y0)
				continue;
			d.vp[0] = x0;
			d.vp[1] = y0;
			d.vp[2] = x1 - x0;
			d.vp[3] = y1 - y0;
			d.vp[4] = 0;
			d.vp[5] = 1;
			d.sc[0] = x0;
			d.sc[1] = y0;
			d.sc[2] = x1 - x0;
			d.sc[3] = y1 - y0;
			d.a[0] = 3;
			d.a[1] = 1;
			d.stencil_ref = ca->clearValue.depthStencil.stencil;
			/* immediates: the colour's 16 bytes as written, then the depth */
			memcpy(d.imm, &ca->clearValue.color, 16);
			float z = CLAMP(ca->clearValue.depthStencil.depth, 0.0f, 1.0f);
			memcpy(d.imm + 16, &z, 4);
			d.imm_size = 32;
			d.use0 = util_dynarray_num_elements(&rp->uses, struct wgvk_use);
			d.tuse0 = util_dynarray_num_elements(&rp->tuses, struct wgvk_tuse);
			util_dynarray_append(&rp->draws, d);
		}
	}
}
