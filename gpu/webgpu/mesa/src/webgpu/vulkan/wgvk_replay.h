/*
 * wgvk command replay (T2), shared by wgvk_cmd.c (the loop, binding, compute,
 * memory traffic, submission), wgvk_render.c (render passes and draws) and
 * wgvk_copy.c (transfers, clears, blits, resolves).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WGVK_REPLAY_H
#define WGVK_REPLAY_H

#include <math.h>
#include "wgvk_private.h"

#include "vk_cmd_queue.h"

#define WGVK_MAX_DYN 32		/* dynamic offsets per set */
#define WGVK_MAX_SETS 4

/* One buffer range a command reads or writes; what WebGPU's usage scopes are about. */
struct wgvk_use {
	struct wgvk_device_memory *mem;	/* NULL: not Vulkan memory (driver buffers) */
	WGPUBuffer buf;
	uint64_t off, size;		/* in buf */
	uint8_t write;
	uint8_t slot;			/* 0..3: bind group; or WGVK_USE_* */
	uint16_t idx;			/* bind group entry, vertex buffer slot */
	int32_t dyn;			/* bind group entries: index in setBindGroup's dynamic offsets, or -1 */
};
enum { WGVK_USE_VB = 4, WGVK_USE_IB, WGVK_USE_INDIRECT, WGVK_USE_DRIVER };

/* One texture subresource range a command samples or writes. */
struct wgvk_tuse {
	WGPUTexture tex;
	uint16_t mip0, nmip, layer0, nlayer;
	uint8_t write;
	uint8_t slot;			/* bind group (draws) */
	uint16_t idx;			/* entry in the bind group layout */
	struct wgvk_image_view *iv;	/* the view bound (NULL: an attachment) */
	uint8_t ibuf;			/* the view's image buffer (T3b's IMAGE_BUFFER), not its texture: tex NULL */
};

/* A descriptor set's bind group for a bind group layout, with what it uses: T1 makes the WebGPU bind group
 * (wgvk_bind_group_get, cached), the replay lists the buffer ranges and textures behind its entries (uploads,
 * downloads, usage scopes, snapshots). */
struct wgvk_bg {
	const struct wgvk_bgl *bgl;
	struct wgvk_descriptor_set *set;	/* one set with this content (rebuilds read its entries) */
	WGPUBindGroup bg;		/* a reference the submission holds */
	struct wgvk_use *use;		/* buffer entries */
	unsigned nuse;
	struct wgvk_tuse *tuse;
	unsigned ntuse;
	uint32_t ndyn;
	uint16_t dyn_vk[WGVK_MAX_DYN];	/* per dynamic entry (WebGPU's order): Vulkan's dynamic offset index */
	WGPUBindGroupEntry *driver;	/* the driver's entries (900..), for rebuilds */
	unsigned ndriver;
};

/* The driver's per-submission memory: uniforms (900, 901), libpoly's parameters. CPU-written, sent with one
 * queue.writeBuffer per chunk before the submission. */
struct wgvk_ring_chunk {
	WGPUBuffer buf;
	uint8_t *cpu;
	uint64_t size, used;
};

struct wgvk_bound_sets {
	struct wgvk_descriptor_set *set[WGVK_MAX_SETS];
	uint32_t dyn[WGVK_MAX_SETS][WGVK_MAX_DYN];
};

struct wgvk_att {
	struct wgvk_image_view *iv, *resolve;
	VkAttachmentLoadOp load;
	VkAttachmentStoreOp store;
	VkClearValue clear;
	VkResolveModeFlagBits resolve_mode;
};

struct wgvk_rendering {
	bool active;
	VkRect2D area;
	uint32_t layers, view_mask, samples;
	uint32_t ncolor;
	struct wgvk_att color[WGVK_MAX_COLOR];
	struct wgvk_att depth, stencil;
	WGPUTextureFormat color_fmt[WGVK_MAX_COLOR], ds_fmt;
	uint32_t width, height;		/* of the attachments (mip level) */
	/* rendering without attachments (GL_ARB_framebuffer_no_attachments: image stores, occlusion): WebGPU
	 * passes need one, so a write-masked R8Unorm colour attachment at index dummy - 1, sized from the
	 * render area, stands in; its sample count is the first draw's rasterization samples */
	uint32_t dummy;
};

enum wgvk_draw_kind { WGVK_DRAW, WGVK_DRAW_INDEXED, WGVK_DRAW_INDIRECT, WGVK_DRAW_INDEXED_INDIRECT,
		      WGVK_DRAW_META /* a driver pipeline (clears inside a pass): meta, imm, vp, sc, stencil_ref, a[0] */ };

