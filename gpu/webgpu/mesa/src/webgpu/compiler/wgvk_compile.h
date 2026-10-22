/*
 * wgvk_compile: NIR in, WGSL + the bindings it declares out (gpu/webgpu/INTERFACES.md, section 6; T3 owns it).
 *
 *   SPIR-V --spirv_to_nir(wgvk_spirv_options(), wgvk_nir_options())--> NIR   (Mesa's vk runtime does this)
 *   NIR --legalisation (T3b)--> --wgsl_preprocess_nir--> --nir_to_wgsl--> WGSL
 *
 * A graphics pipeline compiles its fragment shader first, or at least calls wgvk_gather_fs_varyings() on
 * it, and passes the result to the vertex shader's key: WebGPU wants both sides of every inter-stage
 * location to agree on type and interpolation, Vulkan only reads the fragment side.
 */
#ifndef WGVK_COMPILE_H
#define WGVK_COMPILE_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#include "nir.h"
#include "nir_to_wgsl.h"

#ifdef __cplusplus
extern "C" {
#endif

struct wgvk_device;

/* T1's binding table (INTERFACES 4.2): include/wgvk_remap.h. */
#include "wgvk_remap.h"

/* What of the WebGPU device changes the WGSL. T1 fills it from the device's features and limits. */
struct wgvk_compile_caps {
   bool shader_f16;
   bool subgroups;
   bool clip_distances;
   bool dual_source_blending;
   bool primitive_index;
   uint32_t max_immediate_size;      /* bytes; 64 in Chrome */
   uint32_t max_storage_buffers;     /* per stage; 0 = WebGPU's default 8 (the M4 Max adapter: 10) */
   uint32_t max_uniform_buffers;     /* per stage; 0 = 12 */
   uint32_t heap_arenas;             /* buffer heap arena bindings (910 + a) a stage may use; 0 = 1 */
};

/* key.fs_varyings[location]: what the fragment shader reads there (wgvk_gather_fs_varyings) */
#define WGVK_VARYING_USED        0x80
#define WGVK_VARYING_INTERP(v)   ((v) & 0x3)          /* INTERP_MODE_SMOOTH / FLAT / NOPERSPECTIVE */
#define WGVK_VARYING_SAMPLING(v) (((v) >> 2) & 0x3)   /* 0 center, 1 centroid, 2 sample */
#define WGVK_VARYING_TYPE(v)     (((v) >> 4) & 0x7)   /* 1 f32, 2 i32, 3 u32, 4 f16 */

/* Vertex input for vertex pulling (key.vertex_pulling_mask, key.point_size): what the pipeline's
 * VkPipelineVertexInputStateCreateInfo (+ divisors) says, by location and binding. */
struct wgvk_vertex_attr { uint8_t binding; uint16_t format; uint32_t offset; };  /* format: enum pipe_format, 0 = none */
struct wgvk_vertex_binding { uint32_t stride; uint8_t per_instance; uint32_t divisor; };  /* divisor 1 = Vulkan's default */
struct wgvk_vertex_input {
   struct wgvk_vertex_attr attr[32];
   struct wgvk_vertex_binding binding[16];
};

struct wgvk_shader_key {             /* what a variant depends on; zero = default */
   const struct wgvk_layout_remap *layout;
   uint16_t storage_image_format[16]; /* enum pipe_format of the view bound to formatless storage image i
                                        (info.format_slot[i]); 0: report [storage-format-none] (T3b) */
   uint16_t texel_buffer_format[8];  /* enum pipe_format of the buffer view bound to texel buffer i
                                        (info.texel_slot[i]); 0: report [texel-buffer-format] (T3b) */
   uint8_t provoking_last;           /* GL's "last vertex" (T3b) */
   uint16_t vertex_pulling_mask;     /* vertex buffer bindings fetched in the shader, from vi (T3b);
                                        wgvk_vertex_binding_needs_pulling() says which */
   const struct wgvk_vertex_input *vi; /* with vertex_pulling_mask or point_size */
   uint8_t draw_params;              /* base vertex / base instance / draw id through immediates (T3b) */
   uint8_t point_size;               /* vertex shaders: points expanded to quads, 6 vertices each (enum
                                        wgvk_point_expand); every attribute is pulled from vi (T3b) */
   uint8_t point_coord_varying;      /* with point_size: the fragment shader's info.point_coord_location + 1 */
   uint8_t vs_before_gs;             /* vertex shaders followed by a geometry shader: run as compute, outputs
                                        to libpoly's vertex output buffer; every attribute pulled (T4) */
   uint8_t fs_out_type[8];           /* fragment shaders: the type of each colour output, 0 own, 1 f32, 2 i32, 3 u32 */
   uint8_t fs_varyings[32];          /* vertex shaders: the fragment shader's inputs, see above */
   uint8_t keep_clip_y;              /* vertex shaders: do not negate gl_Position.y (see nir_to_wgsl.h) */
   uint8_t viewport_index_varying;   /* the last vertex stage (VS, a GS's rast part, TES): emulate
                                        gl_ViewportIndex / gl_Layer: map gl_Position from viewport i into
                                        render_area and pass (i, layer) flat at this location - 1, the
                                        fragment shader's legal.viewport_index_location (INTERFACES 6.6) */
   uint8_t viewports;                /* fragment shaders: discard outside viewport i / scissor i and other
                                        layers, i and the layer read from legal.viewport_index_location */
   uint8_t primitive_id_varying;     /* the last vertex stage (a GS's rast part): gl_PrimitiveID it writes goes
                                        to this location - 1 (flat u32), the fragment shader's
                                        legal.primitive_id_location; 0: dropped */
   uint8_t primitive_id_from_varying; /* fragment shaders after a GS that writes gl_PrimitiveID: read it from
                                        a varying (legal.primitive_id_location), not @builtin(primitive_index) */
   uint8_t early_depth;              /* fragment shaders with layout(early_fragment_tests) that write memory,
                                        drawn with a depth test: 1 test depth in the shader first against
                                        driver binding 938 (2: multisampled, sample 0), so rejected fragments
                                        write nothing (legal.early_depth_test) */
};

/* Driver bindings appear in NIR as buffers of this descriptor set; the binding remap puts them at
 * (layout->driver_group, binding). See INTERFACES 4.2. */
#define WGVK_DRIVER_SET 0xff
#define WGVK_DRIVER_BINDING_PUSH_CONSTANTS 900   /* uniform: push constants that do not fit the immediates */
#define WGVK_DRIVER_BINDING_UNIFORMS       901   /* uniform: struct wgvk_driver_uniforms */
#define WGVK_DRIVER_BINDING_HEAP_TABLE     902   /* uniform: buffer heap descriptors (uvec4 per SSBO element) */
#define WGVK_DRIVER_BINDING_HEAP           910   /* storage: the heap's arenas, 910 + i */
#define WGVK_DRIVER_BINDING_VERTEX_BUFFER  920   /* read-only storage: vertex buffer i for vertex pulling, 920 + i */
#define WGVK_DRIVER_BINDING_INDEX_BUFFER   936   /* read-only storage: the index buffer of an expanded point draw,
                                                    or of a draw whose vertex shader runs as compute (T4) */
#define WGVK_DRIVER_BINDING_DEPTH          938   /* texture_depth_2d (key.early_depth 2: _multisampled_2d): the
                                                    pass's depth attachment, read-only, for early fragment tests */
#define WGVK_DRIVER_BINDING_POLY_INDIRECT  937   /* storage (usage Indirect too): an indirect GS draw's command and
                                                    the dispatch counts its setup writes (T4) */
#define WGVK_DRIVER_BINDING_POLY           940   /* storage: libpoly's memory (T4): vertex and geometry params,
                                                    vertex shader outputs, GS index and count buffers */
#define WGVK_DRIVER_BINDING_XFB            941   /* storage: transform feedback buffer i at 941 + i (segment 3 + i) */

/* Driver immediates (draw parameters): 3 words at info.draw_params_offset in the immediates, or at offset
 * 0 of the driver uniforms when they do not fit after the push constants. */
struct wgvk_draw_params {
   int32_t base_vertex;                /* Vulkan BaseVertex: vertexOffset (indexed) or firstVertex */
   uint32_t base_instance;             /* firstInstance */
   uint32_t draw_id;                   /* DrawIndex */
};

/* The driver uniform buffer (binding 901), when a shader needs it (info.driver_uniform_bytes > 0). */
struct wgvk_driver_uniforms {
   struct wgvk_draw_params draw;       /* 0: only when info.draw_params_in_ubo */
   uint32_t first_index;               /* 12: expanded indexed point draws: firstIndex */
   float viewport_inv_size[2];         /* 16: 1 / viewport width, 1 / viewport height (expanded points) */
   float point_size_range[2];          /* 24: clamp of gl_PointSize */
   uint32_t vertex_buffer_offset[16];  /* 32: pulled vertex buffer b starts this many bytes into its binding */
   uint32_t index_buffer_offset;       /* 96: expanded indexed point draws: bytes into the index binding */
   uint32_t poly_vertex_params;        /* 100: libpoly (T4): byte offset of struct poly_vertex_params in the
                                          poly buffer (driver binding 940) */
   uint32_t poly_geometry_params;      /* 104: ... of struct poly_geometry_params */
   uint32_t poly_sink;                 /* 108: ... of 64 zero bytes reads may land on (and stray writes) */
   uint32_t poly_setup_args;           /* 112: indirect GS draws: struct wgvk_gs_indirect_args (poly buffer) */
   uint32_t poly_heap;                 /* 116: ... struct poly_heap (poly buffer) */
   uint32_t poly_indirect_draw;        /* 120: ... the draw command, in the poly indirect buffer (937) */
   uint32_t poly_indirect_dispatch;    /* 124: ... 6 words the setup writes there: the VS part's and the
                                          GS parts' workgroup counts for dispatchWorkgroupsIndirect */
   uint32_t poly_tess_params;          /* 128: tessellation: struct poly_tess_params (poly buffer) */
   uint32_t poly_scratch;              /* 132: private memory of the tessellator kernels (poly buffer):
                                          legal.scratch_bytes per invocation, invocation i at i * that */
   uint32_t view_index;                /* 136: multiview: the view this WebGPU pass renders (gl_ViewIndex,
                                          when info.view_index); the driver renders one pass per view */
   uint32_t poly_draw_params;          /* 140: indirect draws through libpoly: struct wgvk_draw_params in the
                                          poly buffer, written by the setup part from the draw command; the
                                          compute vertex shader reads draw parameters there when this is
                                          not 0 (else from the immediates / offset 0 as usual) */
   /* multiple viewports and layered rendering (key.viewport_index_varying / key.viewports, INTERFACES 6.6) */
   uint32_t layer;                     /* 144: the layer this WebGPU pass renders (fragments of other layers
                                          are discarded) */
   float render_area[4];               /* 148: x, y, width, height of the WebGPU viewport the driver sets
                                          (positive height, depth 0..1) */
   float viewports[16][6];             /* 164: Vulkan's viewports: x, y, width, height (may be negative),
                                          minDepth, maxDepth */
   int32_t scissors[16][4];            /* 548: Vulkan's scissors: x, y, width, height */
   /* early fragment tests (key.early_depth, INTERFACES 6.1) */
   uint32_t depth_compare;             /* 804: VkCompareOp of the pipeline's depth test (ALWAYS when off) */
   float depth_unorm_max;              /* 808: 2^bits - 1 of a unorm depth format (65535, 16777215); 0 for
                                          depth32float: the depths are compared as that format stores them */
};

/* What wgvk_shader_binding.lowered says a binding is now. */
enum wgvk_lowered_kind {
   WGVK_LOWERED_NONE = 0,
   /* A storage image used atomically: a storage buffer laid out as
    *   u32 width, height, depth_or_layers, 0; then texels of the view's level, x fastest, no row padding.
    * The driver keeps it coherent with the image (copies both ways around the commands that use it). */
   WGVK_LOWERED_IMAGE_BUFFER,
   /* A uniform texel buffer (read-only storage buffer) or storage texel buffer (storage buffer) over the
    * buffer view's range; the format is decoded in the shader (wgvk_shader_key.texel_buffer_format). */
   WGVK_LOWERED_TEXEL_BUFFER,
   /* A storage buffer element served from the buffer heap (no binding of its own). */
   WGVK_LOWERED_HEAP,
   /* A uniform buffer element read as a read-only storage buffer (more UBOs than WebGPU's limit). */
   WGVK_LOWERED_UBO_AS_STORAGE,
};

struct wgvk_lowered_binding {
   uint8_t set;
   uint16_t binding, index;
   uint8_t kind;                       /* enum wgvk_lowered_kind */
   uint16_t format;                    /* enum pipe_format: texel buffers, image buffers */
   uint16_t heap_slot;                 /* WGVK_LOWERED_HEAP: entry in the heap table */
   uint8_t storage;                    /* TEXEL_BUFFER: 1 for a storage texel buffer, 0 for a uniform one */
   uint8_t arrayed;                    /* IMAGE_BUFFER: the image is an array (depth_or_layers = layers) */
};

#define WGVK_MAX_LOWERED 32

struct wgvk_legalize_info {
   uint16_t draw_params_offset;        /* byte offset of wgvk_draw_params in the immediates; 0xffff: none */
   bool draw_params_in_ubo;            /* ... or at offset 0 of the driver uniforms */
   bool push_constants_in_ubo;         /* push constants read from driver binding 900 */
   int8_t point_coord_location;        /* fragment: gl_PointCoord is this generic varying (noperspective
                                          vec2); the expanded point's vertices write it. -1: none */
   uint16_t driver_uniform_bytes;      /* bytes of wgvk_driver_uniforms the shader reads (0: none) */
   uint32_t pulled_locations;          /* vertex inputs read from storage buffers: not in the vertex state */
   bool points_expanded;               /* draw 6 vertices per point, as triangles, culling off */
   uint64_t vs_outputs;                /* vs_before_gs: outputs written (poly_vertex_params.outputs) */
   bool vs_writes_memory;              /* a vertex shader that must run as compute (wgvk_compile_vs_compute) */
   bool xfb;                           /* the shader declares transform feedback outputs (xfb_info) */
   bool view_index;                    /* reads gl_ViewIndex: driver uniform view_index (multiview) */
   bool writes_viewport_index;         /* a vertex stage writing gl_ViewportIndex or gl_Layer: dropped
                                          (viewport 0, layer 0) unless key.viewport_index_varying */
   bool writes_primitive_id;           /* a vertex stage (a GS's rast part) writing gl_PrimitiveID */
   int8_t primitive_id_location;       /* fragment, key.primitive_id_from_varying: that varying; -1: none */
   bool early_depth_test;              /* fragment: the shader tests depth itself (key.early_depth) */
   int8_t viewport_index_location;     /* fragment, key.viewports: the varying (flat uvec2: viewport, layer)
                                          the vertex stage must write; -1: none */
   uint32_t scratch_bytes;             /* private memory per invocation, in the poly buffer at poly_scratch
                                          (the tessellator: WGSL limits function variables to 8 KiB) */
   /* tessellation (T4): what struct poly_tess_params needs from the compiled TCS and TES */
   struct {
      uint8_t tcs_vertices_out;        /* TCS: output patch size (its workgroup size) */
      uint8_t nr_patch_outputs;        /* TCS: util_last_bit(patch_outputs_written) */
      uint32_t output_stride;          /* TCS: poly_tcs_output_stride (bytes per patch) */
      uint64_t per_vertex_outputs;     /* TCS: poly_tcs_per_vertex_outputs */
      uint8_t domain;                  /* TES: enum tess_primitive_mode */
      uint8_t spacing;                 /* TES: enum gl_tess_spacing */
      uint8_t ccw, point_mode;         /* TES */
   } tess;
   uint8_t heap_slots;                 /* entries of the heap table (driver binding 902) the shader reads */
   uint8_t heap_arenas;                /* arenas (driver bindings 910 + a) it may access */
   uint8_t num_format_slots;           /* formatless storage images: key.storage_image_format[i] is for */
   struct { uint8_t set; uint16_t binding, index; } format_slot[16];
   uint8_t num_texel_slots;            /* texel buffers: key.texel_buffer_format[i] is for */
   struct { uint8_t set; uint16_t binding, index; } texel_slot[8];
   unsigned num_lowered;
   struct wgvk_lowered_binding lowered[WGVK_MAX_LOWERED];
   uint32_t variant_mask;              /* WGVK_VARIANT_* the passes read */
   char error[512];                    /* "[tag] detail\n" lines for what the passes could not do */
};

enum wgvk_point_expand {
   WGVK_POINTS_AS_IS = 0,
   WGVK_POINTS_EXPAND = 1,           /* a non-indexed draw: vertex v of the point list is point v */
   WGVK_POINTS_EXPAND_INDEXED32 = 2, /* an indexed draw: the index buffer (u32) is at driver binding 936 */
   WGVK_POINTS_EXPAND_INDEXED16 = 3, /* ... (u16) */
};

/* variant_mask bits: the key fields a shader's WGSL depends on */
enum {
   WGVK_VARIANT_LAYOUT = 1 << 0,
   WGVK_VARIANT_STORAGE_IMAGE_FORMAT = 1 << 1,
   WGVK_VARIANT_PROVOKING_LAST = 1 << 2,
   WGVK_VARIANT_VERTEX_PULLING = 1 << 3,
   WGVK_VARIANT_DRAW_PARAMS = 1 << 4,
   WGVK_VARIANT_POINT_SIZE = 1 << 5,
   WGVK_VARIANT_FS_VARYINGS = 1 << 6,
   WGVK_VARIANT_CLIP_Y = 1 << 7,
   WGVK_VARIANT_TEXEL_BUFFER_FORMAT = 1 << 8,
   WGVK_VARIANT_VIEWPORTS = 1 << 9,
   WGVK_VARIANT_EARLY_DEPTH = 1 << 10,
};

struct wgvk_shader_binding {         /* one per WebGPU binding the WGSL declares */
   uint8_t group;
   uint16_t binding;
   uint8_t kind;                     /* enum wgsl_binding_kind */
   uint8_t view_dimension;           /* enum glsl_sampler_dim (1D images are 2D in wgvk) */
   uint8_t arrayed, multisampled;
   uint8_t sample_type;              /* enum wgsl_sample_type */
   uint16_t format;                  /* storage textures: enum pipe_format */
   uint8_t access;                   /* storage textures: ACCESS_NON_READABLE / ACCESS_NON_WRITEABLE */
   uint32_t min_binding_size;
   uint8_t vk_set;                   /* where it came from */
   uint16_t vk_binding, vk_index;
   uint16_t vk_array_size;           /* elements the shader declares at (vk_set, vk_binding) */
   bool sampler_of_combined;
   uint8_t lowered;                  /* enum wgvk_lowered_kind: what the binding is now (T3b) */
   uint16_t lowered_format;          /* enum pipe_format for WGVK_LOWERED_IMAGE_BUFFER / TEXEL_BUFFER */
};

struct wgvk_compiled_shader {
   char *wgsl;                       /* entry point "main"; NULL on failure */
   size_t wgsl_len;
   char *error;                      /* on failure: "[tag] detail" lines, one per unsupported construct */
   struct wgvk_shader_binding *bindings;
   unsigned n_bindings;
   uint32_t variant_mask;            /* WGVK_VARIANT_* */
   uint32_t immediate_bytes;
   uint16_t workgroup_size[3];       /* compute */
   uint64_t hash;                    /* XXH64 of the WGSL */
   uint8_t fs_varyings[32];          /* fragment shaders: what they read, for the vertex shader's key */
   struct wgvk_legalize_info legal;  /* what the legalisation passes need from the driver (T3b) */
   void *xfb_info;                   /* vertex shaders with transform feedback: a malloc'd nir_xfb_info copy */
   unsigned clip_distance_array_size;
};

const nir_shader_compiler_options *wgvk_nir_options(void);
const struct spirv_to_nir_options *wgvk_spirv_options(void);

/* Takes ownership of nir. VK_ERROR_FEATURE_NOT_PRESENT when the shader uses something WebGPU cannot do
 * yet (out->error says what). Natively, WGVK_TINT=1 checks the WGSL with Tint (WGVK_TINT_PATH, default
 * ~/rvwasm/webgpu/tint-native/tint) and fails with Tint's message. */
VkResult wgvk_compile(struct wgvk_device *dev, nir_shader *nir, const struct wgvk_shader_key *key,
                      struct wgvk_compiled_shader *out);
VkResult wgvk_compile_with_caps(const struct wgvk_compile_caps *caps, nir_shader *nir,
                                const struct wgvk_shader_key *key, struct wgvk_compiled_shader *out);
void wgvk_compiled_shader_free(struct wgvk_compiled_shader *s);

/*
 * Geometry shaders (T4) run through Mesa's libpoly: the GS becomes a vertex shader that re-runs it for each
 * output vertex ("rast", drawn by T2), a compute shader per input primitive that writes the index buffer
 * when the output topology is not static ("main"), one that counts what a primitive emits when that is
 * not static ("count", transform feedback), and a 1x1x1 compute shader that updates transform feedback
 * and query counters ("pre"). Parts that are not needed have wgsl == NULL. The vertex shader before it is
 * compiled with key.vs_before_gs. Memory (params, vertex outputs, index buffer) is libpoly's, in the poly
 * buffer at driver binding 940, laid out by poly/geometry.h; INTERFACES.md 6.2 says who fills what.
 */
enum wgvk_gs_part {
   WGVK_GS_RAST,                     /* vertex shader: re-runs the GS per output vertex */
   WGVK_GS_MAIN,                     /* compute: writes the index buffer of a dynamic topology */
   WGVK_GS_COUNT,                    /* compute: what each input primitive emits (counts not static) */
   WGVK_GS_PRE,                      /* compute 1x1x1: transform feedback setup, query counters */
   WGVK_GS_XFB,                      /* compute: the rast part per output vertex, writing transform feedback
                                        (WebGPU vertex shaders cannot store) */
   WGVK_GS_PREFIX,                   /* compute 1x1x1: prefix sums of the count buffer (transform feedback
                                        with counts that are not static) */
   WGVK_GS_PARTS
};
struct wgvk_gs_program {
   struct wgvk_compiled_shader part[WGVK_GS_PARTS];
   uint8_t shape;                    /* enum poly_gs_shape */
   uint8_t mode;                     /* enum mesa_prim the rast part draws */
   uint8_t count_words, prefix_sum, xfb;
   uint32_t max_indices;
   uint8_t topology[64];             /* POLY_GS_SHAPE_STATIC_INDEXED: the index buffer (u8) */
   uint8_t input_primitive;          /* enum mesa_prim the GS takes */
   uint32_t xfb_stride[4];           /* transform feedback: bytes per vertex, per buffer (0: unused) */
   char *error;
};
/* Takes ownership of gs. key is the rast part's (layout, fs_varyings, ...). */
VkResult wgvk_compile_gs(const struct wgvk_compile_caps *caps, nir_shader *gs, const struct wgvk_shader_key *key,
                         struct wgvk_gs_program *out);
void wgvk_gs_program_free(struct wgvk_gs_program *p);
/* Transform feedback without an application GS: a passthrough GS (libpoly's) for the vertex shader whose
 * compiled form is `vs` (key.vs_before_gs; its xfb_info), drawing `prim`. Same parts as a GS. */
VkResult wgvk_compile_xfb_gs(const struct wgvk_compile_caps *caps, const struct wgvk_compiled_shader *vs,
                             unsigned prim, const struct wgvk_shader_key *key, struct wgvk_gs_program *out);
/* The 1x1x1 compute shader that sets up an indirect draw through a GS (libpoly's poly_gs_setup_indirect
 * plus the dispatch counts): one for every GS, its inputs in struct wgvk_gs_indirect_args. */
VkResult wgvk_compile_gs_setup(const struct wgvk_compile_caps *caps, struct wgvk_compiled_shader *out);

/*
 * Tessellation (T4, libpoly): the vertex shader runs as compute (key.vs_before_gs), the TCS as compute (one
 * workgroup of legal.tess.tcs_vertices_out invocations per patch, (patches, instances) workgroups), the
 * tessellator as compute kernels, and the TES as the vertex shader that draws (wgvk_compile does the TCS
 * and TES stages). The kernels, per domain (enum tess_primitive_mode): [0] counts, [1] the prefix sum
 * that allocates the index buffer and writes the draw, [2] the tessellation. INTERFACES.md 6.2.
 */
VkResult wgvk_compile_tess_kernels(const struct wgvk_compile_caps *caps, unsigned domain,
                                   struct wgvk_compiled_shader out[3]);

/*
 * A vertex shader that writes storage buffers or images (WebGPU's vertex stage cannot) runs as compute: the
 * "compute" part (key.vs_before_gs semantics: 64 wide over (vertex count, instances), outputs to libpoly's
 * vertex output buffer, every attribute pulled) and a "rast" vertex shader that passes those outputs on,
 * drawn non-indexed with the original vertex count and instance count, first vertex and instance 0.
 * wgvk_compile reports legal.vs_writes_memory (and fails) for such a shader. INTERFACES.md 6.3.
 */
struct wgvk_vs_program {
   struct wgvk_compiled_shader compute, rast;
   char *error;
};
VkResult wgvk_compile_vs_compute(const struct wgvk_compile_caps *caps, nir_shader *vs,
                                 const struct wgvk_shader_key *key, struct wgvk_vs_program *out);
void wgvk_vs_program_free(struct wgvk_vs_program *p);

/* The fragment shader's inputs, for the vertex shader's key. Works on NIR straight from spirv_to_nir
 * (variables) and after wgsl_preprocess_nir (load_input / load_interpolated_input). */
void wgvk_gather_fs_varyings(nir_shader *fs, uint8_t out[32]);

/* Whether vertex buffer binding b of vi needs vertex pulling (a divisor other than 1, a format, stride or
 * offset WebGPU's vertex state cannot express). */
bool wgvk_vertex_binding_needs_pulling(const struct wgvk_vertex_input *vi, unsigned b);

/* T1 provides this (the device's features); a weak default returns no optional features. */
const struct wgvk_compile_caps *wgvk_device_compile_caps(struct wgvk_device *dev);

#ifdef __cplusplus
}
#endif

#endif
