/*
 * wgsl-corpus: SPIR-V modules -> spirv_to_nir (as a Vulkan driver would) -> wgsl_preprocess_nir ->
 * nir_to_wgsl, one .wgsl (or .err) per module, for checking with Tint.
 *
 *   wgsl-corpus [-o OUTDIR] [-nir] [-remap] [-f16] [-subgroups] FILE.spv...
 *
 *   -o DIR      write DIR/NAME.wgsl, DIR/NAME.err (tagged reasons), DIR/NAME.bind (binding table)
 *   -nir        print the NIR handed to the emitter
 *   -link       a vertex shader takes the interface of the last fragment shader before it on the command
 *               line (what wgvk_compile does with key.fs_varyings): list each pipeline's .frag first
 *   -remap      stand-in for T1's binding remap: array element i of (set, binding) goes to binding
 *               binding + i (combined samplers +500), so constant-indexed binding arrays emit
 *   -features   enable every WebGPU feature the M4 Max adapter has (f16, subgroups, clip
 *               distances, dual-source blending, primitive index, 64 bytes of immediates)
 *   -layout     T1's binding remap as INTERFACES 4.2 builds it, from the shader's own declarations
 *               (wgvk_layout_remap_from_nir); replaces -remap
 *   -scene F    the vertex input and draw of tests/scene/F (tools/scene.h): key.vi, the bindings WebGPU
 *               cannot fetch (or all with "pull all") in key.vertex_pulling_mask, points expanded, and
 *               gl_PointCoord's varying from the last fragment shader before (with -link)
 *   -image-format SLOT=NAME   key.storage_image_format[SLOT] (formatless storage images)
 *   -legalize   run T3b's legalisation passes (wgvk_legalize_pre/post) around wgsl_preprocess_nir, with a
 *               zero shader key, as wgvk_compile does
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nir.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/glsl_types.h"
#include "nir_to_wgsl.h"
#include "wgvk_legalize.h"
#include "wgvk_poly.h"
#include "util/format/u_format.h"
#include "scene.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static mesa_shader_stage
stage_of(const uint32_t *w, size_t n, char *name)
{
   for (size_t i = 5; i < n;) {
      uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
      if (!len)
         break;
      if (op == 15) { /* OpEntryPoint */
         strncpy(name, (const char *)&w[i + 3], 63);
         switch (w[i + 1]) {
         case 0: return MESA_SHADER_VERTEX;
         case 1: return MESA_SHADER_TESS_CTRL;
         case 2: return MESA_SHADER_TESS_EVAL;
         case 3: return MESA_SHADER_GEOMETRY;
         case 4: return MESA_SHADER_FRAGMENT;
         case 5: return MESA_SHADER_COMPUTE;
         }
      }
      i += len;
   }
   return MESA_SHADER_NONE;
}

/* a compiled part's binding table in the .bind format (cs-run and scene-run read it) */
static void
write_part_bind(const char *path, const struct wgvk_compiled_shader *c)
{
   FILE *f = fopen(path, "w");
   if (!f)
      return;
   for (unsigned i = 0; i < c->n_bindings; i++) {
      const struct wgvk_shader_binding *b = &c->bindings[i];
      fprintf(f, "group %u binding %u kind %u vk %u.%u[%u/%u]%s dim %u array %u ms %u sample %u format %u access %u min_size %u atomic %u\n",
              b->group, b->binding, b->kind, b->vk_set, b->vk_binding, b->vk_index, b->vk_array_size,
              b->sampler_of_combined ? " sampler-of-combined" : "", b->view_dimension, b->arrayed, b->multisampled,
              b->sample_type, b->format, b->access, b->min_binding_size, 0);
      if (b->kind == WGSL_BINDING_STORAGE_TEXTURE)
         fprintf(f, "    storage_format %s\n", util_format_name(b->format));
   }
   const struct wgvk_legalize_info *li = &c->legal;
   fprintf(f, "legal draw_params_offset %u draw_params_in_ubo %u push_constants_in_ubo %u point_coord %d "
           "pulled 0x%x points_expanded %u driver_uniform_bytes %u vs_outputs 0x%llx\n",
           li->draw_params_offset, li->draw_params_in_ubo, li->push_constants_in_ubo, li->point_coord_location,
           li->pulled_locations, li->points_expanded, li->driver_uniform_bytes, (unsigned long long)li->vs_outputs);
   fprintf(f, "workgroup %u %u %u immediates %u scratch %u\n", c->workgroup_size[0], c->workgroup_size[1],
           c->workgroup_size[2], c->immediate_bytes, li->scratch_bytes);
   fclose(f);
}

