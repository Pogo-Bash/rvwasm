/*
 * The virgl caps (set 2) the guest is given, made from zink's own caps. The guest's virgl_screen.c turns these
 * back into pipe caps (virgl_init_screen_caps, virgl_get_shader_param, virgl_is_format_supported), so this is
 * that function inverted: for every cap the guest reads, the zink value it should end up with.
 *
 * What the guest takes for granted without asking (its virgl screen leaves Gallium's defaults: flat shading,
 * the alpha test, two-sided colour, user clip planes, GL_CLAMP) vfront does itself (vf_shader.c,
 * vf_decode.c).
 */
#include <stdlib.h>
#include <string.h>

#include "vf_priv.h"

#include "util/format/u_format.h"
#include "util/u_math.h"

static void set_bit(struct virgl_supported_format_mask *m, unsigned f)
{
	if (f < 32 * ARRAY_SIZE(m->bitmask))
		m->bitmask[f / 32] |= 1u << (f % 32);
}

static const struct pipe_shader_caps *sc(mesa_shader_stage st)
{
	return &vf.screen->shader_caps[st];
}

/* virgl's per-stage arrays are in virgl's stage order */
static const mesa_shader_stage virgl_stage[6] = {
	MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT, MESA_SHADER_GEOMETRY,
	MESA_SHADER_TESS_CTRL, MESA_SHADER_TESS_EVAL, MESA_SHADER_COMPUTE,
};

static void formats(union virgl_caps *caps)
{
	struct pipe_screen *ps = vf.screen;
	struct virgl_caps_v1 *v1 = &caps->v1;
	struct virgl_caps_v2 *v2 = &caps->v2;
	for (unsigned vfmt = 1; vfmt < VIRGL_FORMAT_MAX; vfmt++) {
		enum pipe_format f = vf_format(vfmt);
		if (f == PIPE_FORMAT_NONE)
			continue;
		const struct util_format_description *d = util_format_description(f);
		if (!d)
			continue;
		bool zs = d->colorspace == UTIL_FORMAT_COLORSPACE_ZS;
		enum pipe_texture_target t = PIPE_TEXTURE_2D;
		/* (the guest asks about RGB32, which it allows only for texture buffers, with this mask too) */
		if (ps->is_format_supported(ps, f, t, 0, 0, PIPE_BIND_SAMPLER_VIEW) ||
		    (zs && ps->is_format_supported(ps, f, t, 0, 0, PIPE_BIND_DEPTH_STENCIL)) ||
		    ((f == PIPE_FORMAT_R32G32B32_FLOAT || f == PIPE_FORMAT_R32G32B32_UINT ||
		      f == PIPE_FORMAT_R32G32B32_SINT) &&
		     ps->is_format_supported(ps, f, PIPE_BUFFER, 0, 0, PIPE_BIND_SAMPLER_VIEW))) {
			set_bit(&v1->sampler, vfmt);
			/* readbacks copy into a staging resource of the same format: what can be sampled can be
			 * read back */
			set_bit(&v2->supported_readback_formats, vfmt);
		}
		if (!zs && ps->is_format_supported(ps, f, t, 0, 0, PIPE_BIND_RENDER_TARGET))
			set_bit(&v1->render, vfmt);
		if (zs && ps->is_format_supported(ps, f, t, 0, 0, PIPE_BIND_DEPTH_STENCIL))
			set_bit(&v1->depthstencil, vfmt);
		/* (zink asserts on formats that cannot be vertex formats at all) */
		if (((d->layout == UTIL_FORMAT_LAYOUT_PLAIN && !zs && util_format_get_first_non_void_channel(f) == 0) ||
		     f == PIPE_FORMAT_R11G11B10_FLOAT) &&
		    ps->is_format_supported(ps, f, PIPE_BUFFER, 0, 0, PIPE_BIND_VERTEX_BUFFER))
			set_bit(&v1->vertexbuffer, vfmt);
		if (!zs && ps->is_format_supported(ps, f, t, 0, 0, PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET) &&
		    (f == PIPE_FORMAT_B8G8R8A8_UNORM || f == PIPE_FORMAT_B8G8R8X8_UNORM ||
		     f == PIPE_FORMAT_R8G8B8A8_UNORM || f == PIPE_FORMAT_R8G8B8X8_UNORM ||
		     f == PIPE_FORMAT_B5G6R5_UNORM || f == PIPE_FORMAT_R10G10B10A2_UNORM ||
		     f == PIPE_FORMAT_B10G10R10A2_UNORM))
			set_bit(&v2->scanout, vfmt);
		unsigned bind = zs ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET;
		if (ps->is_format_supported(ps, f, t, 4, 4, bind))
			set_bit(&v2->supported_multisample_formats, vfmt);
	}
	/* (for framebuffers without attachments) */
	set_bit(&v2->supported_multisample_formats, VIRGL_FORMAT_NONE);
}

