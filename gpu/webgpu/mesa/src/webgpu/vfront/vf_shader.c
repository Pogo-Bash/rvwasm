/*
 * Shaders: the guest's TGSI text -> tokens (vf_tgsi_text.c) -> NIR (Mesa's tgsi_to_nir) -> zink.
 *
 * The guest's Gallium frontend believes the host does flat shading, the alpha test, two-sided colour, user
 * clip planes and a fixed point size (the virgl screen's caps say so, because vrend does them in GL). zink
 * leaves all of these to its frontend, which here is vfront: each is a NIR pass over a copy of the shader,
 * keyed by the state that needs it (struct vf_variant_key), the way Mesa's st makes shader variants.
 *
 * So the NIR kept per shader is the one before zink's finalize_nir; each variant is that NIR, the variant's
 * passes, then finalize_nir, then zink's create_*_state.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vf_priv.h"
#include "compiler/nir/nir_xfb_info.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nir/tgsi_to_nir.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_parse.h"
#include "util/u_memory.h"

bool vf_tgsi_text_translate(const char *text, struct tgsi_token *tokens, unsigned num_tokens);
nir_shader *vf_tgsi_to_nir(const void *tgsi_tokens, struct pipe_screen *screen);

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Made-up state tokens: Mesa's lowering passes read their parameters from state variables; these mark the
 * ones whose loads become constants of the variant (replace_state_vars). */
enum {
	VF_STATE_ALPHA_REF = 0x7f00,
	VF_STATE_CLIPPLANE,
	VF_STATE_POINT_SIZE,
};

static bool replace_state_load(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
	const struct vf_variant_key *key = data;
	if (in->intrinsic != nir_intrinsic_load_deref)
		return false;
	nir_variable *var = nir_intrinsic_get_var(in, 0);
	if (!var || var->data.mode != nir_var_uniform || !var->num_state_slots)
		return false;
	gl_state_index16 t0 = var->state_slots[0].tokens[0];
	b->cursor = nir_before_instr(&in->instr);
	nir_def *v;
	switch (t0) {
	case VF_STATE_ALPHA_REF:
		v = nir_imm_float(b, key->alpha_ref);
		break;
	case VF_STATE_POINT_SIZE:
		v = nir_imm_float(b, key->point_size);
		break;
	case VF_STATE_CLIPPLANE: {
		unsigned i = var->state_slots[0].tokens[1];
		v = nir_imm_vec4(b, key->ucp[i][0], key->ucp[i][1], key->ucp[i][2], key->ucp[i][3]);
		break;
	}
	default:
		return false;
	}
	if (v->num_components != in->def.num_components)
		v = nir_channels(b, v, nir_component_mask(in->def.num_components));
	nir_def_replace(&in->def, v);
	return true;
}

/* The state variables the passes made are constants of the variant. */
static void replace_state_vars(nir_shader *nir, const struct vf_variant_key *key)
{
	nir_shader_intrinsics_pass(nir, replace_state_load, nir_metadata_control_flow, (void *)key);
	nir_remove_dead_derefs(nir);
	nir_foreach_variable_with_modes_safe(var, nir, nir_var_uniform) {
		if (var->num_state_slots && var->state_slots[0].tokens[0] >= VF_STATE_ALPHA_REF &&
		    var->state_slots[0].tokens[0] <= VF_STATE_POINT_SIZE)
			exec_node_remove(&var->node);
	}
}

/* The passes below work on the shader's variables (Mesa 25's own ones want lowered IO, which zink then turns
 * back into variables): flat colour inputs, two-sided colour, the alpha test and a point size. */

static bool is_color_slot(int loc)
{
	return loc == VARYING_SLOT_COL0 || loc == VARYING_SLOT_COL1 || loc == VARYING_SLOT_BFC0 ||
	       loc == VARYING_SLOT_BFC1;
}

