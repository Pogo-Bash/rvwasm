/*
 * The virgl command decoder: one function per VIRGL_CCMD_*, each the inverse of the encoder in Mesa 25.0.7's
 * virgl_encode.c (what the guest sends) and doing on zink's pipe_context what vrend_renderer.c does in GL.
 *
 * A command is a header dword (command in bits 0-7, object type in 8-15, length in dwords in 16-31) and
 * `length` dwords; below, b[0] is the header and b[1..len] the payload, so the VIRGL_* offsets of
 * virgl_protocol.h index b directly.
 *
 * Enumerations that changed in Gallium since virgl's protocol was fixed are translated: shader stages (virgl
 * keeps the old order: vertex, fragment, geometry, tess control, tess eval, compute), query types, formats.
 * Blend, compare, stencil, wrap and filter enums, texture targets, primitives and swizzles are the same.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vf_priv.h"

#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_prim.h"
#include "util/u_prim_restart.h"
#include "compiler/shader_enums.h"

static const char *const cmd_names[VIRGL_MAX_COMMANDS] = {
	[VIRGL_CCMD_NOP] = "NOP",
	[VIRGL_CCMD_CREATE_OBJECT] = "CREATE_OBJECT",
	[VIRGL_CCMD_BIND_OBJECT] = "BIND_OBJECT",
	[VIRGL_CCMD_DESTROY_OBJECT] = "DESTROY_OBJECT",
	[VIRGL_CCMD_SET_VIEWPORT_STATE] = "SET_VIEWPORT_STATE",
	[VIRGL_CCMD_SET_FRAMEBUFFER_STATE] = "SET_FRAMEBUFFER_STATE",
	[VIRGL_CCMD_SET_VERTEX_BUFFERS] = "SET_VERTEX_BUFFERS",
	[VIRGL_CCMD_CLEAR] = "CLEAR",
	[VIRGL_CCMD_DRAW_VBO] = "DRAW_VBO",
	[VIRGL_CCMD_RESOURCE_INLINE_WRITE] = "RESOURCE_INLINE_WRITE",
	[VIRGL_CCMD_SET_SAMPLER_VIEWS] = "SET_SAMPLER_VIEWS",
	[VIRGL_CCMD_SET_INDEX_BUFFER] = "SET_INDEX_BUFFER",
	[VIRGL_CCMD_SET_CONSTANT_BUFFER] = "SET_CONSTANT_BUFFER",
	[VIRGL_CCMD_SET_STENCIL_REF] = "SET_STENCIL_REF",
	[VIRGL_CCMD_SET_BLEND_COLOR] = "SET_BLEND_COLOR",
	[VIRGL_CCMD_SET_SCISSOR_STATE] = "SET_SCISSOR_STATE",
	[VIRGL_CCMD_BLIT] = "BLIT",
	[VIRGL_CCMD_RESOURCE_COPY_REGION] = "RESOURCE_COPY_REGION",
	[VIRGL_CCMD_BIND_SAMPLER_STATES] = "BIND_SAMPLER_STATES",
	[VIRGL_CCMD_BEGIN_QUERY] = "BEGIN_QUERY",
	[VIRGL_CCMD_END_QUERY] = "END_QUERY",
	[VIRGL_CCMD_GET_QUERY_RESULT] = "GET_QUERY_RESULT",
	[VIRGL_CCMD_SET_POLYGON_STIPPLE] = "SET_POLYGON_STIPPLE",
	[VIRGL_CCMD_SET_CLIP_STATE] = "SET_CLIP_STATE",
	[VIRGL_CCMD_SET_SAMPLE_MASK] = "SET_SAMPLE_MASK",
	[VIRGL_CCMD_SET_STREAMOUT_TARGETS] = "SET_STREAMOUT_TARGETS",
	[VIRGL_CCMD_SET_RENDER_CONDITION] = "SET_RENDER_CONDITION",
	[VIRGL_CCMD_SET_UNIFORM_BUFFER] = "SET_UNIFORM_BUFFER",
	[VIRGL_CCMD_SET_SUB_CTX] = "SET_SUB_CTX",
	[VIRGL_CCMD_CREATE_SUB_CTX] = "CREATE_SUB_CTX",
	[VIRGL_CCMD_DESTROY_SUB_CTX] = "DESTROY_SUB_CTX",
	[VIRGL_CCMD_BIND_SHADER] = "BIND_SHADER",
	[VIRGL_CCMD_SET_TESS_STATE] = "SET_TESS_STATE",
	[VIRGL_CCMD_SET_MIN_SAMPLES] = "SET_MIN_SAMPLES",
	[VIRGL_CCMD_SET_SHADER_BUFFERS] = "SET_SHADER_BUFFERS",
	[VIRGL_CCMD_SET_SHADER_IMAGES] = "SET_SHADER_IMAGES",
	[VIRGL_CCMD_MEMORY_BARRIER] = "MEMORY_BARRIER",
	[VIRGL_CCMD_LAUNCH_GRID] = "LAUNCH_GRID",
	[VIRGL_CCMD_SET_FRAMEBUFFER_STATE_NO_ATTACH] = "SET_FRAMEBUFFER_STATE_NO_ATTACH",
	[VIRGL_CCMD_TEXTURE_BARRIER] = "TEXTURE_BARRIER",
	[VIRGL_CCMD_SET_ATOMIC_BUFFERS] = "SET_ATOMIC_BUFFERS",
	[VIRGL_CCMD_SET_DEBUG_FLAGS] = "SET_DEBUG_FLAGS",
	[VIRGL_CCMD_GET_QUERY_RESULT_QBO] = "GET_QUERY_RESULT_QBO",
	[VIRGL_CCMD_TRANSFER3D] = "TRANSFER3D",
	[VIRGL_CCMD_END_TRANSFERS] = "END_TRANSFERS",
	[VIRGL_CCMD_COPY_TRANSFER3D] = "COPY_TRANSFER3D",
	[VIRGL_CCMD_SET_TWEAKS] = "SET_TWEAKS",
	[VIRGL_CCMD_CLEAR_TEXTURE] = "CLEAR_TEXTURE",
	[VIRGL_CCMD_PIPE_RESOURCE_CREATE] = "PIPE_RESOURCE_CREATE",
	[VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE] = "PIPE_RESOURCE_SET_TYPE",
	[VIRGL_CCMD_GET_MEMORY_INFO] = "GET_MEMORY_INFO",
	[VIRGL_CCMD_EMIT_STRING_MARKER] = "EMIT_STRING_MARKER",
	[VIRGL_CCMD_LINK_SHADER] = "LINK_SHADER",
	[VIRGL_CCMD_CREATE_VIDEO_CODEC] = "CREATE_VIDEO_CODEC",
	[VIRGL_CCMD_DESTROY_VIDEO_CODEC] = "DESTROY_VIDEO_CODEC",
	[VIRGL_CCMD_CREATE_VIDEO_BUFFER] = "CREATE_VIDEO_BUFFER",
	[VIRGL_CCMD_DESTROY_VIDEO_BUFFER] = "DESTROY_VIDEO_BUFFER",
	[VIRGL_CCMD_BEGIN_FRAME] = "BEGIN_FRAME",
	[VIRGL_CCMD_DECODE_MACROBLOCK] = "DECODE_MACROBLOCK",
	[VIRGL_CCMD_DECODE_BITSTREAM] = "DECODE_BITSTREAM",
	[VIRGL_CCMD_ENCODE_BITSTREAM] = "ENCODE_BITSTREAM",
	[VIRGL_CCMD_END_FRAME] = "END_FRAME",
	[VIRGL_CCMD_CLEAR_SURFACE] = "CLEAR_SURFACE",
};

static const char *const obj_names[VIRGL_MAX_OBJECTS] = {
	"NULL", "BLEND", "RASTERIZER", "DSA", "SHADER", "VERTEX_ELEMENTS", "SAMPLER_VIEW", "SAMPLER_STATE",
	"SURFACE", "QUERY", "STREAMOUT_TARGET", "MSAA_SURFACE",
};

static inline float uif32(uint32_t u)
{
	union {
		uint32_t u;
		float f;
	} x = { u };
	return x.f;
}

/* virgl's shader stage numbers (VIRGL_SHADER_*) to Gallium's */
static bool stage_of(uint32_t v, mesa_shader_stage *out)
{
	static const mesa_shader_stage map[] = {
		[VIRGL_SHADER_VERTEX] = MESA_SHADER_VERTEX,
		[VIRGL_SHADER_FRAGMENT] = MESA_SHADER_FRAGMENT,
		[VIRGL_SHADER_GEOMETRY] = MESA_SHADER_GEOMETRY,
		[VIRGL_SHADER_TESS_CTRL] = MESA_SHADER_TESS_CTRL,
		[VIRGL_SHADER_TESS_EVAL] = MESA_SHADER_TESS_EVAL,
		[VIRGL_SHADER_COMPUTE] = MESA_SHADER_COMPUTE,
	};
	if (v >= ARRAY_SIZE(map))
		return false;
	*out = map[v];
	return true;
}

/* ---- objects ---- */

static struct vf_object *obj_lookup(struct vf_sub *s, uint32_t handle, uint32_t type)
{
	if (!handle)
		return NULL;
	struct hash_entry *he = _mesa_hash_table_search(s->objects, (void *)(uintptr_t)handle);
	if (!he)
		return NULL;
	struct vf_object *o = he->data;
	if (type != VIRGL_OBJECT_NULL && o->type != type &&
	    !(type == VIRGL_OBJECT_SURFACE && o->type == VIRGL_OBJECT_MSAA_SURFACE))
		return NULL;
	return o;
}

void vf_query_unref(struct vf_query *q, struct pipe_context *pipe)
{
	if (--q->refs == 0) {
		pipe->destroy_query(pipe, q->q);
		free(q);
	}
}

void vf_object_destroy(struct vf_sub *s, struct vf_object *o)
{
	struct pipe_context *pipe = s->pipe;
	switch (o->type) {
	case VIRGL_OBJECT_BLEND:
		pipe->delete_blend_state(pipe, o->cso);
		break;
	case VIRGL_OBJECT_RASTERIZER:
		if (s->rs == o->rs)
			s->rs = NULL;
		pipe->delete_rasterizer_state(pipe, o->rs->cso);
		free(o->rs);
		break;
	case VIRGL_OBJECT_DSA:
		if (s->dsa == o->dsa)
			s->dsa = NULL;
		pipe->delete_depth_stencil_alpha_state(pipe, o->dsa->cso);
		free(o->dsa);
		break;
	case VIRGL_OBJECT_SAMPLER_STATE:
		pipe->delete_sampler_state(pipe, o->cso);
		break;
	case VIRGL_OBJECT_SHADER:
		for (unsigned i = 0; i < MESA_SHADER_STAGES; i++)
			if (s->long_shader[i] == o->shader)
				s->long_shader[i] = NULL;
		vf_shader_unref(s, o->shader);
		break;
	case VIRGL_OBJECT_VERTEX_ELEMENTS: {
		struct vf_velems *ve = o->velems;
		for (unsigned i = 0; i < VF_VE_CACHE; i++) {
			if (!ve->cache[i].cso)
				continue;
			if (s->velems_cso == ve->cache[i].cso) {
				pipe->bind_vertex_elements_state(pipe, NULL);
				s->velems_cso = NULL;
			}
			pipe->delete_vertex_elements_state(pipe, ve->cache[i].cso);
		}
		if (s->velems == ve) {
			if (vf.debug & VF_DEBUG_CMD)
				vf_log("  vertex elements %u destroyed while bound\n", o->handle);
			s->velems = NULL;
		}
		free(ve);
		break;
	}
	case VIRGL_OBJECT_SAMPLER_VIEW:
		pipe_sampler_view_reference(&o->view, NULL);
		break;
	case VIRGL_OBJECT_SURFACE:
	case VIRGL_OBJECT_MSAA_SURFACE:
		/* (surfaces are plain values in Gallium now: the framebuffer state holds its own copies) */
		pipe_resource_reference(&o->surface->texture, NULL);
		free(o->surface);
		break;
	case VIRGL_OBJECT_QUERY:
		if (s->render_cond == o->query) {
			pipe->render_condition(pipe, NULL, false, 0);
			s->render_cond = NULL;
		}
		vf_query_unref(o->query, pipe);
		break;
	case VIRGL_OBJECT_STREAMOUT_TARGET:
		pipe_so_target_reference(&o->so_target, NULL);
		break;
	}
	free(o);
}

