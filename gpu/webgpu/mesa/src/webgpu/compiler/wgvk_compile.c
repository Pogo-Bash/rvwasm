/*
 * wgvk_compile: the driver's entry into the shader compiler (wgvk_compile.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wgvk_compile.h"
#include "wgvk_legalize.h"
#include "wgvk_poly.h"
#include "compiler/spirv/nir_spirv.h"
#include "nir_xfb_info.h"
#include "util/ralloc.h"
#define XXH_INLINE_ALL
#include "util/xxhash.h"

const nir_shader_compiler_options *
wgvk_nir_options(void)
{
   return &wgsl_nir_options;
}

const struct spirv_to_nir_options *
wgvk_spirv_options(void)
{
   return &wgsl_spirv_options;
}

__attribute__((weak)) const struct wgvk_compile_caps *
wgvk_device_compile_caps(struct wgvk_device *dev)
{
   static const struct wgvk_compile_caps none = { .max_immediate_size = 0 };
   return &none;
}

/* The binding remap over T1's table (or group = set without one), plus the driver set: wgvk_legalize.c. */
static bool
layout_remap(void *data, unsigned set, unsigned binding, unsigned index, bool sampler,
             unsigned *group, unsigned *wb)
{
   return wgvk_remap_binding(data, set, binding, index, sampler, group, wb);
}

#ifndef __EMSCRIPTEN__
static char *
tint_check(const char *wgsl)
{
   const char *tint = getenv("WGVK_TINT_PATH");
   char def[512];
   if (!tint) {
      snprintf(def, sizeof(def), "%s/rvwasm/webgpu/tint-native/tint", getenv("HOME") ? getenv("HOME") : "");
      tint = def;
   }
   char path[] = "/tmp/wgvk-tint-XXXXXX.wgsl";
   int fd = mkstemps(path, 5);
   if (fd < 0)
      return NULL;
   FILE *f = fdopen(fd, "w");
   fputs(wgsl, f);
   fclose(f);
   char cmd[1200];
   snprintf(cmd, sizeof(cmd), "%s --format wgsl -o /dev/null %s 2>&1", tint, path);
   FILE *p = popen(cmd, "r");
   char *msg = NULL;
   if (p) {
      char buf[4096];
      size_t n = fread(buf, 1, sizeof(buf) - 1, p);
      buf[n] = 0;
      if (pclose(p) != 0)
         msg = strdup(n ? buf : "tint failed");
   }
   unlink(path);
   return msg;
}
#endif

