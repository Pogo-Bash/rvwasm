/*
 * wgvk: a Vulkan driver on WebGPU (webgpu.h), built on Mesa's common Vulkan
 * runtime. T0's skeleton, T1's objects (memory, images, descriptors,
 * layouts, formats) and T2's commands (replay, pipelines, render passes,
 * submission and syncs); see gpu/webgpu/INTERFACES.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WGVK_PRIVATE_H
#define WGVK_PRIVATE_H

#ifdef WGVK_WSI
#include "wsi_common.h"
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <webgpu/webgpu.h>

#include "util/hash_table.h"
#include "util/list.h"
#include "util/simple_mtx.h"
#include "util/u_dynarray.h"
#include "vk_alloc.h"
#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_device.h"
#include "vk_buffer_view.h"
#include "vk_device_memory.h"
#include "vk_image.h"
#include "vk_instance.h"
#include "vk_log.h"
#include "vk_physical_device.h"
#include "vk_query_pool.h"
#include "vk_queue.h"
#include "vk_sampler.h"
#include "vk_graphics_state.h"
#include "vk_shader.h"
#include "vk_sync.h"
#include "vk_util.h"

#include "vk_descriptor_set_layout.h"
#include "vk_pipeline_layout.h"

#include "wgvk.h"
#include "wgvk_bo.h"
#include "wgvk_compile.h"
#include "wgvk_format.h"
#include "wgvk_remap.h"
#include "wgvk_entrypoints.h"

#define WGVK_VENDOR_ID 0x10005	/* VK_VENDOR_ID_MESA */
#define WGVK_DEVICE_ID 0x7767	/* "wg" */

/* Memory types (INTERFACES.md 4.1). The heap sizes are nominal: WebGPU does not say. */
enum { WGVK_MEM_DEVICE_LOCAL = 0, WGVK_MEM_HOST = 1, WGVK_MEM_TYPE_COUNT = 2 };
#define WGVK_DEVICE_HEAP_SIZE (4ull << 30)
#define WGVK_HOST_HEAP_SIZE (2ull << 30)

struct wgvk_instance {
	struct vk_instance vk;
};
VK_DEFINE_HANDLE_CASTS(wgvk_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)

/* The WebGPU device's optional features wgvk looks at (wgvk_physical_device.c). */
struct wgvk_wgpu_features {
	bool core;			/* core-features-and-limits (not compatibility mode) */
	bool depth_clip_control, depth32_stencil8, timestamp, indirect_first_instance;
	bool bc, etc2, astc;
	bool shader_f16, subgroups, clip_distances, dual_source_blending, primitive_index;
	bool rg11b10_renderable, bgra8_storage, float32_filterable, float32_blendable;
	bool tier1, tier2, unorm16, swizzle;
};

struct wgvk_physical_device {
	struct vk_physical_device vk;
	WGPUInstance winst;
	WGPUAdapter adapter;
	WGPUDevice wdev;
	WGPULimits limits;
	struct wgvk_wgpu_features wf;
	struct wgvk_compile_caps compile_caps;	/* what wgvk_compile gets (INTERFACES.md 6) */
	struct wgvk_format formats[VK_FORMAT_ASTC_12x12_SRGB_BLOCK + 1];	/* wgvk_format_get() */
	char adapter_name[256];
	WGPUAdapterType adapter_type;	/* WGPUAdapterInfo.adapterType (Dawn native: the real one; the page's emdawnwebgpu: CPU or Unknown) */
	const struct vk_sync_type *sync_types[2];
#ifdef WGVK_WSI
	struct wsi_device wsi_device;	/* T7: wgvk_wsi.c (natively) */
#endif
};
VK_DEFINE_HANDLE_CASTS(wgvk_physical_device, vk.base, VkPhysicalDevice, VK_OBJECT_TYPE_PHYSICAL_DEVICE)

#ifdef WGVK_WSI
VkResult wgvk_wsi_init(struct wgvk_physical_device *pdev);
void wgvk_wsi_finish(struct wgvk_physical_device *pdev);
#endif

/* The queue counts submissions: serial n is the n-th vkQueueSubmit's WebGPU submission (wgvk_cmd.c,
 * wgvk_sync.c). */
struct wgvk_queue {
	struct vk_queue vk;
	uint64_t submitted;		/* the last serial handed out */
	uint64_t completed;		/* every submission up to this one has finished and run its completion */
	struct list_head pending;	/* struct wgvk_submission, in serial order */
};

/* What the command replay counts (WGVK_STATS=1 prints it at vkDestroyDevice; wgvk_device_stats()). */
struct wgvk_stats {
	uint64_t submits, cmd_buffers, render_passes, pass_segments, splits_buffer, snapshots, snapshot_bytes,
		feedback_loops, compute_passes, dispatches, draws, pipelines_created, pipelines_async, pipeline_hits,
		pipeline_waits, upload_bytes, uploads, download_bytes, downloads, bind_groups, skipped_cmds, index_rewrites, pipelines_preloaded, gs_draws, heap_draws, splits_feedback,
		texture_snapshots, dummy_attachments, nonfinite_clears, ibuf_copies, alias_copies, alias_bytes, gs_indirect_draws, early_depth_passes, splits_depth;
	double pipeline_wait_ms;
	double submit_ms;	/* time in vkQueueSubmit: replay, encoding, WebGPU submit (the CPU cost of T2) */
};