/* glShadeModel(GL_FLAT): colour inputs with no interpolation of their own are flat */
static bool lower_flatshade(nir_shader *nir)
{
	bool progress = false;
	nir_foreach_shader_in_variable(var, nir) {
		if (is_color_slot(var->data.location) && var->data.interpolation == INTERP_MODE_NONE) {
			var->data.interpolation = INTERP_MODE_FLAT;
			progress = true;
		}
	}
	if (progress)
		nir_shader_preserve_all_metadata(nir);
	return progress;
}

struct two_side {
	nir_variable *front[2], *back[2];
};

static bool two_side_load(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
	struct two_side *ts = data;
	if (in->intrinsic != nir_intrinsic_load_deref)
		return false;
	nir_variable *var = nir_intrinsic_get_var(in, 0);
	int i = var == ts->front[0] ? 0 : var == ts->front[1] ? 1 : -1;
	if (i < 0)
		return false;
	b->cursor = nir_after_instr(&in->instr);
	nir_def *back = nir_load_var(b, ts->back[i]);
	if (back->num_components != in->def.num_components)
		back = nir_channels(b, back, nir_component_mask(in->def.num_components));
	nir_def *face = nir_load_front_face(b, 1);
	nir_def *sel = nir_bcsel(b, face, &in->def, back);
	nir_def_rewrite_uses_after(&in->def, sel);
	return true;
}

/* GL two-sided lighting: back-facing primitives take the vertex shader's back colours (BCOLOR) */
static bool lower_two_side(nir_shader *nir)
{
	struct two_side ts = { { NULL } };
	nir_foreach_shader_in_variable(var, nir) {
		if (var->data.location == VARYING_SLOT_COL0)
			ts.front[0] = var;
		else if (var->data.location == VARYING_SLOT_COL1)
			ts.front[1] = var;
	}
	if (!ts.front[0] && !ts.front[1])
		return false;
	for (unsigned i = 0; i < 2; i++) {
		if (!ts.front[i])
			continue;
		nir_variable *v = nir_variable_create(nir, nir_var_shader_in, ts.front[i]->type,
						      i ? "vf_back_color1" : "vf_back_color0");
		v->data.location = VARYING_SLOT_BFC0 + i;
		v->data.interpolation = ts.front[i]->data.interpolation;
		v->data.driver_location = nir->num_inputs++;
		ts.back[i] = v;
	}
	return nir_shader_intrinsics_pass(nir, two_side_load, nir_metadata_control_flow, &ts);
}

/* the alpha test, which zink does not have: discard when colour 0's alpha fails the comparison */
static bool lower_alpha_test(nir_shader *nir, unsigned func, float ref)
{
	nir_variable *out = NULL;
	nir_foreach_shader_out_variable(var, nir) {
		if (var->data.location == FRAG_RESULT_COLOR || var->data.location == FRAG_RESULT_DATA0) {
			out = var;
			break;
		}
	}
	if (!out)
		return false;
	nir_function_impl *impl = nir_shader_get_entrypoint(nir);
	nir_builder b = nir_builder_at(nir_after_impl(impl));
	nir_def *c = nir_load_var(&b, out);
	nir_def *a = c->num_components >= 4 ? nir_channel(&b, c, 3) : nir_imm_float(&b, 1.0f);
	if (func == PIPE_FUNC_NEVER) {
		nir_terminate(&b);
	} else {
		nir_def *pass = nir_compare_func(&b, (enum compare_func)func, a, nir_imm_float(&b, ref));
		nir_terminate_if(&b, nir_inot(&b, pass));
	}
	nir_progress(true, impl, nir_metadata_none);
	return true;
}

static bool psiz_before_emit(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
	if (in->intrinsic != nir_intrinsic_emit_vertex && in->intrinsic != nir_intrinsic_emit_vertex_with_counter)
		return false;
	const struct vf_variant_key *key = data;
	nir_variable *var = nir_find_variable_with_location(b->shader, nir_var_shader_out, VARYING_SLOT_PSIZ);
	b->cursor = nir_before_instr(&in->instr);
	nir_store_var(b, var, nir_imm_float(b, key->point_size), 1);
	return true;
}