VkResult
wgvk_compile_with_caps(const struct wgvk_compile_caps *caps, nir_shader *nir,
                       const struct wgvk_shader_key *key, struct wgvk_compiled_shader *out)
{
   static const struct wgvk_shader_key zero_key;
   if (!key)
      key = &zero_key;
   memset(out, 0, sizeof(*out));
   /* tessellation through libpoly: the TCS becomes compute, the TES the vertex shader that draws */
   if (nir->info.stage == MESA_SHADER_TESS_CTRL)
      wgvk_poly_lower_tcs(nir, &out->legal);
   else if (nir->info.stage == MESA_SHADER_TESS_EVAL)
      wgvk_poly_lower_tes(nir, &out->legal);

   struct wgsl_options o = {
      .shader_f16 = caps->shader_f16,
      .subgroups = caps->subgroups,
      .clip_distances = caps->clip_distances,
      .dual_source_blending = caps->dual_source_blending,
      .primitive_index = caps->primitive_index,
      .immediate_size = caps->max_immediate_size,
   };
   o.remap = layout_remap;
   o.remap_data = (void *)key->layout;
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      memcpy(o.fs_out_type, key->fs_out_type, sizeof(o.fs_out_type));
      for (unsigned i = 0; i < 8; i++)
         if (key->fs_out_type[i])
            out->variant_mask |= WGVK_VARIANT_FS_VARYINGS;
   }
   if (nir->info.stage == MESA_SHADER_VERTEX) {
      o.keep_clip_y = key->keep_clip_y;
      out->variant_mask |= WGVK_VARIANT_CLIP_Y;
      for (unsigned i = 0; i < 32; i++) {
         uint8_t v = key->fs_varyings[i];
         if (!(v & WGVK_VARYING_USED))
            continue;
         o.varying_interp[i] = WGVK_VARYING_INTERP(v) + 1;
         o.varying_sampling[i] = WGVK_VARYING_SAMPLING(v) + 1;
         o.varying_type[i] = WGVK_VARYING_TYPE(v);
         out->variant_mask |= WGVK_VARIANT_FS_VARYINGS;
      }
   }
   nir_foreach_variable_with_modes(var, nir, nir_var_image) {
      if (var->data.image.format == PIPE_FORMAT_NONE)
         out->variant_mask |= WGVK_VARIANT_STORAGE_IMAGE_FORMAT;
   }

   struct wgvk_legalize_info *li = &out->legal;
   if (nir->info.stage == MESA_SHADER_VERTEX && !key->vs_before_gs && wgvk_poly_vs_writes_memory(nir)) {
      li->vs_writes_memory = true;
      out->error = strdup("[vs-compute] the vertex shader writes memory: compile it with wgvk_compile_vs_compute\n");
      ralloc_free(nir);
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   wgvk_legalize_pre(nir, key, caps, li);
   /* transform feedback: what the passthrough GS (wgvk_compile_xfb_gs) needs, from the variables */
   if (nir->info.stage == MESA_SHADER_VERTEX && nir->info.has_transform_feedback_varyings) {
      nir_gather_xfb_info_with_varyings(nir, NULL, NULL);
      if (nir->xfb_info) {
         size_t sz = nir_xfb_info_size(nir->xfb_info->output_count);
         out->xfb_info = malloc(sz);
         memcpy(out->xfb_info, nir->xfb_info, sz);
         li->xfb = true;
      }
   }
   out->clip_distance_array_size = nir->info.clip_distance_array_size;
   li->scratch_bytes = nir->scratch_size;
   wgsl_preprocess_nir(nir, &o);
   wgvk_legalize_post(nir, key, caps, li);
   out->variant_mask |= li->variant_mask;
#ifndef __EMSCRIPTEN__
   if (getenv("WGVK_PRINT_NIR"))   /* debugging: the NIR handed to the emitter */
      nir_print_shader(nir, stderr);
#endif
   struct wgsl_result r;
   bool ok = nir_to_wgsl(nir, &o, nir, &r);
   VkResult result = VK_SUCCESS;
   if (!ok || li->error[0]) {
      size_t n = strlen(li->error) + (r.error ? strlen(r.error) : 0) + 32;
      out->error = malloc(n);
      snprintf(out->error, n, "%s%s", li->error, ok ? "" : r.error ? r.error : "[emit] failed\n");
      result = VK_ERROR_FEATURE_NOT_PRESENT;
      goto done;
   }
   out->wgsl = strdup(r.wgsl);
   out->wgsl_len = strlen(out->wgsl);
   out->hash = XXH64(out->wgsl, out->wgsl_len, 0);
   out->immediate_bytes = r.push_constant_size;
   for (unsigned i = 0; i < 3; i++)
      out->workgroup_size[i] = r.workgroup_size[i];
   memcpy(out->fs_varyings, r.fs_varyings, sizeof(out->fs_varyings));
   if (r.num_bindings) {
      out->bindings = calloc(r.num_bindings, sizeof(*out->bindings));
      out->n_bindings = r.num_bindings;
      out->variant_mask |= WGVK_VARIANT_LAYOUT;
   }
   for (unsigned i = 0; i < r.num_bindings; i++) {
      const struct wgsl_binding *b = &r.bindings[i];
      out->bindings[i] = (struct wgvk_shader_binding){
         .group = b->group, .binding = b->binding, .kind = b->kind, .view_dimension = b->dim,
         .arrayed = b->arrayed, .multisampled = b->multisampled, .sample_type = b->sample_type,
         .format = b->format, .access = b->access, .min_binding_size = b->min_size,
         .vk_set = b->vk_set, .vk_binding = b->vk_binding, .vk_index = b->vk_index,
         .vk_array_size = b->vk_array_size,
         .sampler_of_combined = b->from_combined,
      };
      for (unsigned j = 0; j < li->num_lowered; j++) {
         const struct wgvk_lowered_binding *l = &li->lowered[j];
         if (l->set == b->vk_set && l->binding == b->vk_binding && l->index == b->vk_index) {
            out->bindings[i].lowered = l->kind;
            out->bindings[i].lowered_format = l->format;
         }
      }
   }
#ifndef __EMSCRIPTEN__
   if (getenv("WGVK_TINT") && atoi(getenv("WGVK_TINT"))) {
      char *msg = tint_check(out->wgsl);
      if (msg) {
         out->error = msg;
         result = VK_ERROR_UNKNOWN;
      }
   }
#endif
done:
   ralloc_free(nir);
   return result;
}

