/*
 * WebGPU legalisation of NIR (track T3b): what Vulkan shaders do that WebGPU cannot, rewritten into what
 * the WGSL emitter (nir_to_wgsl.h) accepts. wgvk_compile() runs
 *
 *   wgvk_legalize_pre()   on NIR straight from spirv_to_nir (variables, derefs)
 *   wgsl_preprocess_nir() (T3a)
 *   wgvk_legalize_post()  on the preprocessed NIR (system value intrinsics, explicit buffer offsets,
 *                         lowered I/O), followed by a small clean-up
 *   nir_to_wgsl()         (T3a)
 *
 * Each pass is keyed by wgvk_shader_key and reports what the driver has to do for it in
 * struct wgvk_legalize_info (copied into wgvk_compiled_shader). INTERFACES.md section 6 is the contract.
 */
#ifndef WGVK_LEGALIZE_H
#define WGVK_LEGALIZE_H

#include "wgvk_compile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The driver set, driver bindings, lowered binding kinds and struct wgvk_legalize_info are in
 * wgvk_compile.h (they are part of the driver's contract). */

void wgvk_legalize_pre(nir_shader *s, const struct wgvk_shader_key *key, const struct wgvk_compile_caps *caps,
                       struct wgvk_legalize_info *info);
void wgvk_legalize_post(nir_shader *s, const struct wgvk_shader_key *key, const struct wgvk_compile_caps *caps,
                        struct wgvk_legalize_info *info);

/* A load from a driver uniform buffer (WGVK_DRIVER_SET), declaring it with at least `bytes`. */
nir_def *wgvk_load_driver_ubo(nir_builder *b, unsigned binding, unsigned bytes, unsigned nc, nir_def *offset);

/* The binding remap wgvk_compile hands the emitter: T1's layout (or group = set when there is none), plus
 * the driver set at (driver_group, binding). */
bool wgvk_remap_binding(const struct wgvk_layout_remap *layout, unsigned set, unsigned binding, unsigned index,
                        bool sampler, unsigned *group, unsigned *wb);

/* A layout remap built from the shader's own variables by INTERFACES 4.2's rules, as T1 would build it
 * from a descriptor set layout that declares exactly what the shader declares. For tests and the corpus;
 * the remap's arrays are allocated on mem_ctx. */
struct wgvk_layout_remap *wgvk_layout_remap_from_nir(nir_shader *s, void *mem_ctx);
/* ... from the union of several shaders' declarations (a pipeline's stages) */
struct wgvk_layout_remap *wgvk_layout_remap_from_nirs(nir_shader **shaders, unsigned n, void *mem_ctx);

#ifdef __cplusplus
}
#endif

#endif
