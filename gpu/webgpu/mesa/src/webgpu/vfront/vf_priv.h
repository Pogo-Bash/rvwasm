/*
 * vfront's insides. The model follows virglrenderer's: resources are global (ids made by gpuserver), each
 * context has sub-contexts (one per guest pipe_context; Mesa's virgl makes one and switches to it at once),
 * and objects (blend, rasterizer, DSA, shader, vertex elements, sampler view, sampler state, surface, query,
 * streamout target) live in a sub-context under the guest's handles. A sub-context is one zink pipe_context.
 */
#ifndef VF_PRIV_H
#define VF_PRIV_H

#include "vfront.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/hash_table.h"
#include "util/u_dynarray.h"
#include "util/u_inlines.h"

#include "virgl_hw.h"
#include "virgl_protocol.h"

struct vf_resource {
	uint32_t id;
	uint32_t ctx;			/* the context that made it */
	struct pipe_resource *pres;	/* NULL for a resource that has no GPU side (size-0 buffers) */
	uint64_t backing, backing_size;
	uint32_t virgl_format, bind, flags;
};

/* Vertex elements: in Gallium the vertex buffer's stride is part of the vertex elements state, while virgl
 * sends strides with SET_VERTEX_BUFFERS, so the CSO is made at draw time per set of strides. */
#define VF_VE_CACHE 4
struct vf_velems {
	unsigned count;
	struct pipe_vertex_element ve[PIPE_MAX_ATTRIBS];
	unsigned num_buffers;		/* the highest vertex_buffer_index + 1 */
	struct {
		uint32_t strides[PIPE_MAX_ATTRIBS];
		void *cso;
	} cache[VF_VE_CACHE];
	unsigned cache_next;
	/* elements whose 32-bit SSCALED/USCALED/SNORM/UNORM format the driver lacks (Vulkan has none):
	 * fetched as SINT/UINT, and the vertex shader converts (vf_variant_key.vs_*) */
	uint32_t sint_mask, uint_mask, snorm_mask, unorm_mask;
};

/* What the guest's Gallium frontend left to the host and zink does not do: fixed-function bits that become
 * shader variants here (zink's caps say flatshade, alpha_test, two_sided_color and clip_planes are the
 * frontend's job, while the guest's virgl screen promised them). One variant per key; key 0 is the shader as
 * sent. */
struct vf_variant_key {
	/* fragment shader */
	uint8_t alpha_func;		/* PIPE_FUNC_x + 1 when the alpha test is on, else 0 */
	uint8_t flatshade;
	uint8_t two_side;
	uint8_t fs_clipdist;		/* gl_ClipDistance[] length the previous stage writes (Vulkan wants them equal) */
	float alpha_ref;
	/* the last stage before the rasterizer */
	uint8_t ucp_enables;
	uint8_t psiz;			/* add gl_PointSize = point_size */
	uint8_t pad2[2];
	float point_size;
	float ucp[PIPE_MAX_CLIP_PLANES][4];	/* only those in ucp_enables */
	/* vertex shader: inputs fetched as integers that the shader wants as floats (vf_velems) */
	uint32_t vs_sint, vs_uint, vs_snorm, vs_unorm;
};

struct vf_variant {
	struct vf_variant_key key;
	void *cso;
	struct vf_variant *next;
};

struct vf_shader {
	mesa_shader_stage stage;
	struct pipe_stream_output_info so;
	uint32_t req_local_mem;
	struct nir_shader *nir;		/* as translated, for variants */
	struct vf_variant *variants;	/* the first one has key 0 */
	bool failed;
	int refs;			/* the object and the binding */
	/* a shader whose text comes in several commands ("long shader") */
	char *text;
	uint32_t text_total, text_have;
	uint32_t num_tokens;
};

struct vf_rs {
	void *cso;
	struct pipe_rasterizer_state st;
};

struct vf_dsa {
	void *cso;
	struct pipe_depth_stencil_alpha_state st;
};

struct vf_query {
	int refs;			/* the object and pending result writes */
	struct pipe_query *q;
	uint32_t type, index;
	uint32_t res;			/* the resource whose backing receives struct virgl_host_query_state */
	uint32_t offset;
};

struct vf_object {
	uint32_t type;			/* VIRGL_OBJECT_* */
	uint32_t handle;
	union {
		void *cso;		/* blend, sampler state */
		struct vf_rs *rs;
		struct vf_dsa *dsa;
		struct vf_shader *shader;
		struct vf_velems *velems;
		struct pipe_sampler_view *view;
		struct pipe_surface *surface;
		struct vf_query *query;
		struct pipe_stream_output_target *so_target;
	};
};

struct vf_sub {
	uint32_t id;
	struct vf_ctx *ctx;
	struct pipe_context *pipe;
	struct hash_table *objects;	/* handle -> struct vf_object */

	/* bound state the decoder has to combine itself */
	struct vf_velems *velems;
	struct {
		uint32_t res, stride, offset;
	} vb[PIPE_MAX_ATTRIBS];
	unsigned num_vb;
	bool vb_dirty;
	void *velems_cso;		/* what is bound now */

	struct {
		uint32_t res, index_size, offset;
	} ib;

	struct vf_shader *long_shader[MESA_SHADER_STAGES];
	struct vf_shader *bound[MESA_SHADER_STAGES];
	void *bound_cso[MESA_SHADER_STAGES];	/* the variant bound now */
	struct vf_rs *rs;
	struct vf_dsa *dsa;
	struct pipe_clip_state clip;