static struct vf_object *obj_new(struct vf_sub *s, uint32_t handle, uint32_t type)
{
	struct hash_entry *he = _mesa_hash_table_search(s->objects, (void *)(uintptr_t)handle);
	if (he) {
		/* a handle given again: the old object goes (vrend replaces it too) */
		vf_object_destroy(s, he->data);
		_mesa_hash_table_remove(s->objects, he);
	}
	struct vf_object *o = calloc(1, sizeof(*o));
	o->type = type;
	o->handle = handle;
	return o;
}

static void obj_insert(struct vf_sub *s, struct vf_object *o)
{
	_mesa_hash_table_insert(s->objects, (void *)(uintptr_t)o->handle, o);
}

static int create_blend(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle)
{
	if (len != VIRGL_OBJ_BLEND_SIZE)
		return EINVAL;
	struct pipe_blend_state st;
	memset(&st, 0, sizeof(st));
	uint32_t t = b[VIRGL_OBJ_BLEND_S0];
	st.independent_blend_enable = t & 1;
	st.logicop_enable = (t >> 1) & 1;
	st.dither = (t >> 2) & 1;
	st.alpha_to_coverage = (t >> 3) & 1;
	st.alpha_to_one = (t >> 4) & 1;
	st.logicop_func = b[VIRGL_OBJ_BLEND_S1] & 0xf;
	for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
		t = b[VIRGL_OBJ_BLEND_S2(i)];
		st.rt[i].blend_enable = t & 1;
		st.rt[i].rgb_func = (t >> 1) & 7;
		st.rt[i].rgb_src_factor = (t >> 4) & 0x1f;
		st.rt[i].rgb_dst_factor = (t >> 9) & 0x1f;
		st.rt[i].alpha_func = (t >> 14) & 7;
		st.rt[i].alpha_src_factor = (t >> 17) & 0x1f;
		st.rt[i].alpha_dst_factor = (t >> 22) & 0x1f;
		st.rt[i].colormask = (t >> 27) & 0xf;
	}
	/* zink applies rt[0] to every attachment when blending is not independent, up to max_rt */
	st.max_rt = PIPE_MAX_COLOR_BUFS - 1;
	if (vf.debug & VF_DEBUG_CMD)
		vf_log("  blend %u: enable %u func %u/%u src %u/%u dst %u/%u mask %x, indep %u logicop %u/%u a2c %u\n",
		       handle, st.rt[0].blend_enable, st.rt[0].rgb_func, st.rt[0].alpha_func, st.rt[0].rgb_src_factor,
		       st.rt[0].alpha_src_factor, st.rt[0].rgb_dst_factor, st.rt[0].alpha_dst_factor, st.rt[0].colormask,
		       st.independent_blend_enable, st.logicop_enable, st.logicop_func, st.alpha_to_coverage);
	struct vf_object *o = obj_new(s, handle, VIRGL_OBJECT_BLEND);
	o->cso = s->pipe->create_blend_state(s->pipe, &st);
	obj_insert(s, o);
	return 0;
}

static int create_dsa(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle)
{
	if (len != VIRGL_OBJ_DSA_SIZE)
		return EINVAL;
	struct vf_dsa *d = calloc(1, sizeof(*d));
	struct pipe_depth_stencil_alpha_state *st = &d->st;
	uint32_t t = b[VIRGL_OBJ_DSA_S0];
	st->depth_enabled = t & 1;
	st->depth_writemask = (t >> 1) & 1;
	st->depth_func = (t >> 2) & 7;
	st->alpha_enabled = (t >> 8) & 1;
	st->alpha_func = (t >> 9) & 7;
	for (unsigned i = 0; i < 2; i++) {
		t = b[VIRGL_OBJ_DSA_S1 + i];
		st->stencil[i].enabled = t & 1;
		st->stencil[i].func = (t >> 1) & 7;
		st->stencil[i].fail_op = (t >> 4) & 7;
		st->stencil[i].zpass_op = (t >> 7) & 7;
		st->stencil[i].zfail_op = (t >> 10) & 7;
		st->stencil[i].valuemask = (t >> 13) & 0xff;
		st->stencil[i].writemask = (t >> 21) & 0xff;
	}
	st->alpha_ref_value = uif32(b[VIRGL_OBJ_DSA_ALPHA_REF]);
	/* zink has no alpha test: it is lowered into the fragment shader (vf_shader.c); the CSO goes without */
	if (vf.debug & VF_DEBUG_CMD)
		vf_log("  dsa %u: depth %u write %u func %u, stencil %u/%u func %u/%u ops %u%u%u/%u%u%u masks %x/%x %x/%x, "
		       "alpha %u func %u ref %g\n", handle, st->depth_enabled, st->depth_writemask, st->depth_func,
		       st->stencil[0].enabled, st->stencil[1].enabled, st->stencil[0].func, st->stencil[1].func,
		       st->stencil[0].fail_op, st->stencil[0].zfail_op, st->stencil[0].zpass_op, st->stencil[1].fail_op,
		       st->stencil[1].zfail_op, st->stencil[1].zpass_op, st->stencil[0].valuemask, st->stencil[0].writemask,
		       st->stencil[1].valuemask, st->stencil[1].writemask, st->alpha_enabled, st->alpha_func,
		       st->alpha_ref_value);
	struct pipe_depth_stencil_alpha_state hw = *st;
	hw.alpha_enabled = 0;
	d->cso = s->pipe->create_depth_stencil_alpha_state(s->pipe, &hw);
	struct vf_object *o = obj_new(s, handle, VIRGL_OBJECT_DSA);
	o->dsa = d;
	obj_insert(s, o);
	return 0;
}

static int create_rasterizer(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle)
{
	if (len != VIRGL_OBJ_RS_SIZE)
		return EINVAL;
	struct vf_rs *r = calloc(1, sizeof(*r));
	struct pipe_rasterizer_state *st = &r->st;
	uint32_t t = b[VIRGL_OBJ_RS_S0];
#define BIT(name, n) st->name = (t >> (n)) & 1
	BIT(flatshade, 0);
	BIT(depth_clip_near, 1);
	BIT(clip_halfz, 2);
	BIT(rasterizer_discard, 3);
	BIT(flatshade_first, 4);
	BIT(light_twoside, 5);
	BIT(sprite_coord_mode, 6);
	BIT(point_quad_rasterization, 7);
	st->cull_face = (t >> 8) & 3;
	st->fill_front = (t >> 10) & 3;
	st->fill_back = (t >> 12) & 3;
	BIT(scissor, 14);
	BIT(front_ccw, 15);
	BIT(clamp_vertex_color, 16);
	BIT(clamp_fragment_color, 17);
	BIT(offset_line, 18);
	BIT(offset_point, 19);
	BIT(offset_tri, 20);
	BIT(poly_smooth, 21);
	BIT(poly_stipple_enable, 22);
	BIT(point_smooth, 23);
	BIT(point_size_per_vertex, 24);
	BIT(multisample, 25);
	BIT(line_smooth, 26);
	BIT(line_stipple_enable, 27);
	BIT(line_last_pixel, 28);
	BIT(half_pixel_center, 29);
	BIT(bottom_edge_rule, 30);
	BIT(force_persample_interp, 31);
#undef BIT
	/* what Mesa's st derives and virgl does not send */
	st->depth_clip_far = st->depth_clip_near;
	st->depth_clamp = !st->depth_clip_far;
	st->line_rectangular = st->multisample || st->line_smooth;
	st->point_size = uif32(b[VIRGL_OBJ_RS_POINT_SIZE]);
	st->sprite_coord_enable = b[VIRGL_OBJ_RS_SPRITE_COORD_ENABLE];
	t = b[VIRGL_OBJ_RS_S3];
	st->line_stipple_pattern = t & 0xffff;
	st->line_stipple_factor = (t >> 16) & 0xff;
	st->clip_plane_enable = (t >> 24) & 0xff;
	st->line_width = uif32(b[VIRGL_OBJ_RS_LINE_WIDTH]);
	st->offset_units = uif32(b[VIRGL_OBJ_RS_OFFSET_UNITS]);
	st->offset_scale = uif32(b[VIRGL_OBJ_RS_OFFSET_SCALE]);
	st->offset_clamp = uif32(b[VIRGL_OBJ_RS_OFFSET_CLAMP]);
	if (vf.debug & VF_DEBUG_CMD)
		vf_log("  rasterizer %u: cull %u fill %u/%u ccw %u flat %u twoside %u scissor %u offset %u%u%u %g/%g/%g "
		       "clip %x halfz %u depthclip %u msaa %u psize %g/%u sprite %x/%u\n", handle, st->cull_face,
		       st->fill_front, st->fill_back, st->front_ccw, st->flatshade, st->light_twoside, st->scissor,
		       st->offset_point, st->offset_line, st->offset_tri, st->offset_units, st->offset_scale,
		       st->offset_clamp, st->clip_plane_enable, st->clip_halfz, st->depth_clip_near, st->multisample,
		       st->point_size, st->point_size_per_vertex, st->sprite_coord_enable, st->point_quad_rasterization);
	r->cso = s->pipe->create_rasterizer_state(s->pipe, st);
	struct vf_object *o = obj_new(s, handle, VIRGL_OBJECT_RASTERIZER);
	o->rs = r;
	obj_insert(s, o);
	return 0;
}

static int create_shader(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle)
{
	if (len < VIRGL_OBJ_SHADER_HDR_SIZE(0))
		return EINVAL;
	mesa_shader_stage stage;
	if (!stage_of(b[VIRGL_OBJ_SHADER_TYPE], &stage))
		return EINVAL;
	uint32_t offlen = b[VIRGL_OBJ_SHADER_OFFSET];
	uint32_t num_tokens = b[VIRGL_OBJ_SHADER_NUM_TOKENS];
	uint32_t num_so = 0, req_local_mem = 0;
	if (stage == MESA_SHADER_COMPUTE) {
		req_local_mem = b[VIRGL_OBJ_SHADER_SO_NUM_OUTPUTS];
	} else {
		num_so = b[VIRGL_OBJ_SHADER_SO_NUM_OUTPUTS];
		if (num_so > PIPE_MAX_SO_OUTPUTS || len < VIRGL_OBJ_SHADER_HDR_SIZE(num_so))
			return EINVAL;
	}
	uint32_t text_at = 6 + (num_so ? 4 + 2 * num_so : 0);
	if (text_at > len + 1)
		return EINVAL;
	uint32_t text_dw = len + 1 - text_at;
	const char *text = (const char *)&b[text_at];

	bool cont = offlen & VIRGL_OBJ_SHADER_OFFSET_CONT;
	offlen &= ~VIRGL_OBJ_SHADER_OFFSET_CONT;
	struct vf_shader *sh;
	if (!cont) {
		if (s->long_shader[stage]) {
			vf_err("a new shader while a long one is coming in\n");
			return EINVAL;
		}
		if ((offlen + 3) / 4 < text_dw)
			return EINVAL;
		sh = calloc(1, sizeof(*sh));
		sh->stage = stage;
		sh->req_local_mem = req_local_mem;
		sh->num_tokens = num_tokens;
		sh->refs = 1;
		if (num_so) {
			sh->so.num_outputs = num_so;
			for (unsigned i = 0; i < 4; i++)
				sh->so.stride[i] = b[VIRGL_OBJ_SHADER_SO_STRIDE(i)];
			for (unsigned i = 0; i < num_so; i++) {
				uint32_t t = b[VIRGL_OBJ_SHADER_SO_OUTPUT0(i)];
				sh->so.output[i].register_index = t & 0xff;
				sh->so.output[i].start_component = (t >> 8) & 3;
				sh->so.output[i].num_components = (t >> 10) & 7;
				sh->so.output[i].output_buffer = (t >> 13) & 7;
				sh->so.output[i].dst_offset = (t >> 16) & 0xffff;
				sh->so.output[i].stream = b[VIRGL_OBJ_SHADER_SO_OUTPUT0_SO(i)] & 3;
			}
		}
		struct vf_object *o = obj_new(s, handle, VIRGL_OBJECT_SHADER);
		o->shader = sh;
		obj_insert(s, o);
		if ((offlen + 3) / 4 > text_dw) {
			/* the rest follows in continuations */
			sh->text_total = offlen;
			sh->text = malloc(((uint64_t)offlen + 4) & ~3ull);
			memcpy(sh->text, text, text_dw * 4);
			sh->text_have = text_dw * 4;
			s->long_shader[stage] = sh;
			return 0;
		}
		char *t = malloc(text_dw * 4 + 1);
		memcpy(t, text, text_dw * 4);
		t[text_dw * 4] = 0;
		int ret = vf_shader_create(s, sh, t);
		free(t);
		return ret;
	}
	sh = s->long_shader[stage];
	struct vf_object *o = obj_lookup(s, handle, VIRGL_OBJECT_SHADER);
	if (!sh || !o || o->shader != sh || offlen != sh->text_have ||
	    (uint64_t)sh->text_have + text_dw * 4 > ((sh->text_total + 3) & ~3u)) {
		vf_err("a shader continuation that does not fit\n");
		s->long_shader[stage] = NULL;
		return EINVAL;
	}
	memcpy(sh->text + sh->text_have, text, text_dw * 4);
	sh->text_have += text_dw * 4;
	if (sh->text_have >= sh->text_total) {
		s->long_shader[stage] = NULL;
		sh->text[sh->text_total - 1] = 0;
		int ret = vf_shader_create(s, sh, sh->text);
		free(sh->text);
		sh->text = NULL;
		return ret;
	}
	return 0;
}