/* A draw as the pass will encode it; recorded while the pass is collected, encoded when it ends. */
struct wgvk_draw {
	uint8_t kind, nvb, ib_fmt, imm_size;
	struct wgvk_rpipe *pipe;
	WGPURenderPipeline meta;
	uint32_t ngroups;
	struct wgvk_bg *bg[WGVK_MAX_SETS];		/* in replay.bgs */
	WGPUBindGroup bg_snap[WGVK_MAX_SETS];		/* a rebuilt group with snapshot buffers, or NULL */
	uint32_t ndyn[WGVK_MAX_SETS];
	uint32_t dyn[WGVK_MAX_SETS][WGVK_MAX_DYN];
	struct { WGPUBuffer buf; uint64_t off, size; } vb[WGVK_MAX_VBS];
	WGPUBuffer ib;
	uint64_t ib_off, ib_size;
	float vp[6];
	uint32_t sc[4];
	float blend[4];
	uint32_t stencil_ref;
	uint8_t imm[WGVK_MAX_IMMEDIATES];
	uint32_t a[5];			/* counts and firsts, as the WebGPU call takes them */
	WGPUBuffer ind;
	uint64_t ind_off;
	uint32_t prepass0, nprepass;	/* in replay.prepasses */
	int32_t occl1;			/* 1 + the occlusion query accumulator it counts into; 0: none */
	WGPUBuffer poly;		/* geometry shader draws: libpoly's memory for this draw (binding 940) */
	uint64_t poly_size;
	WGPUBuffer poly_ind;		/* indirect GS draws: the command and the parts' dispatch counts (937) */
	uint8_t viewports;		/* gl_ViewportIndex / gl_Layer emulated: viewports in the driver uniforms */
	uint8_t early;			/* the fragment shader tests depth itself against early_view (938) */
	uint8_t writes_depth;		/* (pass segments: a pass with early tests binds depth read-only) */
	WGPUTextureView early_view;
	uint32_t pull_off[16];		/* pulled vertex buffers: bytes past the aligned binding offset */
	uint32_t ib_pull_off;
	uint32_t use0, nuse;		/* in replay.uses */
	uint32_t tuse0, ntuse;		/* in replay.tuses */
};

struct wgvk_range {
	struct wgvk_device_memory *mem;
	uint64_t start, end;		/* in the memory */
};

struct wgvk_vb {
	struct wgvk_buffer *buf;
	uint64_t off, size;
};

/* wgvk_query_cmd.c: queries in command buffers */
struct wgvk_qacc {
	struct wgvk_query_pool *pool;
	uint32_t query;
	int acc;
	bool live, ended;
};
struct wgvk_qts {
	WGPUQuerySet set;
	int acc;
};
struct wgvk_qrep {
	WGPUBuffer acc;			/* accumulators, 2 words each */
	uint32_t nacc;
	int active;			/* the accumulator of the occlusion query active now, or -1 */
	struct util_dynarray accs;	/* struct wgvk_qacc */
	struct util_dynarray pending_ts;	/* struct wgvk_qts: timestamps inside a render pass */
};
struct wgvk_replay {
	struct wgvk_device *dev;
	struct wgvk_cmd_buffer *cmd;
	struct wgvk_submission *sub;
	WGPUCommandEncoder enc;
	WGPUComputePassEncoder cpass;	/* open compute pass, or NULL */

	struct wgvk_shader *vs, *fs, *cs, *gs;
	struct wgvk_bound_sets bind[2];	/* 0: graphics, 1: compute */
	uint8_t push[256];
	struct wgvk_vb vb[MESA_VK_MAX_VERTEX_BINDINGS];
	struct { struct wgvk_buffer *buf; uint64_t off, size; VkIndexType type; } ib;

	struct wgvk_rendering r;
	uint32_t draw_id;		/* DrawIndex of the draw being recorded (multi-draws) */
	struct util_dynarray draws;	/* struct wgvk_draw */
	struct util_dynarray uses;	/* struct wgvk_use */
	struct util_dynarray tuses;	/* struct wgvk_tuse */
	struct util_dynarray written;	/* struct wgvk_range: host-visible ranges the GPU writes */
	struct util_dynarray uploaded;	/* struct wgvk_range: what this submission reads from windows */
	struct util_dynarray bgs;
	struct hash_table *bg_table;	/* WGPUBindGroup -> struct wgvk_bg */
	struct util_dynarray ring;	/* struct wgvk_ring_chunk */
	struct util_dynarray prepasses;	/* struct wgvk_prepass */
	struct wgvk_qrep q;	/* struct wgvk_bg *: made by this replay, freed with it */
	uint64_t serial;
};