VkResult
wgvk_compile(struct wgvk_device *dev, nir_shader *nir, const struct wgvk_shader_key *key,
             struct wgvk_compiled_shader *out)
{
   return wgvk_compile_with_caps(wgvk_device_compile_caps(dev), nir, key, out);
}

void
wgvk_compiled_shader_free(struct wgvk_compiled_shader *s)
{
   free(s->xfb_info);
   free(s->wgsl);
   free(s->error);
   free(s->bindings);
   memset(s, 0, sizeof(*s));
}

void
wgvk_gather_fs_varyings(nir_shader *fs, uint8_t out[32])
{
   memset(out, 0, 32);
   nir_foreach_shader_in_variable(var, fs) {
      int loc = var->data.location - VARYING_SLOT_VAR0;
      if (loc < 0)
         continue;
      const struct glsl_type *t = glsl_without_array(var->type);
      unsigned slots = glsl_count_attribute_slots(var->type, false);
      enum glsl_base_type bt = glsl_get_base_type(glsl_without_array_or_matrix(t));
      unsigned type = bt == GLSL_TYPE_INT ? 2 : bt == GLSL_TYPE_UINT ? 3 : bt == GLSL_TYPE_FLOAT16 ? 4 : 1;
      unsigned interp = type == 2 || type == 3 ? INTERP_MODE_FLAT :
                        var->data.interpolation == INTERP_MODE_NONE ? INTERP_MODE_SMOOTH : var->data.interpolation;
      unsigned sampling = var->data.sample ? 2 : var->data.centroid ? 1 : 0;
      for (unsigned s = 0; s < slots && loc + s < 32; s++)
         out[loc + s] = WGVK_VARYING_USED | (interp & 3) | sampling << 2 | type << 4;
   }
}