static int create_velems(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle)
{
	if (len < 1 || (len - 1) % 4)
		return EINVAL;
	unsigned n = (len - 1) / 4;
	if (n > PIPE_MAX_ATTRIBS)
		return EINVAL;
	struct vf_velems *ve = calloc(1, sizeof(*ve));
	ve->count = n;
	for (unsigned i = 0; i < n; i++) {
		struct pipe_vertex_element *e = &ve->ve[i];
		e->src_offset = b[VIRGL_OBJ_VERTEX_ELEMENTS_V0_SRC_OFFSET(i)];
		e->instance_divisor = b[VIRGL_OBJ_VERTEX_ELEMENTS_V0_INSTANCE_DIVISOR(i)];
		uint32_t vb = b[VIRGL_OBJ_VERTEX_ELEMENTS_V0_VERTEX_BUFFER_INDEX(i)];
		if (vb >= PIPE_MAX_ATTRIBS) {
			free(ve);
			return EINVAL;
		}
		e->vertex_buffer_index = vb;
		enum pipe_format f = vf_format(b[VIRGL_OBJ_VERTEX_ELEMENTS_V0_SRC_FORMAT(i)]);
		if (f == PIPE_FORMAT_NONE || f > 255) {
			vf_err("vertex element %u: format %u cannot be a vertex format here\n", i,
			       b[VIRGL_OBJ_VERTEX_ELEMENTS_V0_SRC_FORMAT(i)]);
			free(ve);
			return EINVAL;
		}
		if (!vf.screen->is_format_supported(vf.screen, f, PIPE_BUFFER, 0, 0, PIPE_BIND_VERTEX_BUFFER)) {
			/* GL_INT/GL_UNSIGNED_INT attributes read as floats: fetch the integers, convert in the
			 * vertex shader (a variant) */
			const struct util_format_description *d = util_format_description(f);
			if (d && d->nr_channels >= 1 && d->nr_channels <= 4 && d->channel[0].size == 32 &&
			    !d->channel[0].pure_integer &&
			    (d->channel[0].type == UTIL_FORMAT_TYPE_SIGNED || d->channel[0].type == UTIL_FORMAT_TYPE_UNSIGNED)) {
				bool sgn = d->channel[0].type == UTIL_FORMAT_TYPE_SIGNED, norm = d->channel[0].normalized;
				static const enum pipe_format si[4] = { PIPE_FORMAT_R32_SINT, PIPE_FORMAT_R32G32_SINT,
					PIPE_FORMAT_R32G32B32_SINT, PIPE_FORMAT_R32G32B32A32_SINT };
				static const enum pipe_format ui[4] = { PIPE_FORMAT_R32_UINT, PIPE_FORMAT_R32G32_UINT,
					PIPE_FORMAT_R32G32B32_UINT, PIPE_FORMAT_R32G32B32A32_UINT };
				f = (sgn ? si : ui)[(d->nr_channels - 1) & 3];
				uint32_t *mask = sgn ? (norm ? &ve->snorm_mask : &ve->sint_mask)
						     : (norm ? &ve->unorm_mask : &ve->uint_mask);
				*mask |= 1u << i;
			}
		}
		if (!vf.screen->is_format_supported(vf.screen, f, PIPE_BUFFER, 0, 0, PIPE_BIND_VERTEX_BUFFER)) {
			/* (zink asserts on such a format; the caps should have kept the guest from sending it) */
			vf_err("vertex element %u: %s is not a vertex format of the driver\n", i, util_format_name(f));
			free(ve);
			return EINVAL;
		}
		e->src_format = f;
		ve->num_buffers = MAX2(ve->num_buffers, vb + 1);
	}
	struct vf_object *o = obj_new(s, handle, VIRGL_OBJECT_VERTEX_ELEMENTS);
	o->velems = ve;
	obj_insert(s, o);
	return 0;
}

static int create_sampler_view(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle)
{
	if (len != VIRGL_OBJ_SAMPLER_VIEW_SIZE)
		return EINVAL;
	struct pipe_resource *p = vf_pres(b[VIRGL_OBJ_SAMPLER_VIEW_RES_HANDLE]);
	if (!p) {
		vf_err("sampler view %u: no resource %u\n", handle, b[VIRGL_OBJ_SAMPLER_VIEW_RES_HANDLE]);
		return EINVAL;
	}
	uint32_t ft = b[VIRGL_OBJ_SAMPLER_VIEW_FORMAT];
	struct pipe_sampler_view t;
	memset(&t, 0, sizeof(t));
	t.format = vf_format(ft & 0xffffff);
	if (t.format == PIPE_FORMAT_NONE)
		return EINVAL;
	/* the target is there when the guest saw VIRGL_CAP_TEXTURE_VIEW */
	t.target = p->target == PIPE_BUFFER ? PIPE_BUFFER : (enum pipe_texture_target)((ft >> 24) & 0xff);
	if (t.target == PIPE_BUFFER && p->target != PIPE_BUFFER)
		t.target = p->target;
	uint32_t v0 = b[VIRGL_OBJ_SAMPLER_VIEW_BUFFER_FIRST_ELEMENT];
	uint32_t v1 = b[VIRGL_OBJ_SAMPLER_VIEW_BUFFER_LAST_ELEMENT];
	if (p->target == PIPE_BUFFER) {
		unsigned el = util_format_get_blocksize(t.format);
		t.u.buf.offset = v0 * el;
		t.u.buf.size = (v1 - v0 + 1) * el;
	} else {
		t.u.tex.first_layer = v0 & 0xffff;
		t.u.tex.last_layer = (v0 >> 16) & 0xffff;
		t.u.tex.first_level = v1 & 0xff;
		t.u.tex.last_level = (v1 >> 8) & 0xff;
	}
	uint32_t sw = b[VIRGL_OBJ_SAMPLER_VIEW_SWIZZLE];
	t.swizzle_r = sw & 7;
	t.swizzle_g = (sw >> 3) & 7;
	t.swizzle_b = (sw >> 6) & 7;
	t.swizzle_a = (sw >> 9) & 7;
	struct pipe_sampler_view *v = s->pipe->create_sampler_view(s->pipe, p, &t);
	if (!v)
		return ENOMEM;
	struct vf_object *o = obj_new(s, handle, VIRGL_OBJECT_SAMPLER_VIEW);
	o->view = v;
	obj_insert(s, o);
	return 0;
}

static int create_sampler_state(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle)
{
	if (len != VIRGL_OBJ_SAMPLER_STATE_SIZE)
		return EINVAL;
	struct pipe_sampler_state st;
	memset(&st, 0, sizeof(st));
	uint32_t t = b[VIRGL_OBJ_SAMPLER_STATE_S0];
	st.wrap_s = t & 7;
	st.wrap_t = (t >> 3) & 7;
	st.wrap_r = (t >> 6) & 7;
	st.min_img_filter = (t >> 9) & 1;
	st.min_mip_filter = (t >> 11) & 3;
	st.mag_img_filter = (t >> 13) & 1;
	st.compare_mode = (t >> 15) & 1;
	st.compare_func = (t >> 16) & 7;
	st.seamless_cube_map = (t >> 19) & 1;
	st.max_anisotropy = (t >> 20) & 0x1f;
	st.lod_bias = uif32(b[VIRGL_OBJ_SAMPLER_STATE_LOD_BIAS]);
	st.min_lod = uif32(b[VIRGL_OBJ_SAMPLER_STATE_MIN_LOD]);
	st.max_lod = uif32(b[VIRGL_OBJ_SAMPLER_STATE_MAX_LOD]);
	for (unsigned i = 0; i < 4; i++)
		st.border_color.ui[i] = b[VIRGL_OBJ_SAMPLER_STATE_BORDER_COLOR(i)];
	/* zink has no GL_CLAMP (its caps leave it to the frontend): the nearest thing it has */
	if (st.wrap_s == PIPE_TEX_WRAP_CLAMP)
		st.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
	if (st.wrap_t == PIPE_TEX_WRAP_CLAMP)
		st.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
	if (st.wrap_r == PIPE_TEX_WRAP_CLAMP)
		st.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
	if (st.wrap_s == PIPE_TEX_WRAP_MIRROR_CLAMP)
		st.wrap_s = PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
	if (st.wrap_t == PIPE_TEX_WRAP_MIRROR_CLAMP)
		st.wrap_t = PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
	if (st.wrap_r == PIPE_TEX_WRAP_MIRROR_CLAMP)
		st.wrap_r = PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
	struct vf_object *o = obj_new(s, handle, VIRGL_OBJECT_SAMPLER_STATE);
	o->cso = s->pipe->create_sampler_state(s->pipe, &st);
	obj_insert(s, o);
	return 0;
}

static int create_surface(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle, bool msaa)
{
	if (len != (msaa ? VIRGL_OBJ_MSAA_SURFACE_SIZE : VIRGL_OBJ_SURFACE_SIZE))
		return EINVAL;
	struct pipe_resource *p = vf_pres(b[VIRGL_OBJ_SURFACE_RES_HANDLE]);
	if (!p) {
		vf_err("surface %u: no resource %u\n", handle, b[VIRGL_OBJ_SURFACE_RES_HANDLE]);
		return EINVAL;
	}
	if (p->target == PIPE_BUFFER)
		return EINVAL;	/* (Gallium has no buffer surfaces any more; virgl never makes them) */
	struct pipe_surface *sf = calloc(1, sizeof(*sf));
	sf->format = vf_format(b[VIRGL_OBJ_SURFACE_FORMAT]);
	if (sf->format == PIPE_FORMAT_NONE) {
		free(sf);
		return EINVAL;
	}
	uint32_t v1 = b[VIRGL_OBJ_SURFACE_BUFFER_LAST_ELEMENT];
	sf->level = b[VIRGL_OBJ_SURFACE_BUFFER_FIRST_ELEMENT];
	sf->first_layer = v1 & 0xffff;
	sf->last_layer = (v1 >> 16) & 0xffff;
	if (msaa)
		sf->nr_samples = b[VIRGL_OBJ_SURFACE_SAMPLE_COUNT];
	pipe_resource_reference(&sf->texture, p);
	struct vf_object *o = obj_new(s, handle, msaa ? VIRGL_OBJECT_MSAA_SURFACE : VIRGL_OBJECT_SURFACE);
	o->surface = sf;
	obj_insert(s, o);
	return 0;
}

/* virgl's query types (virgl_query.c's VIRGL_QUERY_*) to Gallium's */
static int query_type(uint32_t v, uint32_t index, uint32_t *type, uint32_t *pindex)
{
	*pindex = index;
	switch (v) {
	case 0: *type = PIPE_QUERY_OCCLUSION_COUNTER; return 0;
	case 1: *type = PIPE_QUERY_OCCLUSION_PREDICATE; return 0;
	case 2: *type = PIPE_QUERY_TIMESTAMP; return 0;
	case 3: *type = PIPE_QUERY_TIMESTAMP_DISJOINT; return 0;
	case 4: *type = PIPE_QUERY_TIME_ELAPSED; return 0;
	case 5: *type = PIPE_QUERY_PRIMITIVES_GENERATED; return 0;
	case 6: *type = PIPE_QUERY_PRIMITIVES_EMITTED; return 0;
	case 7: *type = PIPE_QUERY_SO_STATISTICS; return 0;
	case 8: *type = PIPE_QUERY_SO_OVERFLOW_PREDICATE; return 0;
	case 9: *type = PIPE_QUERY_GPU_FINISHED; return 0;
	case 10: *type = PIPE_QUERY_PIPELINE_STATISTICS_SINGLE; return 0; /* same index order as PIPE_STAT_QUERY_* */
	case 11: *type = PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE; return 0;
	case 12: *type = PIPE_QUERY_SO_OVERFLOW_ANY_PREDICATE; return 0;
	}
	return EINVAL;
}