static unsigned max_samples(void)
{
	struct pipe_screen *ps = vf.screen;
	unsigned best = 1;
	for (unsigned n = 2; n <= 32; n *= 2)
		if (ps->is_format_supported(ps, PIPE_FORMAT_R8G8B8A8_UNORM, PIPE_TEXTURE_2D, n, n,
					    PIPE_BIND_RENDER_TARGET))
			best = n;
	return best;
}

/* GL 4.3 core in the guest (Mesa's compute_version): 4.0 needs ARB_gpu_shader_fp64 and ARB_tessellation_shader,
 * 4.1 ARB_viewport_array (16 viewports) and ARB_vertex_attrib_64bit (fp64 again). wgvk offers none of the three
 * as Vulkan features (shaderFloat64, tessellationShader, multiViewport): its compiler lowers fp64 and runs
 * tessellation and viewport arrays through libpoly, but the pipelines do not carry them yet (STATUS.md T4). They
 * are therefore faked as design table section 3 defines it: advertised, implemented partly or not at all, and
 * logged. What a guest does with them: fp64 shaders go to zink and fail there (a draw whose shader did not
 * translate is skipped), tessellation shaders are dropped and patch draws skipped (vf_shader.c, vf_decode.c), and
 * only viewport and scissor 0 reach the device (vf_decode.c). Blender 4.3, glmark2 and the piglit GL 1.x-3.3
 * selection use none of them. VFRONT_CAPS=honest advertises what the device has (GL 3.3).
 * Each cap that cannot be honest is listed in STATUS.md (T6, GL 4.3 on wgvk). */
static void gl43_fakes(union virgl_caps *caps)
{
	struct pipe_screen *ps = vf.screen;
	const struct pipe_caps *pc = &ps->caps;
	struct virgl_caps_v1 *v1 = &caps->v1;
	struct virgl_caps_v2 *v2 = &caps->v2;
	const char *mode = getenv("VFRONT_CAPS");
	if (mode && !strcmp(mode, "honest"))
		return;
	if (!pc->doubles) {
		v1->bset.has_fp64 = 1;
		vf.faked |= VF_FAKE_FP64;
	}
	if (!v1->bset.has_tessellation_shaders) {
		v1->bset.has_tessellation_shaders = 1;
		vf.faked |= VF_FAKE_TESS;
		/* GL 4.0's minimums, taken from the vertex stage where the device has a number */
		v2->max_tcs_outputs = MAX2(v2->max_vertex_outputs, 32);
		v2->max_tes_outputs = MAX2(v2->max_vertex_outputs, 32);
		v2->max_shader_patch_varyings = MAX2(v2->max_shader_patch_varyings, 30);
		for (unsigned i = 3; i <= 4; i++) {
			v2->max_const_buffer_size[i] = v2->max_const_buffer_size[0];
			v2->max_shader_storage_blocks[i] = v2->max_shader_storage_blocks[0];
		}
	}
	if (v1->max_viewports < 16) {
		v1->max_viewports = 16;
		vf.faked |= VF_FAKE_VIEWPORTS;
	}
	if (vf.faked)
		vf_log("caps: faked for GL 4.3 (VFRONT_CAPS=honest leaves them out):%s%s%s\n",
		       vf.faked & VF_FAKE_FP64 ? " fp64" : "", vf.faked & VF_FAKE_TESS ? " tessellation" : "",
		       vf.faked & VF_FAKE_VIEWPORTS ? " viewports(16, only 0 is drawn with)" : "");
}