struct wgvk_device {
	struct vk_device vk;
	struct wgvk_physical_device *pdev;
	struct wgvk_queue queue;
	WGPUInstance winst;
	WGPUDevice wdev;
	WGPUQueue wqueue;
	uint64_t next_id;	/* wgvk_object_id() */
	struct wgvk_caches *caches;	/* bind group layouts, bind groups, dummy resources (wgvk_descriptor_set.c) */

	/* T2 (wgvk_cmd.c, wgvk_shader.c, wgvk_pipeline.c, wgvk_meta.c) */
	struct vk_device_dispatch_table replay;	/* recorded state commands are replayed through it */
	simple_mtx_t mutex;			/* the queue and the caches below: apps submit from any thread */
	struct hash_table *modules;		/* WGSL hash -> WGPUShaderModule */
	struct hash_table *pipelines;		/* struct wgvk_pipeline_key -> struct wgvk_rpipe */
	struct hash_table *meta;		/* driver pipelines by their keys */
	struct hash_table *cpipes;		/* struct wgvk_cpipe by key */
	WGPUSampler meta_nearest, meta_linear;
	void (*on_complete)(void *data, uint64_t serial);	/* front ends: submission `serial` completed */
	void (*cache_store)(void *data, const void *record, size_t size);	/* the persistent pipeline cache */
	void *cache_store_data;
	void *on_complete_data;
	void (*on_retire)(void *data);	/* wgvk_device_set_retire_hook: before a submission's downloads land */
	void *on_retire_data;
	/* T7 (wgvk_present.c): memory -> the image bound to it last (how a venus WSI image is found) */
	struct hash_table *mem_images;
	struct wgvk_compiled_shader gs_setup;	/* indirect GS draws' setup part (T4's), made at the first one */
	int gs_setup_state;			/* 0 not tried, 1 made, -1 failed */
	struct wgvk_stats stats;
};
VK_DEFINE_HANDLE_CASTS(wgvk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

/* ---- memory, buffers, buffer views (wgvk_memory.c; INTERFACES.md 4.1) */

struct wgvk_device_memory {
	struct vk_device_memory vk;
	/* bo.buffer is made on the first vkBindBufferMemory (wgvk_memory_buffer): memory only images are bound
	 * to never gets a GPUBuffer. bo.window: HOST (host below) for the host-visible type, GUEST for an
	 * imported venus blob (T5), NONE for device-local. */
	struct wgvk_bo bo;
	void *host;		/* the HOST window's storage, owned by the memory object unless imported */
	bool host_imported;	/* T7: VK_EXT_external_memory_host (Mesa's WSI hands its wl_shm/MIT-SHM memory) */
	uint64_t id;		/* unique per device, never reused (bind group cache keys) */
	/* T2: GPU writes not yet copied back into the window (bytes [dirty_start, dirty_end)) and the submission
	 * whose completion brings them back: uploads skip that range until then */
	uint64_t dirty_start, dirty_end, dirty_serial;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_device_memory, vk.base, VkDeviceMemory, VK_OBJECT_TYPE_DEVICE_MEMORY)

/* The GPUBuffer of a memory object, made with every buffer usage on first need; NULL if WebGPU cannot
 * (larger than maxBufferSize). */
WGPUBuffer wgvk_memory_buffer(struct wgvk_device *dev, struct wgvk_device_memory *mem);

struct wgvk_buffer {
	struct vk_buffer vk;
	struct wgvk_device_memory *mem;
	uint64_t offset;	/* in mem; the GPUBuffer range is mem->bo.offset + offset .. + vk.size */
	uint64_t id;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_buffer, vk.base, VkBuffer, VK_OBJECT_TYPE_BUFFER)

/* Where a buffer's range [offset, offset + size) is: the memory's bo narrowed to it. */
static inline struct wgvk_bo
wgvk_buffer_bo(const struct wgvk_buffer *buf, uint64_t offset, uint64_t size)
{
	struct wgvk_bo bo = buf->mem->bo;
	bo.offset += buf->offset + offset;
	bo.size = size;
	if (bo.window.kind == WGVK_WINDOW_HOST)
		bo.window.host = (char *)bo.window.host + buf->offset + offset;
	else if (bo.window.kind == WGVK_WINDOW_GUEST)
		bo.window.guest += buf->offset + offset;
	return bo;
}

struct wgvk_buffer_view {
	struct vk_buffer_view vk;
	const struct wgvk_format *fmt;	/* the texel format the shader decodes (T3b) */
	uint64_t id;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_buffer_view, vk.base, VkBufferView, VK_OBJECT_TYPE_BUFFER_VIEW)

/* ---- images, image views, samplers (wgvk_image.c, wgvk_sampler.c) */

struct wgvk_image_level {
	uint64_t offset;	/* linear layout in the bound memory (also the nominal size of optimal images) */
	uint64_t row_pitch;	/* a multiple of 256: a buffer <-> texture copy goes straight */
	uint64_t layer_pitch;	/* one array layer or 3D slice */
	uint64_t size;
};

struct wgvk_image {
	struct vk_image vk;
	WGPUTexture tex;		/* made at creation; image memory is nominal unless the image is linear */
	const struct wgvk_format *fmt;
	WGPUTextureFormat wformat;	/* fmt->wf */
	WGPUTextureDimension wdim;	/* 1D images are 2D textures one texel high */
	uint32_t wg_layers;		/* depthOrArrayLayers */
	WGPUTextureUsage wusage;
	uint32_t bpp;			/* bytes per block */
	struct wgvk_device_memory *mem;	/* bound memory; for linear images it holds the texels (T2 syncs) */
	uint64_t mem_offset;
	struct wgvk_image_level level[16];
	uint64_t size;
	uint64_t id;
	/* Views in formats WebGPU cannot reinterpret this texture as (only sRGB <-> not is native): a texture
	 * per such format, the same size; `current` says which holds the latest texels (-1: tex). T2 calls
	 * wgvk_image_make_current before a command touches the image through a view (or directly: -1). */
	struct wgvk_image_alias {
		const struct wgvk_format *fmt;
		WGPUTexture tex;
		WGPUTextureUsage usage;
	} alias[4];
	uint8_t n_alias;
	int8_t current;
	/* (T2) Image buffers: a storage image a shader uses atomically is a storage buffer per view (T3b's
	 * IMAGE_BUFFER: {width, height, layers, 0} then the texels of the view's level). ib_owner is the view
	 * whose buffer holds the latest texels (NULL: the texture does); ib_gen counts changes of the texture,
	 * a view's buffer is current while its ibuf_gen equals it. wgvk_image_before keeps them coherent. */
	struct wgvk_image_view *ib_owner;
	uint64_t ib_gen;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)

/* Make texture `alias` (-1: the image's own) hold the image's latest texels, copying them (whole image,
 * through a staging buffer) on `enc` if another one has them. Encoder-level: outside passes. */
void wgvk_image_make_current(struct wgvk_device *dev, WGPUCommandEncoder enc, struct wgvk_image *img, int alias);

/* T7 (wgvk_present.c): which image is bound to which memory, for wgvk_memory_image (wgvk.h) */
void wgvk_present_bind(struct wgvk_device *dev, struct wgvk_device_memory *mem, struct wgvk_image *img);
void wgvk_present_forget(struct wgvk_device *dev, struct wgvk_image *img, struct wgvk_device_memory *mem);
void wgvk_present_finish(struct wgvk_device *dev);


struct wgvk_image_view {
	struct vk_image_view vk;
	const struct wgvk_format *fmt;	/* of the view format */
	WGPUTextureFormat wformat;	/* what the views are made with (may differ from fmt->wf: see emulated) */
	WGPUTextureViewDimension wdim;	/* of the sampled view */
	WGPUTextureAspect aspect;
	/* One WebGPU view per use (NULL when the image cannot be used so):
	 *   sampled: TextureBinding, with the component swizzle (format + view mapping);
	 *   storage: StorageBinding, base level only, cube as 2D array;
	 *   attachment: RenderAttachment, base level and layer (a 3D image: the 3D view and depth_slice). */
	WGPUTextureView sampled, storage, attachment;
	WGPUTextureViewDimension sdim;	/* of the storage view */
	/* the 2D <-> 2D-array twins of sampled and storage, made when a shader declares the other (GL binds a
	 * one-layer view of either kind to either) */
	WGPUTextureView twin[2];
	WGPUBuffer ibuf;		/* (T2) the image buffer of this view (see wgvk_image), or NULL */
	WGPUTextureView depth_ro;	/* (T2) depth aspect, one level and layer: early depth tests read it (938) */
	uint64_t ibuf_size, ibuf_gen;
	uint32_t depth_slice;		/* WGPU_DEPTH_SLICE_UNDEFINED unless a 3D attachment */
	bool emulated;			/* the view format is not possible at all (logged; views of the image's format) */
	int8_t alias;			/* -1: views of image->tex; else views of image->alias[alias].tex */
	uint64_t id;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_image_view, vk.base, VkImageView, VK_OBJECT_TYPE_IMAGE_VIEW)
WGPUTextureView wgvk_image_view_for_dim(struct wgvk_image_view *v, bool storage, WGPUTextureViewDimension want);
WGPUBuffer wgvk_image_view_ibuf(struct wgvk_device *dev, struct wgvk_image_view *v);
void wgvk_image_view_ibuf_extent(const struct wgvk_image_view *v, uint32_t *w, uint32_t *h, uint32_t *layers);
void wgvk_image_ibuf_flush_now(struct wgvk_device *dev, struct wgvk_image *img);

/* What a sampler asks that WebGPU samplers cannot do: the shader does it (T3b, a variant key). */
enum {
	WGVK_SAMPLER_BORDER = 1 << 0,		/* clamp to border (color in vk.border_color_value) */
	WGVK_SAMPLER_MIRROR_CLAMP = 1 << 1,	/* mirror once, then clamp to edge */
	WGVK_SAMPLER_LOD_BIAS = 1 << 2,		/* vk.mip_lod_bias */
	WGVK_SAMPLER_UNNORMALIZED = 1 << 3,	/* texel coordinates */
	WGVK_SAMPLER_REDUCTION = 1 << 4,	/* min/max reduction */
};

struct wgvk_sampler {
	struct vk_sampler vk;
	WGPUSampler sampler;		/* as asked, as far as WebGPU goes */
	WGPUSampler nonfiltering;	/* nearest everything, for unfilterable textures (made on first use) */
	bool filtering, comparison;
	uint32_t emulate;		/* WGVK_SAMPLER_* */
	WGPUSamplerDescriptor desc;
	uint64_t id;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_sampler, vk.base, VkSampler, VK_OBJECT_TYPE_SAMPLER)

WGPUSampler wgvk_sampler_nonfiltering(struct wgvk_device *dev, struct wgvk_sampler *s);

/* ---- descriptor set layouts and pipeline layouts (wgvk_descriptor.c; INTERFACES.md 4.2) */

struct wgvk_descriptor_set_layout {
	struct vk_descriptor_set_layout vk;
	uint32_t wg_count;			/* WebGPU bindings 0..wg_count-1 */
	uint32_t dynamic_count;			/* dynamic UBOs + SSBOs (array elements) */
	unsigned n_remap;
	struct wgvk_binding_remap *remap;	/* ordered by binding; set = group = 0 here */
	/* per remap entry: inline uniform blocks' byte offset in the set's inline storage, immutable samplers */
	uint32_t *inline_offset;
	struct wgvk_sampler ***immutable;
	uint32_t inline_size;
	/* per-stage use (MESA_SHADER_* index): uniform buffers, storage buffers, textures, samplers, storage textures */
	uint16_t stage_ubos[6], stage_ssbos[6], stage_textures[6], stage_samplers[6], stage_images[6];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_descriptor_set_layout, vk.base, VkDescriptorSetLayout,
			       VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)

/* ---- descriptor pools, sets, bind groups (wgvk_descriptor_set.c; INTERFACES.md 4.3) */

enum wgvk_descriptor_kind {
	WGVK_DESC_NONE,		/* not written: a dummy resource stands in (nullDescriptor) */
	WGVK_DESC_BUFFER,	/* uniform / storage buffer (dynamic ones too) */
	WGVK_DESC_TEXEL,	/* texel buffer: a storage buffer range + the view's format */
	WGVK_DESC_INLINE,	/* inline uniform block: the set's own buffer */
	WGVK_DESC_IMAGE,	/* sampled, storage or input attachment image (the texture half of a combined one) */
	WGVK_DESC_SAMPLER,	/* sampler (the sampler half of a combined one) */
};

/* One WebGPU binding's worth of a descriptor set. Pointers to the Vulkan objects (T2 reads the bos and
 * views to track uses: upload on use, usage scopes); ids for cache keys. */
struct wgvk_descriptor {
	uint8_t kind;			/* enum wgvk_descriptor_kind */
	VkDescriptorType type;
	uint64_t id;			/* the object's wgvk id (0: none) */
	union {
		struct { struct wgvk_buffer *buffer; uint64_t offset, range; } buf;
		struct { struct wgvk_buffer_view *view; } texel;
		struct { uint32_t offset, size; } inl;
		struct { struct wgvk_image_view *view; VkImageLayout layout; } img;
		struct { struct wgvk_sampler *sampler; } smp;
	};
};

struct wgvk_descriptor_set {
	struct vk_object_base base;
	struct wgvk_descriptor_set_layout *layout;	/* referenced */
	struct wgvk_descriptor_pool *pool;		/* NULL for a push descriptor set */
	struct list_head link;
	uint32_t count;					/* layout->wg_count */
	struct wgvk_descriptor *desc;			/* indexed by WebGPU binding */
	uint64_t id, generation;			/* generation moves on every write */
	uint8_t *inline_data;				/* layout->inline_size bytes */
	WGPUBuffer inline_buffer;			/* made, and written, when a bind group needs it */
	bool inline_dirty;
	uint64_t inline_generation;			/* moves on writes to inline blocks only */
	/* the last bind group made for this set (cache in front of the device's) */
	uint64_t last_bgl, last_generation, last_driver;
	WGPUBindGroup last_bg;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_descriptor_set, base, VkDescriptorSet, VK_OBJECT_TYPE_DESCRIPTOR_SET)

struct wgvk_descriptor_pool {
	struct vk_object_base base;
	struct list_head sets;
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_descriptor_pool, base, VkDescriptorPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL)

/* Writes into a set (vkUpdateDescriptorSets, templates, and T2's push descriptors on a set of its own). */
void wgvk_descriptor_set_write(struct wgvk_device *dev, struct wgvk_descriptor_set *set, const VkWriteDescriptorSet *w);
struct vk_descriptor_update_template;
void wgvk_descriptor_set_write_template(struct wgvk_device *dev, struct wgvk_descriptor_set *set,
					const struct vk_descriptor_update_template *t, const void *data);
/* A set outside any pool, for push descriptors (T2 keeps one per command buffer and set index). */
struct wgvk_descriptor_set *wgvk_descriptor_set_create(struct wgvk_device *dev, struct wgvk_descriptor_set_layout *l);
void wgvk_descriptor_set_destroy(struct wgvk_device *dev, struct wgvk_descriptor_set *set);

/* A WebGPU bind group layout, cached by content in the device. T2 makes the entries from the pipeline's
 * compiled shaders (wgvk_bgl_entries_from_shaders) plus its driver bindings. */
struct wgvk_bgl {
	WGPUBindGroupLayout layout;
	uint64_t id;
	unsigned n;
	WGPUBindGroupLayoutEntry *entries;	/* ordered by binding */
};
struct wgvk_bgl *wgvk_bgl_get(struct wgvk_device *dev, const WGPUBindGroupLayoutEntry *entries, unsigned n);

/* The entries of `group` that shaders declare (merged over stages: visibility ORed), with dynamic offsets
 * where the remap says so. Returns the count written to out (at most max). */
unsigned wgvk_bgl_entries_from_shaders(struct wgvk_device *dev, const struct wgvk_layout_remap *remap,
				       const struct wgvk_compiled_shader *const *shaders, const WGPUShaderStage *stages,
				       unsigned n_shaders, unsigned group, WGPUBindGroupLayoutEntry *out, unsigned max);

/* The bind group of `set` (may be NULL: a driver-only group) for `bgl`: every entry below
 * WGVK_DRIVER_BINDING_BASE from the set's descriptors (dummies for unwritten ones, the non-filtering twin
 * of a sampler where the layout says non-filtering), the driver entries from `driver` (n_driver, T2's,
 * ordered by binding). Cached by content; the reference is borrowed: use it while encoding. */
WGPUBindGroup wgvk_bind_group_get(struct wgvk_device *dev, struct wgvk_descriptor_set *set, const struct wgvk_bgl *bgl,
				  const WGPUBindGroupEntry *driver, unsigned n_driver);
/* (T2) the entries of that bind group, bgl->n of them, not cached: for rebuilds with snapshot buffers */
void wgvk_bind_group_entries(struct wgvk_device *dev, struct wgvk_descriptor_set *set, const struct wgvk_bgl *bgl,
			     const WGPUBindGroupEntry *driver, unsigned n_driver, WGPUBindGroupEntry *out);

/* ---- query pools (wgvk_query.c; INTERFACES.md 4.6) */
struct wgvk_query_pool {
	struct vk_query_pool vk;
	WGPUQuerySet set;		/* occlusion, timestamp; NULL for faked types */
	WGPUBuffer resolve;		/* QueryResolve | CopySrc, 8 bytes per query */
	unsigned values;		/* 64-bit values per query */
	uint64_t *results;		/* what vkGetQueryPoolResults returns (T2 fills at completion) */
	uint8_t *available;
	uint64_t *serial;		/* the submission that writes each query (T2 sets it); 0: none */
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_query_pool, vk.base, VkQueryPool, VK_OBJECT_TYPE_QUERY_POOL)
void wgvk_query_pool_reset(struct wgvk_query_pool *pool, uint32_t first, uint32_t count);
/* T2, at the completion of the submission that wrote it: values (pool->values of them; NULL: keep zeros) */
void wgvk_query_pool_complete(struct wgvk_query_pool *pool, uint32_t query, const uint64_t *values);

/* ---- the buffer heap (wgvk_heap.c; INTERFACES.md 4.4) */
#define WGVK_HEAP_ARENAS 2	/* compile caps heap_arenas: driver bindings 910, 911 */
struct wgvk_heap_arena {
	WGPUBuffer buffer;	/* bind at WGVK_DRIVER_BINDING_HEAP + a as a storage buffer */
	uint64_t offset, size;
};
/* The heap table (4 words per slot, for driver binding 902) and the arenas for one stage's heap slots
 * (slots[i]: the descriptor of the shader's info.lowered entry with heap_slot i; NULL = unwritten). Returns
 * the arenas the slots use; the rest are filled with a dummy. */
int wgvk_heap_build(struct wgvk_device *dev, const struct wgvk_descriptor *const *slots, unsigned n, uint32_t *table,
		    struct wgvk_heap_arena arenas[WGVK_HEAP_ARENAS]);
WGPUBuffer wgvk_dummy_buffer(struct wgvk_device *dev);

struct wgvk_pipeline_layout {
	struct vk_pipeline_layout vk;
	struct wgvk_layout_remap remap;		/* key.layout for wgvk_compile */
	struct wgvk_binding_remap *b;		/* remap.b's storage */
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wgvk_pipeline_layout, vk.base, VkPipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT)

void wgvk_caches_init(struct wgvk_device *dev);
void wgvk_caches_finish(struct wgvk_device *dev);

/* The remap of sets set_layouts[0..n-1] (NULL entries allowed) with push constants of push_bytes:
 * fills out->b (caller frees with free()). The runtime's shader path (vk_shader_compile_info) has set
 * layouts and push ranges but no pipeline layout, so this takes those. */
VkResult wgvk_layout_remap_build(struct wgvk_physical_device *pdev, struct vk_descriptor_set_layout *const *set_layouts,
				 unsigned n, uint32_t push_bytes, struct wgvk_layout_remap *out);

struct wgvk_replay;

struct wgvk_cmd_buffer {
	struct vk_command_buffer vk;
	struct wgvk_replay *rp;			/* while the queue replays it */
	struct vk_vertex_input_state dyn_vi;	/* storage for vk.dynamic_graphics_state */
	struct vk_sample_locations_state dyn_sl;
};
VK_DEFINE_HANDLE_CASTS(wgvk_cmd_buffer, vk.base, VkCommandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER)

/* ---- submissions and syncs (T2: wgvk_cmd.c submits, wgvk_sync.c completes; INTERFACES.md 4.6, 5)
 *
 * vkQueueSubmit replays its command buffers into one WebGPU submission with a serial; nothing waits there.
 * A submission completes when WebGPU says its work is done and the MAP_READ copies of what the GPU wrote
 * into host-visible memory are mapped: then, in serial order, the copies go into the windows, the syncs it
 * signals move (each sync keeps its signals pending on serials), the queries it wrote get their results,
 * and the front end's hook runs. Waiting on a sync waits for the futures of the submission that satisfies
 * it (wgvk_wait). One queue, so GPU-side waits are no-ops (WebGPU orders a queue's work). */
struct wgvk_submission {
	struct list_head link;
	uint64_t serial;
	WGPUFuture work_done;
	unsigned callbacks, expected;	/* complete when every callback came */
	bool failed;
	struct util_dynarray downloads;	/* struct wgvk_download */
	struct util_dynarray buffers;	/* WGPUBuffer: temporaries to release */
	struct util_dynarray textures;	/* WGPUTexture: temporaries to destroy and release */
	struct util_dynarray groups;	/* WGPUBindGroup: references held while the replay used them */
	struct util_dynarray sets;	/* struct wgvk_descriptor_set *: push descriptor sets */
	/* queries (wgvk_query_cmd.c) */
	struct util_dynarray qdone;	/* struct wgvk_qdone: queries ended in this submission */
	struct util_dynarray qsets;	/* WGPUQuerySet */
	WGPUBuffer qstaging;		/* the accumulators, mapped at completion */
	uint64_t qstaging_size;
	WGPUFuture qfuture;
};

struct wgvk_qdone {
	struct wgvk_query_pool *pool;
	uint32_t query;
	int acc;			/* -1: zeros (faked types) */
};
void wgvk_q_retire(struct wgvk_device *dev, struct wgvk_submission *sub);

struct wgvk_download {
	struct wgvk_device_memory *mem;
	uint64_t start, size;
	WGPUBuffer staging;
	WGPUFuture future;
};

struct wgvk_sync_signal {
	uint64_t value, serial;
};

struct wgvk_sync {
	struct vk_sync base;
	uint64_t value;			/* what the CPU sees */
	struct wgvk_sync_signal *pending;	/* signals waiting for their submission */
	unsigned n_pending, max_pending;
	/* A front end's completion word (T5's venus syncs in shared memory): called whenever value moves. */
	void (*on_value)(void *data, uint64_t value);
	void *on_value_data;
};
extern const struct vk_sync_type wgvk_sync_type;

/* Retire what WebGPU has finished; with serial != 0, wait (until abs_timeout_ns, os_time_get_nano's clock)
 * for that submission to complete. */
VkResult wgvk_queue_poll(struct wgvk_device *dev, uint64_t serial, uint64_t abs_timeout_ns);
static inline VkResult
wgvk_queue_wait_serial(struct wgvk_device *dev, uint64_t serial, uint64_t abs_timeout_ns)
{
	return wgvk_queue_poll(dev, serial, abs_timeout_ns);
}
void wgvk_sync_signal_at(struct wgvk_device *dev, struct vk_sync *sync, uint64_t value, uint64_t serial);
void wgvk_sync_complete(struct wgvk_device *dev, struct wgvk_sync *s);

extern const struct vk_command_buffer_ops wgvk_cmd_buffer_ops;

/* wgvk_wgpu.c: the WebGPU device and waiting (INTERFACES.md 1, 2). */
VkResult wgvk_wgpu_get(WGPUInstance *inst, WGPUAdapter *adapter, WGPUDevice *dev);
VkResult wgvk_wait(struct wgvk_device *dev, const WGPUFuture *f, unsigned n, uint64_t timeout_ns);
bool wgvk_wgpu_initializing(void);


/* wgvk_p4.c (generated) */
void wgvk_p4_extensions(struct vk_device_extension_table *e);
void wgvk_p4_features(struct vk_features *f);
void wgvk_p4_properties(struct vk_properties *p);

VkResult wgvk_enumerate_physical_devices(struct vk_instance *instance);
void wgvk_physical_device_destroy(struct vk_physical_device *pdev);
VkResult wgvk_queue_submit(struct vk_queue *queue, struct vk_queue_submit *submit);

/* ---- T2: shaders (wgvk_shader.c) */

struct wgvk_unfilterable;

/* What a vertex shader's WGSL depends on at a draw (zeroed, then filled: compared as bytes). */
struct wgvk_vs_key {
	uint8_t fs_varyings[32];	/* the fragment shader's inputs (INTERFACES 6) */
	uint8_t keep_clip_y;		/* a negative-height viewport */
	uint8_t z_minus_one;		/* depth clip control's [-1, 1] */
	uint8_t point_mode;		/* enum wgvk_point_expand */
	uint8_t point_coord_varying;	/* the fragment shader's point coord location + 1 */
	uint16_t pull_mask;		/* vertex buffer bindings fetched in the shader */
	uint8_t vs_before_gs;		/* run as compute before a geometry shader (T4, INTERFACES 6.2) */
	uint8_t viewport_index_varying;	/* gl_ViewportIndex / gl_Layer emulated (INTERFACES 6.6): location + 1 */
	struct wgvk_vertex_input vi;	/* with pull_mask or point_mode; zero otherwise */
};

struct wgvk_vs_variant {
	struct wgvk_vs_variant *next;
	struct wgvk_vs_key key;
	struct wgvk_compiled_shader cs;
};

/* A geometry shader as libpoly's parts (T4), per what its rast part depends on at a draw. */
struct wgvk_gs_key {
	uint8_t fs_varyings[32];
	uint8_t keep_clip_y, z_minus_one, viewport_index_varying, pad;
};

/* A fragment shader's variants (INTERFACES 6.1, 6.6): viewport / layer emulation, the early depth test */
struct wgvk_fs_key {
	uint8_t viewports, early_depth, pad[2];
	uint8_t out_type[8];		/* colour output types: 0 own, 1 f32, 2 i32, 3 u32 (wgvk_shader_key.fs_out_type) */
};
struct wgvk_fs_variant {
	struct wgvk_fs_variant *next;
	struct wgvk_fs_key key;
	struct wgvk_compiled_shader cs;
};
struct wgvk_gs_variant {
	struct wgvk_gs_variant *next;
	struct wgvk_gs_key key;
	struct wgvk_gs_program prog;
	bool failed;
	uint64_t layout_hash;		/* of the rast part */
};

struct wgvk_shader {
	struct vk_shader vk;
	struct wgvk_compiled_shader cs;		/* fragment, compute; vertex: the variant it was compiled for */
	struct wgvk_layout_remap remap;		/* owns remap.b */
	uint64_t layout_hash;			/* of what the WGSL does not say about its layout (dynamic offsets) */
	/* vertex shaders: the NIR again, for variants (fs_varyings and keep_clip_y depend on the draw) */
	void *nir_blob;
	size_t nir_blob_size;
	struct wgvk_vs_variant *variants;
	struct wgvk_gs_variant *gs_variants;	/* geometry shaders */
	uint8_t key_fs_varyings[32];
	uint8_t key_keep_clip_y;
	uint32_t vs_inputs;			/* vertex: attribute locations declared */
	uint32_t vs_float_inputs;		/* vertex: those declared with a float type */
	bool vs_writes_psiz;			/* vertex: gl_PointSize (points need expanding) */
	bool writes_viewport_index;		/* vertex, geometry: gl_ViewportIndex or gl_Layer */
	/* fragment */
	struct wgvk_fs_variant *fs_variants;
	uint32_t fs_outputs;			/* colour locations written */
	uint32_t fs_float_outputs;		/* ... of which declared with a float type */
	bool fs_early;				/* early_fragment_tests and memory writes: emulated (key.early_depth) */
	/* compute */
	WGPUComputePipeline cpipe;
	struct wgvk_bgl *cbgl[4];
	uint32_t cgroups;
};

WGPUShaderModule wgvk_module_get(struct wgvk_device *dev, const struct wgvk_compiled_shader *cs);
const struct wgvk_compiled_shader *wgvk_shader_vs_variant(struct wgvk_device *dev, struct wgvk_shader *vs,
							  const struct wgvk_vs_key *key);
/* The bind group layouts of shaders that run together (a pipeline's stages); returns the group count. */
unsigned wgvk_bgls_for_shaders(struct wgvk_device *dev, struct wgvk_shader *const *sh,
			       const struct wgvk_compiled_shader *const *cs, unsigned n,
			       const struct wgvk_unfilterable *uf, struct wgvk_bgl *out[4]);
VkResult wgvk_compute_pipeline_get(struct wgvk_device *dev, struct wgvk_shader *cs);
const struct wgvk_gs_variant *wgvk_shader_gs_variant(struct wgvk_device *dev, struct wgvk_shader *gs,
						     const struct wgvk_gs_key *key);
const struct wgvk_compiled_shader *wgvk_shader_fs_variant(struct wgvk_device *dev, struct wgvk_shader *fs,
							  const struct wgvk_fs_key *key);
/* A compute pipeline from a compiled shader and a layout (the geometry shader's compute parts, the vertex
 * shader before it), cached by WGSL and layout. */
/* Texture bindings that hold a texture WebGPU cannot filter (depth, unfilterable float formats) at a draw, and
 * the samplers used with them: the layout says UnfilterableFloat / NonFiltering for those (a pipeline variant,
 * the same WGSL). */
struct wgvk_unfilterable {
	unsigned n;
	struct wgvk_uf_entry { uint16_t group, binding; } e[32];
};
struct wgvk_cpipe {
	uint64_t key;
	WGPUComputePipeline pipe;
	struct wgvk_bgl *bgl[4];
	uint32_t ngroups, immediate_bytes;
	uint16_t draw_params_offset;
};
const struct wgvk_cpipe *wgvk_cpipe_get(struct wgvk_device *dev, const struct wgvk_compiled_shader *cs,
					const struct wgvk_layout_remap *remap, const struct wgvk_unfilterable *uf);
extern const struct vk_device_shader_ops wgvk_device_shader_ops;

/* ---- T2: render pipelines (wgvk_pipeline.c): everything WebGPU bakes into a GPURenderPipeline
 * (INTERFACES.md 5). Zeroed, then filled: hashed and compared as bytes. */

#define WGVK_MAX_VBS 8		/* WebGPU's maxVertexBuffers */
#define WGVK_MAX_ATTRS 16	/* WebGPU's maxVertexAttributes */
#define WGVK_MAX_COLOR 8
#define WGVK_MAX_IMMEDIATES 64	/* bytes; Chrome's maxImmediateSize */

struct wgvk_pipeline_key {
	uint64_t vs, fs;			/* WGSL hashes of the variants */
	uint64_t layout;			/* what of the bind group layouts the WGSL does not say */
	uint64_t unfilterable;			/* hash of struct wgvk_unfilterable, 0: none */
	uint8_t topology, strip_index_format, front_face, cull_mode;
	uint8_t unclipped_depth, sample_count, alpha_to_coverage, color_count;
	uint32_t sample_mask;
	uint8_t vb_count, attr_count, pad[2];
	struct { uint32_t stride; uint8_t step_mode, binding, pad[2]; } vb[WGVK_MAX_VBS];	/* binding: Vulkan's */
	struct { uint32_t offset; uint8_t format, location, slot, pad; } attr[WGVK_MAX_ATTRS];
	struct {
		uint32_t format;
		uint8_t write_mask, blend_enable, color_op, color_src, color_dst, alpha_op, alpha_src, alpha_dst;
	} color[WGVK_MAX_COLOR];
	struct {
		uint32_t format;
		uint8_t depth_write, depth_compare, stencil_read_mask, stencil_write_mask;
		uint8_t stencil_front[4], stencil_back[4];	/* compare, fail, depth_fail, pass */
		int32_t depth_bias;
		float depth_bias_slope, depth_bias_clamp;
	} ds;
};

struct wgvk_rpipe {
	struct wgvk_pipeline_key key;
	WGPURenderPipeline pipeline;		/* NULL while the async creation runs, or if it failed */
	WGPUFuture future;
	bool pending, failed;
	uint32_t ngroups;
	struct wgvk_bgl *bgl[4];
	uint32_t immediate_bytes;
	uint16_t draw_params_offset;		/* T3b: where the draw parameters go in the immediates; 0xffff none */
};

/* what a draw's pipeline is made from */
struct wgvk_key_inputs {
	const struct wgvk_physical_device *pdev;
	struct wgvk_shader *vs, *fs;
	const struct wgvk_compiled_shader *vcs;		/* the vertex shader's variant */
	const struct wgvk_compiled_shader *fcs;		/* the fragment shader's variant (NULL: fs->cs) */
	const struct vk_dynamic_graphics_state *dyn;
	uint32_t color_count, samples;
	WGPUTextureFormat color[WGVK_MAX_COLOR], ds;
	WGPUIndexFormat strip_index;			/* indexed draws of strips: the index format */
	int topology;					/* >= 0: drawn as this (index rewriting), not ia's */
	bool no_vertex_input;				/* a geometry shader's rast part: no vertex buffers */
	uint32_t dummy_color;				/* 1 + the index of a write-masked stand-in target, or 0 */
};
void wgvk_pipeline_key_build(const struct wgvk_key_inputs *in, struct wgvk_pipeline_key *key);
struct wgvk_rpipe *wgvk_rpipe_get(struct wgvk_device *dev, const struct wgvk_pipeline_key *key,
				  struct wgvk_shader *vs, const struct wgvk_compiled_shader *vcs, struct wgvk_shader *fs,
				  const struct wgvk_compiled_shader *fcs, const struct wgvk_unfilterable *uf);
WGPURenderPipeline wgvk_rpipe_wait(struct wgvk_device *dev, struct wgvk_rpipe *p);
struct hash_table *wgvk_pipeline_table_create(void);
size_t wgvk_pipeline_cache_preload(struct wgvk_device *dev, const void *data, size_t size);

/* ---- T2: the rest of the command side (wgvk_cmd.c) */
void wgvk_record_dispatch(struct vk_device_dispatch_table *dt);
VkResult wgvk_device_init_t2(struct wgvk_device *dev);
void wgvk_device_finish_t2(struct wgvk_device *dev);

static inline WGPUStringView
wgvk_sv(const char *s)
{
	return (WGPUStringView){ s, WGPU_STRLEN };
}

/* The GPUBuffer and offset of a buffer range (bo offset included). */
static inline WGPUBuffer
wgvk_buffer_wgpu(const struct wgvk_buffer *b, uint64_t offset, uint64_t *wg_offset)
{
	*wg_offset = b->mem->bo.offset + b->offset + offset;
	return b->mem->bo.buffer;
}

/* A per-device id that is never reused (WebGPU handle addresses are). */
static inline uint64_t
wgvk_object_id(struct wgvk_device *dev)
{
	return ++dev->next_id;
}

/* WGVK_DEBUG=word,word: caps, remap, limits, ... */
bool wgvk_debug(const char *what);

/* once per call site */
#define wgvk_warn_once(...) do { static bool warned_; if (!warned_) { warned_ = true; wgvk_log(__VA_ARGS__); } } while (0)

#define wgvk_log(...) fprintf(stderr, "wgvk: " __VA_ARGS__)

#endif