static int create_query(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle)
{
	if (len != VIRGL_OBJ_QUERY_SIZE)
		return EINVAL;
	uint32_t t = b[VIRGL_OBJ_QUERY_TYPE_INDEX];
	uint32_t type, index;
	if (query_type(t & 0xffff, (t >> 16) & 0xffff, &type, &index))
		return EINVAL;
	struct vf_query *q = calloc(1, sizeof(*q));
	q->refs = 1;
	q->type = type;
	q->index = index;
	q->offset = b[VIRGL_OBJ_QUERY_OFFSET];
	q->res = b[VIRGL_OBJ_QUERY_RES_HANDLE];
	q->q = s->pipe->create_query(s->pipe, type, index);
	if (!q->q) {
		vf_err("query %u: zink has no query type %u\n", handle, type);
		free(q);
		return EINVAL;
	}
	struct vf_object *o = obj_new(s, handle, VIRGL_OBJECT_QUERY);
	o->query = q;
	obj_insert(s, o);
	return 0;
}

static int create_so_target(struct vf_sub *s, const uint32_t *b, uint32_t len, uint32_t handle)
{
	if (len != VIRGL_OBJ_STREAMOUT_SIZE)
		return EINVAL;
	struct pipe_resource *p = vf_pres(b[VIRGL_OBJ_STREAMOUT_RES_HANDLE]);
	if (!p)
		return EINVAL;
	struct pipe_stream_output_target *t = s->pipe->create_stream_output_target(
		s->pipe, p, b[VIRGL_OBJ_STREAMOUT_BUFFER_OFFSET], b[VIRGL_OBJ_STREAMOUT_BUFFER_SIZE]);
	if (!t)
		return ENOMEM;
	struct vf_object *o = obj_new(s, handle, VIRGL_OBJECT_STREAMOUT_TARGET);
	o->so_target = t;
	obj_insert(s, o);
	return 0;
}

static int d_create_object(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 1)
		return EINVAL;
	uint32_t type = (b[0] >> 8) & 0xff;
	uint32_t handle = b[VIRGL_OBJ_CREATE_HANDLE];
	if (!handle)
		return EINVAL;
	if ((vf.debug & VF_DEBUG_CMD) && type < VIRGL_MAX_OBJECTS)
		vf_log("  create %s %u\n", obj_names[type], handle);
	if ((vf.debug & VF_DEBUG_SHADER) && type == VIRGL_OBJECT_SHADER)
		vf_log("shader object %u:\n", handle);
	switch (type) {
	case VIRGL_OBJECT_BLEND: return create_blend(s, b, len, handle);
	case VIRGL_OBJECT_RASTERIZER: return create_rasterizer(s, b, len, handle);
	case VIRGL_OBJECT_DSA: return create_dsa(s, b, len, handle);
	case VIRGL_OBJECT_SHADER: return create_shader(s, b, len, handle);
	case VIRGL_OBJECT_VERTEX_ELEMENTS: return create_velems(s, b, len, handle);
	case VIRGL_OBJECT_SAMPLER_VIEW: return create_sampler_view(s, b, len, handle);
	case VIRGL_OBJECT_SAMPLER_STATE: return create_sampler_state(s, b, len, handle);
	case VIRGL_OBJECT_SURFACE: return create_surface(s, b, len, handle, false);
	case VIRGL_OBJECT_MSAA_SURFACE: return create_surface(s, b, len, handle, true);
	case VIRGL_OBJECT_QUERY: return create_query(s, b, len, handle);
	case VIRGL_OBJECT_STREAMOUT_TARGET: return create_so_target(s, b, len, handle);
	}
	return EINVAL;
}

static int d_bind_object(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != 1)
		return EINVAL;
	uint32_t type = (b[0] >> 8) & 0xff;
	uint32_t handle = b[VIRGL_OBJ_BIND_HANDLE];
	struct vf_object *o = obj_lookup(s, handle, type);
	if (handle && !o) {
		vf_err("bind %s %u: no such object\n", type < VIRGL_MAX_OBJECTS ? obj_names[type] : "?", handle);
		return EINVAL;
	}
	struct pipe_context *pipe = s->pipe;
	switch (type) {
	case VIRGL_OBJECT_BLEND:
		pipe->bind_blend_state(pipe, o ? o->cso : NULL);
		return 0;
	case VIRGL_OBJECT_DSA:
		s->dsa = o ? o->dsa : NULL;
		pipe->bind_depth_stencil_alpha_state(pipe, o ? o->dsa->cso : NULL);
		return 0;
	case VIRGL_OBJECT_RASTERIZER:
		s->rs = o ? o->rs : NULL;
		pipe->bind_rasterizer_state(pipe, o ? o->rs->cso : NULL);
		return 0;
	case VIRGL_OBJECT_VERTEX_ELEMENTS:
		s->velems = o ? o->velems : NULL;
		s->vb_dirty = true;
		return 0;
	}
	return EINVAL;
}

static int d_destroy_object(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != 1)
		return EINVAL;
	uint32_t handle = b[VIRGL_OBJ_DESTROY_HANDLE];
	struct hash_entry *he = _mesa_hash_table_search(s->objects, (void *)(uintptr_t)handle);
	if (!he)
		return 0;
	struct vf_object *o = he->data;
	_mesa_hash_table_remove(s->objects, he);
	vf_object_destroy(s, o);
	return 0;
}

/* ---- state ---- */

static int d_set_viewport_state(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 1 || (len - 1) % 6)
		return EINVAL;
	unsigned n = (len - 1) / 6, start = b[VIRGL_SET_VIEWPORT_START_SLOT];
	if (n > PIPE_MAX_VIEWPORTS || start > PIPE_MAX_VIEWPORTS - n)
		return EINVAL;
	struct pipe_viewport_state vp[PIPE_MAX_VIEWPORTS];
	for (unsigned v = 0; v < n; v++) {
		for (unsigned i = 0; i < 3; i++) {
			vp[v].scale[i] = uif32(b[VIRGL_SET_VIEWPORT_STATE_SCALE_0(v) + i]);
			vp[v].translate[i] = uif32(b[VIRGL_SET_VIEWPORT_STATE_TRANSLATE_0(v) + i]);
		}
		vp[v].swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
		vp[v].swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
		vp[v].swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
		vp[v].swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;
	}
	/* (a faked viewport array: the device has max_viewports, the rest is accepted and dropped) */
	unsigned maxv = vf.screen->caps.max_viewports ? vf.screen->caps.max_viewports : 1;
	if (start >= maxv)
		return 0;
	n = MIN2(n, maxv - start);
	s->pipe->set_viewport_states(s->pipe, start, n, vp);
	return 0;
}

static void fb_size(struct pipe_framebuffer_state *fb)
{
	unsigned w = ~0u, h = ~0u;
	for (unsigned i = 0; i < fb->nr_cbufs; i++) {
		if (!fb->cbufs[i].texture)
			continue;
		w = MIN2(w, pipe_surface_width(&fb->cbufs[i]));
		h = MIN2(h, pipe_surface_height(&fb->cbufs[i]));
	}
	if (fb->zsbuf.texture) {
		w = MIN2(w, pipe_surface_width(&fb->zsbuf));
		h = MIN2(h, pipe_surface_height(&fb->zsbuf));
	}
	if (w != ~0u) {
		fb->width = w;
		fb->height = h;
	}
}

static int d_set_framebuffer_state(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 2)
		return EINVAL;
	uint32_t n = b[VIRGL_SET_FRAMEBUFFER_STATE_NR_CBUFS];
	if (n > PIPE_MAX_COLOR_BUFS || len != 2 + n)
		return EINVAL;
	struct pipe_framebuffer_state *fb = &s->fb;
	memset(fb, 0, sizeof(*fb));
	fb->nr_cbufs = n;
	for (unsigned i = 0; i < n; i++) {
		struct vf_object *o = obj_lookup(s, b[VIRGL_SET_FRAMEBUFFER_STATE_CBUF_HANDLE(i)],
						 VIRGL_OBJECT_SURFACE);
		if (o)
			fb->cbufs[i] = *o->surface;
	}
	struct vf_object *z = obj_lookup(s, b[VIRGL_SET_FRAMEBUFFER_STATE_NR_ZSURF_HANDLE], VIRGL_OBJECT_SURFACE);
	if (z)
		fb->zsbuf = *z->surface;
	fb_size(fb);
	s->pipe->set_framebuffer_state(s->pipe, fb);
	return 0;
}

static int d_set_framebuffer_state_no_attach(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_SET_FRAMEBUFFER_STATE_NO_ATTACH_SIZE)
		return EINVAL;
	struct pipe_framebuffer_state *fb = &s->fb;
	bool none = !fb->zsbuf.texture;
	for (unsigned i = 0; i < fb->nr_cbufs; i++)
		none = none && !fb->cbufs[i].texture;
	if (!none)
		return 0;	/* (the size comes from the attachments) */
	uint32_t wh = b[VIRGL_SET_FRAMEBUFFER_STATE_NO_ATTACH_WIDTH_HEIGHT];
	uint32_t ls = b[VIRGL_SET_FRAMEBUFFER_STATE_NO_ATTACH_LAYERS_SAMPLES];
	fb->width = VIRGL_SET_FRAMEBUFFER_STATE_NO_ATTACH_WIDTH(wh);
	fb->height = VIRGL_SET_FRAMEBUFFER_STATE_NO_ATTACH_HEIGHT(wh);
	fb->layers = VIRGL_SET_FRAMEBUFFER_STATE_NO_ATTACH_LAYERS(ls);
	fb->samples = VIRGL_SET_FRAMEBUFFER_STATE_NO_ATTACH_SAMPLES(ls);
	s->pipe->set_framebuffer_state(s->pipe, fb);
	return 0;
}

static int d_set_vertex_buffers(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len % 3)
		return EINVAL;
	unsigned n = len / 3;
	if (n > PIPE_MAX_ATTRIBS)
		return EINVAL;
	for (unsigned i = 0; i < n; i++) {
		s->vb[i].stride = b[VIRGL_SET_VERTEX_BUFFER_STRIDE(i)];
		s->vb[i].offset = b[VIRGL_SET_VERTEX_BUFFER_OFFSET(i)];
		s->vb[i].res = b[VIRGL_SET_VERTEX_BUFFER_HANDLE(i)];
	}
	s->num_vb = n;
	s->vb_dirty = true;
	return 0;
}

/* Vertex elements with the current strides, and the vertex buffers, before a draw. */
static void update_vertex_state(struct vf_sub *s)
{
	struct pipe_context *pipe = s->pipe;
	struct vf_velems *ve = s->velems;
	if (!ve) {
		if (s->velems_cso) {
			pipe->bind_vertex_elements_state(pipe, NULL);
			s->velems_cso = NULL;
		}
		pipe->set_vertex_buffers(pipe, 0, NULL);
		s->vb_dirty = false;
		return;
	}
	uint32_t strides[PIPE_MAX_ATTRIBS];
	for (unsigned i = 0; i < ve->num_buffers; i++)
		strides[i] = i < s->num_vb ? s->vb[i].stride : 0;
	void *cso = NULL;
	for (unsigned i = 0; i < VF_VE_CACHE; i++) {
		if (ve->cache[i].cso && !memcmp(ve->cache[i].strides, strides, ve->num_buffers * sizeof(uint32_t))) {
			cso = ve->cache[i].cso;
			break;
		}
	}
	if (!cso) {
		struct pipe_vertex_element e[PIPE_MAX_ATTRIBS];
		memcpy(e, ve->ve, ve->count * sizeof(e[0]));
		for (unsigned i = 0; i < ve->count; i++)
			e[i].src_stride = strides[e[i].vertex_buffer_index];
		unsigned slot = ve->cache_next;
		ve->cache_next = (slot + 1) % VF_VE_CACHE;
		if (ve->cache[slot].cso) {
			if (s->velems_cso == ve->cache[slot].cso) {
				pipe->bind_vertex_elements_state(pipe, NULL);
				s->velems_cso = NULL;
			}
			pipe->delete_vertex_elements_state(pipe, ve->cache[slot].cso);
		}
		cso = pipe->create_vertex_elements_state(pipe, ve->count, e);
		ve->cache[slot].cso = cso;
		memcpy(ve->cache[slot].strides, strides, ve->num_buffers * sizeof(uint32_t));
	}
	if (cso != s->velems_cso) {
		pipe->bind_vertex_elements_state(pipe, cso);
		s->velems_cso = cso;
	}
	struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
	memset(vb, 0, sizeof(vb));
	for (unsigned i = 0; i < ve->num_buffers; i++) {
		if (i >= s->num_vb)
			continue;
		/* set_vertex_buffers takes over these references */
		pipe_resource_reference(&vb[i].buffer.resource, vf_pres(s->vb[i].res));
		vb[i].buffer_offset = s->vb[i].offset;
	}
	pipe->set_vertex_buffers(pipe, ve->num_buffers, vb);
	s->vb_dirty = false;
}