static void
write_text(const char *path, const char *text)
{
   FILE *f = fopen(path, "w");
   if (f) {
      fputs(text, f);
      fclose(f);
   }
}

static void
write_gs_program(const char *out, const char *stem, struct wgvk_gs_program *gp, const struct wgvk_compile_caps *caps)
{
   char path[1024];
   static const char *pn[] = { "", ".gs-main", ".gs-count", ".gs-pre", ".gs-xfb", ".gs-prefix" };
   for (unsigned i = 0; i < WGVK_GS_PARTS; i++) {
      if (!gp->part[i].wgsl)
         continue;
      snprintf(path, sizeof(path), "%s/%s%s.wgsl", out, stem, pn[i]);
      write_text(path, gp->part[i].wgsl);
      snprintf(path, sizeof(path), "%s/%s%s.bind", out, stem, pn[i]);
      write_part_bind(path, &gp->part[i]);
   }
   struct wgvk_compiled_shader setup;
   if (wgvk_compile_gs_setup(caps, &setup) == VK_SUCCESS) {
      snprintf(path, sizeof(path), "%s/%s.gs-setup.wgsl", out, stem);
      write_text(path, setup.wgsl);
      snprintf(path, sizeof(path), "%s/%s.gs-setup.bind", out, stem);
      write_part_bind(path, &setup);
   }
   wgvk_compiled_shader_free(&setup);
   snprintf(path, sizeof(path), "%s/%s.gs", out, stem);
   FILE *gf = fopen(path, "w");
   if (gf) {
      fprintf(gf, "shape %u mode %u max_indices %u input %u count_words %u prefix_sum %u xfb %u parts %u%u%u%u%u%u "
              "xfb_stride %u %u %u %u\ntopology",
              gp->shape, gp->mode, gp->max_indices, gp->input_primitive, gp->count_words, gp->prefix_sum,
              gp->xfb, !!gp->part[0].wgsl, !!gp->part[1].wgsl, !!gp->part[2].wgsl, !!gp->part[3].wgsl,
              !!gp->part[4].wgsl, !!gp->part[5].wgsl, gp->xfb_stride[0], gp->xfb_stride[1], gp->xfb_stride[2],
              gp->xfb_stride[3]);
      for (unsigned i = 0; i < 64 && i < gp->max_indices; i++)
         fprintf(gf, " %u", gp->topology[i]);
      fprintf(gf, "\n");
      fclose(gf);
   }
}

static bool
layout_remap(void *data, unsigned set, unsigned binding, unsigned index, bool sampler,
             unsigned *group, unsigned *wb)
{
   return wgvk_remap_binding(data, set, binding, index, sampler, group, wb);
}

static bool
standin_remap(void *data, unsigned set, unsigned binding, unsigned index, bool sampler,
              unsigned *group, unsigned *wb)
{
   /* zink's bindings are small numbers plus 128 (samplers) / 160+ (images): element i of binding b
    * goes to b + i, the sampler of a combined image sampler to +500; collisions are reported */
   *group = set;
   *wb = binding + index + (sampler ? 500 : 0);
   return *wb < 1000;
}

/* The first steps of Mesa's vk_spirv_to_nir (src/vulkan/runtime/vk_nir.c). */
static nir_shader *
vk_front(const uint32_t *w, size_t n, mesa_shader_stage st, const char *name)
{
   struct spirv_to_nir_options so = wgsl_spirv_options;
   nir_shader *s = spirv_to_nir(w, n, NULL, st, name, &so, &wgsl_nir_options);
   if (!s)
      return NULL;
   NIR_PASS(_, s, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, s, nir_lower_returns);
   NIR_PASS(_, s, nir_inline_functions);
   NIR_PASS(_, s, nir_opt_copy_prop);
   NIR_PASS(_, s, nir_opt_deref);
   nir_remove_non_cmat_call_entrypoints(s);
   NIR_PASS(_, s, nir_lower_variable_initializers, ~0);
   NIR_PASS(_, s, nir_split_var_copies);
   NIR_PASS(_, s, nir_split_per_member_structs);
   NIR_PASS(_, s, nir_remove_dead_variables,
            nir_var_shader_in | nir_var_shader_out | nir_var_system_value, NULL);
   nir_gather_clip_cull_distance_sizes_from_vars(s);
   NIR_PASS(_, s, nir_merge_clip_cull_distance_vars);
   NIR_PASS(_, s, nir_propagate_invariant, false);
   return s;
}

