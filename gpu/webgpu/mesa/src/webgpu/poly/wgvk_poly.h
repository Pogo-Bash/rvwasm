/*
 * wgvk on Mesa's libpoly (track T4): see wgvk_poly.c.
 */
#ifndef WGVK_POLY_H
#define WGVK_POLY_H

#include "wgvk_compile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* libpoly's sysvals and global memory (64-bit segment:offset addresses) to wgvk's driver bindings. Runs in
 * wgvk_legalize_post; nothing to do for shaders that use none of it. */
void wgvk_poly_lower(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info);

/* key.vs_before_gs: a lowered vertex shader (store_output) becomes a compute shader (WGVK_POLY_WORKGROUP
 * wide) writing its outputs to libpoly's vertex output buffer. */
void wgvk_poly_vs_before_gs(nir_shader *s, struct wgvk_legalize_info *info);

/* A geometry shader (from spirv_to_nir) into libpoly's parts (wgvk_gs_part); parts[i] NULL when not
 * needed. Fills out's topology fields. Takes ownership of gs. */
bool wgvk_poly_lower_gs(nir_shader *gs, nir_shader *parts[WGVK_GS_PARTS], struct wgvk_gs_program *out);
/* libpoly's passthrough GS for transform feedback of a vertex shader (its nir_xfb_info, outputs) */
nir_shader *wgvk_poly_xfb_passthrough_gs(const nir_shader_compiler_options *opts, const void *xfb_info,
                                         uint64_t outputs, unsigned clip_size, unsigned prim);
nir_shader *wgvk_poly_prefix_sum_shader(const nir_shader_compiler_options *opts);
/* tessellation: the TCS to compute, the TES to the vertex shader that draws, the tessellator kernels
 * (which: 0 counts, 1 prefix sum + draw, 2 tessellate) */
void wgvk_poly_lower_tcs(nir_shader *s, struct wgvk_legalize_info *info);
void wgvk_poly_lower_tes(nir_shader *s, struct wgvk_legalize_info *info);
nir_shader *wgvk_poly_tess_kernel(const nir_shader_compiler_options *opts, unsigned domain, unsigned which);

/* A vertex shader from spirv_to_nir that stores to buffers or images (it must run as compute). */
bool wgvk_poly_vs_writes_memory(nir_shader *s);

/* The vertex shader that rasterizes a vertex shader run as compute (outputs: its legal.vs_outputs). */
nir_shader *wgvk_poly_passthrough_vs(const nir_shader_compiler_options *opts, uint64_t outputs, unsigned clip_size);

/* The compute shader behind wgvk_compile_gs_setup. */
nir_shader *wgvk_poly_gs_setup_shader(const nir_shader_compiler_options *opts);

#ifdef __cplusplus
}
#endif

#endif