static int d_set_index_buffer(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != 1 && len != 3)
		return EINVAL;
	s->ib.res = b[VIRGL_SET_INDEX_BUFFER_HANDLE];
	s->ib.index_size = len == 3 ? b[VIRGL_SET_INDEX_BUFFER_INDEX_SIZE] : 0;
	s->ib.offset = len == 3 ? b[VIRGL_SET_INDEX_BUFFER_OFFSET] : 0;
	return 0;
}

static int d_set_constant_buffer(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 2)
		return EINVAL;
	mesa_shader_stage st;
	if (!stage_of(b[VIRGL_SET_CONSTANT_BUFFER_SHADER_TYPE], &st))
		return EINVAL;
	if (len == 2) {
		s->pipe->set_constant_buffer(s->pipe, st, 0, NULL);
		return 0;
	}
	struct pipe_constant_buffer cb;
	memset(&cb, 0, sizeof(cb));
	cb.user_buffer = &b[VIRGL_SET_CONSTANT_BUFFER_DATA_START];
	cb.buffer_size = (len - 2) * 4;
	s->pipe->set_constant_buffer(s->pipe, st, 0, &cb);
	return 0;
}

static int d_set_uniform_buffer(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_SET_UNIFORM_BUFFER_SIZE)
		return EINVAL;
	mesa_shader_stage st;
	if (!stage_of(b[VIRGL_SET_UNIFORM_BUFFER_SHADER_TYPE], &st))
		return EINVAL;
	uint32_t index = b[VIRGL_SET_UNIFORM_BUFFER_INDEX];
	if (index >= PIPE_MAX_CONSTANT_BUFFERS)
		return EINVAL;
	struct pipe_resource *p = vf_pres(b[VIRGL_SET_UNIFORM_BUFFER_RES_HANDLE]);
	if (!p) {
		s->pipe->set_constant_buffer(s->pipe, st, index, NULL);
		return 0;
	}
	struct pipe_constant_buffer cb;
	memset(&cb, 0, sizeof(cb));
	cb.buffer = p;
	cb.buffer_offset = b[VIRGL_SET_UNIFORM_BUFFER_OFFSET];
	cb.buffer_size = b[VIRGL_SET_UNIFORM_BUFFER_LENGTH];
	s->pipe->set_constant_buffer(s->pipe, st, index, &cb);
	return 0;
}

static int d_set_sampler_views(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 2)
		return EINVAL;
	mesa_shader_stage st;
	if (!stage_of(b[VIRGL_SET_SAMPLER_VIEWS_SHADER_TYPE], &st))
		return EINVAL;
	unsigned n = len - 2, start = b[VIRGL_SET_SAMPLER_VIEWS_START_SLOT];
	if (n > PIPE_MAX_SHADER_SAMPLER_VIEWS || start > PIPE_MAX_SHADER_SAMPLER_VIEWS - n)
		return EINVAL;
	struct pipe_sampler_view *v[PIPE_MAX_SHADER_SAMPLER_VIEWS];
	for (unsigned i = 0; i < n; i++) {
		struct vf_object *o = obj_lookup(s, b[VIRGL_SET_SAMPLER_VIEWS_V0_HANDLE + i], VIRGL_OBJECT_SAMPLER_VIEW);
		v[i] = o ? o->view : NULL;
	}
	s->pipe->set_sampler_views(s->pipe, st, start, n, 0, v);
	return 0;
}

static int d_bind_sampler_states(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 2)
		return EINVAL;
	mesa_shader_stage st;
	if (!stage_of(b[VIRGL_BIND_SAMPLER_STATES_SHADER_TYPE], &st))
		return EINVAL;
	unsigned n = len - 2, start = b[VIRGL_BIND_SAMPLER_STATES_START_SLOT];
	if (n > PIPE_MAX_SAMPLERS || start > PIPE_MAX_SAMPLERS - n)
		return EINVAL;
	void *cso[PIPE_MAX_SAMPLERS];
	for (unsigned i = 0; i < n; i++) {
		struct vf_object *o = obj_lookup(s, b[VIRGL_BIND_SAMPLER_STATES_S0_HANDLE + i],
						 VIRGL_OBJECT_SAMPLER_STATE);
		cso[i] = o ? o->cso : NULL;
	}
	s->pipe->bind_sampler_states(s->pipe, st, start, n, cso);
	return 0;
}

static int d_set_stencil_ref(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_SET_STENCIL_REF_SIZE)
		return EINVAL;
	struct pipe_stencil_ref r;
	r.ref_value[0] = b[1] & 0xff;
	r.ref_value[1] = (b[1] >> 8) & 0xff;
	s->pipe->set_stencil_ref(s->pipe, r);
	return 0;
}

static int d_set_blend_color(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_SET_BLEND_COLOR_SIZE)
		return EINVAL;
	struct pipe_blend_color bc;
	for (unsigned i = 0; i < 4; i++)
		bc.color[i] = uif32(b[VIRGL_SET_BLEND_COLOR(i)]);
	s->pipe->set_blend_color(s->pipe, &bc);
	return 0;
}

static int d_set_scissor_state(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 1 || (len - 1) % 2)
		return EINVAL;
	unsigned n = (len - 1) / 2, start = b[VIRGL_SET_SCISSOR_START_SLOT];
	if (n > PIPE_MAX_VIEWPORTS || start > PIPE_MAX_VIEWPORTS - n)
		return EINVAL;
	struct pipe_scissor_state ss[PIPE_MAX_VIEWPORTS];
	for (unsigned i = 0; i < n; i++) {
		uint32_t mn = b[VIRGL_SET_SCISSOR_MINX_MINY(i)], mx = b[VIRGL_SET_SCISSOR_MAXX_MAXY(i)];
		ss[i].minx = mn & 0xffff;
		ss[i].miny = mn >> 16;
		ss[i].maxx = mx & 0xffff;
		ss[i].maxy = mx >> 16;
	}
	unsigned maxv = vf.screen->caps.max_viewports ? vf.screen->caps.max_viewports : 1;
	if (start >= maxv)
		return 0;
	n = MIN2(n, maxv - start);
	s->pipe->set_scissor_states(s->pipe, start, n, ss);
	return 0;
}

static int d_set_polygon_stipple(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_POLYGON_STIPPLE_SIZE)
		return EINVAL;
	struct pipe_poly_stipple ps;
	for (unsigned i = 0; i < 32; i++)
		ps.stipple[i] = b[VIRGL_POLYGON_STIPPLE_P0 + i];
	s->pipe->set_polygon_stipple(s->pipe, &ps);
	return 0;
}

static int d_set_clip_state(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_SET_CLIP_STATE_SIZE)
		return EINVAL;
	for (unsigned i = 0; i < 8; i++)
		for (unsigned j = 0; j < 4; j++)
			s->clip.ucp[i][j] = uif32(b[VIRGL_SET_CLIP_STATE_C0 + i * 4 + j]);
	/* zink has no user clip planes (clip_planes = 0): they go into the shaders (vf_shader.c) */
	return 0;
}

static int d_set_sample_mask(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_SET_SAMPLE_MASK_SIZE)
		return EINVAL;
	s->pipe->set_sample_mask(s->pipe, b[VIRGL_SET_SAMPLE_MASK_MASK]);
	return 0;
}

static int d_set_min_samples(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_SET_MIN_SAMPLES_SIZE)
		return EINVAL;
	if (s->pipe->set_min_samples)
		s->pipe->set_min_samples(s->pipe, MAX2(b[VIRGL_SET_MIN_SAMPLES_MASK], 1));
	return 0;
}

static int d_set_tess_state(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_TESS_STATE_SIZE)
		return EINVAL;
	float outer[4], inner[2];
	for (unsigned i = 0; i < 4; i++)
		outer[i] = uif32(b[1 + i]);
	for (unsigned i = 0; i < 2; i++)
		inner[i] = uif32(b[5 + i]);
	if (s->pipe->set_tess_state)
		s->pipe->set_tess_state(s->pipe, outer, inner);
	return 0;
}

static int d_set_streamout_targets(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 1 || len - 1 > PIPE_MAX_SO_BUFFERS)
		return EINVAL;
	unsigned n = len - 1;
	uint32_t append = b[VIRGL_SET_STREAMOUT_TARGETS_APPEND_BITMASK];
	struct pipe_stream_output_target *t[PIPE_MAX_SO_BUFFERS];
	unsigned offsets[PIPE_MAX_SO_BUFFERS];
	for (unsigned i = 0; i < n; i++) {
		struct vf_object *o = obj_lookup(s, b[VIRGL_SET_STREAMOUT_TARGETS_H0 + i],
						 VIRGL_OBJECT_STREAMOUT_TARGET);
		t[i] = o ? o->so_target : NULL;
		offsets[i] = (append & (1u << i)) ? (unsigned)-1 : 0;
	}
	for (unsigned i = 0; i < s->num_so_targets; i++)
		pipe_so_target_reference(&s->so_targets[i], NULL);
	for (unsigned i = 0; i < n; i++)
		pipe_so_target_reference(&s->so_targets[i], t[i]);
	s->num_so_targets = n;
	if (s->pipe->set_stream_output_targets)
		s->pipe->set_stream_output_targets(s->pipe, n, t, offsets, MESA_PRIM_UNKNOWN);
	return 0;
}

static int d_set_shader_buffers(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 2)
		return EINVAL;
	mesa_shader_stage st;
	if (!stage_of(b[VIRGL_SET_SHADER_BUFFER_SHADER_TYPE], &st))
		return EINVAL;
	unsigned n = (len - 2) / VIRGL_SET_SHADER_BUFFER_ELEMENT_SIZE, start = b[VIRGL_SET_SHADER_BUFFER_START_SLOT];
	if (!n)
		return 0;
	if (start > PIPE_MAX_SHADER_BUFFERS || n > PIPE_MAX_SHADER_BUFFERS - start)
		return EINVAL;
	struct pipe_shader_buffer sb[PIPE_MAX_SHADER_BUFFERS];
	for (unsigned i = 0; i < n; i++) {
		sb[i].buffer = vf_pres(b[VIRGL_SET_SHADER_BUFFER_RES_HANDLE(i)]);
		sb[i].buffer_offset = b[VIRGL_SET_SHADER_BUFFER_OFFSET(i)];
		sb[i].buffer_size = b[VIRGL_SET_SHADER_BUFFER_LENGTH(i)];
	}
	if (s->pipe->set_shader_buffers)
		s->pipe->set_shader_buffers(s->pipe, st, start, n, sb, ~0u);
	for (unsigned i = 0; i < n; i++)
		pipe_resource_reference(&s->ssbo_res[st][start + i], sb[i].buffer);
	return 0;
}

static int d_set_atomic_buffers(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 1)
		return EINVAL;
	unsigned n = (len - 1) / VIRGL_SET_ATOMIC_BUFFER_ELEMENT_SIZE, start = b[VIRGL_SET_ATOMIC_BUFFER_START_SLOT];
	if (!n)
		return 0;
	if (n > PIPE_MAX_HW_ATOMIC_BUFFERS || start > PIPE_MAX_HW_ATOMIC_BUFFERS - n)
		return EINVAL;
	if (!s->pipe->set_hw_atomic_buffers)
		return EINVAL;	/* (the caps say 0 hardware atomic counters, so the guest lowers them) */
	struct pipe_shader_buffer sb[PIPE_MAX_HW_ATOMIC_BUFFERS];
	for (unsigned i = 0; i < n; i++) {
		sb[i].buffer = vf_pres(b[VIRGL_SET_ATOMIC_BUFFER_RES_HANDLE(i)]);
		sb[i].buffer_offset = b[VIRGL_SET_ATOMIC_BUFFER_OFFSET(i)];
		sb[i].buffer_size = b[VIRGL_SET_ATOMIC_BUFFER_LENGTH(i)];
	}
	s->pipe->set_hw_atomic_buffers(s->pipe, start, n, sb);
	for (unsigned i = 0; i < n; i++)
		pipe_resource_reference(&s->atomic_res[start + i], sb[i].buffer);
	return 0;
}