	struct pipe_framebuffer_state fb;
	uint8_t patch_vertices;
	/* Resources bound as images, storage and atomic buffers: zink holds no reference to a buffer bound
	 * there (the frontend must keep it alive while bound), and the guest may unref it while bound. */
	struct pipe_resource *image_res[MESA_SHADER_STAGES][PIPE_MAX_SHADER_IMAGES];
	struct pipe_resource *ssbo_res[MESA_SHADER_STAGES][PIPE_MAX_SHADER_BUFFERS];
	struct pipe_resource *atomic_res[PIPE_MAX_HW_ATOMIC_BUFFERS];
	struct pipe_stream_output_target *so_targets[PIPE_MAX_SO_BUFFERS];
	unsigned num_so_targets;
	struct vf_query *render_cond;
};

/* What the completion of a flush leaves to do. */
struct vf_readback {
	uint32_t res;
	struct pipe_resource *staging;	/* a copy of the box, read once the flush is done */
	uint32_t level;
	struct vf_box box;
	uint32_t stride, layer_stride;	/* of the data in the backing */
	uint64_t offset;		/* into the resource's backing */
	enum pipe_format format;	/* of the texture read back */
	bool packed;			/* a texture copied into a staging buffer, rows tightly packed */
};

struct vf_query_write {
	struct vf_query *query;		/* holds a reference: the object may go meanwhile */
	struct pipe_query *q;
	uint32_t res, offset, type;
	struct pipe_context *pipe;
};

struct vf_pending {
	uint32_t serial;
	struct pipe_fence_handle *fence;
	struct util_dynarray readbacks;	/* struct vf_readback */
	struct util_dynarray queries;	/* struct vf_query_write */
};

struct vf_ctx {
	uint32_t id;
	char name[64];
	struct hash_table *subs;	/* sub-context id -> struct vf_sub */
	struct vf_sub *sub;		/* the current one */

	/* work since the last flush */
	struct util_dynarray readbacks;
	struct util_dynarray queries;
	struct pipe_context *last_pipe;	/* the pipe_context the last work went to */

	/* flushed and not yet complete, oldest first */
	struct util_dynarray pending;	/* struct vf_pending */
	uint32_t done_serial;
	uint32_t flushed_serial;
};

struct vf_state {
	bool ready;
	struct pipe_screen *screen;
	struct vf_mem_ops mem;
	bool mem_plain;
	bool image_to_buffer;		/* resource_copy_region copies an image into a buffer (zink does) */
	uint32_t debug;
	struct hash_table *resources;	/* id -> struct vf_resource */
	struct vf_ctx *ctx[256];
	struct pipe_context *last_pipe;	/* over all contexts: keeps GPU order = stream order */
	struct vf_stats stats;
	union virgl_caps caps;
	char renderer[64];
	/* caps given to the guest that the Vulkan device does not back (vf_caps.c, VF_FAKE_*) */
	uint32_t faked;
};

/* "fake" in the design table's sense: advertised so the guest reaches GL 4.3 core, implemented partly or not
 * at all, and logged when a guest uses it. VFRONT_CAPS=honest leaves them out (the guest then gets GL 3.3). */
#define VF_FAKE_FP64      (1u << 0)	/* doubles: no shaderFloat64 */
#define VF_FAKE_TESS      (1u << 1)	/* tessellation: shaders are accepted and dropped, patch draws skipped */
#define VF_FAKE_VIEWPORTS (1u << 2)	/* viewport arrays: only viewport 0 (and scissor 0) reach the device */

extern struct vf_state vf;

#define vf_log(...) fprintf(stderr, "vfront: " __VA_ARGS__)
#define vf_err(...) do { if (vf.debug & VF_DEBUG_ERROR) fprintf(stderr, "vfront: " __VA_ARGS__); } while (0)

/* vf_main.c */
struct vf_resource *vf_res_lookup(uint32_t id);
struct pipe_resource *vf_pres(uint32_t id);
void *vf_backing_ptr(struct vf_resource *r, uint64_t offset, uint64_t size, void **tmp);
void *vf_backing_ptr2(struct vf_resource *r, uint64_t offset, uint64_t size, void **tmp, bool fill);
void vf_backing_done(struct vf_resource *r, uint64_t offset, uint64_t size, void *tmp, bool written);
void vf_use_pipe(struct vf_ctx *c, struct pipe_context *pipe);
struct vf_sub *vf_sub_get(struct vf_ctx *c, uint32_t id, bool create);
void vf_sub_destroy(struct vf_sub *s);
int vf_put_from_backing(struct vf_sub *s, struct vf_resource *r, uint32_t level, const struct vf_box *box,
			uint32_t stride, uint32_t layer_stride, uint64_t src_res_addr_offset, struct vf_resource *src);
int vf_get_to_backing(struct vf_sub *s, struct vf_resource *r, uint32_t level, const struct vf_box *box,
		      uint32_t stride, uint32_t layer_stride, uint64_t offset, struct vf_resource *dst);

/* vf_decode.c */
int vf_decode(struct vf_ctx *c, const uint32_t *dw, uint32_t ndw);
void vf_object_destroy(struct vf_sub *s, struct vf_object *o);

void vf_query_unref(struct vf_query *q, struct pipe_context *pipe);

/* vf_shader.c */
int vf_shader_create(struct vf_sub *s, struct vf_shader *sh, const char *text);
void vf_shader_unref(struct vf_sub *s, struct vf_shader *sh);
void vf_shader_bind(struct vf_sub *s, mesa_shader_stage stage, struct vf_shader *sh);
void vf_shaders_update(struct vf_sub *s, enum mesa_prim mode);
void *vf_shader_cso(struct vf_sub *s, struct vf_shader *sh, const struct vf_variant_key *key);

/* vf_caps.c */
void vf_caps_fill(union virgl_caps *caps);

/* vf_formats.c */
enum pipe_format vf_format(uint32_t virgl_format);
uint32_t vf_virgl_format(enum pipe_format format);

#endif