void vf_caps_fill(union virgl_caps *caps)
{
	struct pipe_screen *ps = vf.screen;
	const struct pipe_caps *pc = &ps->caps;
	struct virgl_caps_v1 *v1 = &caps->v1;
	struct virgl_caps_v2 *v2 = &caps->v2;
	memset(caps, 0, sizeof(*caps));

	v1->max_version = 2;

	/* ---- v1 ---- */
	v1->bset.indep_blend_enable = pc->indep_blend_enable;
	v1->bset.indep_blend_func = pc->indep_blend_func;
	v1->bset.cube_map_array = pc->cube_map_array;
	v1->bset.shader_stencil_export = pc->shader_stencil_export;
	v1->bset.conditional_render = pc->conditional_render;
	v1->bset.start_instance = pc->start_instance;
	v1->bset.primitive_restart = pc->primitive_restart;
	v1->bset.blend_eq_sep = pc->blend_equation_separate;
	v1->bset.instanceid = pc->vs_instanceid;
	v1->bset.vertex_element_instance_divisor = pc->vertex_element_instance_divisor;
	v1->bset.seamless_cube_map = pc->seamless_cube_map;
	v1->bset.occlusion_query = pc->occlusion_query;
	v1->bset.timer_query = pc->query_timestamp && pc->query_time_elapsed;
	v1->bset.streamout_pause_resume = pc->stream_output_pause_resume;
	v1->bset.texture_multisample = pc->texture_multisample;
	/* gl_FragCoord: zink (Vulkan) has only an upper-left origin and half-integer centres. Without this bit
	 * the guest's Gallium frontend turns a lower-left origin into upper-left + its own transform; integer
	 * centres it always believes the host has, so vfront subtracts the half pixel (vf_shader.c). */
	v1->bset.fragment_coord_conventions = pc->fs_coord_origin_lower_left;
	v1->bset.depth_clip_disable = pc->depth_clip_disable;
	v1->bset.seamless_cube_map_per_texture = pc->seamless_cube_map_per_texture;
	v1->bset.ubo = 1;
	v1->bset.color_clamping = 0;
	v1->bset.poly_stipple = 1;
	v1->bset.mirror_clamp = pc->texture_mirror_clamp_to_edge;
	v1->bset.texture_query_lod = pc->texture_query_lod;
	v1->bset.has_fp64 = pc->doubles;
	v1->bset.has_tessellation_shaders = sc(MESA_SHADER_TESS_CTRL)->max_instructions > 0;
	v1->bset.has_indirect_draw = pc->draw_indirect;
	v1->bset.has_sample_shading = pc->sample_shading;
	v1->bset.has_cull = pc->cull_distance;
	v1->bset.conditional_render_inverted = pc->conditional_render_inverted;
	v1->bset.derivative_control = pc->fs_fine_derivative;
	v1->bset.polygon_offset_clamp = pc->polygon_offset_clamp;
	v1->bset.transform_feedback_overflow_query = pc->query_so_overflow;

	v1->glsl_level = pc->glsl_feature_level;
	v1->max_texture_array_layers = pc->max_texture_array_layers;
	v1->max_streamout_buffers = pc->max_stream_output_buffers;
	v1->max_dual_source_render_targets = pc->max_dual_source_render_targets;
	v1->max_render_targets = pc->max_render_targets;
	v1->max_samples = max_samples();
	/* Primitives zink cannot draw the guest converts (util_primconvert); the restart support of each is
	 * handled here (vf_decode.c) */
	v1->prim_mask = pc->supported_prim_modes;
	v1->max_tbo_size = pc->max_texel_buffer_elements;
	v1->max_uniform_blocks = sc(MESA_SHADER_FRAGMENT)->max_const_buffers;
	v1->max_viewports = pc->max_viewports;
	v1->max_texture_gather_components = pc->max_texture_gather_components;

	/* ---- v2 ---- */
	v2->min_aliased_point_size = pc->min_point_size;
	v2->max_aliased_point_size = pc->max_point_size;
	v2->min_smooth_point_size = pc->min_point_size_aa;
	v2->max_smooth_point_size = pc->max_point_size_aa;
	v2->min_aliased_line_width = pc->min_line_width;
	v2->max_aliased_line_width = pc->max_line_width;
	v2->min_smooth_line_width = pc->min_line_width_aa;
	v2->max_smooth_line_width = pc->max_line_width_aa;
	v2->max_texture_lod_bias = pc->max_texture_lod_bias;
	v2->max_geom_output_vertices = pc->max_geometry_output_vertices;
	v2->max_geom_total_output_components = pc->max_geometry_total_output_components;
	v2->max_vertex_outputs = sc(MESA_SHADER_VERTEX)->max_outputs;
	v2->max_vertex_attribs = sc(MESA_SHADER_VERTEX)->max_inputs;
	v2->max_shader_patch_varyings = pc->max_shader_patch_varyings;
	v2->min_texel_offset = pc->min_texel_offset;
	v2->max_texel_offset = pc->max_texel_offset;
	v2->min_texture_gather_offset = pc->min_texture_gather_offset;
	v2->max_texture_gather_offset = pc->max_texture_gather_offset;
	v2->texture_buffer_offset_alignment = pc->texture_buffer_offset_alignment;
	v2->uniform_buffer_offset_alignment = pc->constant_buffer_offset_alignment;
	v2->shader_buffer_offset_alignment = pc->shader_buffer_offset_alignment;

	uint32_t cb = VIRGL_CAP_TGSI_INVARIANT | VIRGL_CAP_SET_MIN_SAMPLES | VIRGL_CAP_TGSI_PRECISE |
		      VIRGL_CAP_MEMORY_BARRIER | VIRGL_CAP_TRANSFER | VIRGL_CAP_COPY_TRANSFER |
		      VIRGL_CAP_INDIRECT_INPUT_ADDR | VIRGL_CAP_CLEAR_TEXTURE | VIRGL_CAP_TGSI_COMPONENTS;
	if (pc->sampler_view_target)
		cb |= VIRGL_CAP_TEXTURE_VIEW;
	if (pc->copy_between_compressed_and_plain_formats)
		cb |= VIRGL_CAP_COPY_IMAGE;
	if (pc->texture_query_samples)
		cb |= VIRGL_CAP_TXQS;
	if (pc->compute)
		cb |= VIRGL_CAP_COMPUTE_SHADER;
	if (pc->framebuffer_no_attachment)
		cb |= VIRGL_CAP_FB_NO_ATTACH;
	if (pc->robust_buffer_access_behavior)
		cb |= VIRGL_CAP_ROBUST_BUFFER_ACCESS;
	if (pc->shader_clock)
		cb |= VIRGL_CAP_SHADER_CLOCK;
	if (pc->texture_barrier)
		cb |= VIRGL_CAP_TEXTURE_BARRIER;
	if (pc->dest_surface_srgb_control)
		cb |= VIRGL_CAP_SRGB_WRITE_CONTROL;
	if (pc->query_buffer_object)
		cb |= VIRGL_CAP_QBO;
	if (pc->mixed_colorbuffer_formats)
		cb |= VIRGL_CAP_FBO_MIXED_COLOR_FORMATS;
	if (pc->draw_indirect)
		cb |= VIRGL_CAP_BIND_COMMAND_ARGS;
	if (pc->multi_draw_indirect)
		cb |= VIRGL_CAP_MULTI_DRAW_INDIRECT;
	if (pc->multi_draw_indirect_params)
		cb |= VIRGL_CAP_INDIRECT_PARAMS;
	if (pc->max_vertex_streams > 1)
		cb |= VIRGL_CAP_TRANSFORM_FEEDBACK3;
	if (pc->clip_halfz)
		cb |= VIRGL_CAP_CLIP_HALFZ;
	v2->capability_bits = cb;

	v2->max_vertex_attrib_stride = pc->max_vertex_attrib_stride;
	v2->max_shader_buffer_frag_compute = sc(MESA_SHADER_FRAGMENT)->max_shader_buffers;
	v2->max_shader_buffer_other_stages = sc(MESA_SHADER_VERTEX)->max_shader_buffers;
	v2->max_shader_image_frag_compute = sc(MESA_SHADER_FRAGMENT)->max_shader_images;
	v2->max_shader_image_other_stages = sc(MESA_SHADER_VERTEX)->max_shader_images;
	v2->max_image_samples = 0;

	if (pc->compute) {
		const struct pipe_compute_caps *cc = &ps->compute_caps;
		for (unsigned i = 0; i < 3; i++) {
			v2->max_compute_grid_size[i] = cc->max_grid_size[i];
			v2->max_compute_block_size[i] = cc->max_block_size[i];
		}
		v2->max_compute_work_group_invocations = cc->max_threads_per_block;
		v2->max_compute_shared_memory_size = cc->max_local_size;
	}

	v2->max_texture_2d_size = pc->max_texture_2d_size;
	v2->max_texture_3d_size = pc->max_texture_3d_levels ? 1u << (pc->max_texture_3d_levels - 1) : 0;
	v2->max_texture_cube_size = pc->max_texture_cube_levels ? 1u << (pc->max_texture_cube_levels - 1) : 0;
	v2->max_combined_shader_buffers = pc->max_combined_shader_buffers;
	/* no hardware atomic counters: the guest lowers them to storage buffers */
	v2->max_combined_atomic_counters = 0;
	v2->max_combined_atomic_counter_buffers = 0;

	/* What a guest Mesa checks before it relies on fixes and newer protocol (see vrend_renderer.c); at
	 * least 23, or the guest falls back to vtest protocol 2 (gpu/PROTOCOL.md). */
	v2->host_feature_check_version = 23;

	uint32_t cb2 = 0;
	if (pc->string_marker)
		cb2 |= VIRGL_CAP_V2_STRING_MARKER;
	if (pc->surface_sample_count)
		cb2 |= VIRGL_CAP_V2_IMPLICIT_MSAA;
	if (pc->texture_shadow_lod)
		cb2 |= VIRGL_CAP_V2_TEXTURE_SHADOW_LOD;
	if (pc->vs_layer_viewport)
		cb2 |= VIRGL_CAP_V2_VS_VERTEX_LAYER | VIRGL_CAP_V2_VS_VIEWPORT_INDEX;
	if (pc->query_pipeline_statistics_single || pc->query_pipeline_statistics)
		cb2 |= VIRGL_CAP_V2_PIPELINE_STATISTICS_QUERY;
	if (pc->draw_parameters)
		cb2 |= VIRGL_CAP_V2_DRAW_PARAMETERS;
	if (pc->shader_group_vote)
		cb2 |= VIRGL_CAP_V2_GROUP_VOTE;
	if (pc->texture_mirror_clamp_to_edge)
		cb2 |= VIRGL_CAP_V2_MIRROR_CLAMP_TO_EDGE;
	if (pc->texture_mirror_clamp)
		cb2 |= VIRGL_CAP_V2_MIRROR_CLAMP;
	v2->capability_bits_v2 = cb2;

	snprintf(v2->renderer, sizeof(v2->renderer), "%s", vf.renderer);
	/* (the guest refuses llvmpipe-like names for some paths; vrend hides "llvmpipe" the same way) */
	char *lp = strstr(v2->renderer, "llvmpipe");
	if (lp)
		memcpy(lp, "LLVMPIPE", 8);

	v2->max_anisotropy = MIN2(pc->max_texture_anisotropy, 16.0f);
	v2->max_texture_samplers = sc(MESA_SHADER_FRAGMENT)->max_texture_samplers;
	v2->max_uniform_block_size = pc->max_constant_buffer_size;
	v2->max_tcs_outputs = sc(MESA_SHADER_TESS_CTRL)->max_outputs;
	v2->max_tes_outputs = sc(MESA_SHADER_TESS_EVAL)->max_outputs;
	for (unsigned i = 0; i < 6; i++) {
		v2->max_const_buffer_size[i] = sc(virgl_stage[i])->max_const_buffer0_size;
		v2->max_shader_storage_blocks[i] = sc(virgl_stage[i])->max_shader_buffers;
		v2->max_atomic_counters[i] = 0;
		v2->max_atomic_counter_buffers[i] = 0;
	}
	v2->num_video_caps = 0;

	formats(caps);
	gl43_fakes(caps);

	if (vf.debug & VF_DEBUG_CAPS) {
		vf_log("caps: glsl %u, samples %u, prims 0x%x, ubos %u, tbo %u, streams %u, viewports %u, rts %u, "
		       "bits 0x%08x bits2 0x%08x, vertex attribs %u outputs %u, tess %u, 2d %u, 3d %u, cube %u, "
		       "layers %u, ssbo vs %u fs %u, images vs %u fs %u\n", v1->glsl_level, v1->max_samples,
		       v1->prim_mask, v1->max_uniform_blocks, v1->max_tbo_size, v1->max_streamout_buffers,
		       v1->max_viewports, v1->max_render_targets, v2->capability_bits, v2->capability_bits_v2,
		       v2->max_vertex_attribs, v2->max_vertex_outputs, v1->bset.has_tessellation_shaders,
		       v2->max_texture_2d_size, v2->max_texture_3d_size, v2->max_texture_cube_size,
		       v1->max_texture_array_layers, v2->max_shader_buffer_other_stages,
		       v2->max_shader_buffer_frag_compute, v2->max_shader_image_other_stages,
		       v2->max_shader_image_frag_compute);
		uint32_t b;
		memcpy(&b, &v1->bset, 4);
		vf_log("caps: bset 0x%08x\n", b);
	}
}