static int d_set_shader_images(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 2)
		return EINVAL;
	mesa_shader_stage st;
	if (!stage_of(b[VIRGL_SET_SHADER_IMAGE_SHADER_TYPE], &st))
		return EINVAL;
	unsigned n = (len - 2) / VIRGL_SET_SHADER_IMAGE_ELEMENT_SIZE, start = b[VIRGL_SET_SHADER_IMAGE_START_SLOT];
	if (!n)
		return 0;
	if (start > PIPE_MAX_SHADER_IMAGES || n > PIPE_MAX_SHADER_IMAGES - start)
		return EINVAL;
	struct pipe_image_view iv[PIPE_MAX_SHADER_IMAGES];
	memset(iv, 0, sizeof(iv));
	for (unsigned i = 0; i < n; i++) {
		struct pipe_resource *p = vf_pres(b[VIRGL_SET_SHADER_IMAGE_RES_HANDLE(i)]);
		iv[i].resource = p;
		if (!p)
			continue;
		iv[i].format = vf_format(b[VIRGL_SET_SHADER_IMAGE_FORMAT(i)]);
		iv[i].access = b[VIRGL_SET_SHADER_IMAGE_ACCESS(i)];
		iv[i].shader_access = iv[i].access;
		uint32_t lo = b[VIRGL_SET_SHADER_IMAGE_LAYER_OFFSET(i)], ls = b[VIRGL_SET_SHADER_IMAGE_LEVEL_SIZE(i)];
		if (p->target == PIPE_BUFFER) {
			iv[i].u.buf.offset = lo;
			iv[i].u.buf.size = ls;
		} else {
			/* the guest sends the union pipe_image_view::u as two dwords */
			iv[i].u.tex.first_layer = lo & 0xffff;
			iv[i].u.tex.last_layer = lo >> 16;
			iv[i].u.tex.level = ls & 0xff;
			iv[i].u.tex.single_layer_view = (ls >> 8) & 1;
			iv[i].u.tex.is_2d_view_of_3d = (ls >> 16) & 1;
		}
	}
	if (s->pipe->set_shader_images)
		s->pipe->set_shader_images(s->pipe, st, start, n, 0, iv);
	for (unsigned i = 0; i < n; i++)
		pipe_resource_reference(&s->image_res[st][start + i], iv[i].resource);
	return 0;
}

/* ---- drawing ---- */

/* Debugging: VFRONT_DRAWDUMP=DIR:FIRST:LAST writes colour buffer 0 after each draw numbered FIRST..LAST (the
 * count of draws since vf_init) as DIR/draw-NNNNNN.ppm, waiting for the GPU each time. */
static void draw_dump(struct vf_sub *s)
{
	static int state = -1;
	static char dir[256];
	static unsigned long first, last;
	if (state < 0) {
		const char *e = getenv("VFRONT_DRAWDUMP");
		state = e && sscanf(e, "%255[^:]:%lu:%lu", dir, &first, &last) == 3;
	}
	unsigned long n = vf.stats.draws;
	if (!state || n < first || n > last || !s->fb.nr_cbufs || !s->fb.cbufs[0].texture)
		return;
	struct pipe_surface *sf = &s->fb.cbufs[0];
	unsigned sw = pipe_surface_width(sf), sh = pipe_surface_height(sf);
	struct pipe_context *pipe = s->pipe;
	struct pipe_transfer *xfer;
	struct pipe_box box;
	u_box_2d_zslice(0, 0, sf->first_layer, sw, sh, &box);
	const uint8_t *m = pipe->texture_map(pipe, sf->texture, sf->level, PIPE_MAP_READ, &box, &xfer);
	if (!m)
		return;
	char path[512];
	snprintf(path, sizeof(path), "%s/draw-%06lu.ppm", dir, n);
	FILE *o = fopen(path, "wb");
	if (o) {
		fprintf(o, "P6\n%u %u\n255\n", sw, sh);
		uint8_t *rgba = malloc(sw * 4), *rgb = malloc(sw * 3);
		for (unsigned y = 0; y < sh; y++) {
			util_format_unpack_rgba_8unorm_rect(sf->format, rgba, 0, m + y * xfer->stride, 0, sw, 1);
			for (unsigned x = 0; x < sw; x++)
				memcpy(rgb + x * 3, rgba + x * 4, 3);
			fwrite(rgb, 3, sw, o);
		}
		free(rgba);
		free(rgb);
		fclose(o);
	}
	pipe->texture_unmap(pipe, xfer);
}

static int d_draw_vbo(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_DRAW_VBO_SIZE && len != VIRGL_DRAW_VBO_SIZE_TESS && len != VIRGL_DRAW_VBO_SIZE_INDIRECT)
		return EINVAL;
	struct pipe_context *pipe = s->pipe;
	struct pipe_draw_info info;
	memset(&info, 0, sizeof(info));
	struct pipe_draw_start_count_bias draw;
	draw.start = b[VIRGL_DRAW_VBO_START];
	draw.count = b[VIRGL_DRAW_VBO_COUNT];
	info.mode = b[VIRGL_DRAW_VBO_MODE];
	bool indexed = b[VIRGL_DRAW_VBO_INDEXED];
	info.instance_count = b[VIRGL_DRAW_VBO_INSTANCE_COUNT];
	draw.index_bias = b[VIRGL_DRAW_VBO_INDEX_BIAS];
	info.start_instance = b[VIRGL_DRAW_VBO_START_INSTANCE];
	info.primitive_restart = !!b[VIRGL_DRAW_VBO_PRIMITIVE_RESTART];
	info.restart_index = b[VIRGL_DRAW_VBO_RESTART_INDEX];
	info.min_index = b[VIRGL_DRAW_VBO_MIN_INDEX];
	info.max_index = b[VIRGL_DRAW_VBO_MAX_INDEX];
	uint32_t count_from_so = b[VIRGL_DRAW_VBO_COUNT_FROM_SO];
	unsigned drawid = 0;
	if (info.mode >= MESA_PRIM_COUNT)
		return EINVAL;
	if (info.mode == MESA_PRIM_PATCHES && (vf.faked & VF_FAKE_TESS)) {
		static bool said;
		if (!said) {
			said = true;
			vf_log("a patch draw: tessellation is advertised but the device has none, draws skipped\n");
		}
		return 0;
	}
	if (len >= VIRGL_DRAW_VBO_SIZE_TESS) {
		uint32_t vpp = b[VIRGL_DRAW_VBO_VERTICES_PER_PATCH];
		drawid = b[VIRGL_DRAW_VBO_DRAWID];
		if (info.mode == MESA_PRIM_PATCHES && vpp && vpp != s->patch_vertices) {
			if (pipe->set_patch_vertices)
				pipe->set_patch_vertices(pipe, vpp);
			s->patch_vertices = vpp;
		}
	}
	struct pipe_draw_indirect_info ind;
	bool indirect = len == VIRGL_DRAW_VBO_SIZE_INDIRECT && b[VIRGL_DRAW_VBO_INDIRECT_HANDLE];
	if (indirect) {
		memset(&ind, 0, sizeof(ind));
		ind.buffer = vf_pres(b[VIRGL_DRAW_VBO_INDIRECT_HANDLE]);
		ind.offset = b[VIRGL_DRAW_VBO_INDIRECT_OFFSET];
		ind.stride = b[VIRGL_DRAW_VBO_INDIRECT_STRIDE];
		ind.draw_count = MAX2(b[VIRGL_DRAW_VBO_INDIRECT_DRAW_COUNT], 1);
		ind.indirect_draw_count_offset = b[VIRGL_DRAW_VBO_INDIRECT_DRAW_COUNT_OFFSET];
		ind.indirect_draw_count = vf_pres(b[VIRGL_DRAW_VBO_INDIRECT_DRAW_COUNT_HANDLE]);
		if (!ind.buffer)
			return EINVAL;
		if (!ind.stride)
			ind.stride = indexed ? 20 : 16;
	}
	if (indexed) {
		struct pipe_resource *ib = vf_pres(s->ib.res);
		if (!ib || !s->ib.index_size)
			return EINVAL;
		info.index_size = s->ib.index_size;
		info.index.resource = ib;
		/* Gallium has no index buffer offset: the guest put the first index there (vrend draws from
		 * ib.offset and ignores start), so it becomes start. Indirect draws have firstIndex in the
		 * command, relative to the buffer. */
		if (!indirect) {
			if (s->ib.offset % info.index_size) {
				vf_err("index buffer offset %u is not a multiple of the index size %u\n", s->ib.offset,
				       info.index_size);
				return EINVAL;
			}
			draw.start = s->ib.offset / info.index_size;
		}
		info.index_bounds_valid = !(info.min_index == 0 && info.max_index == ~0u);
		if (info.index_bounds_valid && info.max_index < info.min_index)
			info.index_bounds_valid = false;
	} else {
		draw.index_bias = 0;
		if (count_from_so && !indirect) {
			/* what vrend does with it: the count the guest computed from the target */
			draw.count = count_from_so;
			draw.start = 0;
		}
	}
	if (!indirect && (!draw.count || !info.instance_count))
		return 0;

	if (s->vb_dirty)
		update_vertex_state(s);
	vf_shaders_update(s, info.mode);
	/* a vertex shader that did not translate (zink draws nothing sensible without one, and may crash) */
	if (s->bound[MESA_SHADER_VERTEX] && !s->bound_cso[MESA_SHADER_VERTEX])
		return 0;

	/* zink only takes the fixed restart index (0xff.., its caps say emulate_nonfixed_primitive_restart) */
	if (info.primitive_restart && indexed &&
	    info.restart_index != (info.index_size == 1 ? 0xffu : info.index_size == 2 ? 0xffffu : 0xffffffffu)) {
		util_draw_vbo_without_prim_restart(pipe, &info, drawid, indirect ? &ind : NULL, &draw);
		vf.stats.draws++;
		return 0;
	}
	if (info.primitive_restart && !(vf.screen->caps.supported_prim_modes_with_restart & BITFIELD_BIT(info.mode)))
		info.primitive_restart = false;	/* (strips restart; lists are said to restart by having no restart) */
	pipe->draw_vbo(pipe, &info, drawid, indirect ? &ind : NULL, &draw, 1);
	vf.stats.draws++;
	draw_dump(s);
	return 0;
}

static int d_clear(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_OBJ_CLEAR_SIZE)
		return EINVAL;
	union pipe_color_union color;
	for (unsigned i = 0; i < 4; i++)
		color.ui[i] = b[VIRGL_OBJ_CLEAR_COLOR_0 + i];
	double depth;
	memcpy(&depth, &b[VIRGL_OBJ_CLEAR_DEPTH_0], sizeof(depth));
	s->pipe->clear(s->pipe, b[VIRGL_OBJ_CLEAR_BUFFERS], ~0u, 0xff, NULL, &color, depth, b[VIRGL_OBJ_CLEAR_STENCIL]);
	return 0;
}

static int d_clear_surface(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_CLEAR_SURFACE_SIZE)
		return EINVAL;
	uint32_t s0 = b[VIRGL_CLEAR_SURFACE_S0];
	bool cond = s0 & 1;
	unsigned buffers = (s0 >> 1) & 7;
	struct vf_object *o = obj_lookup(s, b[VIRGL_CLEAR_SURFACE_HANDLE], VIRGL_OBJECT_SURFACE);
	if (!o)
		return EINVAL;
	union pipe_color_union color;
	for (unsigned i = 0; i < 4; i++)
		color.ui[i] = b[VIRGL_CLEAR_SURFACE_COLOR_0 + i];
	unsigned x = b[VIRGL_CLEAR_SURFACE_DST_X], y = b[VIRGL_CLEAR_SURFACE_DST_Y];
	unsigned w = b[VIRGL_CLEAR_SURFACE_WIDTH], h = b[VIRGL_CLEAR_SURFACE_HEIGHT];
	if (buffers & PIPE_CLEAR_COLOR0) {
		s->pipe->clear_render_target(s->pipe, o->surface, &color, x, y, w, h, cond);
	} else {
		double depth;
		memcpy(&depth, color.ui, sizeof(depth));
		s->pipe->clear_depth_stencil(s->pipe, o->surface, buffers & PIPE_CLEAR_DEPTHSTENCIL, depth,
					     color.ui[3] & 0xff, x, y, w, h, cond);
	}
	return 0;
}

static int d_clear_texture(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_CLEAR_TEXTURE_SIZE)
		return EINVAL;
	struct pipe_resource *p = vf_pres(b[VIRGL_TEXTURE_HANDLE]);
	if (!p)
		return EINVAL;
	struct pipe_box box;
	u_box_3d(b[VIRGL_TEXTURE_SRC_X], b[VIRGL_TEXTURE_SRC_Y], b[VIRGL_TEXTURE_SRC_Z], b[VIRGL_TEXTURE_SRC_W],
		 b[VIRGL_TEXTURE_SRC_H], b[VIRGL_TEXTURE_SRC_D], &box);
	uint32_t data[4] = { b[VIRGL_TEXTURE_ARRAY_A], b[VIRGL_TEXTURE_ARRAY_B], b[VIRGL_TEXTURE_ARRAY_C],
			     b[VIRGL_TEXTURE_ARRAY_D] };
	if (!s->pipe->clear_texture)
		return EINVAL;
	s->pipe->clear_texture(s->pipe, p, b[VIRGL_TEXTURE_LEVEL], &box, data);
	return 0;
}