/* glPointSize without a shader that writes gl_PointSize: write it */
static bool add_point_size(nir_shader *nir, const struct vf_variant_key *key)
{
	nir_variable *var = nir_variable_create(nir, nir_var_shader_out, glsl_float_type(), "vf_point_size");
	var->data.location = VARYING_SLOT_PSIZ;
	var->data.driver_location = nir->num_outputs++;
	if (nir->info.stage == MESA_SHADER_GEOMETRY)
		return nir_shader_intrinsics_pass(nir, psiz_before_emit, nir_metadata_control_flow, (void *)key);
	nir_function_impl *impl = nir_shader_get_entrypoint(nir);
	nir_builder b = nir_builder_at(nir_after_impl(impl));
	nir_store_var(&b, var, nir_imm_float(&b, key->point_size), 1);
	nir_progress(true, impl, nir_metadata_control_flow);
	return true;
}

/* Vertex inputs fetched as integers (vf_velems: 32-bit SSCALED/USCALED) that the shader reads as floats:
 * the variable becomes an integer vector and every load converts. */
static bool int_attribs_to_float(nir_shader *nir, const struct vf_variant_key *key)
{
	bool progress = false;
	uint32_t sint = key->vs_sint | key->vs_snorm, uint = key->vs_uint | key->vs_unorm;
	nir_foreach_shader_in_variable(var, nir) {
		int a = var->data.location - VERT_ATTRIB_GENERIC0;
		if (a < 0 || a >= 32 || !((sint | uint) >> a & 1))
			continue;
		bool sgn = sint >> a & 1;
		const struct glsl_type *old = var->type;
		if (glsl_get_base_type(old) != GLSL_TYPE_FLOAT || !glsl_type_is_vector_or_scalar(old))
			continue;
		var->type = glsl_vector_type(sgn ? GLSL_TYPE_INT : GLSL_TYPE_UINT, glsl_get_vector_elements(old));
		nir_foreach_function_impl(impl, nir) {
			nir_builder b = nir_builder_create(impl);
			nir_foreach_block(block, impl) {
				nir_foreach_instr_safe(instr, block) {
					if (instr->type == nir_instr_type_deref) {
						nir_deref_instr *d = nir_instr_as_deref(instr);
						if (d->deref_type == nir_deref_type_var && d->var == var)
							d->type = var->type;
						continue;
					}
					if (instr->type != nir_instr_type_intrinsic)
						continue;
					nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
					if (in->intrinsic != nir_intrinsic_load_deref || nir_intrinsic_get_var(in, 0) != var)
						continue;
					b.cursor = nir_after_instr(instr);
					nir_def *f = sgn ? nir_i2f32(&b, &in->def) : nir_u2f32(&b, &in->def);
					/* (GL's normalized 32-bit integers: c / (2^31 - 1) clamped to -1, c / (2^32 - 1)) */
					if (key->vs_snorm >> a & 1)
						f = nir_fmax(&b, nir_fmul_imm(&b, f, 1.0 / 2147483647.0), nir_imm_float(&b, -1.0f));
					else if (key->vs_unorm >> a & 1)
						f = nir_fmul_imm(&b, f, 1.0 / 4294967295.0);
					nir_def_rewrite_uses_after(&in->def, f);
					progress = true;
				}
			}
			nir_progress(true, impl, nir_metadata_control_flow);
		}
	}
	return progress;
}

/* The fragment shader's gl_ClipDistance[] input made as long as what the previous stage writes (the
 * guest's TGSI says only how many registers, 4 distances each): elements past the end read 0. */
