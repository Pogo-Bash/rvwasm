/*
 * NIR -> WGSL: the shader back end of wgvk (Vulkan on WebGPU).
 *
 * Two steps, both here:
 *   wgsl_preprocess_nir()  the lowering every shader needs before emission, the part that is not
 *                          WebGPU legalisation: inlining, continue constructs, I/O to load_input/
 *                          store_output, explicit buffer offsets, system values, texture fix-ups,
 *                          the optimisation loop. Legalisation passes (track T3b) run before it.
 *   nir_to_wgsl()          prints WGSL from the result and lists every resource binding it declared.
 *
 * What the emitter accepts (the contract with the legalisation passes):
 *   - one entry point, no calls, structured control flow (if/loop/break/continue), SSA with phis;
 *   - UBO and SSBO access as load_ubo/load_ssbo/store_ssbo/ssbo_atomic* with a vec2 index
 *     (x = buffer slot from wgsl_lower_vk_resources, y = array element, constant) and a byte offset;
 *   - push constants as load_push_constant with a byte offset;
 *   - textures, samplers and storage images as derefs of nir_var_uniform / nir_var_image variables
 *     (a whole variable, or a constant array element when a binding remap is given);
 *   - shared, private and function memory as derefs of variables of those modes;
 *   - shader I/O as load_input / load_interpolated_input / store_output (nir_lower_io, vec4 slots);
 *   - 32-bit integers and floats, 1-bit booleans, 16-bit floats when shader-f16 is on.
 * Anything else is reported, not guessed: the result carries one tagged reason per construct
 * (see wgsl_result.error), and the WGSL is not to be used.
 */
#ifndef NIR_TO_WGSL_H
#define NIR_TO_WGSL_H

#include <stdbool.h>
#include <stdint.h>
#include "nir.h"

#ifdef __cplusplus
extern "C" {
#endif

enum wgsl_binding_kind {
   WGSL_BINDING_UNIFORM_BUFFER,
   WGSL_BINDING_STORAGE_BUFFER,          /* read-write */
   WGSL_BINDING_READONLY_STORAGE_BUFFER,
   WGSL_BINDING_TEXTURE,
   WGSL_BINDING_STORAGE_TEXTURE,
   WGSL_BINDING_SAMPLER,
   WGSL_BINDING_COMPARISON_SAMPLER,
};

enum wgsl_sample_type {
   WGSL_SAMPLE_FLOAT,
   WGSL_SAMPLE_SINT,
   WGSL_SAMPLE_UINT,
   WGSL_SAMPLE_DEPTH,
};

/* One resource variable of the WGSL module. */
struct wgsl_binding {
   enum wgsl_binding_kind kind;
   unsigned group, binding;             /* WGSL @group/@binding */
   unsigned vk_set, vk_binding, vk_index; /* the Vulkan descriptor it came from (array element) */
   unsigned vk_array_size;              /* elements the shader declares at (vk_set, vk_binding) */
   bool from_combined;                  /* the sampler half of a combined image sampler */
   /* textures and storage textures */
   enum glsl_sampler_dim dim;
   bool arrayed, multisampled;
   enum wgsl_sample_type sample_type;
   enum pipe_format format;             /* storage textures */
   unsigned access;                     /* storage textures: ACCESS_NON_READABLE / ACCESS_NON_WRITEABLE */
   /* buffers */
   uint32_t min_size;                   /* bytes the shader declares (WebGPU's minBindingSize) */
   bool atomic;                         /* declared as array<atomic<u32>> */
};

/*
 * Binding remap: where a Vulkan (set, binding, array element) goes in WGSL. T1 owns the policy;
 * NULL means group = set, binding = binding, element 0 only, and the sampler half of a combined
 * image sampler at binding + combined_sampler_offset.
 */
typedef bool (*wgsl_remap_cb)(void *data, unsigned set, unsigned binding, unsigned index,
                              bool sampler_of_combined, unsigned *group, unsigned *wgsl_binding);

#define WGSL_MAX_VARYINGS 32

struct wgsl_options {
   wgsl_remap_cb remap;
   void *remap_data;
   unsigned combined_sampler_offset;    /* default remap only; 0 means 500 */

   /* WebGPU features the device has (they decide the enable directives and what is allowed) */
   bool shader_f16;
   bool subgroups;
   bool clip_distances;
   bool dual_source_blending;
   bool primitive_index;
   unsigned immediate_size;             /* bytes of immediates (push constants); 0: none */

   /* Vulkan's clip space has y down, WebGPU's y up: vertex shaders negate gl_Position.y on the way out.
    * The front face stays as Vulkan says (draw-run: gl_FrontFacing agrees with lavapipe). Set this when
    * the pipeline flips instead (a Vulkan viewport with negative height, which WebGPU cannot express,
    * is the usual reason). */
   bool keep_clip_y;

   /* Vertex stage: how the fragment stage reads each location (WebGPU wants the two stages to
    * agree; Vulkan only looks at the fragment side). 0 = take the vertex shader's own. */
   uint8_t varying_interp[WGSL_MAX_VARYINGS];  /* INTERP_MODE_* + 1 */
   uint8_t varying_sampling[WGSL_MAX_VARYINGS]; /* 0 own, 1 center, 2 centroid, 3 sample */
   uint8_t fs_out_type[8];                   /* fragment stage: the type of each colour output, 0 own, 1 f32,
                                                 2 i32, 3 u32 (the target's: Vulkan lets a shader write float bits
                                                 to an integer attachment, WebGPU wants the types to agree) */
   uint8_t varying_type[WGSL_MAX_VARYINGS];   /* 0 own, 1 f32, 2 i32, 3 u32, 4 f16; a location the vertex
                                                 shader does not write but the fragment shader reads is
                                                 declared (and left zero) when this is set */
};

struct wgsl_result {
   char *wgsl;                          /* the module, or NULL when error is set */
   char *error;                         /* "[tag] detail\n" per unsupported construct, or NULL */
   struct wgsl_binding *bindings;
   unsigned num_bindings;
   unsigned workgroup_size[3];
   unsigned push_constant_size;         /* bytes read through immediates */
   uint8_t fs_varyings[32];             /* fragment shaders: per location, WGVK_VARYING_* (wgvk_compile.h) */
};

extern const nir_shader_compiler_options wgsl_nir_options;
extern const struct spirv_to_nir_options wgsl_spirv_options;

/* Lower vulkan_resource_index / reindex / load_vulkan_descriptor to vec3(slot, element, 0), the
 * index format the emitter reads. slot = set << 16 | binding. */
bool wgsl_lower_vk_resources(nir_shader *s);

void wgsl_preprocess_nir(nir_shader *s, const struct wgsl_options *o);

/* Returns true when result->wgsl was produced. Everything is allocated on mem_ctx. */
bool nir_to_wgsl(nir_shader *s, const struct wgsl_options *o, void *mem_ctx,
                 struct wgsl_result *result);

#ifdef __cplusplus
}
#endif

#endif