static int d_resource_copy_region(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_CMD_RESOURCE_COPY_REGION_SIZE)
		return EINVAL;
	struct pipe_resource *dst = vf_pres(b[VIRGL_CMD_RCR_DST_RES_HANDLE]);
	struct pipe_resource *src = vf_pres(b[VIRGL_CMD_RCR_SRC_RES_HANDLE]);
	if (!dst || !src)
		return EINVAL;
	struct pipe_box box;
	u_box_3d(b[VIRGL_CMD_RCR_SRC_X], b[VIRGL_CMD_RCR_SRC_Y], b[VIRGL_CMD_RCR_SRC_Z], b[VIRGL_CMD_RCR_SRC_W],
		 b[VIRGL_CMD_RCR_SRC_H], b[VIRGL_CMD_RCR_SRC_D], &box);
	s->pipe->resource_copy_region(s->pipe, dst, b[VIRGL_CMD_RCR_DST_LEVEL], b[VIRGL_CMD_RCR_DST_X],
				      b[VIRGL_CMD_RCR_DST_Y], b[VIRGL_CMD_RCR_DST_Z], src, b[VIRGL_CMD_RCR_SRC_LEVEL],
				      &box);
	return 0;
}

static int d_blit(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_CMD_BLIT_SIZE)
		return EINVAL;
	struct pipe_blit_info bi;
	memset(&bi, 0, sizeof(bi));
	uint32_t t = b[VIRGL_CMD_BLIT_S0];
	bi.mask = t & 0xff;
	bi.filter = (t >> 8) & 3;
	bi.scissor_enable = (t >> 10) & 1;
	bi.render_condition_enable = (t >> 11) & 1;
	bi.alpha_blend = (t >> 12) & 1;
	t = b[VIRGL_CMD_BLIT_SCISSOR_MINX_MINY];
	bi.scissor.minx = t & 0xffff;
	bi.scissor.miny = t >> 16;
	t = b[VIRGL_CMD_BLIT_SCISSOR_MAXX_MAXY];
	bi.scissor.maxx = t & 0xffff;
	bi.scissor.maxy = t >> 16;
	bi.dst.resource = vf_pres(b[VIRGL_CMD_BLIT_DST_RES_HANDLE]);
	bi.dst.level = b[VIRGL_CMD_BLIT_DST_LEVEL];
	bi.dst.format = vf_format(b[VIRGL_CMD_BLIT_DST_FORMAT]);
	u_box_3d(b[VIRGL_CMD_BLIT_DST_X], b[VIRGL_CMD_BLIT_DST_Y], b[VIRGL_CMD_BLIT_DST_Z], b[VIRGL_CMD_BLIT_DST_W],
		 b[VIRGL_CMD_BLIT_DST_H], b[VIRGL_CMD_BLIT_DST_D], &bi.dst.box);
	bi.src.resource = vf_pres(b[VIRGL_CMD_BLIT_SRC_RES_HANDLE]);
	bi.src.level = b[VIRGL_CMD_BLIT_SRC_LEVEL];
	bi.src.format = vf_format(b[VIRGL_CMD_BLIT_SRC_FORMAT]);
	u_box_3d((int)b[VIRGL_CMD_BLIT_SRC_X], (int)b[VIRGL_CMD_BLIT_SRC_Y], (int)b[VIRGL_CMD_BLIT_SRC_Z],
		 (int)b[VIRGL_CMD_BLIT_SRC_W], (int)b[VIRGL_CMD_BLIT_SRC_H], (int)b[VIRGL_CMD_BLIT_SRC_D],
		 &bi.src.box);
	if (!bi.dst.resource || !bi.src.resource)
		return EINVAL;
	s->pipe->blit(s->pipe, &bi);
	return 0;
}

static int inline_or_transfer_write(struct vf_sub *s, const uint32_t *b, const void *data, uint64_t size)
{
	struct vf_resource *r = vf_res_lookup(b[VIRGL_RESOURCE_IW_RES_HANDLE]);
	if (!r)
		return EINVAL;
	struct pipe_resource *p = r->pres;
	if (!p)
		return 0;
	uint32_t level = b[VIRGL_RESOURCE_IW_LEVEL];
	uint32_t stride = b[VIRGL_RESOURCE_IW_STRIDE], layer_stride = b[VIRGL_RESOURCE_IW_LAYER_STRIDE];
	struct pipe_box box;
	u_box_3d(b[VIRGL_RESOURCE_IW_X], b[VIRGL_RESOURCE_IW_Y], b[VIRGL_RESOURCE_IW_Z], b[VIRGL_RESOURCE_IW_W],
		 b[VIRGL_RESOURCE_IW_H], b[VIRGL_RESOURCE_IW_D], &box);
	if (p->target == PIPE_BUFFER) {
		if (box.width > size || (uint64_t)box.x + box.width > p->width0)
			return EINVAL;
		s->pipe->buffer_subdata(s->pipe, p, PIPE_MAP_WRITE, box.x, box.width, data);
		return 0;
	}
	if (!stride)
		stride = util_format_get_stride(p->format, u_minify(p->width0, level));
	if (!layer_stride)
		layer_stride = util_format_get_2d_size(p->format, stride, u_minify(p->height0, level));
	s->pipe->texture_subdata(s->pipe, p, level, PIPE_MAP_WRITE, &box, data, stride, layer_stride);
	return 0;
}

static int d_resource_inline_write(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < 12)
		return EINVAL;
	return inline_or_transfer_write(s, b, &b[VIRGL_RESOURCE_IW_DATA_START], (uint64_t)(len - 11) * 4);
}

static int d_transfer3d(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < VIRGL_TRANSFER3D_SIZE)
		return EINVAL;
	struct vf_resource *r = vf_res_lookup(b[VIRGL_RESOURCE_IW_RES_HANDLE]);
	if (!r)
		return EINVAL;
	struct vf_box box = { b[VIRGL_RESOURCE_IW_X], b[VIRGL_RESOURCE_IW_Y], b[VIRGL_RESOURCE_IW_Z],
			      b[VIRGL_RESOURCE_IW_W], b[VIRGL_RESOURCE_IW_H], b[VIRGL_RESOURCE_IW_D] };
	uint32_t level = b[VIRGL_RESOURCE_IW_LEVEL];
	uint32_t stride = b[VIRGL_RESOURCE_IW_STRIDE], layer_stride = b[VIRGL_RESOURCE_IW_LAYER_STRIDE];
	uint64_t offset = b[VIRGL_TRANSFER3D_DATA_OFFSET];
	switch (b[VIRGL_TRANSFER3D_DIRECTION]) {
	case VIRGL_TRANSFER_TO_HOST:
		vf.stats.puts++;
		return vf_put_from_backing(s, r, level, &box, stride, layer_stride, offset, r) ? EINVAL : 0;
	case VIRGL_TRANSFER_FROM_HOST:
		vf.stats.gets++;
		return vf_get_to_backing(s, r, level, &box, stride, layer_stride, offset, r) ? EINVAL : 0;
	}
	return EINVAL;
}

static int d_copy_transfer3d(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_COPY_TRANSFER3D_SIZE)
		return EINVAL;
	struct vf_resource *r = vf_res_lookup(b[VIRGL_RESOURCE_IW_RES_HANDLE]);
	struct vf_resource *src = vf_res_lookup(b[VIRGL_COPY_TRANSFER3D_SRC_RES_HANDLE]);
	if (!r || !src)
		return EINVAL;
	struct vf_box box = { b[VIRGL_RESOURCE_IW_X], b[VIRGL_RESOURCE_IW_Y], b[VIRGL_RESOURCE_IW_Z],
			      b[VIRGL_RESOURCE_IW_W], b[VIRGL_RESOURCE_IW_H], b[VIRGL_RESOURCE_IW_D] };
	uint32_t level = b[VIRGL_RESOURCE_IW_LEVEL];
	uint32_t stride = b[VIRGL_RESOURCE_IW_STRIDE], layer_stride = b[VIRGL_RESOURCE_IW_LAYER_STRIDE];
	uint64_t offset = b[VIRGL_COPY_TRANSFER3D_SRC_RES_OFFSET];
	vf.stats.puts++;
	if (b[VIRGL_COPY_TRANSFER3D_FLAGS] & VIRGL_COPY_TRANSFER3D_FLAGS_READ_FROM_HOST)
		return vf_get_to_backing(s, r, level, &box, stride, layer_stride, offset, src) ? EINVAL : 0;
	/* the data is in the staging resource's backing, laid out with the strides given */
	return vf_put_from_backing(s, r, level, &box, stride, layer_stride, offset, src) ? EINVAL : 0;
}

/* ---- queries ---- */

static int d_begin_query(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != 1)
		return EINVAL;
	struct vf_object *o = obj_lookup(s, b[VIRGL_QUERY_BEGIN_HANDLE], VIRGL_OBJECT_QUERY);
	if (!o)
		return EINVAL;
	s->pipe->begin_query(s->pipe, o->query->q);
	return 0;
}

static int d_end_query(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != 1)
		return EINVAL;
	struct vf_object *o = obj_lookup(s, b[VIRGL_QUERY_END_HANDLE], VIRGL_OBJECT_QUERY);
	if (!o)
		return EINVAL;
	s->pipe->end_query(s->pipe, o->query->q);
	return 0;
}

static int d_get_query_result(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	if (len != 2)
		return EINVAL;
	struct vf_object *o = obj_lookup(s, b[VIRGL_QUERY_RESULT_HANDLE], VIRGL_OBJECT_QUERY);
	if (!o)
		return EINVAL;
	/* The result goes into the query's resource when this submit's flush has completed: the guest
	 * waits for that (RESOURCE_BUSY_WAIT) before reading it. */
	struct vf_query_write qw;
	qw.query = o->query;
	qw.q = o->query->q;
	qw.res = o->query->res;
	qw.offset = o->query->offset;
	qw.type = o->query->type;
	qw.pipe = s->pipe;
	o->query->refs++;
	util_dynarray_append_typed(&c->queries, struct vf_query_write, qw);
	return 0;
}

static int d_get_query_result_qbo(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_QUERY_RESULT_QBO_SIZE)
		return EINVAL;
	struct vf_object *o = obj_lookup(s, b[VIRGL_QUERY_RESULT_QBO_HANDLE], VIRGL_OBJECT_QUERY);
	struct pipe_resource *qbo = vf_pres(b[VIRGL_QUERY_RESULT_QBO_QBO_HANDLE]);
	if (!o || !qbo)
		return EINVAL;
	if (!s->pipe->get_query_result_resource)
		return EINVAL;
	s->pipe->get_query_result_resource(s->pipe, o->query->q,
					   b[VIRGL_QUERY_RESULT_QBO_WAIT] ? PIPE_QUERY_WAIT : 0,
					   b[VIRGL_QUERY_RESULT_QBO_RESULT_TYPE],
					   (int)b[VIRGL_QUERY_RESULT_QBO_INDEX], qbo, b[VIRGL_QUERY_RESULT_QBO_OFFSET]);
	return 0;
}

static int d_set_render_condition(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_RENDER_CONDITION_SIZE)
		return EINVAL;
	struct vf_object *o = obj_lookup(s, b[VIRGL_RENDER_CONDITION_HANDLE], VIRGL_OBJECT_QUERY);
	s->render_cond = o ? o->query : NULL;
	s->pipe->render_condition(s->pipe, o ? o->query->q : NULL, b[VIRGL_RENDER_CONDITION_CONDITION] & 1,
				  b[VIRGL_RENDER_CONDITION_MODE]);
	return 0;
}

/* ---- sub-contexts, shaders, compute, barriers ---- */

static int d_set_sub_ctx(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)s;
	if (len != 1)
		return EINVAL;
	struct vf_sub *n = vf_sub_get(c, b[1], false);
	if (!n) {
		vf_err("context %u: no sub-context %u\n", c->id, b[1]);
		return 0;	/* (vrend ignores this too) */
	}
	c->sub = n;
	return 0;
}

static int d_create_sub_ctx(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)s;
	if (len != 1)
		return EINVAL;
	return vf_sub_get(c, b[1], true) ? 0 : ENOMEM;
}