static bool resize_clipdist_input(nir_shader *nir, unsigned len)
{
	nir_variable *var = NULL;
	nir_foreach_shader_in_variable(v, nir)
		if (v->data.compact && v->data.location == VARYING_SLOT_CLIP_DIST0)
			var = v;
	if (!var || !glsl_type_is_array(var->type) || glsl_get_length(var->type) == len)
		return false;
	var->type = glsl_array_type(glsl_float_type(), len, 0);
	nir_foreach_function_impl(impl, nir) {
		nir_builder b = nir_builder_create(impl);
		nir_foreach_block(block, impl) {
			nir_foreach_instr_safe(instr, block) {
				if (instr->type == nir_instr_type_deref) {
					nir_deref_instr *d = nir_instr_as_deref(instr);
					if (d->deref_type == nir_deref_type_var && d->var == var)
						d->type = var->type;
					continue;
				}
				if (instr->type != nir_instr_type_intrinsic)
					continue;
				nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
				if (in->intrinsic != nir_intrinsic_load_deref || nir_intrinsic_get_var(in, 0) != var)
					continue;
				nir_deref_instr *d = nir_src_as_deref(in->src[0]);
				if (d->deref_type != nir_deref_type_array || !nir_src_is_const(d->arr.index) ||
				    nir_src_as_uint(d->arr.index) < len)
					continue;
				b.cursor = nir_before_instr(instr);
				nir_def_replace(&in->def, nir_imm_zero(&b, in->def.num_components, in->def.bit_size));
			}
		}
		nir_progress(true, impl, nir_metadata_control_flow);
	}
	nir->info.clip_distance_array_size = len;
	NIR_PASS(_, nir, nir_opt_dce);
	return true;
}