/* wgvk_cmd.c */
void wgvk_upload(struct wgvk_replay *rp, struct wgvk_device_memory *mem, uint64_t start, uint64_t size);
void wgvk_wrote(struct wgvk_replay *rp, struct wgvk_device_memory *mem, uint64_t start, uint64_t size);
void wgvk_use_buffer(struct wgvk_replay *rp, const struct wgvk_use *u);
WGPUBuffer wgvk_temp_buffer(struct wgvk_replay *rp, uint64_t size, WGPUBufferUsage usage);
WGPUTexture wgvk_temp_texture(struct wgvk_replay *rp, const WGPUTextureDescriptor *td);
void wgvk_end_compute(struct wgvk_replay *rp);
struct wgvk_bg *wgvk_replay_bind_group(struct wgvk_replay *rp, struct wgvk_descriptor_set *set,
				       const struct wgvk_bgl *bgl, const WGPUBindGroupEntry *driver, unsigned ndriver);
/* size bytes of the driver ring, 256-aligned: the CPU pointer to fill, the GPUBuffer and offset to bind */
struct wgvk_heap_state {
	bool ready;
	int narenas;
	WGPUBuffer table;
	uint64_t table_off, table_size;
	struct wgvk_heap_arena arenas[WGVK_HEAP_ARENAS];
	struct wgvk_device_memory *mem[WGVK_HEAP_ARENAS];
};
void wgvk_heap_prepare(struct wgvk_replay *rp, const struct wgvk_compiled_shader *cs, const struct wgvk_layout_remap *remap,
		       const struct wgvk_bound_sets *bind, struct wgvk_heap_state *hs);
bool wgvk_heap_entry(const struct wgvk_heap_state *hs, uint32_t binding, WGPUBindGroupEntry *e, struct wgvk_use *use);
void *wgvk_ring_alloc(struct wgvk_replay *rp, uint64_t size, WGPUBuffer *buf, uint64_t *off);
unsigned wgvk_driver_entries_compute(struct wgvk_replay *rp, struct wgvk_shader *cs, const struct wgvk_bgl *bgl,
				     WGPUBindGroupEntry *out, struct wgvk_use *uses, unsigned *nuse);
uint32_t wgvk_bind_group_dyn(const struct wgvk_bg *bg, const uint32_t *vk_dyn, uint32_t *out);
WGPUBindGroup wgvk_bind_group_rebuild(struct wgvk_replay *rp, const struct wgvk_bg *bg,
				      const WGPUBuffer *bufs, const uint64_t *offs, const WGPUTextureView *views);

/* A compute dispatch a draw needs before its render pass (index rewriting; T4's geometry shader parts):
 * encoded ahead of the WebGPU pass the draw lands in (INTERFACES 6.2: hoisted). */
struct wgvk_prepass {
	WGPUComputePipeline pipe;
	uint32_t ngroups;
	WGPUBindGroup bg[WGVK_MAX_SETS];
	uint32_t ndyn[WGVK_MAX_SETS];
	uint32_t dyn[WGVK_MAX_SETS][WGVK_MAX_DYN];
	uint32_t imm_size;
	uint8_t imm[WGVK_MAX_IMMEDIATES];
	uint32_t groups[3];
	WGPUBuffer ind;			/* dispatchWorkgroupsIndirect from here (groups unused) */
	uint64_t ind_off;
	/* or no dispatch but a buffer copy (an indirect GS draw's command into its poly indirect buffer) */
	WGPUBuffer copy_src, copy_dst;
	uint64_t copy_src_off, copy_dst_off, copy_size;
};

/* wgvk_index.c: fans, provoking vertex "last" and 8-bit indices as a list index buffer made on the CPU, or by
 * a compute pre-pass when the indices are GPU-side */
struct wgvk_index_override {
	bool active, empty;		/* empty: nothing to draw */
	bool has_prepass;
	struct wgvk_prepass prepass;
	bool indirect;			/* draw from buf's words 0..4 (indexed indirect), indices from word 8 */
	WGPUBuffer buf;			/* 32-bit indices */
	uint64_t size;
	VkPrimitiveTopology topology;	/* the list topology drawn */
};
bool wgvk_index_rewrite(struct wgvk_replay *rp, enum wgvk_draw_kind *kind, uint32_t a[5], struct wgvk_buffer *ind,
			uint64_t ind_off, struct wgvk_index_override *o);
bool wgvk_cpu_bytes(struct wgvk_replay *rp, struct wgvk_buffer *buf, uint64_t off, uint64_t size, void *out);

void wgvk_q_reset(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t first, uint32_t count);
void wgvk_q_begin(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t q);
void wgvk_q_end(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t q);
void wgvk_q_timestamp(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t q);
void wgvk_q_copy(struct wgvk_replay *rp, struct wgvk_query_pool *pool, uint32_t first, uint32_t count,
		 struct wgvk_buffer *dst, uint64_t off, uint64_t stride, VkQueryResultFlags flags);