static int d_destroy_sub_ctx(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)s;
	if (len != 1)
		return EINVAL;
	if (b[1] == 0)
		return 0;	/* sub-context 0 lives as long as the context */
	struct vf_sub *d = vf_sub_get(c, b[1], false);
	if (!d)
		return 0;
	bool current = c->sub == d;
	vf_sub_destroy(d);
	if (current)
		c->sub = vf_sub_get(c, 0, true);
	return 0;
}

static int d_bind_shader(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_BIND_SHADER_SIZE)
		return EINVAL;
	mesa_shader_stage st;
	if (!stage_of(b[VIRGL_BIND_SHADER_TYPE], &st))
		return EINVAL;
	uint32_t handle = b[VIRGL_BIND_SHADER_HANDLE];
	struct vf_object *o = obj_lookup(s, handle, VIRGL_OBJECT_SHADER);
	if (handle && !o)
		return EINVAL;
	if (o && o->shader->stage != st)
		return EINVAL;
	if (vf.debug & VF_DEBUG_CMD)
		vf_log("  bind %s shader %u\n", _mesa_shader_stage_to_string((mesa_shader_stage)st), handle);
	vf_shader_bind(s, st, o ? o->shader : NULL);
	return 0;
}

static int d_link_shader(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_LINK_SHADER_SIZE)
		return EINVAL;
	if (!s->pipe->link_shader)
		return 0;
	/* zink compiles the linked pipeline ahead of the first draw; only for the shaders as sent (key 0) */
	static const mesa_shader_stage order[] = { MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, MESA_SHADER_GEOMETRY,
						       MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL, MESA_SHADER_COMPUTE };
	/* (zink reads link_shader's array up to MESA_SHADER_MESH_STAGES: task and mesh too) */
	void *h[MESA_SHADER_MESH_STAGES] = { NULL };
	for (unsigned i = 0; i < 6; i++) {
		struct vf_object *o = obj_lookup(s, b[1 + i], VIRGL_OBJECT_SHADER);
		if (o && o->shader->variants)
			h[order[i]] = o->shader->variants->cso;
	}
	if (h[MESA_SHADER_VERTEX] && h[MESA_SHADER_FRAGMENT])
		s->pipe->link_shader(s->pipe, h);
	return 0;
}

static int d_launch_grid(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_LAUNCH_GRID_SIZE)
		return EINVAL;
	struct pipe_grid_info g;
	memset(&g, 0, sizeof(g));
	g.work_dim = 3;
	for (unsigned i = 0; i < 3; i++) {
		g.block[i] = b[VIRGL_LAUNCH_BLOCK_X + i];
		g.grid[i] = b[VIRGL_LAUNCH_GRID_X + i];
	}
	g.indirect = vf_pres(b[VIRGL_LAUNCH_INDIRECT_HANDLE]);
	g.indirect_offset = b[VIRGL_LAUNCH_INDIRECT_OFFSET];
	vf_shaders_update(s, MESA_PRIM_UNKNOWN);
	if (!s->pipe->launch_grid)
		return EINVAL;
	if (!s->bound_cso[MESA_SHADER_COMPUTE])
		return EINVAL;	/* (no compute shader, or one that did not translate: zink would crash) */
	s->pipe->launch_grid(s->pipe, &g);
	return 0;
}

static int d_memory_barrier(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_MEMORY_BARRIER_SIZE)
		return EINVAL;
	if (b[VIRGL_MEMORY_BARRIER_FLAGS] && s->pipe->memory_barrier)
		s->pipe->memory_barrier(s->pipe, PIPE_BARRIER_ALL);
	return 0;
}

static int d_texture_barrier(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len != VIRGL_TEXTURE_BARRIER_SIZE)
		return EINVAL;
	if (s->pipe->texture_barrier)
		s->pipe->texture_barrier(s->pipe, b[VIRGL_TEXTURE_BARRIER_FLAGS]);
	return 0;
}

static int d_nop(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	(void)s;
	(void)b;
	(void)len;
	return 0;
}

static int d_string_marker(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	if (len < VIRGL_SEND_STRING_MARKER_MIN_SIZE)
		return EINVAL;
	uint32_t n = b[VIRGL_SEND_STRING_MARKER_STRING_SIZE];
	if (n > (len - 1) * 4)
		return EINVAL;
	if (s->pipe->emit_string_marker)
		s->pipe->emit_string_marker(s->pipe, (const char *)&b[VIRGL_SEND_STRING_MARKER_OFFSET], n);
	return 0;
}

static int d_unsupported(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len)
{
	(void)c;
	(void)s;
	(void)len;
	uint32_t cmd = b[0] & 0xff;
	vf_err("%s is not supported (the caps do not offer it)\n", cmd < VIRGL_MAX_COMMANDS && cmd_names[cmd] ?
	       cmd_names[cmd] : "?");
	return EINVAL;
}

typedef int (*decode_fn)(struct vf_ctx *c, struct vf_sub *s, const uint32_t *b, uint32_t len);

static const decode_fn table[VIRGL_MAX_COMMANDS] = {
	[VIRGL_CCMD_NOP] = d_nop,
	[VIRGL_CCMD_CREATE_OBJECT] = d_create_object,
	[VIRGL_CCMD_BIND_OBJECT] = d_bind_object,
	[VIRGL_CCMD_DESTROY_OBJECT] = d_destroy_object,
	[VIRGL_CCMD_SET_VIEWPORT_STATE] = d_set_viewport_state,
	[VIRGL_CCMD_SET_FRAMEBUFFER_STATE] = d_set_framebuffer_state,
	[VIRGL_CCMD_SET_VERTEX_BUFFERS] = d_set_vertex_buffers,
	[VIRGL_CCMD_CLEAR] = d_clear,
	[VIRGL_CCMD_DRAW_VBO] = d_draw_vbo,
	[VIRGL_CCMD_RESOURCE_INLINE_WRITE] = d_resource_inline_write,
	[VIRGL_CCMD_SET_SAMPLER_VIEWS] = d_set_sampler_views,
	[VIRGL_CCMD_SET_INDEX_BUFFER] = d_set_index_buffer,
	[VIRGL_CCMD_SET_CONSTANT_BUFFER] = d_set_constant_buffer,
	[VIRGL_CCMD_SET_STENCIL_REF] = d_set_stencil_ref,
	[VIRGL_CCMD_SET_BLEND_COLOR] = d_set_blend_color,
	[VIRGL_CCMD_SET_SCISSOR_STATE] = d_set_scissor_state,
	[VIRGL_CCMD_BLIT] = d_blit,
	[VIRGL_CCMD_RESOURCE_COPY_REGION] = d_resource_copy_region,
	[VIRGL_CCMD_BIND_SAMPLER_STATES] = d_bind_sampler_states,
	[VIRGL_CCMD_BEGIN_QUERY] = d_begin_query,
	[VIRGL_CCMD_END_QUERY] = d_end_query,
	[VIRGL_CCMD_GET_QUERY_RESULT] = d_get_query_result,
	[VIRGL_CCMD_SET_POLYGON_STIPPLE] = d_set_polygon_stipple,
	[VIRGL_CCMD_SET_CLIP_STATE] = d_set_clip_state,
	[VIRGL_CCMD_SET_SAMPLE_MASK] = d_set_sample_mask,
	[VIRGL_CCMD_SET_STREAMOUT_TARGETS] = d_set_streamout_targets,
	[VIRGL_CCMD_SET_RENDER_CONDITION] = d_set_render_condition,
	[VIRGL_CCMD_SET_UNIFORM_BUFFER] = d_set_uniform_buffer,
	[VIRGL_CCMD_SET_SUB_CTX] = d_set_sub_ctx,
	[VIRGL_CCMD_CREATE_SUB_CTX] = d_create_sub_ctx,
	[VIRGL_CCMD_DESTROY_SUB_CTX] = d_destroy_sub_ctx,
	[VIRGL_CCMD_BIND_SHADER] = d_bind_shader,
	[VIRGL_CCMD_SET_TESS_STATE] = d_set_tess_state,
	[VIRGL_CCMD_SET_MIN_SAMPLES] = d_set_min_samples,
	[VIRGL_CCMD_SET_SHADER_BUFFERS] = d_set_shader_buffers,
	[VIRGL_CCMD_SET_SHADER_IMAGES] = d_set_shader_images,
	[VIRGL_CCMD_MEMORY_BARRIER] = d_memory_barrier,
	[VIRGL_CCMD_LAUNCH_GRID] = d_launch_grid,
	[VIRGL_CCMD_SET_FRAMEBUFFER_STATE_NO_ATTACH] = d_set_framebuffer_state_no_attach,
	[VIRGL_CCMD_TEXTURE_BARRIER] = d_texture_barrier,
	[VIRGL_CCMD_SET_ATOMIC_BUFFERS] = d_set_atomic_buffers,
	[VIRGL_CCMD_SET_DEBUG_FLAGS] = d_nop,
	[VIRGL_CCMD_GET_QUERY_RESULT_QBO] = d_get_query_result_qbo,
	[VIRGL_CCMD_TRANSFER3D] = d_transfer3d,
	[VIRGL_CCMD_END_TRANSFERS] = d_nop,
	[VIRGL_CCMD_COPY_TRANSFER3D] = d_copy_transfer3d,
	[VIRGL_CCMD_SET_TWEAKS] = d_nop,
	[VIRGL_CCMD_CLEAR_TEXTURE] = d_clear_texture,
	[VIRGL_CCMD_PIPE_RESOURCE_CREATE] = d_unsupported,
	[VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE] = d_unsupported,
	[VIRGL_CCMD_GET_MEMORY_INFO] = d_unsupported,
	[VIRGL_CCMD_EMIT_STRING_MARKER] = d_string_marker,
	[VIRGL_CCMD_LINK_SHADER] = d_link_shader,
	[VIRGL_CCMD_CREATE_VIDEO_CODEC] = d_unsupported,
	[VIRGL_CCMD_DESTROY_VIDEO_CODEC] = d_unsupported,
	[VIRGL_CCMD_CREATE_VIDEO_BUFFER] = d_unsupported,
	[VIRGL_CCMD_DESTROY_VIDEO_BUFFER] = d_unsupported,
	[VIRGL_CCMD_BEGIN_FRAME] = d_unsupported,
	[VIRGL_CCMD_DECODE_MACROBLOCK] = d_unsupported,
	[VIRGL_CCMD_DECODE_BITSTREAM] = d_unsupported,
	[VIRGL_CCMD_ENCODE_BITSTREAM] = d_unsupported,
	[VIRGL_CCMD_END_FRAME] = d_unsupported,
	[VIRGL_CCMD_CLEAR_SURFACE] = d_clear_surface,
};

/* Commands that do not need a sub-context to exist yet */
static bool needs_sub(uint32_t cmd)
{
	return cmd != VIRGL_CCMD_NOP && cmd != VIRGL_CCMD_CREATE_SUB_CTX && cmd != VIRGL_CCMD_SET_SUB_CTX &&
	       cmd != VIRGL_CCMD_DESTROY_SUB_CTX && cmd != VIRGL_CCMD_SET_DEBUG_FLAGS &&
	       cmd != VIRGL_CCMD_SET_TWEAKS && cmd != VIRGL_CCMD_END_TRANSFERS;
}

int vf_decode(struct vf_ctx *c, const uint32_t *dw, uint32_t ndw)
{
	uint32_t off = 0;
	while (off < ndw) {
		const uint32_t *b = &dw[off];
		uint32_t len = b[0] >> 16, cmd = b[0] & 0xff;
		if (cmd >= VIRGL_MAX_COMMANDS || !table[cmd]) {
			vf_err("context %u: unknown command %u at dword %u\n", c->id, cmd, off);
			vf.stats.failed++;
			return EINVAL;
		}
		if (off + 1 + len > ndw) {
			vf_err("context %u: %s at dword %u runs past the buffer\n", c->id, cmd_names[cmd], off);
			vf.stats.failed++;
			return EINVAL;
		}
		struct vf_sub *s = c->sub;
		if (needs_sub(cmd)) {
			if (!s)
				s = c->sub = vf_sub_get(c, 0, true);
			if (!s)
				return ENOMEM;
			vf_use_pipe(c, s->pipe);
		}
		if (vf.debug & VF_DEBUG_CMD)
			vf_log("ctx %u sub %u: %s len %u\n", c->id, s ? s->id : 0, cmd_names[cmd], len);
		int ret = table[cmd](c, s, b, len);
		vf.stats.commands++;
		if (ret) {
			/* vrend stops at the first failing command; so does this (the guest's state would be
			 * off for the rest anyway) */
			vf_err("context %u: %s failed (%d) at dword %u of %u\n", c->id, cmd_names[cmd], ret, off, ndw);
			vf.stats.failed++;
			return ret;
		}
		off += len + 1;
	}
	return 0;
}