static void apply_key(struct vf_shader *sh, nir_shader *nir, const struct vf_variant_key *key)
{
	bool state_vars = false, changed = false;
	if (sh->stage == MESA_SHADER_FRAGMENT) {
		if (key->fs_clipdist)
			NIR_PASS(changed, nir, resize_clipdist_input, key->fs_clipdist);
		if (key->two_side)
			NIR_PASS(changed, nir, lower_two_side);
		if (key->flatshade)
			NIR_PASS(changed, nir, lower_flatshade);
		if (key->alpha_func)
			NIR_PASS(changed, nir, lower_alpha_test, key->alpha_func - 1, key->alpha_ref);
	} else {
		if (key->ucp_enables) {
			gl_state_index16 tokens[PIPE_MAX_CLIP_PLANES][STATE_LENGTH];
			memset(tokens, 0, sizeof(tokens));
			for (unsigned i = 0; i < PIPE_MAX_CLIP_PLANES; i++) {
				tokens[i][0] = VF_STATE_CLIPPLANE;
				tokens[i][1] = i;
			}
			if (sh->stage == MESA_SHADER_GEOMETRY) {
				NIR_PASS(state_vars, nir, nir_lower_clip_gs, key->ucp_enables, nir->options->compact_arrays, tokens);
			} else {
				NIR_PASS(state_vars, nir, nir_lower_clip_vs, key->ucp_enables, true, nir->options->compact_arrays, tokens);
				NIR_PASS(changed, nir, nir_lower_io_vars_to_temporaries, nir_shader_get_entrypoint(nir),
					 nir_var_shader_out);
				NIR_PASS(changed, nir, nir_lower_global_vars_to_local);
				/* (the temporaries come back to the outputs as copy_deref of whole variables, and
				 * vars_to_ssa takes no copies of arrays: gl_ClipDistance[] itself, or a TGSI output
				 * ARRAY. user-clip-all-planes and fpexceptions took the server down without this) */
				NIR_PASS(changed, nir, nir_split_var_copies);	/* (whole arrays into wildcards) */
				NIR_PASS(changed, nir, nir_lower_var_copies);
				NIR_PASS(changed, nir, nir_lower_vars_to_ssa);
			}
		}
		if (key->psiz)
			NIR_PASS(changed, nir, add_point_size, key);
		if (sh->stage == MESA_SHADER_VERTEX && (key->vs_sint | key->vs_uint | key->vs_snorm | key->vs_unorm))
			NIR_PASS(changed, nir, int_attribs_to_float, key);
	}
	if (state_vars)
		replace_state_vars(nir, key);
	if (changed || state_vars)
		nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

static bool center_load(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
	(void)data;
	bool frag_coord = in->intrinsic == nir_intrinsic_load_frag_coord;
	if (in->intrinsic == nir_intrinsic_load_deref) {
		nir_variable *var = nir_intrinsic_get_var(in, 0);
		frag_coord = var && var->data.mode == nir_var_shader_in && var->data.location == VARYING_SLOT_POS;
	}
	if (!frag_coord)
		return false;
	b->cursor = nir_after_instr(&in->instr);
	nir_def *c = &in->def;
	nir_def *xy = nir_fadd_imm(b, nir_channels(b, c, 0x3), -0.5f);
	nir_def *v = nir_vec4(b, nir_channel(b, xy, 0), nir_channel(b, xy, 1), nir_channel(b, c, 2),
			      nir_channel(b, c, 3));
	nir_def_rewrite_uses_after(c, v);
	return true;
}

/* The guest asks for integer pixel centres when its program does (it believes the host has them); zink has
 * half-integer ones only: gl_FragCoord.xy - 0.5 is the same thing. */
static void lower_pixel_center_integer(nir_shader *nir)
{
	if (nir->info.stage != MESA_SHADER_FRAGMENT || !nir->info.fs.pixel_center_integer ||
	    vf.screen->caps.fs_coord_pixel_center_integer)
		return;	/* (a driver with integer centres takes the property as it is) */
	nir_shader_intrinsics_pass(nir, center_load, nir_metadata_control_flow, NULL);
	nir->info.fs.pixel_center_integer = false;
}

/* Transform feedback. NIR drivers on Mesa main (zink) take it from the IO intrinsics' xfb info, not from
 * pipe_shader_state.stream_output (TGSI's), and zink reads it only from IO-lowered shaders: so the guest's
 * stream output table becomes nir->xfb_info (TGSI output registers -> varying slots), and the shader is
 * IO-lowered here as radeonsi does for its TGSI shaders (nir_lower_io_passes, VS inputs kept in place). */
static void add_xfb(nir_shader *nir, const struct pipe_stream_output_info *so)
{
	nir_xfb_info *x = rzalloc_size(nir, nir_xfb_info_size(so->num_outputs));
	for (unsigned b = 0; b < PIPE_MAX_SO_BUFFERS; b++) {
		x->buffers[b].stride = so->stride[b] * 4;
		nir->info.xfb_stride[b] = so->stride[b];
	}
	for (unsigned i = 0; i < so->num_outputs; i++) {
		const struct pipe_stream_output *o = &so->output[i];
		int loc = -1;
		nir_foreach_shader_out_variable(var, nir) {
			unsigned slots = glsl_count_attribute_slots(var->type, false);
			if (var->data.compact)
				slots = DIV_ROUND_UP(glsl_get_length(var->type), 4);
			if (o->register_index >= var->data.driver_location &&
			    o->register_index < var->data.driver_location + slots) {
				loc = var->data.location + (o->register_index - var->data.driver_location);
				break;
			}
		}
		if (loc < 0)
			continue;
		nir_xfb_output_info *out = &x->outputs[x->output_count++];
		out->buffer = o->output_buffer;
		out->offset = o->dst_offset * 4;
		out->location = loc;
		out->component_offset = o->start_component;
		out->component_mask = BITFIELD_RANGE(o->start_component, o->num_components);
		x->buffers_written |= 1u << o->output_buffer;
		x->buffers[o->output_buffer].varying_count++;
		x->buffer_to_stream[o->output_buffer] = o->stream;
		x->streams_written |= 1u << o->stream;
	}
	if (!x->output_count) {
		/* (nothing the shader writes is captured: no xfb info at all, which NIR requires then) */
		ralloc_free(x);
		return;
	}
	nir->xfb_info = x;
	nir->info.has_transform_feedback_varyings = true;
	nir_lower_io_passes(nir, false);
}

static void *make_cso(struct vf_sub *s, struct vf_shader *sh, nir_shader *nir)
{
	if (sh->so.num_outputs && sh->stage != MESA_SHADER_COMPUTE && sh->stage != MESA_SHADER_FRAGMENT &&
	    !nir->info.io_lowered)
		add_xfb(nir, &sh->so);
	struct pipe_context *pipe = s->pipe;
	if (vf.screen->finalize_nir)
		vf.screen->finalize_nir(vf.screen, nir, true);
	if (vf.debug & VF_DEBUG_SHADER)
		nir_print_shader(nir, stderr);
	if (sh->stage == MESA_SHADER_COMPUTE) {
		struct pipe_compute_state cs;
		memset(&cs, 0, sizeof(cs));
		cs.ir_type = PIPE_SHADER_IR_NIR;
		cs.prog = nir;
		cs.static_shared_mem = sh->req_local_mem;
		return pipe->create_compute_state(pipe, &cs);
	}
	struct pipe_shader_state ss;
	memset(&ss, 0, sizeof(ss));
	ss.type = PIPE_SHADER_IR_NIR;
	ss.ir.nir = nir;
	ss.stream_output = sh->so;
	switch (sh->stage) {
	case MESA_SHADER_VERTEX: return pipe->create_vs_state(pipe, &ss);
	case MESA_SHADER_FRAGMENT: return pipe->create_fs_state(pipe, &ss);
	case MESA_SHADER_GEOMETRY: return pipe->create_gs_state(pipe, &ss);
	case MESA_SHADER_TESS_CTRL: return pipe->create_tcs_state(pipe, &ss);
	case MESA_SHADER_TESS_EVAL: return pipe->create_tes_state(pipe, &ss);
	default: return NULL;
	}
}

static void delete_cso(struct pipe_context *pipe, mesa_shader_stage stage, void *cso)
{
	switch (stage) {
	case MESA_SHADER_VERTEX: pipe->delete_vs_state(pipe, cso); break;
	case MESA_SHADER_FRAGMENT: pipe->delete_fs_state(pipe, cso); break;
	case MESA_SHADER_GEOMETRY: pipe->delete_gs_state(pipe, cso); break;
	case MESA_SHADER_TESS_CTRL: pipe->delete_tcs_state(pipe, cso); break;
	case MESA_SHADER_TESS_EVAL: pipe->delete_tes_state(pipe, cso); break;
	case MESA_SHADER_COMPUTE: pipe->delete_compute_state(pipe, cso); break;
	default: break;
	}
}

static void bind_cso(struct pipe_context *pipe, mesa_shader_stage stage, void *cso)
{
	switch (stage) {
	case MESA_SHADER_VERTEX: pipe->bind_vs_state(pipe, cso); break;
	case MESA_SHADER_FRAGMENT: pipe->bind_fs_state(pipe, cso); break;
	case MESA_SHADER_GEOMETRY: pipe->bind_gs_state(pipe, cso); break;
	case MESA_SHADER_TESS_CTRL: pipe->bind_tcs_state(pipe, cso); break;
	case MESA_SHADER_TESS_EVAL: pipe->bind_tes_state(pipe, cso); break;
	case MESA_SHADER_COMPUTE: pipe->bind_compute_state(pipe, cso); break;
	default: break;
	}
}

void *vf_shader_cso(struct vf_sub *s, struct vf_shader *sh, const struct vf_variant_key *key)
{
	for (struct vf_variant *v = sh->variants; v; v = v->next)
		if (!memcmp(&v->key, key, sizeof(*key)))
			return v->cso;
	if (!sh->nir)
		return NULL;
	uint64_t t0 = now_ns();
	nir_shader *nir = nir_shader_clone(NULL, sh->nir);
	apply_key(sh, nir, key);
	void *cso = make_cso(s, sh, nir);
	vf.stats.shader_ns += now_ns() - t0;
	if (!cso) {
		vf_err("zink did not take a %s shader\n", _mesa_shader_stage_to_string((mesa_shader_stage)sh->stage));
		vf.stats.shader_failed++;
		return NULL;
	}
	struct vf_variant *v = calloc(1, sizeof(*v));
	v->key = *key;
	v->cso = cso;
	/* keep key 0 first: LINK_SHADER uses it */
	if (sh->variants) {
		v->next = sh->variants->next;
		sh->variants->next = v;
	} else {
		sh->variants = v;
	}
	return cso;
}

int vf_shader_create(struct vf_sub *s, struct vf_shader *sh, const char *text)
{
	vf.stats.shaders++;
	if ((vf.faked & VF_FAKE_TESS) && (sh->stage == MESA_SHADER_TESS_CTRL || sh->stage == MESA_SHADER_TESS_EVAL)) {
		/* (advertised without the device: kept as an object, never given to zink) */
		static bool said;
		if (!said) {
			said = true;
			vf_log("a tessellation shader: advertised but the device has none, the shader is dropped\n");
		}
		return 0;
	}
	uint64_t t0 = now_ns();
	if (vf.debug & VF_DEBUG_SHADER)
		vf_log("TGSI:\n%s\n", text);
	unsigned n = MAX2(sh->num_tokens + 64, 512);
	struct tgsi_token *tokens = NULL;
	bool ok = false;
	for (int tries = 0; tries < 6 && !ok; tries++, n *= 2) {
		free(tokens);
		tokens = calloc(n, sizeof(*tokens));
		ok = vf_tgsi_text_translate(text, tokens, n);
	}
	if (!ok) {
		vf_err("TGSI text did not parse:\n%s\n", text);
		free(tokens);
		sh->failed = true;
		vf.stats.shader_failed++;
		return EINVAL;
	}
	/* NIR before zink's finalize_nir, so variants can still be made from it (finalize_nir runs per variant) */
	sh->nir = vf_tgsi_to_nir(tokens, vf.screen);
	free(tokens);
	if (sh->nir && getenv("VFRONT_PRINT_TTN")) {
		fprintf(stderr, "vfront: NIR as translated from TGSI:\n");
		nir_print_shader(sh->nir, stderr);
	}
	/* shared memory (TGSI MEMORY) is addressed in bytes; its size comes with the shader object */
	if (sh->nir && sh->nir->info.stage == MESA_SHADER_COMPUTE)
		sh->nir->info.shared_size = MAX2(sh->nir->info.shared_size, sh->req_local_mem);
	if (sh->nir && sh->nir->info.stage == MESA_SHADER_FRAGMENT) {
		/* Without a lower-left origin in the caps, the guest's frontend already transformed gl_FragCoord
		 * for the driver's upper-left one (nir_lower_wpos_ytransform) and left the property as the
		 * program had it; drivers without lower-left ignore it, and so must zink. */
		if (!vf.caps.v1.bset.fragment_coord_conventions)
			sh->nir->info.fs.origin_upper_left = true;
		lower_pixel_center_integer(sh->nir);
	}
	vf.stats.shader_ns += now_ns() - t0;
	if (!sh->nir) {
		vf_err("a %s shader that cannot be translated:\n%s\n",
		       _mesa_shader_stage_to_string((mesa_shader_stage)sh->stage), text);
		sh->failed = true;
		vf.stats.shader_failed++;
		return EINVAL;
	}
	/* the shader as sent, made now (most shaders never need another variant) */
	struct vf_variant_key key;
	memset(&key, 0, sizeof(key));
	return vf_shader_cso(s, sh, &key) ? 0 : EINVAL;
}

void vf_shader_unref(struct vf_sub *s, struct vf_shader *sh)
{
	if (--sh->refs > 0)
		return;
	for (struct vf_variant *v = sh->variants, *next; v; v = next) {
		next = v->next;
		delete_cso(s->pipe, sh->stage, v->cso);
		free(v);
	}
	ralloc_free(sh->nir);
	free(sh->text);
	free(sh);
}

void vf_shader_bind(struct vf_sub *s, mesa_shader_stage stage, struct vf_shader *sh)
{
	struct vf_shader *old = s->bound[stage];
	if (old == sh)
		return;
	if (sh)
		sh->refs++;
	s->bound[stage] = sh;
	/* the variant is chosen at the next draw (vf_shaders_update); until then the shader as sent */
	void *cso = sh && sh->variants ? sh->variants->cso : NULL;
	bind_cso(s->pipe, stage, cso);
	s->bound_cso[stage] = cso;
	if (old)
		vf_shader_unref(s, old);
}

static bool reads_color(nir_shader *nir)
{
	return nir->info.inputs_read & (VARYING_BIT_COL0 | VARYING_BIT_COL1);
}

static bool is_points(enum mesa_prim mode, const struct pipe_rasterizer_state *rs)
{
	if (mode == MESA_PRIM_POINTS)
		return true;
	return rs && (rs->fill_front == PIPE_POLYGON_MODE_POINT || rs->fill_back == PIPE_POLYGON_MODE_POINT);
}

/* Before a draw (or dispatch): bind the variant each bound shader needs for the current state. */
void vf_shaders_update(struct vf_sub *s, enum mesa_prim mode)
{
	const struct pipe_rasterizer_state *rs = s->rs ? &s->rs->st : NULL;
	mesa_shader_stage last = s->bound[MESA_SHADER_GEOMETRY] ? MESA_SHADER_GEOMETRY :
				     s->bound[MESA_SHADER_TESS_EVAL] ? MESA_SHADER_TESS_EVAL : MESA_SHADER_VERTEX;
	for (unsigned st = 0; st < MESA_SHADER_STAGES; st++) {
		struct vf_shader *sh = s->bound[st];
		if (!sh || !sh->nir)
			continue;
		struct vf_variant_key key;
		memset(&key, 0, sizeof(key));
		if (st == MESA_SHADER_FRAGMENT) {
			if (s->dsa && s->dsa->st.alpha_enabled && s->dsa->st.alpha_func != PIPE_FUNC_ALWAYS) {
				key.alpha_func = s->dsa->st.alpha_func + 1;
				key.alpha_ref = s->dsa->st.alpha_ref_value;
			}
			struct vf_shader *prev = s->bound[MESA_SHADER_GEOMETRY] ? s->bound[MESA_SHADER_GEOMETRY] :
						 s->bound[MESA_SHADER_TESS_EVAL] ? s->bound[MESA_SHADER_TESS_EVAL] :
						 s->bound[MESA_SHADER_VERTEX];
			if (prev && prev->nir && prev->nir->info.clip_distance_array_size &&
			    (sh->nir->info.inputs_read & (VARYING_BIT_CLIP_DIST0 | VARYING_BIT_CLIP_DIST1)))
				key.fs_clipdist = prev->nir->info.clip_distance_array_size;
			if (rs && reads_color(sh->nir)) {
				key.flatshade = rs->flatshade;
				key.two_side = rs->light_twoside;
			}
		}
		if (st == MESA_SHADER_VERTEX && s->velems) {
			key.vs_sint = s->velems->sint_mask;
			key.vs_uint = s->velems->uint_mask;
			key.vs_snorm = s->velems->snorm_mask;
			key.vs_unorm = s->velems->unorm_mask;
		}
		if (st != MESA_SHADER_FRAGMENT && st == last && mode != MESA_PRIM_UNKNOWN) {
			uint64_t written = sh->nir->info.outputs_written;
			if (rs && rs->clip_plane_enable &&
			    !(written & (VARYING_BIT_CLIP_DIST0 | VARYING_BIT_CLIP_DIST1))) {
				key.ucp_enables = rs->clip_plane_enable;
				for (unsigned i = 0; i < PIPE_MAX_CLIP_PLANES; i++)
					if (key.ucp_enables & (1u << i))
						memcpy(key.ucp[i], s->clip.ucp[i], sizeof(key.ucp[i]));
			}
			/* zink sets size 1 itself when nothing is written (PIPE_POINT_SIZE_LOWER_USER_ONLY) */
			if (rs && !rs->point_size_per_vertex && rs->point_size != 1.0f && is_points(mode, rs) &&
			    !(written & VARYING_BIT_PSIZ)) {
				key.psiz = 1;
				key.point_size = rs->point_size;
			}
		}
		void *cso = vf_shader_cso(s, sh, &key);
		if (cso && cso != s->bound_cso[st]) {
			bind_cso(s->pipe, st, cso);
			s->bound_cso[st] = cso;
		}
	}
}