VkResult
wgvk_compile_gs(const struct wgvk_compile_caps *caps, nir_shader *gs, const struct wgvk_shader_key *key,
                struct wgvk_gs_program *out)
{
   static const struct wgvk_shader_key zero_key;
   if (!key)
      key = &zero_key;
   memset(out, 0, sizeof(*out));
   nir_shader *parts[WGVK_GS_PARTS];
   if (!wgvk_poly_lower_gs(gs, parts, out)) {
      out->error = strdup("[poly] libpoly could not lower the geometry shader\n");
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   /* the compute parts share the pipeline layout; the rast part also gets the fragment shader's interface */
   struct wgvk_shader_key ckey = { .layout = key->layout, .provoking_last = key->provoking_last };
   VkResult result = VK_SUCCESS;
   char err[4096] = "";
   static const char *names[] = { "rast", "main", "count", "pre", "xfb", "prefix" };
   for (unsigned i = 0; i < WGVK_GS_PARTS; i++) {
      if (!parts[i])
         continue;
      VkResult r = wgvk_compile_with_caps(caps, parts[i], i == WGVK_GS_RAST ? key : &ckey, &out->part[i]);
      if (r != VK_SUCCESS) {
         result = r;
         size_t n = strlen(err);
         snprintf(err + n, sizeof(err) - n, "%s: %s", names[i],
                  out->part[i].error ? out->part[i].error : "failed\n");
      }
   }
   if (result != VK_SUCCESS)
      out->error = strdup(err);
   return result;
}

void
wgvk_gs_program_free(struct wgvk_gs_program *p)
{
   for (unsigned i = 0; i < WGVK_GS_PARTS; i++)
      wgvk_compiled_shader_free(&p->part[i]);
   free(p->error);
   memset(p, 0, sizeof(*p));
}

VkResult
wgvk_compile_vs_compute(const struct wgvk_compile_caps *caps, nir_shader *vs, const struct wgvk_shader_key *key,
                        struct wgvk_vs_program *out)
{
   static const struct wgvk_shader_key zero_key;
   if (!key)
      key = &zero_key;
   memset(out, 0, sizeof(*out));
   const nir_shader_compiler_options *opts = vs->options;
   unsigned clip = vs->info.clip_distance_array_size;
   struct wgvk_shader_key ck = *key;
   ck.vs_before_gs = 1;
   ck.point_size = 0;
   VkResult r = wgvk_compile_with_caps(caps, vs, &ck, &out->compute);
   if (r != VK_SUCCESS) {
      out->error = strdup(out->compute.error ? out->compute.error : "[vs-compute] failed\n");
      return r;
   }
   nir_shader *pt = wgvk_poly_passthrough_vs(opts, out->compute.legal.vs_outputs, clip);
   struct wgvk_shader_key rk = { .layout = key->layout, .keep_clip_y = key->keep_clip_y };
   memcpy(rk.fs_varyings, key->fs_varyings, sizeof(rk.fs_varyings));
   r = wgvk_compile_with_caps(caps, pt, &rk, &out->rast);
   if (r != VK_SUCCESS)
      out->error = strdup(out->rast.error ? out->rast.error : "[vs-compute] passthrough failed\n");
   return r;
}

void
wgvk_vs_program_free(struct wgvk_vs_program *p)
{
   wgvk_compiled_shader_free(&p->compute);
   wgvk_compiled_shader_free(&p->rast);
   free(p->error);
   memset(p, 0, sizeof(*p));
}

VkResult
wgvk_compile_gs_setup(const struct wgvk_compile_caps *caps, struct wgvk_compiled_shader *out)
{
   static const struct wgvk_shader_key zero_key;
   nir_shader *s = wgvk_poly_gs_setup_shader(wgvk_nir_options());
   return wgvk_compile_with_caps(caps, s, &zero_key, out);
}

VkResult
wgvk_compile_xfb_gs(const struct wgvk_compile_caps *caps, const struct wgvk_compiled_shader *vs, unsigned prim,
                    const struct wgvk_shader_key *key, struct wgvk_gs_program *out)
{
   memset(out, 0, sizeof(*out));
   if (!vs->xfb_info) {
      out->error = strdup("[xfb] the vertex shader has no transform feedback\n");
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }
   nir_shader *gs = wgvk_poly_xfb_passthrough_gs(wgvk_nir_options(), vs->xfb_info, vs->legal.vs_outputs,
                                                 vs->clip_distance_array_size, prim);
   return wgvk_compile_gs(caps, gs, key, out);
}

VkResult
wgvk_compile_tess_kernels(const struct wgvk_compile_caps *caps, unsigned domain, struct wgvk_compiled_shader out[3])
{
   static const struct wgvk_shader_key zero_key;
   VkResult r = VK_SUCCESS;
   for (unsigned i = 0; i < 3; i++) {
      VkResult ri = wgvk_compile_with_caps(caps, wgvk_poly_tess_kernel(wgvk_nir_options(), domain, i), &zero_key,
                                           &out[i]);
      if (ri != VK_SUCCESS)
         r = ri;
   }
   return r;
}