void wgvk_q_flush_slots(struct wgvk_replay *rp, WGPUQuerySet set, uint32_t n, const int32_t *map, bool set_mode);
WGPUQuerySet wgvk_q_occlusion_set(struct wgvk_replay *rp, uint32_t n);
void wgvk_q_after_pass(struct wgvk_replay *rp);
void wgvk_q_finish(struct wgvk_replay *rp);

/* wgvk_render.c */
void wgvk_unfilterable(const struct wgvk_bound_sets *bind, const struct wgvk_compiled_shader *const *cs, unsigned n,
		       struct wgvk_unfilterable *uf);
void wgvk_cmd_begin_rendering(struct wgvk_replay *rp, const VkRenderingInfo *info);
void wgvk_cmd_end_rendering(struct wgvk_replay *rp);
void wgvk_cmd_draw(struct wgvk_replay *rp, enum wgvk_draw_kind kind, const uint32_t a[5], struct wgvk_buffer *ind,
		   uint64_t ind_off);
void wgvk_cmd_clear_attachments(struct wgvk_replay *rp, const struct vk_cmd_clear_attachments *c);
bool wgvk_image_needs_sync(const struct wgvk_image *img, const struct wgvk_image_view *ibuf_view, int alias);
void wgvk_image_before(struct wgvk_replay *rp, struct wgvk_image *img, int alias, bool write);
void wgvk_image_ibuf_use(struct wgvk_replay *rp, struct wgvk_image_view *v);
void wgvk_tuses_before(struct wgvk_replay *rp, const struct wgvk_tuse *t, unsigned n);
bool wgvk_tuses_need_sync(const struct wgvk_tuse *t, unsigned n);
static inline bool
wgvk_color_is_finite(const float *c)
{
	return isfinite(c[0]) && isfinite(c[1]) && isfinite(c[2]) && isfinite(c[3]);
}

/* wgvk_copy.c */
void wgvk_cmd_copy_buffer(struct wgvk_replay *rp, const VkCopyBufferInfo2 *info);
void wgvk_cmd_copy_buffer_image(struct wgvk_replay *rp, struct wgvk_buffer *buf, struct wgvk_image *img,
				const VkBufferImageCopy2 *rg, bool to_buffer);
void wgvk_cmd_copy_image(struct wgvk_replay *rp, const VkCopyImageInfo2 *info);
void wgvk_cmd_blit_image(struct wgvk_replay *rp, const VkBlitImageInfo2 *info);
void wgvk_cmd_resolve_image(struct wgvk_replay *rp, const VkResolveImageInfo2 *info);
void wgvk_cmd_fill_buffer(struct wgvk_replay *rp, struct wgvk_buffer *buf, uint64_t off, uint64_t size, uint32_t data);
void wgvk_cmd_update_buffer(struct wgvk_replay *rp, struct wgvk_buffer *buf, uint64_t off, uint64_t size,
			    const void *data);
void wgvk_cmd_clear_color_image(struct wgvk_replay *rp, struct wgvk_image *img, const VkClearColorValue *color,
				uint32_t n, const VkImageSubresourceRange *ranges);
void wgvk_cmd_clear_ds_image(struct wgvk_replay *rp, struct wgvk_image *img, const VkClearDepthStencilValue *v,
			     uint32_t n, const VkImageSubresourceRange *ranges);

/* wgvk_meta.c: WebGPU pipelines of the driver's own (blits, clears inside passes) */
struct wgvk_meta_entry {
	uint64_t key;
	WGPURenderPipeline pipeline;
	WGPUBindGroupLayout bgl;
};
WGPURenderPipeline wgvk_meta_blit_pipeline(struct wgvk_device *dev, WGPUTextureFormat dst, WGPUTextureViewDimension sdim,
					   int sample_kind, WGPUBindGroupLayout *bgl);
WGPURenderPipeline wgvk_meta_depth_upload_pipeline(struct wgvk_device *dev, WGPUTextureFormat ds,
						   WGPUBindGroupLayout *bgl_out);
WGPUComputePipeline wgvk_meta_depth_readback_pipeline(struct wgvk_device *dev, WGPUBindGroupLayout *bgl_out);
WGPUComputePipeline wgvk_meta_tail_copy_pipeline(struct wgvk_device *dev, WGPUBindGroupLayout *bgl_out);
WGPURenderPipeline wgvk_meta_clear_pipeline(struct wgvk_device *dev, const struct wgvk_rendering *r,
					    uint32_t color_mask, bool depth, bool stencil);

#endif