int
main(int argc, char **argv)
{
   const char *out = NULL;
   bool print_nir = false, link = false, use_layout = false, legalize = false, features = false;
   uint8_t fs_varyings[32] = { 0 };
   int fs_point_coord = -1, fs_viewport_loc = -1, fs_prim_loc = -1;
   bool has_geom = false;
   for (int i = 1; i < argc; i++)
      has_geom |= strstr(argv[i], "-geom.spv") != NULL || strstr(argv[i], "geom/") != NULL;
   bool saw_gs = false;
   static struct scene scene;
   static struct wgvk_vertex_input vi;
   static uint16_t image_format[16];
   struct wgsl_options o = { 0 };
   int a = 1;
   for (; a < argc && argv[a][0] == '-'; a++) {
      if (!strcmp(argv[a], "-o") && a + 1 < argc)
         out = argv[++a];
      else if (!strcmp(argv[a], "-nir"))
         print_nir = true;
      else if (!strcmp(argv[a], "-link"))
         link = true;
      else if (!strcmp(argv[a], "-layout"))
         use_layout = true;
      else if (!strcmp(argv[a], "-scene") && a + 1 < argc) {
         if (!scene_load(argv[++a], &scene)) {
            perror(argv[a]);
            return 2;
         }
         for (unsigned l = 0; l < 16; l++) {
            if (!scene.attr[l].used)
               continue;
            char name[80];
            snprintf(name, sizeof(name), "PIPE_FORMAT_%s", scene.attr[l].format);
            for (unsigned f = 1; f < PIPE_FORMAT_COUNT; f++)
               if (!strcmp(util_format_name(f), name))
                  vi.attr[l].format = f;
            if (!vi.attr[l].format)
               fprintf(stderr, "unknown format %s\n", name);
            vi.attr[l].binding = scene.attr[l].binding;
            vi.attr[l].offset = scene.attr[l].offset;
         }
         for (unsigned b = 0; b < 8; b++)
            vi.binding[b] = (struct wgvk_vertex_binding){ scene.vb[b].stride, scene.vb[b].per_instance,
                                                          scene.vb[b].divisor };
      } else if (!strcmp(argv[a], "-image-format") && a + 1 < argc) {
         /* -image-format SLOT=NAME: key.storage_image_format[SLOT] (NAME without PIPE_FORMAT_) */
         unsigned slot = atoi(argv[++a]);
         const char *eq = strchr(argv[a], '=');
         char name[80];
         snprintf(name, sizeof(name), "PIPE_FORMAT_%s", eq ? eq + 1 : "");
         for (unsigned f = 1; f < PIPE_FORMAT_COUNT && slot < 16; f++)
            if (!strcmp(util_format_name(f), name))
               image_format[slot] = f;
      } else if (!strcmp(argv[a], "-legalize"))
         legalize = true;
      else if (!strcmp(argv[a], "-remap"))
         o.remap = standin_remap;
      else if (!strcmp(argv[a], "-features")) {
         o.shader_f16 = o.subgroups = o.clip_distances = o.dual_source_blending = o.primitive_index = true;
         o.immediate_size = 64;
         features = true;
      } else {
         fprintf(stderr, "unknown option %s\n", argv[a]);
         return 2;
      }
   }
   glsl_type_singleton_init_or_ref();
   /* -link -layout: one pipeline, one layout: the union of every module's declarations */
   struct wgvk_layout_remap *link_layout = NULL;
   void *link_ctx = ralloc_context(NULL);
   if (link && use_layout) {
      nir_shader *all[16];
      unsigned nall = 0;
      for (int k = a; k < argc && nall < 16; k++) {
         FILE *lf2 = fopen(argv[k], "rb");
         if (!lf2)
            continue;
         fseek(lf2, 0, SEEK_END);
         long lsz = ftell(lf2);
         fseek(lf2, 0, SEEK_SET);
         uint32_t *lw = malloc(lsz);
         if (fread(lw, 1, lsz, lf2) != (size_t)lsz)
            lsz = 0;
         fclose(lf2);
         char lname[64] = "main";
         mesa_shader_stage lst = stage_of(lw, lsz / 4, lname);
         nir_shader *ls = vk_front(lw, lsz / 4, lst, lname);
         if (ls) {
            ralloc_steal(link_ctx, ls);
            all[nall++] = ls;
         }
         free(lw);
      }
      link_layout = wgvk_layout_remap_from_nirs(all, nall, link_ctx);
   }
   int ok = 0, bad = 0;
   double t_front = 0, t_pre = 0, t_emit = 0;
   for (; a < argc; a++) {
      FILE *f = fopen(argv[a], "rb");
      if (!f) {
         perror(argv[a]);
         bad++;
         continue;
      }
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      uint32_t *w = malloc(sz);
      if (fread(w, 1, sz, f) != (size_t)sz)
         sz = 0;
      fclose(f);
      char name[64] = "main";
      mesa_shader_stage st = stage_of(w, sz / 4, name);
      const char *base = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
      /* NAME = parent directory + file stem, so dumps from several runs do not collide */
      char stem[256], dir[128] = "";
      const char *slash = strrchr(argv[a], '/');
      if (slash) {
         const char *p = slash;
         while (p > argv[a] && p[-1] != '/')
            p--;
         snprintf(dir, sizeof(dir), "%.*s-", (int)(slash - p), p);
      }
      snprintf(stem, sizeof(stem), "%s%.*s", dir, (int)(strrchr(base, '.') ? strrchr(base, '.') - base : (long)strlen(base)), base);

      double t0 = now();
      nir_shader *s = vk_front(w, sz / 4, st, name);
      double t1 = now();
      struct wgsl_result r = { 0 };
      bool good = false;
      struct wgvk_legalize_info li;
      memset(&li, 0, sizeof(li));
      const char *lerr = NULL;
      if (s) {
         struct wgsl_options po = o;
         struct wgvk_shader_key key;
         memset(&key, 0, sizeof(key));
         /* multiple viewports: the fragment shader picks the varying, the last vertex stage writes it */
         if (scene.nviewports && st == MESA_SHADER_FRAGMENT)
            key.viewports = 1;
         /* a pipeline with a GS: the fragment shader reads gl_PrimitiveID from the varying the GS's rast
          * part writes */
         if (link && has_geom && st == MESA_SHADER_FRAGMENT)
            key.primitive_id_from_varying = 1;
         if (fs_prim_loc >= 0 && st != MESA_SHADER_FRAGMENT)
            key.primitive_id_varying = fs_prim_loc + 1;
         if (scene.depth_bits && st == MESA_SHADER_FRAGMENT && !getenv("WGVK_NO_EARLY_DEPTH"))
            key.early_depth = 1;
         if (scene.nviewports && fs_viewport_loc >= 0 && st != MESA_SHADER_FRAGMENT)
            key.viewport_index_varying = fs_viewport_loc + 1;
         struct wgvk_compile_caps caps = {
            .shader_f16 = o.shader_f16, .subgroups = o.subgroups, .clip_distances = o.clip_distances,
            .dual_source_blending = o.dual_source_blending, .primitive_index = o.primitive_index,
            .max_immediate_size = o.immediate_size,
            /* the M4 Max adapter's per-stage limits (spike S6); WebGPU's defaults otherwise */
            .max_storage_buffers = features ? 10 : 8,
            .max_uniform_buffers = 12,
         };
         if ((st == MESA_SHADER_TESS_CTRL || st == MESA_SHADER_TESS_EVAL) && legalize) {
            /* tessellation through libpoly: NAME.wgsl (TCS as compute / TES as the vertex shader that draws),
             * NAME.tess (what struct poly_tess_params needs), and with the TES the tessellator kernels */
            void *lctx = ralloc_context(NULL);
            if (use_layout)
               key.layout = link_layout ? link_layout : wgvk_layout_remap_from_nir(s, lctx);
            if (link && st == MESA_SHADER_TESS_EVAL)
               memcpy(key.fs_varyings, fs_varyings, 32);
            struct wgvk_compiled_shader tc;
            VkResult vr = wgvk_compile_with_caps(&caps, s, &key, &tc);
            s = NULL;
            saw_gs = link;
            char path[1024];
            if (out && vr == VK_SUCCESS) {
               snprintf(path, sizeof(path), "%s/%s.wgsl", out, stem);
               write_text(path, tc.wgsl);
               snprintf(path, sizeof(path), "%s/%s.bind", out, stem);
               write_part_bind(path, &tc);
               snprintf(path, sizeof(path), "%s/%s.tess", out, stem);
               FILE *tf = fopen(path, "w");
               if (tf) {
                  fprintf(tf, "tcs_vertices_out %u nr_patch_outputs %u output_stride %u per_vertex_outputs 0x%llx "
                          "domain %u spacing %u ccw %u point_mode %u\n", tc.legal.tess.tcs_vertices_out,
                          tc.legal.tess.nr_patch_outputs, tc.legal.tess.output_stride,
                          (unsigned long long)tc.legal.tess.per_vertex_outputs, tc.legal.tess.domain,
                          tc.legal.tess.spacing, tc.legal.tess.ccw, tc.legal.tess.point_mode);
                  fclose(tf);
               }
               if (st == MESA_SHADER_TESS_EVAL) {
                  struct wgvk_compiled_shader k[3];
                  static const char *kn[] = { "count", "prefix", "tess" };
                  VkResult kr = wgvk_compile_tess_kernels(&caps, tc.legal.tess.domain, k);
                  for (unsigned i = 0; i < 3; i++) {
                     if (k[i].wgsl) {
                        snprintf(path, sizeof(path), "%s/%s.tess-%s.wgsl", out, stem, kn[i]);
                        write_text(path, k[i].wgsl);
                        snprintf(path, sizeof(path), "%s/%s.tess-%s.bind", out, stem, kn[i]);
                        write_part_bind(path, &k[i]);
                     } else if (k[i].error) {
                        snprintf(path, sizeof(path), "%s/%s.err", out, stem);
                        write_text(path, k[i].error);
                     }
                     wgvk_compiled_shader_free(&k[i]);
                  }
                  if (kr != VK_SUCCESS)
                     vr = kr;
               }
            } else if (out) {
               snprintf(path, sizeof(path), "%s/%s.err", out, stem);
               write_text(path, tc.error ? tc.error : "[tess] failed\n");
            }
            vr == VK_SUCCESS ? ok++ : bad++;
            wgvk_compiled_shader_free(&tc);
            ralloc_free(lctx);
            free(w);
            continue;
         }
         if (st == MESA_SHADER_GEOMETRY && legalize) {
            /* libpoly: the rast part (NAME.wgsl) and the compute parts (NAME.gs-PART.wgsl) */
            void *lctx = ralloc_context(NULL);
            if (use_layout)
               key.layout = link_layout ? link_layout : wgvk_layout_remap_from_nir(s, lctx);
            if (link)
               memcpy(key.fs_varyings, fs_varyings, 32);
            struct wgvk_gs_program gp;
            double tg = now();
            VkResult vr = wgvk_compile_gs(&caps, s, &key, &gp);
            t_pre += now() - tg;
            s = NULL;
            saw_gs = link;   /* -link: the vertex shader after it runs before this GS */
            char path[1024];
            if (out && vr == VK_SUCCESS) {
               write_gs_program(out, stem, &gp, &caps);
            } else if (out) {
               snprintf(path, sizeof(path), "%s/%s.err", out, stem);
               write_text(path, gp.error ? gp.error : "[poly] failed\n");
            } else {
               fputs(vr == VK_SUCCESS ? gp.part[0].wgsl : gp.error, stdout);
            }
            vr == VK_SUCCESS ? ok++ : bad++;
            wgvk_gs_program_free(&gp);
            ralloc_free(lctx);
            free(w);
            continue;
         }
         if (st == MESA_SHADER_VERTEX && legalize && !saw_gs && s->info.has_transform_feedback_varyings) {
            /* transform feedback without a GS: NAME.wgsl (the vertex shader as compute) and libpoly's
             * passthrough GS as NAME-geom.* (NAME-vert -> NAME-geom), for the draw's topology */
            void *lctx = ralloc_context(NULL);
            if (use_layout)
               key.layout = link_layout ? link_layout : wgvk_layout_remap_from_nir(s, lctx);
            key.vs_before_gs = 1;
            key.vi = &vi;
            for (unsigned b = 0; b < 8; b++)
               if (scene.vb[b].used)
                  key.vertex_pulling_mask |= 1u << b;
            struct wgvk_compiled_shader vc;
            VkResult vr = wgvk_compile_with_caps(&caps, s, &key, &vc);
            s = NULL;
            char path[1024], gstem[256];
            snprintf(gstem, sizeof(gstem), "%s", stem);
            char *v = strstr(gstem, "-vert");
            if (v)
               memcpy(v, "-geom", 5);
            else
               strncat(gstem, ".xfbgs", sizeof(gstem) - strlen(gstem) - 1);
            if (vr == VK_SUCCESS && out) {
               snprintf(path, sizeof(path), "%s/%s.wgsl", out, stem);
               write_text(path, vc.wgsl);
               snprintf(path, sizeof(path), "%s/%s.bind", out, stem);
               write_part_bind(path, &vc);
               struct wgvk_shader_key gk = { .layout = key.layout };
               if (link)
                  memcpy(gk.fs_varyings, fs_varyings, 32);
               unsigned prim = scene.topology == 0 ? MESA_PRIM_POINTS : scene.topology == 1 ? MESA_PRIM_LINES :
                               scene.topology == 2 ? MESA_PRIM_LINE_STRIP : scene.topology == 4 ? MESA_PRIM_TRIANGLE_STRIP :
                               MESA_PRIM_TRIANGLES;
               struct wgvk_gs_program gp;
               vr = wgvk_compile_xfb_gs(&caps, &vc, prim, &gk, &gp);
               if (vr == VK_SUCCESS)
                  write_gs_program(out, gstem, &gp, &caps);
               else {
                  snprintf(path, sizeof(path), "%s/%s.err", out, stem);
                  write_text(path, gp.error ? gp.error : "[xfb] failed\n");
               }
               wgvk_gs_program_free(&gp);
            } else if (out) {
               snprintf(path, sizeof(path), "%s/%s.err", out, stem);
               write_text(path, vc.error ? vc.error : "[xfb] failed\n");
            }
            vr == VK_SUCCESS ? ok++ : bad++;
            wgvk_compiled_shader_free(&vc);
            ralloc_free(lctx);
            free(w);
            continue;
         }
         if (st == MESA_SHADER_VERTEX && legalize && !saw_gs && wgvk_poly_vs_writes_memory(s)) {
            /* a vertex shader that writes memory runs as compute + a passthrough (NAME.vs-compute.wgsl,
             * NAME.wgsl) */
            void *lctx = ralloc_context(NULL);
            if (use_layout)
               key.layout = link_layout ? link_layout : wgvk_layout_remap_from_nir(s, lctx);
            if (link)
               memcpy(key.fs_varyings, fs_varyings, 32);
            if (scene.valid) {
               key.vi = &vi;
               for (unsigned b = 0; b < 8; b++)
                  if (scene.vb[b].used)
                     key.vertex_pulling_mask |= 1u << b;
            }
            struct wgvk_vs_program vp;
            VkResult vr = wgvk_compile_vs_compute(&caps, s, &key, &vp);
            s = NULL;
            char path[1024];
            if (out && vr == VK_SUCCESS) {
               snprintf(path, sizeof(path), "%s/%s.wgsl", out, stem);
               write_text(path, vp.rast.wgsl);
               snprintf(path, sizeof(path), "%s/%s.bind", out, stem);
               write_part_bind(path, &vp.rast);
               snprintf(path, sizeof(path), "%s/%s.vs-compute.wgsl", out, stem);
               write_text(path, vp.compute.wgsl);
               snprintf(path, sizeof(path), "%s/%s.vs-compute.bind", out, stem);
               write_part_bind(path, &vp.compute);
            } else if (out) {
               snprintf(path, sizeof(path), "%s/%s.err", out, stem);
               write_text(path, vp.error ? vp.error : "[vs-compute] failed\n");
            }
            vr == VK_SUCCESS ? ok++ : bad++;
            wgvk_vs_program_free(&vp);
            ralloc_free(lctx);
            free(w);
            continue;
         }
         if ((scene.valid || saw_gs) && st == MESA_SHADER_VERTEX) {
            key.vi = &vi;
            key.vs_before_gs = saw_gs;
            for (unsigned b = 0; b < 8; b++)
               if (scene.vb[b].used && (saw_gs || scene.pull_all || wgvk_vertex_binding_needs_pulling(&vi, b)))
                  key.vertex_pulling_mask |= 1u << b;
            if (scene.points && !saw_gs)
               key.point_size = !scene.indexed ? WGVK_POINTS_EXPAND :
                                scene.index_bits == 16 ? WGVK_POINTS_EXPAND_INDEXED16 : WGVK_POINTS_EXPAND_INDEXED32;
            if (link && fs_point_coord >= 0)
               key.point_coord_varying = fs_point_coord + 1;
         }
         memcpy(key.storage_image_format, image_format, sizeof(image_format));
         if (use_layout) {
            key.layout = link_layout ? link_layout : wgvk_layout_remap_from_nir(s, s);
            o.remap = layout_remap;
            o.remap_data = (void *)key.layout;
         } else if (legalize && !o.remap) {
            o.remap = layout_remap;   /* group = set, plus the driver set */
            o.remap_data = NULL;
         }
         if (legalize)
            wgvk_legalize_pre(s, &key, &caps, &li);
         wgsl_preprocess_nir(s, &o);
         if (legalize) {
            wgvk_legalize_post(s, &key, &caps, &li);
            if (li.error[0])
               lerr = li.error;
         }
         double t2 = now();
         if (print_nir) {
            printf("==== %s\n", argv[a]);
            nir_print_shader(s, stdout);
         }
         struct wgsl_options so = o;
         if (link && st == MESA_SHADER_VERTEX) {
            for (unsigned i = 0; i < 32; i++) {
               if (!(fs_varyings[i] & 0x80))
                  continue;
               so.varying_interp[i] = (fs_varyings[i] & 3) + 1;
               so.varying_sampling[i] = ((fs_varyings[i] >> 2) & 3) + 1;
               so.varying_type[i] = (fs_varyings[i] >> 4) & 7;
            }
         }
         good = nir_to_wgsl(s, &so, s, &r);
         if (lerr) {
            r.error = ralloc_asprintf(s, "%s%s", lerr, good ? "" : r.error ? r.error : "");
            good = false;
         }
         o = po;
         if (st == MESA_SHADER_FRAGMENT)
            memcpy(fs_varyings, r.fs_varyings, 32);
         if (st == MESA_SHADER_FRAGMENT)
            fs_point_coord = li.point_coord_location;
         if (st == MESA_SHADER_FRAGMENT)
            fs_viewport_loc = li.viewport_index_location;
         if (st == MESA_SHADER_FRAGMENT)
            fs_prim_loc = li.primitive_id_location;
         double t3 = now();
         t_front += t1 - t0;
         t_pre += t2 - t1;
         t_emit += t3 - t2;
      } else {
         r.error = "[spirv_to_nir] failed\n";
      }
      if (out) {
         char path[1024];
         snprintf(path, sizeof(path), "%s/%s.%s", out, stem, good ? "wgsl" : "err");
         FILE *o2 = fopen(path, "w");
         if (o2) {
            fputs(good ? r.wgsl : r.error, o2);
            fclose(o2);
         }
         if (good) {
            /* WebGPU limits of the M4 Max adapter (spike S6) that this module exceeds */
            unsigned n[8] = { 0 };
            for (unsigned i = 0; i < r.num_bindings; i++)
               n[r.bindings[i].kind]++;
            unsigned shared = 0;
            nir_foreach_variable_with_modes(var, s, nir_var_mem_shared) {
               unsigned sz, al;
               glsl_get_natural_size_align_bytes(var->type, &sz, &al);
               shared += sz;
            }
            unsigned loc = 0;
            const char *io = strstr(r.wgsl, s->info.stage == MESA_SHADER_VERTEX ? "struct Out {" : "struct In {");
            const char *end = io ? strstr(io, "};") : NULL;
            for (const char *q = io; q && q < end && (q = strstr(q, "@location(")) && q < end; q++)
               loc++;
            snprintf(path, sizeof(path), "%s/%s.limits", out, stem);
            FILE *lf = NULL;
#define LIMIT(cond, ...) if (cond) { if (!lf) lf = fopen(path, "w"); if (lf) fprintf(lf, __VA_ARGS__); }
            LIMIT(n[WGSL_BINDING_STORAGE_BUFFER] + n[WGSL_BINDING_READONLY_STORAGE_BUFFER] > 10,
                  "[limit-storage-buffers] %u storage buffers (10 per stage)\n",
                  n[WGSL_BINDING_STORAGE_BUFFER] + n[WGSL_BINDING_READONLY_STORAGE_BUFFER]);
            LIMIT(n[WGSL_BINDING_UNIFORM_BUFFER] > 12, "[limit-uniform-buffers] %u uniform buffers (12 per stage)\n",
                  n[WGSL_BINDING_UNIFORM_BUFFER]);
            LIMIT(n[WGSL_BINDING_TEXTURE] > 48, "[limit-textures] %u textures (48 per stage)\n", n[WGSL_BINDING_TEXTURE]);
            LIMIT(n[WGSL_BINDING_SAMPLER] + n[WGSL_BINDING_COMPARISON_SAMPLER] > 16, "[limit-samplers] %u samplers (16 per stage)\n",
                  n[WGSL_BINDING_SAMPLER] + n[WGSL_BINDING_COMPARISON_SAMPLER]);
            LIMIT(n[WGSL_BINDING_STORAGE_TEXTURE] > 8, "[limit-storage-textures] %u storage textures (8 per stage)\n",
                  n[WGSL_BINDING_STORAGE_TEXTURE]);
            LIMIT(shared > 32768, "[limit-workgroup-storage] %u bytes of workgroup memory (32 KiB)\n", shared);
            LIMIT(loc > 28 && s->info.stage != MESA_SHADER_COMPUTE, "[limit-inter-stage] %u inter-stage locations (28)\n", loc);
            for (unsigned i = 0; i < r.num_bindings; i++)
               LIMIT(r.bindings[i].kind == WGSL_BINDING_UNIFORM_BUFFER && r.bindings[i].min_size > 65536,
                     "[limit-uniform-size] uniform buffer of %u bytes (64 KiB)\n", r.bindings[i].min_size);
#undef LIMIT
            if (lf)
               fclose(lf);
            snprintf(path, sizeof(path), "%s/%s.bind", out, stem);
            o2 = fopen(path, "w");
            for (unsigned i = 0; o2 && i < r.num_bindings; i++) {
               const struct wgsl_binding *b = &r.bindings[i];
               fprintf(o2, "group %u binding %u kind %u vk %u.%u[%u/%u]%s dim %u array %u ms %u sample %u format %u access %u min_size %u atomic %u\n",
                       b->group, b->binding, b->kind, b->vk_set, b->vk_binding, b->vk_index, b->vk_array_size,
                       b->from_combined ? " sampler-of-combined" : "", b->dim, b->arrayed,
                       b->multisampled, b->sample_type, b->format, b->access, b->min_size, b->atomic);
               if (b->kind == WGSL_BINDING_STORAGE_TEXTURE)
                  fprintf(o2, "    storage_format %s\n", util_format_name(b->format));
               for (unsigned j = 0; j < li.num_lowered; j++)
                  if (li.lowered[j].set == b->vk_set && li.lowered[j].binding == b->vk_binding &&
                      li.lowered[j].index == b->vk_index)
                     fprintf(o2, "    lowered %u format %s storage %u arrayed %u\n", li.lowered[j].kind,
                             util_format_name(li.lowered[j].format), li.lowered[j].storage,
                             li.lowered[j].arrayed);
            }
            for (unsigned j = 0; o2 && j < li.num_lowered; j++)
               if (li.lowered[j].kind == WGVK_LOWERED_HEAP)
                  fprintf(o2, "heap slot %u vk %u.%u[%u]\n", li.lowered[j].heap_slot, li.lowered[j].set,
                          li.lowered[j].binding, li.lowered[j].index);
            if (o2 && legalize)
               fprintf(o2, "legal draw_params_offset %u draw_params_in_ubo %u push_constants_in_ubo %u point_coord %d "
                       "pulled 0x%x points_expanded %u driver_uniform_bytes %u view_index %u\n",
                       li.draw_params_offset, li.draw_params_in_ubo, li.push_constants_in_ubo, li.point_coord_location,
                       li.pulled_locations, li.points_expanded, li.driver_uniform_bytes, li.view_index);
            if (o2)
               fprintf(o2, "workgroup %u %u %u vs_outputs 0x%llx\n", r.workgroup_size[0], r.workgroup_size[1],
                       r.workgroup_size[2], (unsigned long long)li.vs_outputs);
            if (o2)
               fclose(o2);
         }
      } else if (!print_nir) {
         fputs(good ? r.wgsl : r.error, stdout);
      }
      good ? ok++ : bad++;
      if (s)
         ralloc_free(s);
      free(w);
   }
   fprintf(stderr, "emitted %d failed %d; ms per module: front %.2f preprocess %.2f emit %.2f\n", ok, bad,
           (ok + bad) ? t_front * 1000 / (ok + bad) : 0, (ok + bad) ? t_pre * 1000 / (ok + bad) : 0,
           (ok + bad) ? t_emit * 1000 / (ok + bad) : 0);
   return 0;
}
