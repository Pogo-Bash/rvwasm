/*
 * WebGPU legalisation passes (track T3b); see wgvk_legalize.h for where they run and
 * gpu/webgpu/INTERFACES.md section 6 for what the driver does with what they report.
 */
#include <stdio.h>
#include <string.h>
#include "wgvk_legalize.h"
#include "wgvk_poly.h"
#include "nir_builder.h"
#include "nir_format_convert.h"
#include "util/format/u_format.h"
#include "util/ralloc.h"
#include "util/u_dynarray.h"

static void PRINTFLIKE(3, 4)
lfail(struct wgvk_legalize_info *info, const char *tag, const char *fmt, ...)
{
   char line[200];
   va_list ap;
   va_start(ap, fmt);
   int n = snprintf(line, sizeof(line), "[%s] ", tag);
   vsnprintf(line + n, sizeof(line) - n, fmt, ap);
   va_end(ap);
   if (strstr(info->error, line))
      return;
   size_t len = strlen(info->error);
   snprintf(info->error + len, sizeof(info->error) - len, "%s\n", line);
}

static void
add_lowered(struct wgvk_legalize_info *info, unsigned set, unsigned binding, unsigned index,
            enum wgvk_lowered_kind kind, unsigned format)
{
   for (unsigned i = 0; i < info->num_lowered; i++) {
      struct wgvk_lowered_binding *l = &info->lowered[i];
      if (l->set == set && l->binding == binding && l->index == index)
         return;
   }
   if (info->num_lowered == WGVK_MAX_LOWERED) {
      lfail(info, "limits", "more than %u lowered bindings", WGVK_MAX_LOWERED);
      return;
   }
   info->lowered[info->num_lowered++] = (struct wgvk_lowered_binding){
      .set = set, .binding = binding, .index = index, .kind = kind, .format = format,
   };
}

/* ---------------------------------------------------------------- binding remap */

bool
wgvk_remap_binding(const struct wgvk_layout_remap *l, unsigned set, unsigned binding, unsigned index,
                   bool sampler, unsigned *group, unsigned *wb)
{
   if (set == WGVK_DRIVER_SET) {
      if (sampler)
         return false;
      *group = l ? l->driver_group : 0;
      *wb = binding + index;
      return *wb < 1000;
   }
   if (!l) {
      /* no layout: group = set, binding = binding, element 0, combined samplers at +500 */
      if (index)
         return false;
      *group = set;
      *wb = binding + (sampler ? 500 : 0);
      return true;
   }
   for (unsigned i = 0; i < l->n; i++) {
      const struct wgvk_binding_remap *b = &l->b[i];
      if (b->set != set || b->binding != binding)
         continue;
      if (index >= b->count)
         return false;
      if (sampler && b->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
         return false;
      *group = b->group;
      *wb = (sampler ? b->wg_sampler_binding : b->wg_binding) + index;
      return true;
   }
   return false;
}

static int
cmp_remap(const void *a, const void *b)
{
   const struct wgvk_binding_remap *x = a, *y = b;
   return x->set != y->set ? (int)x->set - (int)y->set : (int)x->binding - (int)y->binding;
}

struct wgvk_layout_remap *
wgvk_layout_remap_from_nir(nir_shader *s, void *mem_ctx)
{
   return wgvk_layout_remap_from_nirs(&s, 1, mem_ctx);
}

struct wgvk_layout_remap *
wgvk_layout_remap_from_nirs(nir_shader **shaders, unsigned count_shaders, void *mem_ctx)
{
   struct wgvk_layout_remap *l = rzalloc(mem_ctx, struct wgvk_layout_remap);
   struct wgvk_binding_remap *b = rzalloc_array(l, struct wgvk_binding_remap, 128);
   unsigned n = 0, max_set = 0;
   bool any_set = false;
   for (unsigned si = 0; si < count_shaders; si++)
   nir_foreach_variable_with_modes(var, shaders[si], nir_var_mem_ubo | nir_var_mem_ssbo | nir_var_uniform | nir_var_image) {
      const struct glsl_type *t = glsl_without_array(var->type);
      VkDescriptorType type;
      if (var->data.mode == nir_var_mem_ubo)
         type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      else if (var->data.mode == nir_var_mem_ssbo)
         type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      else if (var->data.mode == nir_var_image)
         type = glsl_get_sampler_dim(t) == GLSL_SAMPLER_DIM_BUF ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER :
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      else if (glsl_type_is_bare_sampler(t))
         type = VK_DESCRIPTOR_TYPE_SAMPLER;
      else if (glsl_type_is_texture(t))
         type = glsl_get_sampler_dim(t) == GLSL_SAMPLER_DIM_BUF ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER :
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      else if (glsl_type_is_sampler(t))
         type = glsl_get_sampler_dim(t) == GLSL_SAMPLER_DIM_BUF ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER :
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      else
         continue;                  /* plain uniforms (GL only) */
      unsigned count = glsl_type_is_array(var->type) ? glsl_get_aoa_size(var->type) : 1;
      bool dup = false;
      for (unsigned i = 0; i < n; i++) {
         if (b[i].set == var->data.descriptor_set && b[i].binding == var->data.binding) {
            b[i].count = MAX2(b[i].count, count);
            dup = true;
         }
      }
      if (dup || n == 128)
         continue;
      b[n++] = (struct wgvk_binding_remap){
         .set = var->data.descriptor_set, .group = var->data.descriptor_set, .binding = var->data.binding,
         .count = count, .type = type,
      };
      max_set = MAX2(max_set, var->data.descriptor_set);
      any_set = true;
   }
   qsort(b, n, sizeof(*b), cmp_remap);
   unsigned next = 0, cur_set = ~0u;
   for (unsigned i = 0; i < n; i++) {
      if (b[i].set != cur_set) {
         cur_set = b[i].set;
         next = 0;
      }
      b[i].wg_binding = next;
      if (b[i].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
         b[i].wg_sampler_binding = next + b[i].count;
         next += 2 * b[i].count;
      } else {
         next += b[i].count;
      }
   }
   l->b = b;
   l->n = n;
   l->driver_group = any_set ? max_set : 0;
   unsigned pc = 0;
   for (unsigned si = 0; si < count_shaders; si++)
      nir_foreach_variable_with_modes(var, shaders[si], nir_var_mem_push_const)
         pc = MAX2(pc, glsl_get_explicit_size(var->type, false));
   l->push_constant_bytes = MIN2(pc, 255);
   return l;
}

/* ---------------------------------------------------------------- helpers */

/* A uniform buffer of the driver set, declared with its size so that the emitter sizes the WGSL array for
 * dynamic offsets too. */
static nir_def *
driver_ubo_index(nir_builder *b, unsigned binding, unsigned bytes)
{
   bool have = false;
   nir_foreach_variable_with_modes(var, b->shader, nir_var_mem_ubo) {
      if (var->data.descriptor_set == WGVK_DRIVER_SET && var->data.binding == binding) {
         have = true;
         if (glsl_get_explicit_size(var->type, false) < bytes) {
            glsl_struct_field f = { .type = glsl_array_type(glsl_uint_type(), DIV_ROUND_UP(bytes, 4), 4),
                                    .name = "w", .offset = 0 };
            var->type = glsl_struct_type(&f, 1, "wgvk_driver_ubo", false);
         }
      }
   }
   if (!have) {
      glsl_struct_field f = { .type = glsl_array_type(glsl_uint_type(), DIV_ROUND_UP(bytes, 4), 4),
                              .name = "w", .offset = 0 };
      nir_variable *var = nir_variable_create(b->shader, nir_var_mem_ubo,
                                              glsl_struct_type(&f, 1, "wgvk_driver_ubo", false),
                                              binding == WGVK_DRIVER_BINDING_PUSH_CONSTANTS ? "wgvk_pc_ubo" :
                                              "wgvk_driver_ubo");
      var->data.descriptor_set = WGVK_DRIVER_SET;
      var->data.binding = binding;
   }
   return nir_imm_ivec2(b, WGVK_DRIVER_SET << 16 | binding, 0);
}

static nir_def *load_driver_ubo(nir_builder *b, unsigned binding, unsigned bytes, unsigned nc, nir_def *offset);

nir_def *
wgvk_load_driver_ubo(nir_builder *b, unsigned binding, unsigned bytes, unsigned nc, nir_def *offset)
{
   return load_driver_ubo(b, binding, bytes, nc, offset);
}

static nir_def *
load_driver_ubo(nir_builder *b, unsigned binding, unsigned bytes, unsigned nc, nir_def *offset)
{
   nir_def *idx = driver_ubo_index(b, binding, bytes);
   return nir_load_ubo(b, nc, 32, idx, offset, .align_mul = 4, .align_offset = 0, .range_base = 0,
                       .range = ~0);
}

static unsigned
shader_push_constant_bytes(nir_shader *s)
{
   unsigned pc = 0;
   nir_foreach_variable_with_modes(var, s, nir_var_mem_push_const)
      pc = MAX2(pc, glsl_get_explicit_size(var->type, false));
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_load_push_constant)
               pc = MAX2(pc, nir_intrinsic_base(intr) + nir_intrinsic_range(intr));
         }
      }
   }
   return pc;
}

/* ---------------------------------------------------------------- push constants and draw parameters */

/*
 * Immediates hold, in this order: the app's push constants (bytes [0, pc)), then the driver's draw
 * parameters (wgvk_draw_params, 12 bytes at align4(pc)) when they fit. Push constants that do not fit go
 * to the driver uniform buffer at 900 (same offsets), and draw parameters that do not fit after them to
 * the driver uniforms at 901. The rule depends only on the pipeline layout's push constant size, so every
 * stage of a pipeline agrees.
 */
struct pc_state {
   const struct wgvk_compile_caps *caps;
   struct wgvk_legalize_info *info;
   unsigned pc_bytes;
};

static nir_def *
load_pc_words(nir_builder *b, struct pc_state *st, unsigned nc, unsigned base, nir_def *off)
{
   nir_def *full = nir_iadd_imm(b, off, base);
   if (st->info->push_constants_in_ubo)
      return load_driver_ubo(b, WGVK_DRIVER_BINDING_PUSH_CONSTANTS, MAX2(st->pc_bytes, 16), nc, full);
   if (nir_src_is_const(nir_src_for_ssa(off)))
      return NULL;   /* the emitter takes it as it is */
   /* a dynamic offset into the immediates (a WGSL struct): select among the words it can reach */
   unsigned words = DIV_ROUND_UP(st->pc_bytes, 4);
   nir_def *w = nir_ushr_imm(b, full, 2);
   nir_def *comps[4];
   for (unsigned c = 0; c < nc; c++) {
      nir_def *wc = nir_iadd_imm(b, w, c);
      nir_def *v = nir_imm_int(b, 0);
      for (unsigned i = 0; i < words; i++) {
         nir_def *x = nir_load_push_constant(b, 1, 32, nir_imm_int(b, 0), .base = i * 4, .range = 4);
         v = nir_bcsel(b, nir_ieq_imm(b, wc, i), x, v);
      }
      comps[c] = v;
   }
   return nir_vec(b, comps, nc);
}

static nir_def *
load_draw_param(nir_builder *b, struct pc_state *st, unsigned word)
{
   struct wgvk_legalize_info *info = st->info;
   if (info->draw_params_in_ubo) {
      info->driver_uniform_bytes = MAX2(info->driver_uniform_bytes, sizeof(struct wgvk_driver_uniforms));
      return load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 1,
                             nir_imm_int(b, word * 4));
   }
   return nir_load_push_constant(b, 1, 32, nir_imm_int(b, 0), .base = info->draw_params_offset + word * 4,
                                 .range = 4);
}

static bool
lower_pc_dp(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct pc_state *st = data;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *v;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_push_constant:
      if (intr->def.bit_size != 32)
         return false;
      v = load_pc_words(b, st, intr->def.num_components, nir_intrinsic_base(intr), intr->src[0].ssa);
      if (!v)
         return false;
      break;
   case nir_intrinsic_load_first_vertex:
   case nir_intrinsic_load_base_vertex:
      v = load_draw_param(b, st, 0);
      break;
   case nir_intrinsic_load_base_instance:
      v = load_draw_param(b, st, 1);
      break;
   case nir_intrinsic_load_draw_id:
      v = load_draw_param(b, st, 2);
      break;
   case nir_intrinsic_load_is_indexed_draw:
      /* only GL's gl_BaseVertex reads it, and Vulkan's BaseVertex is already 0 for non-indexed draws */
      v = nir_imm_int(b, ~0);
      break;
   default:
      return false;
   }
   nir_def_replace(&intr->def, v);
   return true;
}

static bool
reads_draw_params(nir_shader *s)
{
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            switch (nir_instr_as_intrinsic(instr)->intrinsic) {
            case nir_intrinsic_load_first_vertex:
            case nir_intrinsic_load_base_vertex:
            case nir_intrinsic_load_base_instance:
            case nir_intrinsic_load_draw_id:
               return true;
            default:
               break;
            }
         }
      }
   }
   return false;
}

static void
lower_push_constants_and_draw_params(nir_shader *s, const struct wgvk_shader_key *key,
                                     const struct wgvk_compile_caps *caps, struct wgvk_legalize_info *info)
{
   unsigned pc = key->layout ? key->layout->push_constant_bytes : shader_push_constant_bytes(s);
   unsigned max = caps->max_immediate_size;
   info->push_constants_in_ubo = pc > max;
   unsigned imm_pc = info->push_constants_in_ubo ? 0 : pc;
   unsigned dp_off = (imm_pc + 3) & ~3u;
   bool dp_fits = dp_off + sizeof(struct wgvk_draw_params) <= max;
   info->draw_params_offset = 0xffff;
   bool dp = reads_draw_params(s);
   if (dp) {
      info->variant_mask |= WGVK_VARIANT_DRAW_PARAMS;
      if (dp_fits)
         info->draw_params_offset = dp_off;
      else
         info->draw_params_in_ubo = true;
   }
   if (!dp && !info->push_constants_in_ubo && !shader_push_constant_bytes(s))
      return;
   struct pc_state st = { .caps = caps, .info = info, .pc_bytes = pc };
   nir_shader_intrinsics_pass(s, lower_pc_dp, nir_metadata_control_flow, &st);
}

/* ---------------------------------------------------------------- multiview */

/* gl_ViewIndex: a word of the driver uniforms. WebGPU has no multiview; the driver renders a subpass once
 * per view into that view's layer, writing view_index before each (INTERFACES 6.4). */
static bool
lower_view_index_instr(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct wgvk_legalize_info *info = data;
   if (intr->intrinsic != nir_intrinsic_load_view_index)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   info->view_index = true;
   info->driver_uniform_bytes = MAX2(info->driver_uniform_bytes, sizeof(struct wgvk_driver_uniforms));
   nir_def_replace(&intr->def, load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms),
                                               1, nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, view_index))));
   return true;
}

static bool
lower_view_index(nir_shader *s, struct wgvk_legalize_info *info)
{
   return nir_shader_intrinsics_pass(s, lower_view_index_instr, nir_metadata_control_flow, info);
}

/* ---------------------------------------------------------------- integer textures through a sampler */

static nir_tex_instr *
tex_like(nir_builder *b, nir_tex_instr *orig, nir_texop op, nir_def *coord, nir_def *lod, unsigned ncomp,
         nir_alu_type dest_type)
{
   int ti = nir_tex_instr_src_index(orig, nir_tex_src_texture_deref);
   unsigned n = 1 + (coord != NULL) + (lod != NULL);
   nir_tex_instr *t = nir_tex_instr_create(b->shader, n);
   t->op = op;
   t->sampler_dim = orig->sampler_dim;
   t->is_array = orig->is_array;
   t->texture_index = orig->texture_index;
   t->dest_type = dest_type;
   t->coord_components = coord ? coord->num_components : 0;
   unsigned i = 0;
   t->src[i++] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, orig->src[ti].src.ssa);
   if (coord)
      t->src[i++] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   if (lod)
      t->src[i++] = nir_tex_src_for_ssa(nir_tex_src_lod, lod);
   nir_def_init(&t->instr, &t->def, ncomp, 32);
   nir_builder_instr_insert(b, &t->instr);
   return t;
}

/*
 * WGSL samples only float textures. An integer texture is never filtered (GL and Vulkan: nearest only),
 * so a sample is a textureLoad of the texel the coordinate falls in, clamped to the edge (the usual wrap
 * for id/stencil-like integer textures; a repeat sampler would differ at the borders), at level 0 for
 * implicit LOD, or the rounded explicit LOD.
 */
static bool
lower_int_tex(nir_builder *b, nir_instr *instr, void *data)
{
   struct wgvk_legalize_info *info = data;
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   if (tex->op != nir_texop_tex && tex->op != nir_texop_txb && tex->op != nir_texop_txl &&
       tex->op != nir_texop_txd)
      return false;
   nir_alu_type base = nir_alu_type_get_base_type(tex->dest_type);
   if (base != nir_type_int && base != nir_type_uint)
      return false;
   if (tex->is_shadow || tex->sampler_dim == GLSL_SAMPLER_DIM_BUF)
      return false;
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE) {
      lfail(info, "int-texture-sample", "sampling an integer cube texture");
      return false;
   }
   b->cursor = nir_before_instr(instr);
   int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   int li = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   int oi = nir_tex_instr_src_index(tex, nir_tex_src_offset);
   nir_def *coord = tex->src[ci].src.ssa;
   unsigned nc = tex->coord_components, spatial = nc - (tex->is_array ? 1 : 0);

   /* the level: Vulkan's LOD (explicit, or from the coordinate's derivatives in fragment shaders, plus the
    * bias), then a nearest mip level, ceil(lod + 0.5) - 1, clamped to the view's levels. (The sampler's
    * min/max LOD and a linear mipmap mode are not known here: nearest is what integer textures get from
    * the usual samplers.) */
   nir_def *flod = NULL;
   int bi = nir_tex_instr_src_index(tex, nir_tex_src_bias);
   int dxi = nir_tex_instr_src_index(tex, nir_tex_src_ddx), dyi = nir_tex_instr_src_index(tex, nir_tex_src_ddy);
   if (tex->op == nir_texop_txl && li >= 0) {
      flod = tex->src[li].src.ssa;
   } else if ((tex->op == nir_texop_txd && dxi >= 0 && dyi >= 0) ||
              ((tex->op == nir_texop_tex || tex->op == nir_texop_txb) && b->shader->info.stage == MESA_SHADER_FRAGMENT)) {
      nir_def *size0 = nir_i2f32(b, &tex_like(b, tex, nir_texop_txs, NULL, nir_imm_int(b, 0), nc, nir_type_int32)->def);
      nir_def *sp = nir_trim_vector(b, coord, spatial);
      nir_def *dx = tex->op == nir_texop_txd ? tex->src[dxi].src.ssa : nir_ddx(b, sp);
      nir_def *dy = tex->op == nir_texop_txd ? tex->src[dyi].src.ssa : nir_ddy(b, sp);
      nir_def *sz = nir_trim_vector(b, size0, spatial);
      dx = nir_fmul(b, dx, sz);
      dy = nir_fmul(b, dy, sz);
      nir_def *rho = nir_fmax(b, nir_fsqrt(b, nir_fdot(b, dx, dx)), nir_fsqrt(b, nir_fdot(b, dy, dy)));
      flod = nir_flog2(b, rho);
      if (bi >= 0)
         flod = nir_fadd(b, flod, tex->src[bi].src.ssa);
   }
   nir_def *lod = nir_imm_int(b, 0);
   if (flod) {
      nir_def *levels = &tex_like(b, tex, nir_texop_query_levels, NULL, NULL, 1, nir_type_int32)->def;
      lod = nir_iadd_imm(b, nir_f2i32(b, nir_fceil(b, nir_fadd_imm(b, flod, 0.5f))), -1);
      lod = nir_imax(b, nir_imin(b, lod, nir_iadd_imm(b, levels, -1)), nir_imm_int(b, 0));
      /* a NaN or -inf LOD (zero derivatives) is level 0 */
      lod = nir_bcsel(b, nir_fgt_imm(b, flod, 0.0f), lod, nir_imm_int(b, 0));
   }
   nir_def *size = &tex_like(b, tex, nir_texop_txs, NULL, lod, nc, nir_type_int32)->def;
   nir_def *c[4];
   for (unsigned i = 0; i < spatial; i++) {
      nir_def *x = nir_f2i32(b, nir_ffloor(b, nir_fmul(b, nir_channel(b, coord, i),
                                                         nir_i2f32(b, nir_channel(b, size, i)))));
      if (oi >= 0)
         x = nir_iadd(b, x, nir_channel(b, tex->src[oi].src.ssa, i));
      c[i] = nir_imax(b, nir_imin(b, x, nir_iadd_imm(b, nir_channel(b, size, i), -1)), nir_imm_int(b, 0));
   }
   if (tex->is_array) {
      nir_def *layer = nir_f2i32(b, nir_fround_even(b, nir_channel(b, coord, nc - 1)));
      c[nc - 1] = nir_imax(b, nir_imin(b, layer, nir_iadd_imm(b, nir_channel(b, size, nc - 1), -1)),
                           nir_imm_int(b, 0));
   }
   nir_tex_instr *txf = tex_like(b, tex, nir_texop_txf, nir_vec(b, c, nc), lod, tex->def.num_components,
                                 tex->dest_type);
   nir_def_replace(&tex->def, &txf->def);
   return true;
}

/* ---------------------------------------------------------------- gl_PointCoord */

/*
 * WebGPU points are one pixel. Large points become quads (T4 / T2 expand them); the fragment shader then
 * reads gl_PointCoord from a generic varying that the expanded vertices write: the highest location the
 * shader does not read, noperspective.
 */
static bool
lower_point_coord(nir_shader *s, struct wgvk_legalize_info *info)
{
   uint32_t used = 0;
   bool any = false;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_load_point_coord ||
                intr->intrinsic == nir_intrinsic_load_point_coord_maybe_flipped)
               any = true;
            if (intr->intrinsic == nir_intrinsic_load_input ||
                intr->intrinsic == nir_intrinsic_load_interpolated_input) {
               nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
               if (sem.location >= VARYING_SLOT_VAR0 && sem.location < VARYING_SLOT_VAR0 + 32)
                  used |= BITFIELD_RANGE(sem.location - VARYING_SLOT_VAR0, MAX2(sem.num_slots, 1));
            }
         }
      }
   }
   if (!any)
      return false;
   int loc = -1;
   for (int i = 15; i >= 0; i--) {
      if (!(used & BITFIELD_BIT(i))) {
         loc = i;
         break;
      }
   }
   if (loc < 0) {
      lfail(info, "point-coord", "no free varying location for gl_PointCoord");
      return false;
   }
   info->point_coord_location = loc;
   info->variant_mask |= WGVK_VARIANT_POINT_SIZE;
   nir_foreach_function_impl(impl, s) {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_point_coord &&
                intr->intrinsic != nir_intrinsic_load_point_coord_maybe_flipped)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_def *bary = nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_NOPERSPECTIVE);
            nir_io_semantics sem = { .location = VARYING_SLOT_VAR0 + loc, .num_slots = 1 };
            nir_def *v = nir_load_interpolated_input(&b, 2, 32, bary, nir_imm_int(&b, 0), .base = loc,
                                                     .component = 0, .dest_type = nir_type_float32,
                                                     .io_semantics = sem);
            nir_def_replace(&intr->def, v);
         }
      }
      nir_progress(true, impl, nir_metadata_control_flow);
   }
   s->info.inputs_read |= BITFIELD64_BIT(VARYING_SLOT_VAR0 + loc);
   return true;
}

/* ---------------------------------------------------------------- image atomics */

/*
 * WebGPU has no image atomics. A storage image the shader uses atomically is a storage buffer instead
 * (WGVK_LOWERED_IMAGE_BUFFER): a 16-byte header {width, height, depth or layers, 0}, then 32-bit texels,
 * x fastest. Every access of that image in the shader goes to the buffer; out-of-bounds reads give 0 and
 * out-of-bounds writes and atomics do nothing, as with robust image access.
 */
struct img_state {
   struct set *vars;
   struct wgvk_legalize_info *info;
};

static nir_def *
img_buffer_index(nir_builder *b, nir_deref_instr *deref, struct wgvk_legalize_info *info, nir_variable **out)
{
   nir_variable *var = nir_deref_instr_get_variable(deref);
   unsigned index = 0;
   if (deref->deref_type == nir_deref_type_array) {
      if (!nir_src_is_const(deref->arr.index)) {
         lfail(info, "image-atomic", "dynamically indexed array of atomically used images");
         return NULL;
      }
      index = nir_src_as_uint(deref->arr.index);
   }
   *out = var;
   unsigned before = info->num_lowered;
   add_lowered(info, var->data.descriptor_set, var->data.binding, index, WGVK_LOWERED_IMAGE_BUFFER,
               var->data.image.format);
   if (info->num_lowered > before)
      info->lowered[before].arrayed = glsl_sampler_type_is_array(glsl_without_array(var->type));
   return nir_imm_ivec2(b, var->data.descriptor_set << 16 | var->data.binding, index);
}

static bool
lower_image_atomic_instr(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct img_state *st = data;
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
   case nir_intrinsic_image_deref_samples:
      break;
   default:
      return false;
   }
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   if (!var || !_mesa_set_search(st->vars, var))
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   if (intr->intrinsic == nir_intrinsic_image_deref_samples) {
      nir_def_replace(&intr->def, nir_imm_int(b, 1));
      return true;
   }
   nir_variable *v;
   nir_def *idx = img_buffer_index(b, deref, st->info, &v);
   if (!idx)
      return false;
   enum glsl_sampler_dim dim = nir_intrinsic_image_dim(intr);
   bool arrayed = nir_intrinsic_image_array(intr);
   nir_def *hdr = nir_load_ssbo(b, 3, 32, idx, nir_imm_int(b, 0), .align_mul = 16);
   nir_def *w = nir_channel(b, hdr, 0), *h = nir_channel(b, hdr, 1), *d = nir_channel(b, hdr, 2);

   if (intr->intrinsic == nir_intrinsic_image_deref_size) {
      nir_def *c[3] = { w, h, d };
      if (dim == GLSL_SAMPLER_DIM_1D && arrayed)
         c[1] = d;
      unsigned n = intr->def.num_components;
      nir_def_replace(&intr->def, nir_vec(b, c, n));
      return true;
   }
   nir_def *coord = intr->src[1].ssa;
   nir_def *x = nir_channel(b, coord, 0), *y = nir_imm_int(b, 0), *z = nir_imm_int(b, 0);
   if (dim == GLSL_SAMPLER_DIM_1D) {
      if (arrayed)
         z = nir_channel(b, coord, 1);
   } else {
      y = nir_channel(b, coord, 1);
      if (arrayed || dim == GLSL_SAMPLER_DIM_3D)
         z = nir_channel(b, coord, 2);
   }
   if (dim == GLSL_SAMPLER_DIM_CUBE || dim == GLSL_SAMPLER_DIM_MS) {
      lfail(st->info, "image-atomic", "cube or multisampled image used atomically");
      return false;
   }
   nir_def *inb = nir_iand(b, nir_iand(b, nir_ult(b, x, w), nir_ult(b, y, h)), nir_ult(b, z, d));
   nir_def *texel = nir_iadd(b, nir_imul(b, nir_iadd(b, nir_imul(b, z, h), y), w), x);
   nir_def *off = nir_ishl_imm(b, nir_iadd_imm(b, texel, 4), 2);

   nir_def *res = NULL, *zero = nir_imm_int(b, 0);   /* before the if: a phi must come first in its block */
   nir_push_if(b, inb);
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load: {
      nir_def *t = nir_load_ssbo(b, 1, 32, idx, off, .align_mul = 4);
      nir_pop_if(b, NULL);
      t = nir_if_phi(b, t, zero);
      nir_alu_type dt = nir_intrinsic_dest_type(intr);
      nir_def *one = nir_alu_type_get_base_type(dt) == nir_type_float ? nir_imm_float(b, 1.0f) : nir_imm_int(b, 1);
      nir_def *c[4] = { t, nir_imm_int(b, 0), nir_imm_int(b, 0), one };
      res = nir_vec(b, c, intr->def.num_components);
      break;
   }
   case nir_intrinsic_image_deref_store:
      nir_store_ssbo(b, nir_channel(b, intr->src[3].ssa, 0), idx, off, .write_mask = 1, .align_mul = 4);
      nir_pop_if(b, NULL);
      nir_instr_remove(&intr->instr);
      return true;
   case nir_intrinsic_image_deref_atomic: {
      nir_def *t = nir_ssbo_atomic(b, 32, idx, off, intr->src[3].ssa,
                                   .atomic_op = nir_intrinsic_atomic_op(intr));
      nir_pop_if(b, NULL);
      res = nir_if_phi(b, t, zero);
      break;
   }
   case nir_intrinsic_image_deref_atomic_swap: {
      nir_def *t = nir_ssbo_atomic_swap(b, 32, idx, off, intr->src[3].ssa, intr->src[4].ssa,
                                        .atomic_op = nir_intrinsic_atomic_op(intr));
      nir_pop_if(b, NULL);
      res = nir_if_phi(b, t, zero);
      break;
   }
   default:
      UNREACHABLE("handled above");
   }
   nir_def_replace(&intr->def, res);
   return true;
}

static void
lower_image_atomics(nir_shader *s, struct wgvk_legalize_info *info)
{
   struct set *vars = _mesa_pointer_set_create(NULL);
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_image_deref_atomic &&
                intr->intrinsic != nir_intrinsic_image_deref_atomic_swap)
               continue;
            nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
            if (var)
               _mesa_set_add(vars, var);
         }
      }
   }
   if (vars->entries) {
      /* collected first: the lowering splits blocks, which an instruction walk would trip over */
      struct img_state st = { .vars = vars, .info = info };
      nir_function_impl *impl = nir_shader_get_entrypoint(s);
      struct util_dynarray list;
      util_dynarray_init(&list, NULL);
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic)
               util_dynarray_append(&list, nir_instr_as_intrinsic(instr));
         }
      }
      nir_builder b = nir_builder_create(impl);
      bool progress = false;
      util_dynarray_foreach(&list, nir_intrinsic_instr *, ip)
         progress |= lower_image_atomic_instr(&b, *ip, &st);
      util_dynarray_fini(&list);
      nir_progress(progress, impl, nir_metadata_none);
   }
   _mesa_set_destroy(vars, NULL);
}

/* ---------------------------------------------------------------- instruction lists */

/* The passes below split blocks (they add ifs), so they collect their instructions first. */
static void
collect_instrs(nir_function_impl *impl, struct util_dynarray *list)
{
   util_dynarray_init(list, NULL);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block)
         util_dynarray_append(list, instr);
   }
}

/* ---------------------------------------------------------------- dynamically indexed binding arrays */

/*
 * WebGPU has no binding arrays (without a flag): wgvk gives every array element its own binding
 * (INTERFACES 4.2). An access with a dynamic element index becomes a binary tree of ifs on the index,
 * each leaf the same access with a constant index (log2 N deep). Out-of-range indices land on the last
 * element, which Vulkan leaves undefined.
 */
struct ladder {
   nir_instr *instr;
   nir_def *index;               /* the dynamic element index */
   /* how to make the copy for element i */
   enum { LADDER_DEREF, LADDER_BUFFER } kind;
   nir_deref_instr *deref;       /* LADDER_DEREF: the array deref the instruction uses */
   unsigned buf_src;             /* LADDER_BUFFER: which source is the (slot, element) index */
};

static void
ladder_fix_copy(nir_builder *b, struct ladder *l, nir_instr *copy, unsigned i)
{
   if (l->kind == LADDER_DEREF) {
      nir_deref_instr *parent = nir_deref_instr_parent(l->deref);
      nir_deref_instr *d = nir_build_deref_array_imm(b, parent, i);
      if (copy->type == nir_instr_type_tex) {
         nir_tex_instr *t = nir_instr_as_tex(copy);
         for (unsigned s = 0; s < t->num_srcs; s++)
            if (t->src[s].src.ssa == &l->deref->def)
               nir_src_rewrite(&t->src[s].src, &d->def);
      } else {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(copy);
         nir_src_rewrite(&intr->src[0], &d->def);
      }
   } else {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(copy);
      nir_def *old = intr->src[l->buf_src].ssa;
      nir_def *c[4];
      for (unsigned k = 0; k < old->num_components; k++)
         c[k] = k == 1 ? nir_imm_int(b, i) : nir_channel(b, old, k);
      nir_src_rewrite(&intr->src[l->buf_src], nir_vec(b, c, old->num_components));
   }
}

static nir_def *
ladder_emit(nir_builder *b, struct ladder *l, unsigned lo, unsigned hi)
{
   nir_def *def = nir_instr_def(l->instr);
   if (lo == hi) {
      nir_instr *copy = nir_instr_clone(b->shader, l->instr);
      nir_builder_instr_insert(b, copy);
      b->cursor = nir_before_instr(copy);
      ladder_fix_copy(b, l, copy, lo);
      b->cursor = nir_after_instr(copy);
      return def ? nir_instr_def(copy) : NULL;
   }
   unsigned mid = (lo + hi) / 2;
   nir_push_if(b, nir_ule_imm(b, l->index, mid));
   nir_def *a = ladder_emit(b, l, lo, mid);
   nir_push_else(b, NULL);
   nir_def *c = ladder_emit(b, l, mid + 1, hi);
   nir_pop_if(b, NULL);
   return def ? nir_if_phi(b, a, c) : NULL;
}

static unsigned
var_array_size(nir_variable *var)
{
   return var && glsl_type_is_array(var->type) ? glsl_get_aoa_size(var->type) : 1;
}

static nir_variable *
buffer_var(nir_shader *s, bool ubo, unsigned slot)
{
   nir_foreach_variable_with_modes(var, s, ubo ? nir_var_mem_ubo : nir_var_mem_ssbo)
      if (var->data.descriptor_set == slot >> 16 && var->data.binding == (slot & 0xffff))
         return var;
   return NULL;
}

static bool
lower_dynamic_arrays(nir_shader *s, struct wgvk_legalize_info *info)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   struct util_dynarray list;
   collect_instrs(impl, &list);
   nir_builder b = nir_builder_create(impl);
   bool progress = false;
   util_dynarray_foreach(&list, nir_instr *, ip) {
      nir_instr *instr = *ip;
      struct ladder l = { .instr = instr };
      unsigned n = 0;
      if (instr->type == nir_instr_type_tex) {
         nir_tex_instr *t = nir_instr_as_tex(instr);
         for (unsigned i = 0; i < t->num_srcs; i++) {
            if (t->src[i].src_type != nir_tex_src_texture_deref && t->src[i].src_type != nir_tex_src_sampler_deref)
               continue;
            nir_deref_instr *d = nir_src_as_deref(t->src[i].src);
            if (d->deref_type == nir_deref_type_array && !nir_src_is_const(d->arr.index)) {
               if (l.deref && l.deref != d) {
                  lfail(info, "binding-array-dynamic", "texture and sampler indexed by different dynamic indices");
                  n = 0;
                  break;
               }
               l.kind = LADDER_DEREF;
               l.deref = d;
               l.index = d->arr.index.ssa;
               n = var_array_size(nir_deref_instr_get_variable(d));
            }
         }
      } else if (instr->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         const char *name = nir_intrinsic_infos[intr->intrinsic].name;
         if (!strncmp(name, "image_deref_", 12)) {
            nir_deref_instr *d = nir_src_as_deref(intr->src[0]);
            if (d->deref_type == nir_deref_type_array && !nir_src_is_const(d->arr.index)) {
               l.kind = LADDER_DEREF;
               l.deref = d;
               l.index = d->arr.index.ssa;
               n = var_array_size(nir_deref_instr_get_variable(d));
            }
         } else {
            int src = -1;
            bool ubo = false;
            switch (intr->intrinsic) {
            case nir_intrinsic_load_ubo: src = 0; ubo = true; break;
            case nir_intrinsic_load_ssbo:
            case nir_intrinsic_ssbo_atomic:
            case nir_intrinsic_ssbo_atomic_swap:
            case nir_intrinsic_get_ssbo_size: src = 0; break;
            case nir_intrinsic_store_ssbo: src = 1; break;
            default: break;
            }
            if (src >= 0 && intr->src[src].ssa->num_components >= 2) {
               nir_scalar sx = nir_scalar_resolved(intr->src[src].ssa, 0);
               nir_scalar sy = nir_scalar_resolved(intr->src[src].ssa, 1);
               if (nir_scalar_is_const(sx) && !nir_scalar_is_const(sy)) {
                  l.kind = LADDER_BUFFER;
                  l.buf_src = src;
                  b.cursor = nir_before_instr(instr);
                  l.index = nir_channel(&b, intr->src[src].ssa, 1);
                  n = var_array_size(buffer_var(s, ubo, nir_scalar_as_uint(sx)));
               }
            }
         }
      }
      if (n <= 1 && l.index) {
         /* an array of one, or unknown: element 0 */
         n = 1;
      }
      if (!l.index || !n)
         continue;
      b.cursor = nir_before_instr(instr);
      nir_def *r = ladder_emit(&b, &l, 0, n - 1);
      if (r)
         nir_def_rewrite_uses(nir_instr_def(instr), r);
      nir_instr_remove(instr);
      progress = true;
   }
   util_dynarray_fini(&list);
   nir_progress(progress, impl, nir_metadata_none);
   return progress;
}

/* ---------------------------------------------------------------- input attachments */

/*
 * A subpass input is the attachment's texel at this fragment: a texture (the descriptor binds the view)
 * read with textureLoad at the fragment's pixel. Layered rendering into input attachments reads layer 0.
 */
static bool
lower_input_attachments(nir_shader *s, struct wgvk_legalize_info *info)
{
   bool progress = false;
   nir_foreach_variable_with_modes(var, s, nir_var_image) {
      const struct glsl_type *t = glsl_without_array(var->type);
      enum glsl_sampler_dim dim = glsl_get_sampler_dim(t);
      if (dim != GLSL_SAMPLER_DIM_SUBPASS && dim != GLSL_SAMPLER_DIM_SUBPASS_MS)
         continue;
      const struct glsl_type *tex = glsl_texture_type(dim == GLSL_SAMPLER_DIM_SUBPASS ? GLSL_SAMPLER_DIM_2D :
                                                      GLSL_SAMPLER_DIM_MS, false, glsl_get_sampler_result_type(t));
      var->type = glsl_type_wrap_in_arrays(tex, var->type);
      var->data.mode = nir_var_uniform;
      progress = true;
   }
   if (!progress)
      return false;
   nir_fixup_deref_modes(s);
   nir_fixup_deref_types(s);
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_builder b = nir_builder_create(impl);
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_image_deref_load)
            continue;
         nir_deref_instr *d = nir_src_as_deref(intr->src[0]);
         if (!d || !nir_deref_mode_is(d, nir_var_uniform))
            continue;
         enum glsl_sampler_dim dim = nir_intrinsic_image_dim(intr);
         bool ms = dim == GLSL_SAMPLER_DIM_SUBPASS_MS;
         b.cursor = nir_before_instr(instr);
         nir_def *pos = nir_f2i32(&b, nir_trim_vector(&b, nir_load_frag_coord(&b), 2));
         nir_def *coord = nir_iadd(&b, pos, nir_trim_vector(&b, intr->src[1].ssa, 2));
         nir_tex_instr *t = nir_tex_instr_create(s, ms ? 3 : 3);
         t->op = ms ? nir_texop_txf_ms : nir_texop_txf;
         t->sampler_dim = ms ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D;
         t->dest_type = nir_intrinsic_dest_type(intr);
         t->coord_components = 2;
         t->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &d->def);
         t->src[1] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
         t->src[2] = ms ? nir_tex_src_for_ssa(nir_tex_src_ms_index, intr->src[2].ssa) :
                          nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(&b, 0));
         nir_def_init(&t->instr, &t->def, 4, 32);
         nir_builder_instr_insert(&b, &t->instr);
         nir_def_replace(&intr->def, nir_trim_vector(&b, &t->def, intr->def.num_components));
      }
   }
   nir_progress(true, impl, nir_metadata_control_flow);
   (void)info;
   return true;
}

/* ---------------------------------------------------------------- texel formats in the shader */

/* A 32-bit float from the low 16 bits of x (an IEEE half). */
static nir_def *
half_to_float(nir_builder *b, nir_def *x)
{
   return nir_channel(b, nir_unpack_half_2x16(b, x), 0);
}

static bool
format_supported_in_shader(enum pipe_format f)
{
   if (f == PIPE_FORMAT_R11G11B10_FLOAT || f == PIPE_FORMAT_R9G9B9E5_FLOAT)
      return true;
   const struct util_format_description *d = util_format_description(f);
   if (!d || d->layout != UTIL_FORMAT_LAYOUT_PLAIN || d->block.width != 1 || d->block.height != 1)
      return false;
   if (d->block.bits % 8 || d->block.bits > 128 || d->block.bits == 24 || d->block.bits == 48 || d->block.bits == 96)
      return d->block.bits == 96;   /* 3-channel 32-bit formats are word aligned */
   for (unsigned c = 0; c < d->nr_channels; c++) {
      const struct util_format_channel_description *ch = &d->channel[c];
      if (ch->size > 32 || ch->type == UTIL_FORMAT_TYPE_FIXED)
         return false;
      if (ch->type == UTIL_FORMAT_TYPE_FLOAT && ch->size != 16 && ch->size != 32)
         return false;
      if (ch->size == 32 && ch->shift % 32)
         return false;
   }
   return true;
}

/* rgba (32-bit components) from the texel's words (w[0] holds the lowest bits). Only 32-bit NIR. */
static nir_def *
unpack_texel(nir_builder *b, nir_def **w, enum pipe_format f)
{
   if (f == PIPE_FORMAT_R9G9B9E5_FLOAT) {
      nir_def *rgb = nir_format_unpack_r9g9b9e5(b, w[0]);
      return nir_vec4(b, nir_channel(b, rgb, 0), nir_channel(b, rgb, 1), nir_channel(b, rgb, 2), nir_imm_float(b, 1));
   }
   if (f == PIPE_FORMAT_R11G11B10_FLOAT) {
      /* 11/10-bit floats are halves without the sign and the low mantissa bits */
      nir_def *r = half_to_float(b, nir_ishl_imm(b, nir_ubitfield_extract_imm(b, w[0], 0, 11), 4));
      nir_def *g = half_to_float(b, nir_ishl_imm(b, nir_ubitfield_extract_imm(b, w[0], 11, 11), 4));
      nir_def *bb = half_to_float(b, nir_ishl_imm(b, nir_ubitfield_extract_imm(b, w[0], 22, 10), 5));
      return nir_vec4(b, r, g, bb, nir_imm_float(b, 1));
   }
   const struct util_format_description *d = util_format_description(f);
   nir_def *comps[4] = { NULL };
   for (unsigned c = 0; c < d->nr_channels; c++) {
      const struct util_format_channel_description *ch = &d->channel[c];
      nir_def *word = w[ch->shift / 32];
      unsigned sh = ch->shift % 32;
      nir_def *raw = ch->size == 32 ? word : nir_ubitfield_extract_imm(b, word, sh, ch->size);
      unsigned bits[1] = { ch->size };
      switch (ch->type) {
      case UTIL_FORMAT_TYPE_VOID:
         comps[c] = nir_imm_int(b, 0);
         break;
      case UTIL_FORMAT_TYPE_UNSIGNED:
         comps[c] = ch->normalized ? nir_format_unorm_to_float(b, raw, bits) :
                    ch->pure_integer ? raw : nir_u2f32(b, raw);
         break;
      case UTIL_FORMAT_TYPE_SIGNED:
         if (ch->size < 32)
            raw = nir_ibitfield_extract_imm(b, word, sh, ch->size);
         comps[c] = ch->normalized ? nir_format_snorm_to_float(b, raw, bits) :
                    ch->pure_integer ? raw : nir_i2f32(b, raw);
         break;
      case UTIL_FORMAT_TYPE_FLOAT:
         comps[c] = ch->size == 16 ? half_to_float(b, raw) : raw;
         break;
      default:
         comps[c] = nir_imm_int(b, 0);
      }
   }
   nir_def *out[4];
   for (unsigned i = 0; i < 4; i++) {
      enum pipe_swizzle sw = d->swizzle[i];
      if (sw <= PIPE_SWIZZLE_W && comps[sw])
         out[i] = comps[sw];
      else if (sw == PIPE_SWIZZLE_1)
         out[i] = util_format_is_pure_integer(f) ? nir_imm_int(b, 1) : nir_imm_float(b, 1);
      else
         out[i] = nir_imm_int(b, 0);
   }
   nir_def *rgba = nir_vec(b, out, 4);
   if (d->colorspace == UTIL_FORMAT_COLORSPACE_SRGB)
      rgba = nir_vector_insert_imm(b, nir_format_srgb_to_linear(b, rgba), nir_channel(b, rgba, 3), 3);
   return rgba;
}

/* The texel's words (n = block bits / 32, at least 1) for rgba. Unused bits are zero. */
static void
pack_texel(nir_builder *b, nir_def *rgba, enum pipe_format f, nir_def **w, unsigned *n)
{
   const struct util_format_description *d = util_format_description(f);
   *n = MAX2(1, d->block.bits / 32);
   if (rgba->num_components < 4)
      rgba = nir_pad_vector_imm_int(b, rgba, 0, 4);
   if (f == PIPE_FORMAT_R9G9B9E5_FLOAT) {
      w[0] = nir_format_pack_r9g9b9e5(b, rgba);
      return;
   }
   if (f == PIPE_FORMAT_R11G11B10_FLOAT) {
      nir_def *c = nir_fmax(b, rgba, nir_imm_float(b, 0));
      nir_def *r = nir_pack_half_2x16_split(b, nir_channel(b, c, 0), nir_imm_float(b, 0));
      nir_def *g = nir_pack_half_2x16_split(b, nir_channel(b, c, 1), nir_imm_float(b, 0));
      nir_def *bb = nir_pack_half_2x16_split(b, nir_channel(b, c, 2), nir_imm_float(b, 0));
      w[0] = nir_ior(b, nir_ior(b, nir_ubitfield_extract_imm(b, r, 4, 11),
                                nir_ishl_imm(b, nir_ubitfield_extract_imm(b, g, 4, 11), 11)),
                     nir_ishl_imm(b, nir_ubitfield_extract_imm(b, bb, 5, 10), 22));
      return;
   }
   if (d->colorspace == UTIL_FORMAT_COLORSPACE_SRGB)
      rgba = nir_vector_insert_imm(b, nir_format_linear_to_srgb(b, rgba), nir_channel(b, rgba, 3), 3);
   for (unsigned i = 0; i < *n; i++)
      w[i] = nir_imm_int(b, 0);
   for (unsigned i = 0; i < 4; i++) {
      enum pipe_swizzle sw = d->swizzle[i];
      if (sw > PIPE_SWIZZLE_W)
         continue;
      const struct util_format_channel_description *ch = &d->channel[sw];
      nir_def *v = nir_channel(b, rgba, i);
      unsigned bits[1] = { ch->size };
      switch (ch->type) {
      case UTIL_FORMAT_TYPE_UNSIGNED:
         v = ch->normalized ? nir_format_float_to_unorm(b, v, bits) :
             ch->pure_integer ? (ch->size < 32 ? nir_umin(b, v, nir_imm_int(b, u_uintN_max(ch->size))) : v) :
             nir_f2u32(b, v);
         break;
      case UTIL_FORMAT_TYPE_SIGNED:
         v = ch->normalized ? nir_format_float_to_snorm(b, v, bits) :
             ch->pure_integer ? (ch->size < 32 ? nir_format_clamp_sint(b, v, bits) : v) :
             nir_f2i32(b, v);
         break;
      case UTIL_FORMAT_TYPE_FLOAT:
         if (ch->size == 16)
            v = nir_pack_half_2x16_split(b, v, nir_imm_float(b, 0));
         break;
      default:
         continue;
      }
      if (ch->size < 32)
         v = nir_iand_imm(b, v, u_uintN_max(ch->size));
      unsigned k = ch->shift / 32, sh = ch->shift % 32;
      w[k] = nir_ior(b, w[k], sh ? nir_ishl_imm(b, v, sh) : v);
   }
}

/* ---------------------------------------------------------------- texel buffers */

/*
 * WebGPU has no texel buffers. A uniform texel buffer is a read-only storage buffer and a storage texel
 * buffer a storage buffer (WGVK_LOWERED_TEXEL_BUFFER), over the buffer view's range; texel i is at byte
 * i * texel size, decoded or encoded in the shader. Reads out of range give 0, writes and atomics out of
 * range do nothing. Texels smaller than a word are written with atomicAnd/atomicOr so that neighbours
 * are kept.
 */
struct tb_slot {
   uint8_t set;
   uint16_t binding, index;
   nir_alu_type type;          /* sampled / image type */
   enum pipe_format qualifier;  /* storage texel buffers: the shader's format qualifier, if any */
   bool storage;
};

static int
cmp_tb_slot(const void *a, const void *b)
{
   const struct tb_slot *x = a, *y = b;
   if (x->set != y->set)
      return x->set - y->set;
   if (x->binding != y->binding)
      return x->binding - y->binding;
   return x->index - y->index;
}

static bool
deref_slot(nir_deref_instr *d, unsigned *set, unsigned *binding, unsigned *index)
{
   nir_variable *var = nir_deref_instr_get_variable(d);
   if (!var)
      return false;
   *index = 0;
   if (d->deref_type == nir_deref_type_array) {
      if (!nir_src_is_const(d->arr.index))
         return false;
      *index = nir_src_as_uint(d->arr.index);
   }
   *set = var->data.descriptor_set;
   *binding = var->data.binding;
   return true;
}

static nir_deref_instr *
texel_buffer_deref(nir_instr *instr)
{
   if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *t = nir_instr_as_tex(instr);
      if (t->sampler_dim != GLSL_SAMPLER_DIM_BUF)
         return NULL;
      int i = nir_tex_instr_src_index(t, nir_tex_src_texture_deref);
      return i >= 0 ? nir_src_as_deref(t->src[i].src) : NULL;
   }
   if (instr->type != nir_instr_type_intrinsic)
      return NULL;
   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
   case nir_intrinsic_image_deref_size:
      return nir_intrinsic_image_dim(intr) == GLSL_SAMPLER_DIM_BUF ? nir_src_as_deref(intr->src[0]) : NULL;
   default:
      return NULL;
   }
}

static enum pipe_format
default_format(nir_alu_type t)
{
   switch (nir_alu_type_get_base_type(t)) {
   case nir_type_int: return PIPE_FORMAT_R32G32B32A32_SINT;
   case nir_type_uint: return PIPE_FORMAT_R32G32B32A32_UINT;
   default: return PIPE_FORMAT_R32G32B32A32_FLOAT;
   }
}

static nir_alu_type
var_result_type(nir_variable *var)
{
   return nir_get_nir_type_for_glsl_base_type(glsl_get_sampler_result_type(glsl_without_array(var->type)));
}

static bool
lower_texel_buffers(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   struct util_dynarray list;
   collect_instrs(impl, &list);
   struct tb_slot slots[8];
   unsigned nslots = 0;
   bool any = false;
   util_dynarray_foreach(&list, nir_instr *, ip) {
      nir_deref_instr *d = texel_buffer_deref(*ip);
      if (!d)
         continue;
      any = true;
      unsigned set, binding, index;
      if (!deref_slot(d, &set, &binding, &index)) {
         lfail(info, "texel-buffer", "texel buffer through a dynamic index");
         continue;
      }
      bool have = false;
      for (unsigned i = 0; i < nslots; i++)
         have |= slots[i].set == set && slots[i].binding == binding && slots[i].index == index;
      if (have)
         continue;
      if (nslots == ARRAY_SIZE(slots)) {
         lfail(info, "limits", "more than %u texel buffers", (unsigned)ARRAY_SIZE(slots));
         continue;
      }
      nir_variable *var = nir_deref_instr_get_variable(d);
      slots[nslots++] = (struct tb_slot){ .set = set, .binding = binding, .index = index,
                                          .type = var_result_type(var),
                                          .qualifier = var->data.mode == nir_var_image ? var->data.image.format :
                                                       PIPE_FORMAT_NONE,
                                          .storage = var->data.mode == nir_var_image };
   }
   if (!any) {
      util_dynarray_fini(&list);
      return false;
   }
   qsort(slots, nslots, sizeof(slots[0]), cmp_tb_slot);
   enum pipe_format fmt[8];
   info->num_texel_slots = nslots;
   for (unsigned i = 0; i < nslots; i++) {
      info->texel_slot[i].set = slots[i].set;
      info->texel_slot[i].binding = slots[i].binding;
      info->texel_slot[i].index = slots[i].index;
      fmt[i] = key->texel_buffer_format[i];
      if (fmt[i] == PIPE_FORMAT_NONE)
         fmt[i] = slots[i].qualifier;
      if (fmt[i] == PIPE_FORMAT_NONE)
         fmt[i] = default_format(slots[i].type);
      if (slots[i].qualifier == PIPE_FORMAT_NONE)
         info->variant_mask |= WGVK_VARIANT_TEXEL_BUFFER_FORMAT;
      if (!format_supported_in_shader(fmt[i]))
         lfail(info, "texel-buffer-format", "texel buffer format %s", util_format_short_name(fmt[i]));
      add_lowered(info, slots[i].set, slots[i].binding, slots[i].index, WGVK_LOWERED_TEXEL_BUFFER, fmt[i]);
      info->lowered[info->num_lowered - 1].storage = slots[i].storage;
   }

   nir_builder b = nir_builder_create(impl);
   bool progress = false;
   util_dynarray_foreach(&list, nir_instr *, ip) {
      nir_instr *instr = *ip;
      nir_deref_instr *d = texel_buffer_deref(instr);
      unsigned set, binding, index, k = ~0u;
      if (!d || !deref_slot(d, &set, &binding, &index))
         continue;
      for (unsigned i = 0; i < nslots; i++)
         if (slots[i].set == set && slots[i].binding == binding && slots[i].index == index)
            k = i;
      if (k == ~0u || !format_supported_in_shader(fmt[k]))
         continue;
      enum pipe_format f = fmt[k];
      unsigned bpp = util_format_get_blocksize(f);
      b.cursor = nir_before_instr(instr);
      nir_def *idx = nir_imm_ivec2(&b, set << 16 | binding, index);
      nir_def *count = nir_udiv_imm(&b, nir_get_ssbo_size(&b, 32, idx), bpp);
      nir_def *zero = nir_imm_int(&b, 0);

      nir_intrinsic_instr *intr = instr->type == nir_instr_type_intrinsic ? nir_instr_as_intrinsic(instr) : NULL;
      nir_tex_instr *tex = instr->type == nir_instr_type_tex ? nir_instr_as_tex(instr) : NULL;
      bool size_q = (tex && tex->op == nir_texop_txs) || (intr && intr->intrinsic == nir_intrinsic_image_deref_size);
      if (size_q) {
         nir_def_replace(nir_instr_def(instr), nir_pad_vector_imm_int(&b, count, 0, nir_instr_def(instr)->num_components));
         progress = true;
         continue;
      }
      if (tex && tex->op != nir_texop_txf) {
         lfail(info, "texel-buffer", "texel buffer op %d", tex->op);
         continue;
      }
      nir_def *elem = tex ? nir_channel(&b, tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa, 0) :
                      nir_channel(&b, intr->src[1].ssa, 0);
      nir_def *off = nir_imul_imm(&b, elem, bpp);
      nir_def *inb = nir_ult(&b, elem, count);
      nir_def *def = nir_instr_def(instr);

      if (tex || intr->intrinsic == nir_intrinsic_image_deref_load) {
         nir_def *z4 = nir_imm_zero(&b, 4, 32);
         nir_push_if(&b, inb);
         nir_def *w[4];
         if (bpp >= 4) {
            nir_def *v = nir_load_ssbo(&b, bpp / 4, 32, idx, off, .align_mul = 4);
            for (unsigned i = 0; i < bpp / 4; i++)
               w[i] = nir_channel(&b, v, i);
         } else {
            nir_def *word = nir_load_ssbo(&b, 1, 32, idx, nir_iand_imm(&b, off, ~3u), .align_mul = 4);
            w[0] = nir_ushr(&b, word, nir_ishl_imm(&b, nir_iand_imm(&b, off, 3), 3));
         }
         nir_def *rgba = unpack_texel(&b, w, f);
         nir_pop_if(&b, NULL);
         nir_def *r = nir_if_phi(&b, rgba, z4);
         nir_def_replace(def, nir_trim_vector(&b, r, def->num_components));
      } else if (intr->intrinsic == nir_intrinsic_image_deref_store) {
         nir_push_if(&b, inb);
         nir_def *w[4];
         unsigned n;
         pack_texel(&b, intr->src[3].ssa, f, w, &n);
         if (bpp >= 4) {
            nir_store_ssbo(&b, nir_vec(&b, w, n), idx, off, .write_mask = BITFIELD_MASK(n), .align_mul = 4);
         } else {
            nir_def *wo = nir_iand_imm(&b, off, ~3u);
            nir_def *sh = nir_ishl_imm(&b, nir_iand_imm(&b, off, 3), 3);
            nir_def *mask = nir_ishl(&b, nir_imm_int(&b, (1u << (bpp * 8)) - 1), sh);
            nir_ssbo_atomic(&b, 32, idx, wo, nir_inot(&b, mask), .atomic_op = nir_atomic_op_iand);
            nir_ssbo_atomic(&b, 32, idx, wo, nir_ishl(&b, w[0], sh), .atomic_op = nir_atomic_op_ior);
         }
         nir_pop_if(&b, NULL);
         nir_instr_remove(instr);
      } else {
         if (bpp != 4) {
            lfail(info, "texel-buffer", "atomics on a texel buffer of %u-byte texels", bpp);
            continue;
         }
         nir_push_if(&b, inb);
         nir_def *t = intr->intrinsic == nir_intrinsic_image_deref_atomic ?
            nir_ssbo_atomic(&b, 32, idx, off, intr->src[3].ssa, .atomic_op = nir_intrinsic_atomic_op(intr)) :
            nir_ssbo_atomic_swap(&b, 32, idx, off, intr->src[3].ssa, intr->src[4].ssa,
                                 .atomic_op = nir_intrinsic_atomic_op(intr));
         nir_pop_if(&b, NULL);
         nir_def_replace(def, nir_if_phi(&b, t, zero));
      }
      progress = true;
   }
   util_dynarray_fini(&list);
   nir_progress(progress, impl, nir_metadata_none);
   return progress;
}

/* ---------------------------------------------------------------- storage images without a format */

/*
 * WGSL storage textures name their format. A storage image declared without one (Vulkan's
 * shaderStorageImage{Read,Write}WithoutFormat) gets the format of the view bound to it: the driver
 * compiles a variant per format (key.storage_image_format[slot], slots in info.format_slot, sorted by
 * set and binding; all elements of an array share one). Until it knows, RGBA32 of the image's type.
 */
static void
lower_formatless_images(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info)
{
   nir_variable *vars[16];
   unsigned n = 0;
   nir_foreach_variable_with_modes(var, s, nir_var_image) {
      enum glsl_sampler_dim dim = glsl_get_sampler_dim(glsl_without_array(var->type));
      if (var->data.image.format != PIPE_FORMAT_NONE || dim == GLSL_SAMPLER_DIM_BUF)
         continue;
      if (n == ARRAY_SIZE(vars)) {
         lfail(info, "limits", "more than 16 storage images without a format");
         break;
      }
      vars[n++] = var;
   }
   if (!n)
      return;
   /* sort by (set, binding) */
   for (unsigned i = 1; i < n; i++)
      for (unsigned j = i; j > 0 && (vars[j - 1]->data.descriptor_set > vars[j]->data.descriptor_set ||
                                     (vars[j - 1]->data.descriptor_set == vars[j]->data.descriptor_set &&
                                      vars[j - 1]->data.binding > vars[j]->data.binding)); j--) {
         nir_variable *t = vars[j];
         vars[j] = vars[j - 1];
         vars[j - 1] = t;
      }
   info->variant_mask |= WGVK_VARIANT_STORAGE_IMAGE_FORMAT;
   info->num_format_slots = n;
   for (unsigned i = 0; i < n; i++) {
      info->format_slot[i].set = vars[i]->data.descriptor_set;
      info->format_slot[i].binding = vars[i]->data.binding;
      info->format_slot[i].index = 0;
      enum pipe_format f = key->storage_image_format[i];
      vars[i]->data.image.format = f != PIPE_FORMAT_NONE ? f : default_format(var_result_type(vars[i]));
   }
   /* the image intrinsics carry the format too */
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (!nir_intrinsic_has_format(intr) || !nir_intrinsic_has_image_dim(intr))
               continue;
            nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
            if (var && var->data.mode == nir_var_image)
               nir_intrinsic_set_format(intr, var->data.image.format);
         }
      }
   }
}

/* ---------------------------------------------------------------- 8-, 16- and 64-bit values */

/*
 * WGSL has 32-bit integers and floats, plus f16 with shader-f16. Integer arithmetic on 8 and 16 bits (and
 * f16 without the feature) is widened to 32 bits (nir_lower_bit_size); 64-bit integers become pairs of
 * words (nir_lower_int64) and 64-bit floats the soft-float library (Mesa's float64.glsl, built into
 * float64_spv.h) when it is there. Buffer loads and stores of 8, 16 and 64 bits become 32-bit word
 * accesses: sub-word loads extract their bits, sub-word stores go through atomicAnd/atomicOr on the word
 * (neighbours kept), 64-bit values are split into two words.
 */
#ifdef WGVK_HAVE_SOFTFP64
#include "float64_spv.h"
#include "compiler/spirv/nir_spirv.h"
#include "compiler/spirv/spirv_info.h"
#endif

static unsigned
bit_size_cb(const nir_instr *instr, void *data)
{
   const struct wgvk_compile_caps *caps = data;
   if (instr->type == nir_instr_type_phi) {
      unsigned bs = nir_instr_as_phi((nir_instr *)instr)->def.bit_size;
      return bs == 8 || bs == 16 ? 32 : 0;
   }
   if (instr->type != nir_instr_type_alu)
      return 0;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (nir_op_infos[alu->op].is_conversion || nir_op_is_vec_or_mov(alu->op))
      return 0;
   /* the narrowest non-boolean size among the result and the sources (comparisons have 1-bit results) */
   unsigned bs = alu->def.bit_size == 1 ? 64 : alu->def.bit_size;
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
      if (nir_src_bit_size(alu->src[i].src) != 1)
         bs = MIN2(bs, nir_src_bit_size(alu->src[i].src));
   if (bs == 8)
      return 32;
   if (bs == 16) {
      nir_alu_type t = nir_op_infos[alu->op].output_type;
      bool fl = nir_alu_type_get_base_type(t) == nir_type_float;
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
         fl |= nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[i]) == nir_type_float;
      return fl && caps->shader_f16 ? 0 : 32;
   }
   return 0;
}

static bool
has_odd_bit_sizes(nir_shader *s, bool *has64, bool *hasf64)
{
   bool any = false;
   *has64 = *hasf64 = false;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_def *d = nir_instr_def(instr);
            if (instr->type == nir_instr_type_alu) {
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
                  unsigned bs = nir_src_bit_size(alu->src[i].src);
                  if (bs == 64 && nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[i]) == nir_type_float)
                     *hasf64 = true;
               }
               if (alu->def.bit_size == 64 &&
                   nir_alu_type_get_base_type(nir_op_infos[alu->op].output_type) == nir_type_float)
                  *hasf64 = true;
            }
            if (!d)
               continue;
            if (d->bit_size == 64)
               *has64 = any = true;
            else if (d->bit_size == 8 || d->bit_size == 16)
               any = true;
         }
      }
   }
   return any;
}

/* The containing word and the bit position of byte offset `off`. */
static void
word_of(nir_builder *b, nir_def *off, nir_def **word_off, nir_def **shift)
{
   *word_off = nir_iand_imm(b, off, ~3u);
   *shift = nir_ishl_imm(b, nir_iand_imm(b, off, 3), 3);
}

static nir_def *
load_words(nir_builder *b, nir_intrinsic_instr *intr, nir_def *off, unsigned n)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_ssbo:
      return nir_load_ssbo(b, n, 32, intr->src[0].ssa, off, .access = nir_intrinsic_access(intr), .align_mul = 4);
   case nir_intrinsic_load_ubo:
      return nir_load_ubo(b, n, 32, intr->src[0].ssa, off, .align_mul = 4, .align_offset = 0, .range_base = 0,
                          .range = ~0);
   case nir_intrinsic_load_push_constant:
      return nir_load_push_constant(b, n, 32, off, .base = nir_intrinsic_base(intr), .range = nir_intrinsic_range(intr));
   default:
      UNREACHABLE("load");
   }
}

/* 8/16-bit loads whose every use widens them to 32 bits (after nir_lower_bit_size), and 64-bit loads. */
static bool
lower_odd_load(nir_builder *b, nir_intrinsic_instr *intr)
{
   unsigned src_off = intr->intrinsic == nir_intrinsic_load_push_constant ? 0 : 1;
   unsigned bs = intr->def.bit_size, n = intr->def.num_components;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *off = intr->src[src_off].ssa;
   if (bs == 64) {
      nir_def *c[NIR_MAX_VEC_COMPONENTS];
      for (unsigned i = 0; i < n; i++) {
         nir_def *w = load_words(b, intr, nir_iadd_imm(b, off, 8 * i), 2);
         c[i] = nir_pack_64_2x32_split(b, nir_channel(b, w, 0), nir_channel(b, w, 1));
      }
      nir_def_replace(&intr->def, nir_vec(b, c, n));
      return true;
   }
   if (bs != 8 && bs != 16)
      return false;
   /* f16 loads stay (the emitter reads halves of words) when they are used as floats */
   nir_foreach_use(use, &intr->def) {
      nir_instr *u = nir_src_use_instr(use);
      if (u->type != nir_instr_type_alu)
         return false;
      nir_op op = nir_instr_as_alu(u)->op;
      if (op != nir_op_u2u32 && op != nir_op_i2i32 && op != nir_op_mov)
         return false;
      if (op == nir_op_mov)
         return false;
   }
   nir_def *zc[4], *sc[4];
   for (unsigned i = 0; i < n; i++) {
      nir_def *o = nir_iadd_imm(b, off, i * bs / 8);
      nir_def *wo, *sh;
      word_of(b, o, &wo, &sh);
      nir_def *word = load_words(b, intr, wo, 1);
      zc[i] = nir_ubitfield_extract(b, word, sh, nir_imm_int(b, bs));
      sc[i] = nir_ibitfield_extract(b, word, sh, nir_imm_int(b, bs));
   }
   nir_def *z = nir_vec(b, zc, n), *sx = nir_vec(b, sc, n);
   nir_foreach_use_safe(use, &intr->def) {
      nir_alu_instr *alu = nir_instr_as_alu(nir_src_use_instr(use));
      /* the conversion reads some components: rebuild it as a swizzle of the extended words */
      nir_def *src = alu->op == nir_op_i2i32 ? sx : z;
      b->cursor = nir_before_instr(&alu->instr);
      unsigned swz[NIR_MAX_VEC_COMPONENTS];
      for (unsigned k = 0; k < alu->def.num_components; k++)
         swz[k] = alu->src[0].swizzle[k];
      nir_def *v = nir_swizzle(b, src, swz, alu->def.num_components);
      nir_def_replace(&alu->def, v);
   }
   nir_instr_remove(&intr->instr);
   return true;
}

/* A store of an 8/16/64-bit value to a storage buffer, as word stores and atomics. */
static bool
lower_odd_store(nir_builder *b, nir_intrinsic_instr *intr)
{
   nir_def *val = intr->src[0].ssa;
   unsigned bs = val->bit_size, mask = nir_intrinsic_write_mask(intr);
   if (bs == 32)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *idx = intr->src[1].ssa, *off = intr->src[2].ssa;
   u_foreach_bit(i, mask) {
      nir_def *v = nir_channel(b, val, i);
      nir_def *o = nir_iadd_imm(b, off, i * bs / 8);
      if (bs == 64) {
         nir_store_ssbo(b, nir_unpack_64_2x32(b, v), idx, o, .write_mask = 3, .align_mul = 4);
         continue;
      }
      /* the value's bits in a 32-bit word: integers widen, halves are packed */
      nir_def *w = bs == 16 ? nir_u2u32(b, v) : nir_u2u32(b, v);
      w = nir_iand_imm(b, w, BITFIELD_MASK(bs));
      nir_def *wo, *sh;
      word_of(b, o, &wo, &sh);
      nir_def *m = nir_ishl(b, nir_imm_int(b, BITFIELD_MASK(bs)), sh);
      nir_ssbo_atomic(b, 32, idx, wo, nir_inot(b, m), .atomic_op = nir_atomic_op_iand);
      nir_ssbo_atomic(b, 32, idx, wo, nir_ishl(b, w, sh), .atomic_op = nir_atomic_op_ior);
   }
   nir_instr_remove(&intr->instr);
   return true;
}

/* 8/16-bit integer shader inputs and outputs travel as 32-bit values. */
static bool
lower_odd_io(nir_builder *b, nir_intrinsic_instr *intr)
{
   if (intr->intrinsic == nir_intrinsic_store_output) {
      nir_def *v = intr->src[0].ssa;
      if (v->bit_size != 8 && v->bit_size != 16)
         return false;
      nir_alu_type t = nir_intrinsic_src_type(intr);
      if (nir_alu_type_get_base_type(t) == nir_type_float)
         return false;
      b->cursor = nir_before_instr(&intr->instr);
      bool sgn = nir_alu_type_get_base_type(t) == nir_type_int;
      nir_src_rewrite(&intr->src[0], sgn ? nir_i2i32(b, v) : nir_u2u32(b, v));
      nir_intrinsic_set_src_type(intr, nir_alu_type_get_base_type(t) | 32);
      return true;
   }
   if (intr->intrinsic != nir_intrinsic_load_input && intr->intrinsic != nir_intrinsic_load_interpolated_input)
      return false;
   unsigned bs = intr->def.bit_size;
   nir_alu_type t = nir_intrinsic_dest_type(intr);
   if ((bs != 8 && bs != 16) || nir_alu_type_get_base_type(t) == nir_type_float)
      return false;
   b->cursor = nir_after_instr(&intr->instr);
   intr->def.bit_size = 32;
   nir_intrinsic_set_dest_type(intr, nir_alu_type_get_base_type(t) | 32);
   nir_def *narrow = nir_u2uN(b, &intr->def, bs);
   nir_def_rewrite_uses_after(&intr->def, narrow);
   return true;
}

/* x8 = u2u8/i2i8(x32) (or 16) read only through widening conversions: the widened value straight from x */
static bool
fold_narrow_conversions(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if (alu->op != nir_op_u2u8 && alu->op != nir_op_i2i8 && alu->op != nir_op_u2u16 && alu->op != nir_op_i2i16)
      return false;
   if (nir_src_bit_size(alu->src[0].src) != 32 || alu->def.num_components != 1)
      return false;
   unsigned bits = alu->def.bit_size;
   nir_foreach_use(use, &alu->def) {
      nir_instr *u = nir_src_use_instr(use);
      if (u->type != nir_instr_type_alu)
         return false;
      nir_op op = nir_instr_as_alu(u)->op;
      if (op != nir_op_u2u32 && op != nir_op_i2i32 && op != nir_op_u2f32 && op != nir_op_i2f32)
         return false;
   }
   b->cursor = nir_after_instr(&alu->instr);
   nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
   nir_def *z = nir_iand_imm(b, x, BITFIELD_MASK(bits));
   nir_def *sx = nir_ibitfield_extract_imm(b, x, 0, bits);
   nir_foreach_use_safe(use, &alu->def) {
      nir_alu_instr *u = nir_instr_as_alu(nir_src_use_instr(use));
      b->cursor = nir_before_instr(&u->instr);
      nir_def *v;
      switch (u->op) {
      case nir_op_u2u32: v = z; break;
      case nir_op_i2i32: v = sx; break;
      case nir_op_u2f32: v = nir_u2f32(b, z); break;
      default: v = nir_i2f32(b, sx); break;
      }
      nir_def_replace(&u->def, v);
   }
   nir_instr_remove(&alu->instr);
   return true;
}

static bool
lower_odd_mem_instr(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_load_ubo:
   case nir_intrinsic_load_push_constant:
      return lower_odd_load(b, intr);
   case nir_intrinsic_store_ssbo:
      return lower_odd_store(b, intr);
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_store_output:
      return lower_odd_io(b, intr);
   default:
      return false;
   }
}

static bool
alu_is_64bit(const nir_instr *instr, const void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;
   const nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->def.bit_size == 64)
      return true;
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
      if (nir_src_bit_size(alu->src[i].src) == 64)
         return true;
   return false;
}

#ifdef WGVK_HAVE_SOFTFP64
static nir_shader *
softfp64_library(const nir_shader_compiler_options *opts)
{
   static nir_shader *lib;
   if (lib)
      return lib;
   static const struct spirv_capabilities caps = {
      .Addresses = true, .Float64 = true, .Int8 = true, .Int16 = true, .Int64 = true, .Shader = true,
   };
   struct spirv_to_nir_options so = { .capabilities = &caps, .environment = NIR_SPIRV_VULKAN, .create_library = true };
   nir_shader *n = spirv_to_nir(float64_spv_source, sizeof(float64_spv_source) / 4, NULL, MESA_SHADER_VERTEX,
                                "main", &so, opts);
   if (!n)
      return NULL;
   NIR_PASS(_, n, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, n, nir_lower_returns);
   NIR_PASS(_, n, nir_inline_functions);
   nir_sweep(n);
   lib = n;
   return lib;
}
#endif

static void
lower_odd_bit_sizes(nir_shader *s, const struct wgvk_compile_caps *caps, struct wgvk_legalize_info *info)
{
   bool has64, hasf64;
   if (!has_odd_bit_sizes(s, &has64, &hasf64))
      return;
   static nir_shader_compiler_options opts;
   opts = *s->options;
   opts.lower_int64_options = ~0u & ~nir_lower_iadd3_64;
   const nir_shader_compiler_options *old = s->options;
   s->options = &opts;
   if (hasf64) {
#ifdef WGVK_HAVE_SOFTFP64
      opts.lower_doubles_options = ~0u;
      nir_shader *lib = softfp64_library(&opts);
      if (lib) {
         /* the soft-float calls take scalars */
         NIR_PASS(_, s, nir_lower_alu_to_scalar, alu_is_64bit, NULL);
         NIR_PASS(_, s, nir_lower_phis_to_scalar, NULL, NULL);
         NIR_PASS(_, s, nir_lower_doubles, lib, opts.lower_doubles_options);
         bool conv = false;
         NIR_PASS(conv, s, nir_lower_int64_float_conversions);
         if (conv) {
            NIR_PASS(_, s, nir_opt_algebraic);
            NIR_PASS(_, s, nir_lower_doubles, lib, opts.lower_doubles_options);
         }
         NIR_PASS(_, s, nir_inline_functions);
         nir_remove_non_entrypoints(s);
         NIR_PASS(_, s, nir_opt_deref);
         NIR_PASS(_, s, nir_split_var_copies);
         NIR_PASS(_, s, nir_lower_var_copies);
         NIR_PASS(_, s, nir_split_struct_vars, nir_var_function_temp);
         NIR_PASS(_, s, nir_split_array_vars, nir_var_function_temp);
         NIR_PASS(_, s, nir_lower_vars_to_ssa);
         NIR_PASS(_, s, nir_opt_copy_prop);
         NIR_PASS(_, s, nir_opt_dce);
         NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp, NULL);
      } else {
         lfail(info, "fp64", "the soft-fp64 library did not load");
      }
#else
      lfail(info, "fp64", "64-bit floats (built without the soft-fp64 library)");
#endif
   }
   bool progress;
   unsigned n = 0;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_lower_vars_to_ssa);
      NIR_PASS(progress, s, nir_opt_copy_prop_vars);
      NIR_PASS(progress, s, nir_lower_bit_size, bit_size_cb, (void *)caps);
      NIR_PASS(progress, s, nir_shader_alu_pass, fold_narrow_conversions, nir_metadata_control_flow, NULL);
      NIR_PASS(progress, s, nir_shader_intrinsics_pass, lower_odd_mem_instr, nir_metadata_control_flow, NULL);
      if (has64) {
         NIR_PASS(progress, s, nir_lower_int64);
         NIR_PASS(progress, s, nir_lower_64bit_phis);
         NIR_PASS(progress, s, nir_lower_pack);
      }
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_cse);
      NIR_PASS(progress, s, nir_opt_undef);
      NIR_PASS(progress, s, nir_opt_remove_phis);
   } while (progress && ++n < 16);
   s->options = old;
}

/* ---------------------------------------------------------------- float atomics */

/*
 * WGSL atomics are integer only. Float add/min/max on buffers and shared memory become a compare-exchange
 * loop on the word's bits.
 */
static nir_def *
float_atomic_op(nir_builder *b, nir_atomic_op op, nir_def *old_bits, nir_def *v)
{
   nir_def *old = old_bits;
   switch (op) {
   case nir_atomic_op_fadd: return nir_fadd(b, old, v);
   case nir_atomic_op_fmin: return nir_fmin(b, old, v);
   case nir_atomic_op_fmax: return nir_fmax(b, old, v);
   default: return NULL;
   }
}

static bool
lower_float_atomic(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   bool ssbo = intr->intrinsic == nir_intrinsic_ssbo_atomic;
   bool deref = intr->intrinsic == nir_intrinsic_deref_atomic;
   if (!ssbo && !deref)
      return false;
   nir_atomic_op op = nir_intrinsic_atomic_op(intr);
   if (op != nir_atomic_op_fadd && op != nir_atomic_op_fmin && op != nir_atomic_op_fmax)
      return false;
   if (intr->def.bit_size != 32)
      return false;
   if (deref) {
      /* WGSL atomics are on atomic<u32>: the variable holds the float's bits as a u32 */
      nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
      if (var && glsl_get_base_type(glsl_without_array(var->type)) == GLSL_TYPE_FLOAT) {
         var->type = glsl_type_wrap_in_arrays(glsl_uint_type(), var->type);
         nir_fixup_deref_types(b->shader);
      }
   }
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *v = intr->src[ssbo ? 2 : 1].ssa;
   nir_def *first = ssbo ? nir_load_ssbo(b, 1, 32, intr->src[0].ssa, intr->src[1].ssa, .align_mul = 4,
                                         .access = ACCESS_COHERENT) :
                           nir_load_deref(b, nir_src_as_deref(intr->src[0]));
   /* the phi is inserted last, with both sources (inserting it registers their uses) */
   nir_phi_instr *phi = nir_phi_instr_create(b->shader);
   nir_def_init(&phi->instr, &phi->def, 1, 32);
   nir_loop *loop = nir_push_loop(b);
   nir_block *entry = nir_cf_node_as_block(nir_cf_node_prev(&loop->cf_node));
   nir_def *old = &phi->def;
   nir_def *nv = float_atomic_op(b, op, old, v);
   nir_def *r = ssbo ? nir_ssbo_atomic_swap(b, 32, intr->src[0].ssa, intr->src[1].ssa, old, nv,
                                            .atomic_op = nir_atomic_op_cmpxchg) :
                       nir_deref_atomic_swap(b, 32, intr->src[0].ssa, old, nv, .atomic_op = nir_atomic_op_cmpxchg);
   nir_break_if(b, nir_ieq(b, r, old));
   nir_block *back = nir_cursor_current_block(b->cursor);
   nir_pop_loop(b, loop);
   nir_phi_instr_add_src(phi, entry, first);
   nir_phi_instr_add_src(phi, back, r);
   nir_instr_insert(nir_before_block(nir_loop_first_block(loop)), &phi->instr);
   nir_def_replace(&intr->def, old);
   return true;
}

/* ---------------------------------------------------------------- binding limits */

/*
 * WebGPU allows 12 uniform and 10 storage buffers per stage on the M4 Max adapter (8 by default); Blender
 * asks for 24 storage buffers. wgvk's pipelines declare only what a stage uses (the binding list of each
 * compiled shader), so the limits bind only when one stage really reads more. Then:
 *   - uniform buffers past the limit are read as read-only storage buffers (WGVK_LOWERED_UBO_AS_STORAGE;
 *     the driver gives uniform-capable buffers the storage usage too);
 *   - storage buffers past the limit come from the buffer heap (WGVK_LOWERED_HEAP): large arena buffers
 *     at driver bindings 910 + a, and a table at driver binding 902 with one uvec4 per heap slot,
 *     {arena, byte offset, byte size, 0}. The driver suballocates storage-capable VkBuffer memory from the
 *     arenas and writes the table. Accesses past the size read 0 and write nothing.
 */
struct buf_ref {
   unsigned slot, index;   /* (set << 16 | binding, element) */
   bool ubo;
};

static int
buf_index_src(nir_intrinsic_instr *intr, bool *ubo)
{
   *ubo = false;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_ubo: *ubo = true; return 0;
   case nir_intrinsic_load_ssbo:
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap:
   case nir_intrinsic_get_ssbo_size: return 0;
   case nir_intrinsic_store_ssbo: return 1;
   default: return -1;
   }
}

static bool
buf_ref_of(nir_intrinsic_instr *intr, struct buf_ref *r)
{
   bool ubo;
   int src = buf_index_src(intr, &ubo);
   if (src < 0 || intr->src[src].ssa->num_components < 2)
      return false;
   nir_scalar sx = nir_scalar_resolved(intr->src[src].ssa, 0), sy = nir_scalar_resolved(intr->src[src].ssa, 1);
   if (!nir_scalar_is_const(sx) || !nir_scalar_is_const(sy))
      return false;
   r->slot = nir_scalar_as_uint(sx);
   r->index = nir_scalar_as_uint(sy);
   r->ubo = ubo;
   return true;
}

static int
cmp_buf_ref(const void *a, const void *b)
{
   const struct buf_ref *x = a, *y = b;
   if (x->slot != y->slot)
      return x->slot < y->slot ? -1 : 1;
   return (int)x->index - (int)y->index;
}

static unsigned
collect_buf_refs(nir_function_impl *impl, struct buf_ref *refs, unsigned max, bool ubo)
{
   unsigned n = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         struct buf_ref r;
         if (!buf_ref_of(nir_instr_as_intrinsic(instr), &r) || r.ubo != ubo)
            continue;
         bool have = false;
         for (unsigned i = 0; i < n; i++)
            have |= refs[i].slot == r.slot && refs[i].index == r.index;
         if (!have && n < max)
            refs[n++] = r;
      }
   }
   qsort(refs, n, sizeof(*refs), cmp_buf_ref);
   return n;
}

static int
find_ref(const struct buf_ref *refs, unsigned n, const struct buf_ref *r)
{
   for (unsigned i = 0; i < n; i++)
      if (refs[i].slot == r->slot && refs[i].index == r->index)
         return i;
   return -1;
}

/* An access through the heap: the same intrinsic on arena `a` at base + offset. */
static nir_def *
heap_op(nir_builder *b, nir_intrinsic_instr *intr, unsigned arena, nir_def *base)
{
   nir_intrinsic_instr *c = nir_instr_as_intrinsic(nir_instr_clone(b->shader, &intr->instr));
   nir_builder_instr_insert(b, &c->instr);
   b->cursor = nir_before_instr(&c->instr);
   bool ubo;
   int src = buf_index_src(intr, &ubo);
   nir_src_rewrite(&c->src[src], nir_imm_ivec2(b, WGVK_DRIVER_SET << 16 | (WGVK_DRIVER_BINDING_HEAP + arena), 0));
   if (intr->intrinsic != nir_intrinsic_get_ssbo_size)
      nir_src_rewrite(&c->src[src + 1], nir_iadd(b, base, intr->src[src + 1].ssa));
   b->cursor = nir_after_instr(&c->instr);
   return nir_instr_def(&c->instr);
}

static nir_def *
heap_arena_tree(nir_builder *b, nir_intrinsic_instr *intr, nir_def *arena, nir_def *base, unsigned lo, unsigned hi)
{
   if (lo == hi)
      return heap_op(b, intr, lo, base);
   unsigned mid = (lo + hi) / 2;
   nir_push_if(b, nir_ule_imm(b, arena, mid));
   nir_def *x = heap_arena_tree(b, intr, arena, base, lo, mid);
   nir_push_else(b, NULL);
   nir_def *y = heap_arena_tree(b, intr, arena, base, mid + 1, hi);
   nir_pop_if(b, NULL);
   return x ? nir_if_phi(b, x, y) : NULL;
}

static void
lower_to_heap(nir_builder *b, nir_intrinsic_instr *intr, unsigned slot, unsigned arenas, unsigned table_bytes)
{
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *desc = load_driver_ubo(b, WGVK_DRIVER_BINDING_HEAP_TABLE, table_bytes, 4, nir_imm_int(b, slot * 16));
   nir_def *arena = nir_channel(b, desc, 0), *base = nir_channel(b, desc, 1), *size = nir_channel(b, desc, 2);
   if (intr->intrinsic == nir_intrinsic_get_ssbo_size) {
      nir_def_replace(&intr->def, size);
      return;
   }
   bool ubo;
   int src = buf_index_src(intr, &ubo);
   nir_def *off = intr->src[src + 1].ssa;
   unsigned span = 4;
   if (intr->intrinsic == nir_intrinsic_load_ssbo)
      span = intr->def.num_components * intr->def.bit_size / 8;
   else if (intr->intrinsic == nir_intrinsic_store_ssbo)
      span = util_last_bit(nir_intrinsic_write_mask(intr)) * intr->src[0].ssa->bit_size / 8;
   nir_def *inb = nir_uge(b, size, nir_iadd_imm(b, off, span));
   nir_def *def = nir_instr_def(&intr->instr);
   nir_def *zero = def ? nir_imm_zero(b, def->num_components, def->bit_size) : NULL;
   nir_push_if(b, inb);
   nir_def *r = heap_arena_tree(b, intr, arena, base, 0, arenas - 1);
   nir_pop_if(b, NULL);
   if (def)
      nir_def_rewrite_uses(def, nir_if_phi(b, r, zero));
   nir_instr_remove(&intr->instr);
}

static void
lower_binding_limits(nir_shader *s, const struct wgvk_compile_caps *caps, struct wgvk_legalize_info *info)
{
   unsigned max_ubo = caps->max_uniform_buffers ? caps->max_uniform_buffers : 12;
   unsigned max_sb = caps->max_storage_buffers ? caps->max_storage_buffers : 8;
   unsigned arenas = caps->heap_arenas ? caps->heap_arenas : 1;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   struct buf_ref ubos[64], sbs[64];
   unsigned nubo = collect_buf_refs(impl, ubos, 64, true);
   unsigned nsb = collect_buf_refs(impl, sbs, 64, false);
   /* the heap's table is a uniform buffer of its own */
   bool heap = false;
   unsigned extra_ubo = 0;
   if (nubo > max_ubo) {
      /* the last ones (by set, binding, element) become storage buffers; driver UBOs stay */
      unsigned move = nubo - max_ubo;
      struct buf_ref moved[64];
      unsigned nmoved = 0;
      for (int i = nubo - 1; i >= 0 && nmoved < move; i--)
         if ((ubos[i].slot >> 16) != WGVK_DRIVER_SET)
            moved[nmoved++] = ubos[i];
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            struct buf_ref r;
            if (intr->intrinsic != nir_intrinsic_load_ubo || !buf_ref_of(intr, &r) || find_ref(moved, nmoved, &r) < 0)
               continue;
            b.cursor = nir_before_instr(instr);
            nir_def *v = nir_load_ssbo(&b, intr->def.num_components, intr->def.bit_size, intr->src[0].ssa,
                                       intr->src[1].ssa, .align_mul = nir_intrinsic_align_mul(intr),
                                       .align_offset = nir_intrinsic_align_offset(intr),
                                       .access = ACCESS_NON_WRITEABLE);
            nir_def_replace(&intr->def, v);
         }
      }
      for (unsigned i = 0; i < nmoved; i++)
         add_lowered(info, moved[i].slot >> 16, moved[i].slot & 0xffff, moved[i].index,
                     WGVK_LOWERED_UBO_AS_STORAGE, 0);
      nir_progress(true, impl, nir_metadata_control_flow);
      nsb = collect_buf_refs(impl, sbs, 64, false);
      extra_ubo = 0;
   }
   /* driver storage buffers (vertex pulling, index buffer) keep their bindings */
   unsigned ndriver = 0;
   for (unsigned i = 0; i < nsb; i++)
      ndriver += (sbs[i].slot >> 16) == WGVK_DRIVER_SET;
   if (nsb <= max_sb)
      return;
   if (ndriver >= max_sb) {
      lfail(info, "limits", "%u driver storage buffers", ndriver);
      return;
   }
   max_sb -= ndriver;
   nsb -= ndriver;
   if (max_sb <= arenas) {
      lfail(info, "limits", "%u storage buffers, %u allowed, %u heap arenas", nsb, max_sb, arenas);
      return;
   }
   heap = true;
   (void)extra_ubo;
   /* the first (max_sb - arenas) keep their bindings, the rest go through the heap */
   unsigned direct = max_sb - arenas, nheap = nsb - direct;
   struct buf_ref *heaped = &sbs[direct];
   unsigned table_bytes = nheap * 16;
   struct util_dynarray list;
   collect_instrs(impl, &list);
   nir_builder b = nir_builder_create(impl);
   util_dynarray_foreach(&list, nir_instr *, ip) {
      if ((*ip)->type != nir_instr_type_intrinsic)
         continue;
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(*ip);
      struct buf_ref r;
      int k;
      if (!buf_ref_of(intr, &r) || r.ubo || (k = find_ref(heaped, nheap, &r)) < 0)
         continue;
      lower_to_heap(&b, intr, k, arenas, table_bytes);
   }
   util_dynarray_fini(&list);
   nir_progress(true, impl, nir_metadata_none);
   for (unsigned i = 0; i < nheap; i++) {
      unsigned before = info->num_lowered;
      add_lowered(info, heaped[i].slot >> 16, heaped[i].slot & 0xffff, heaped[i].index, WGVK_LOWERED_HEAP, 0);
      if (info->num_lowered > before)
         info->lowered[before].heap_slot = i;
   }
   info->heap_slots = nheap;
   info->heap_arenas = arenas;
   (void)heap;
}

/* ---------------------------------------------------------------- vertex pulling and expanded points */

/*
 * WebGPU's vertex input has instance step rate 1 only, a fixed list of formats (no 3-component 8/16-bit
 * formats, no scaled formats, no 11/11/10 float...), strides that are multiples of 4 up to 2048 and
 * offsets aligned to min(4, format size). A vertex buffer binding outside that is fetched by the shader
 * ("pulled"): the buffer is bound as read-only storage at driver binding 920 + b, starting
 * vertex_buffer_offset[b] bytes in (driver uniforms, 901), and every attribute of it is read and decoded
 * like a texel. Vulkan's addressing:
 *   per vertex:   VertexIndex * stride + offset             (VertexIndex includes the base vertex)
 *   per instance: (firstInstance + (InstanceIndex - firstInstance) / divisor) * stride + offset,
 *                 firstInstance * stride when divisor is 0
 *
 * Points larger than a pixel (key.point_size): WebGPU draws 1-pixel points only, so T2 draws each point as
 * two triangles, 6 vertices (draw(6 * count, instances, 6 * first, firstInstance), triangle list, culling
 * off). The vertex shader finds its point (vertex_index / 6, through the index buffer at driver binding
 * 936 for indexed draws) and its corner (vertex_index % 6), pulls every attribute, and moves gl_Position
 * by the corner times gl_PointSize in clip space. It writes gl_PointCoord into the fragment shader's
 * varying (key.point_coord_varying - 1).
 */
static bool
vertex_format_native(enum pipe_format f)
{
   switch (f) {
   case PIPE_FORMAT_R8_UINT: case PIPE_FORMAT_R8G8_UINT: case PIPE_FORMAT_R8G8B8A8_UINT:
   case PIPE_FORMAT_R8_SINT: case PIPE_FORMAT_R8G8_SINT: case PIPE_FORMAT_R8G8B8A8_SINT:
   case PIPE_FORMAT_R8_UNORM: case PIPE_FORMAT_R8G8_UNORM: case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8_SNORM: case PIPE_FORMAT_R8G8_SNORM: case PIPE_FORMAT_R8G8B8A8_SNORM:
   case PIPE_FORMAT_R16_UINT: case PIPE_FORMAT_R16G16_UINT: case PIPE_FORMAT_R16G16B16A16_UINT:
   case PIPE_FORMAT_R16_SINT: case PIPE_FORMAT_R16G16_SINT: case PIPE_FORMAT_R16G16B16A16_SINT:
   case PIPE_FORMAT_R16_UNORM: case PIPE_FORMAT_R16G16_UNORM: case PIPE_FORMAT_R16G16B16A16_UNORM:
   case PIPE_FORMAT_R16_SNORM: case PIPE_FORMAT_R16G16_SNORM: case PIPE_FORMAT_R16G16B16A16_SNORM:
   case PIPE_FORMAT_R16_FLOAT: case PIPE_FORMAT_R16G16_FLOAT: case PIPE_FORMAT_R16G16B16A16_FLOAT:
   case PIPE_FORMAT_R32_FLOAT: case PIPE_FORMAT_R32G32_FLOAT: case PIPE_FORMAT_R32G32B32_FLOAT:
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
   case PIPE_FORMAT_R32_UINT: case PIPE_FORMAT_R32G32_UINT: case PIPE_FORMAT_R32G32B32_UINT:
   case PIPE_FORMAT_R32G32B32A32_UINT:
   case PIPE_FORMAT_R32_SINT: case PIPE_FORMAT_R32G32_SINT: case PIPE_FORMAT_R32G32B32_SINT:
   case PIPE_FORMAT_R32G32B32A32_SINT:
   case PIPE_FORMAT_R10G10B10A2_UNORM:
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      return true;
   default:
      return false;
   }
}

bool
wgvk_vertex_binding_needs_pulling(const struct wgvk_vertex_input *vi, unsigned b)
{
   const struct wgvk_vertex_binding *bd = &vi->binding[b];
   if (bd->stride % 4 || bd->stride > 2048 || (bd->per_instance && bd->divisor != 1))
      return true;
   for (unsigned l = 0; l < 32; l++) {
      enum pipe_format f = vi->attr[l].format;
      if (f == PIPE_FORMAT_NONE || vi->attr[l].binding != b)
         continue;
      unsigned size = util_format_get_blocksize(f);
      if (!vertex_format_native(f) || vi->attr[l].offset % MIN2(4, size))
         return true;
   }
   return false;
}

static bool
vertex_format_pullable(enum pipe_format f)
{
   if (f == PIPE_FORMAT_R11G11B10_FLOAT || f == PIPE_FORMAT_R9G9B9E5_FLOAT)
      return true;
   const struct util_format_description *d = util_format_description(f);
   if (!d || d->layout != UTIL_FORMAT_LAYOUT_PLAIN || d->block.bits % 8 || d->block.bits > 128)
      return false;
   for (unsigned c = 0; c < d->nr_channels; c++) {
      const struct util_format_channel_description *ch = &d->channel[c];
      if (ch->size > 32 || ch->type == UTIL_FORMAT_TYPE_FIXED ||
          (ch->type == UTIL_FORMAT_TYPE_FLOAT && ch->size != 16 && ch->size != 32) ||
          (ch->size == 32 && ch->shift % 32) || (ch->shift % 32) + ch->size > 32)
         return false;
   }
   return true;
}

/* `bytes` bytes at byte address `addr` of the storage buffer `idx`, as words (w[0] the lowest). */
static void
load_unaligned(nir_builder *b, nir_def *idx, nir_def *addr, unsigned bytes, nir_def **w)
{
   unsigned n = DIV_ROUND_UP(bytes, 4);
   nir_def *wa = nir_iand_imm(b, addr, ~3u);
   nir_def *sh = nir_ishl_imm(b, nir_iand_imm(b, addr, 3), 3);
   nir_def *aligned = nir_ieq_imm(b, sh, 0);
   nir_def *raw[5];
   for (unsigned i = 0; i <= n; i++)
      raw[i] = nir_load_ssbo(b, 1, 32, idx, nir_iadd_imm(b, wa, 4 * i), .align_mul = 4,
                             .access = ACCESS_NON_WRITEABLE | ACCESS_CAN_REORDER);
   nir_def *rsh = nir_isub(b, nir_imm_int(b, 32), sh);
   for (unsigned i = 0; i < n; i++)
      w[i] = nir_bcsel(b, aligned, raw[i], nir_ior(b, nir_ushr(b, raw[i], sh), nir_ishl(b, raw[i + 1], rsh)));
}

static nir_def *
driver_uniform_word(nir_builder *b, struct wgvk_legalize_info *info, unsigned byte_offset)
{
   info->driver_uniform_bytes = MAX2(info->driver_uniform_bytes, sizeof(struct wgvk_driver_uniforms));
   return load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 1,
                          nir_imm_int(b, byte_offset));
}

static nir_def *
pull_attribute(nir_builder *b, const struct wgvk_vertex_input *vi, unsigned loc, nir_def *vid,
               struct wgvk_legalize_info *info)
{
   unsigned bi = vi->attr[loc].binding;
   enum pipe_format f = vi->attr[loc].format;
   const struct wgvk_vertex_binding *bd = &vi->binding[bi];
   nir_def *index;
   if (!bd->per_instance) {
      index = vid;
   } else {
      nir_def *first = nir_load_base_instance(b);
      if (bd->divisor == 0)
         index = first;
      else
         index = nir_iadd(b, first, nir_udiv_imm(b, nir_isub(b, nir_load_instance_id(b), first), bd->divisor));
   }
   nir_def *addr = nir_iadd(b, driver_uniform_word(b, info, offsetof(struct wgvk_driver_uniforms, vertex_buffer_offset) + 4 * bi),
                            nir_iadd_imm(b, nir_imul_imm(b, index, bd->stride), vi->attr[loc].offset));
   nir_def *w[4];
   load_unaligned(b, nir_imm_ivec2(b, WGVK_DRIVER_SET << 16 | (WGVK_DRIVER_BINDING_VERTEX_BUFFER + bi), 0), addr,
                  util_format_get_blocksize(f), w);
   return unpack_texel(b, w, f);
}

/* The store_output of `slot` in the vertex shader (outputs are written once, at the end, after
 * nir_lower_io_vars_to_temporaries), or NULL. */
static nir_intrinsic_instr *
find_output_store(nir_function_impl *impl, gl_varying_slot slot)
{
   nir_intrinsic_instr *found = NULL;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_store_output && nir_intrinsic_io_semantics(intr).location == slot)
            found = intr;
      }
   }
   return found;
}

static void
expand_point(nir_builder *b, nir_function_impl *impl, nir_def *corner, const struct wgvk_shader_key *key,
             struct wgvk_legalize_info *info)
{
   nir_intrinsic_instr *pos = find_output_store(impl, VARYING_SLOT_POS);
   nir_intrinsic_instr *psiz = find_output_store(impl, VARYING_SLOT_PSIZ);
   if (!pos) {
      lfail(info, "point-size", "an expanded point without gl_Position");
      return;
   }
   if (psiz && (psiz->instr.block != pos->instr.block || nir_intrinsic_write_mask(pos) != 0xf ||
                nir_intrinsic_component(pos) != 0)) {
      lfail(info, "point-size", "gl_Position and gl_PointSize written in different places");
      return;
   }
   /* after both stores (in the same block): at its end; instruction indices may be stale here */
   b->cursor = nir_after_block_before_jump(pos->instr.block);
   nir_def *size = psiz ? nir_channel(b, psiz->src[0].ssa, 0) : nir_imm_float(b, 1.0f);
   nir_def *range = load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 2,
                                    nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, point_size_range)));
   size = nir_fmin(b, nir_fmax(b, size, nir_channel(b, range, 0)), nir_channel(b, range, 1));
   nir_def *inv = load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 2,
                                  nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, viewport_inv_size)));
   info->driver_uniform_bytes = MAX2(info->driver_uniform_bytes, sizeof(struct wgvk_driver_uniforms));
   /* corners in framebuffer terms (Vulkan: y down): (-,-) (+,-) (-,+) (+,-) (+,+) (-,+) */
   nir_def *right = nir_ine_imm(b, nir_iand(b, nir_ishl(b, nir_imm_int(b, 1), corner), nir_imm_int(b, 0x1a)), 0);
   nir_def *down = nir_ine_imm(b, nir_iand(b, nir_ishl(b, nir_imm_int(b, 1), corner), nir_imm_int(b, 0x34)), 0);
   nir_def *sx = nir_bcsel(b, right, nir_imm_float(b, 1), nir_imm_float(b, -1));
   nir_def *sy = nir_bcsel(b, down, nir_imm_float(b, 1), nir_imm_float(b, -1));
   nir_def *p = pos->src[0].ssa;
   nir_def *wv = nir_channel(b, p, 3);
   nir_def *dx = nir_fmul(b, nir_fmul(b, sx, size), nir_fmul(b, nir_channel(b, inv, 0), wv));
   nir_def *dy = nir_fmul(b, nir_fmul(b, sy, size), nir_fmul(b, nir_channel(b, inv, 1), wv));
   nir_def *np = nir_vec4(b, nir_fadd(b, nir_channel(b, p, 0), dx), nir_fadd(b, nir_channel(b, p, 1), dy),
                          nir_channel(b, p, 2), wv);
   nir_store_output(b, np, nir_imm_int(b, 0), .base = nir_intrinsic_base(pos), .write_mask = 0xf,
                    .component = 0, .src_type = nir_type_float32, .io_semantics = nir_intrinsic_io_semantics(pos));
   if (key->point_coord_varying) {
      unsigned loc = key->point_coord_varying - 1;
      nir_def *pc = nir_vec2(b, nir_bcsel(b, right, nir_imm_float(b, 1), nir_imm_float(b, 0)),
                             nir_bcsel(b, down, nir_imm_float(b, 1), nir_imm_float(b, 0)));
      nir_io_semantics sem = { .location = VARYING_SLOT_VAR0 + loc, .num_slots = 1 };
      nir_store_output(b, pc, nir_imm_int(b, 0), .base = loc, .write_mask = 3, .component = 0,
                       .src_type = nir_type_float32, .io_semantics = sem);
      b->shader->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_VAR0 + loc);
   }
   nir_instr_remove(&pos->instr);
   if (psiz)
      nir_instr_remove(&psiz->instr);
   info->points_expanded = true;
}

static void
lower_vertex_input(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info)
{
   if (s->info.stage != MESA_SHADER_VERTEX || (!key->vertex_pulling_mask && !key->point_size))
      return;
   /* points into a geometry shader are its input, not something to rasterize */
   struct wgvk_shader_key k2;
   if (key->vs_before_gs && key->point_size) {
      k2 = *key;
      k2.point_size = 0;
      key = &k2;
      if (!key->vertex_pulling_mask)
         return;
   }
   const struct wgvk_vertex_input *vi = key->vi;
   if (!vi) {
      lfail(info, "vertex-pulling", "vertex pulling without the vertex input (key.vi)");
      return;
   }
   info->variant_mask |= WGVK_VARIANT_VERTEX_PULLING | (key->point_size ? WGVK_VARIANT_POINT_SIZE : 0);
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_builder b = nir_builder_create(impl);
   b.cursor = nir_before_impl(impl);

   /* the Vulkan vertex index (and, for points, the corner) */
   nir_def *raw = nir_load_vertex_id(&b);
   nir_def *vid = raw, *corner = NULL;
   if (key->point_size) {
      nir_def *p = nir_udiv_imm(&b, raw, 6);
      corner = nir_umod_imm(&b, raw, 6);
      vid = p;
      if (key->point_size == WGVK_POINTS_EXPAND_INDEXED32 || key->point_size == WGVK_POINTS_EXPAND_INDEXED16) {
         bool i16 = key->point_size == WGVK_POINTS_EXPAND_INDEXED16;
         nir_def *addr = nir_iadd(&b, driver_uniform_word(&b, info, offsetof(struct wgvk_driver_uniforms, index_buffer_offset)),
                                  nir_imul_imm(&b, p, i16 ? 2 : 4));
         nir_def *w;
         load_unaligned(&b, nir_imm_ivec2(&b, WGVK_DRIVER_SET << 16 | WGVK_DRIVER_BINDING_INDEX_BUFFER, 0), addr,
                        i16 ? 2 : 4, &w);
         if (i16)
            w = nir_iand_imm(&b, w, 0xffff);
         vid = nir_iadd(&b, w, nir_load_base_vertex(&b));
      }
   }
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_load_vertex_id && &intr->def != raw) {
            nir_def_replace(&intr->def, vid);
            continue;
         }
         if (intr->intrinsic != nir_intrinsic_load_input)
            continue;
         unsigned loc = nir_intrinsic_io_semantics(intr).location - VERT_ATTRIB_GENERIC0;
         if (loc >= 32 || vi->attr[loc].format == PIPE_FORMAT_NONE)
            continue;
         unsigned bi = vi->attr[loc].binding;
         if (!key->point_size && !(key->vertex_pulling_mask & BITFIELD_BIT(bi)))
            continue;
         if (!vertex_format_pullable(vi->attr[loc].format)) {
            lfail(info, "vertex-format", "vertex format %s", util_format_short_name(vi->attr[loc].format));
            continue;
         }
         b.cursor = nir_before_instr(instr);
         nir_def *rgba = pull_attribute(&b, vi, loc, vid, info);
         unsigned comp = nir_intrinsic_component(intr), n = intr->def.num_components;
         nir_def *c[4];
         for (unsigned i = 0; i < n; i++)
            c[i] = nir_channel(&b, rgba, comp + i);
         nir_def *v = nir_vec(&b, c, n);
         /* (an integer format read as a float input keeps its bits: that is what zink asks of a driver, see
          * wgvk_render.c; pulled, it is the shader's, which WebGPU's vertex state would refuse) */
         if (intr->def.bit_size == 16)
            v = nir_alu_type_get_base_type(nir_intrinsic_dest_type(intr)) == nir_type_float ? nir_f2f16(&b, v) : nir_u2u16(&b, v);
         nir_def_replace(&intr->def, v);
         info->pulled_locations |= BITFIELD_BIT(loc);
      }
   }
   if (key->point_size)
      expand_point(&b, impl, corner, key, info);
   nir_progress(true, impl, nir_metadata_none);
}

/* ---------------------------------------------------------------- driver */

static void
cleanup(nir_shader *s)
{
   bool progress;
   unsigned n = 0;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_cse);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_opt_remove_phis);
   } while (progress && ++n < 10);
   NIR_PASS(_, s, nir_opt_move, nir_move_const_undef);
   nir_rematerialize_derefs_in_use_blocks_impl(nir_shader_get_entrypoint(s));
   NIR_PASS(_, s, nir_opt_dce);
   NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp | nir_var_shader_temp, NULL);
   nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
}

/* ---------------------------------------------------------------- viewports and layers */

/*
 * WebGPU has one viewport and renders one layer per pass. The last vertex stage (key.viewport_index_varying)
 * maps gl_Position from viewport i (driver uniforms viewports[i]) into the render area the WebGPU viewport
 * covers (clip coordinates are linear in that: x' = (vw / Rw) x + ((2 (vx - Rx) + vw) / Rw - 1) w, the
 * same for y, z' = n w + (f - n) z) and passes (i, layer) flat; the fragment shader (key.viewports)
 * discards outside viewport i and scissor i and in other layers than driver uniform layer. Without the
 * key gl_ViewportIndex / gl_Layer are dropped (viewport 0, layer 0).
 */
static nir_def *
du_load(nir_builder *b, unsigned nc, nir_def *offset, struct wgvk_legalize_info *info)
{
   info->driver_uniform_bytes = MAX2(info->driver_uniform_bytes, sizeof(struct wgvk_driver_uniforms));
   return load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), nc, offset);
}

static void
lower_viewport_index_vs(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info)
{
   if (s->info.stage != MESA_SHADER_VERTEX || key->vs_before_gs)
      return;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_intrinsic_instr *vp = find_output_store(impl, VARYING_SLOT_VIEWPORT);
   nir_intrinsic_instr *layer = find_output_store(impl, VARYING_SLOT_LAYER);
   nir_intrinsic_instr *pos = find_output_store(impl, VARYING_SLOT_POS);
   if (!vp && !layer && !key->viewport_index_varying)
      return;
   info->writes_viewport_index = vp || layer;
   if (key->viewport_index_varying) {
      info->variant_mask |= WGVK_VARIANT_VIEWPORTS;
      unsigned loc = key->viewport_index_varying - 1;
      /* (outputs at that location: the fragment shader does not read them) */
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_output &&
                nir_intrinsic_io_semantics(intr).location == VARYING_SLOT_VAR0 + loc)
               nir_instr_remove(instr);
         }
      }
      if (!pos) {
         lfail(info, "viewport-index", "gl_ViewportIndex without gl_Position");
         return;
      }
      if ((vp && vp->instr.block != pos->instr.block) || (layer && layer->instr.block != pos->instr.block) ||
          nir_intrinsic_write_mask(pos) != 0xf || nir_intrinsic_component(pos) != 0) {
         lfail(info, "viewport-index", "gl_Position and gl_ViewportIndex/gl_Layer written in different places");
         return;
      }
      nir_builder bb = nir_builder_create(impl);
      nir_builder *b = &bb;
      b->cursor = nir_after_block_before_jump(pos->instr.block);
      nir_def *i = vp ? nir_umin(b, nir_channel(b, vp->src[0].ssa, 0), nir_imm_int(b, 15)) : nir_imm_int(b, 0);
      nir_def *l = layer ? nir_channel(b, layer->src[0].ssa, 0) : nir_imm_int(b, 0);
      nir_def *r = du_load(b, 4, nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, render_area)), info);
      nir_def *voff = nir_iadd_imm(b, nir_imul_imm(b, i, 24), offsetof(struct wgvk_driver_uniforms, viewports));
      nir_def *v = du_load(b, 4, voff, info);
      nir_def *d = du_load(b, 2, nir_iadd_imm(b, voff, 16), info);
      nir_def *p = pos->src[0].ssa;
      nir_def *w = nir_channel(b, p, 3);
      nir_def *c[4];
      for (unsigned k = 0; k < 2; k++) {
         nir_def *vo = nir_channel(b, v, k), *vs = nir_channel(b, v, 2 + k);
         nir_def *ro = nir_channel(b, r, k), *rs = nir_channel(b, r, 2 + k);
         nir_def *scale = nir_fdiv(b, vs, rs);
         nir_def *off = nir_fadd_imm(b, nir_fdiv(b, nir_fadd(b, nir_fmul_imm(b, nir_fsub(b, vo, ro), 2.0), vs), rs), -1.0);
         c[k] = nir_ffma(b, scale, nir_channel(b, p, k), nir_fmul(b, off, w));
      }
      nir_def *n = nir_channel(b, d, 0), *f = nir_channel(b, d, 1);
      c[2] = nir_ffma(b, nir_fsub(b, f, n), nir_channel(b, p, 2), nir_fmul(b, n, w));
      c[3] = w;
      nir_store_output(b, nir_vec(b, c, 4), nir_imm_int(b, 0), .base = nir_intrinsic_base(pos), .write_mask = 0xf,
                       .component = 0, .src_type = nir_type_float32, .io_semantics = nir_intrinsic_io_semantics(pos));
      nir_io_semantics sem = { .location = VARYING_SLOT_VAR0 + loc, .num_slots = 1 };
      nir_store_output(b, nir_vec2(b, i, l), nir_imm_int(b, 0), .base = loc, .write_mask = 3, .component = 0,
                       .src_type = nir_type_uint32, .io_semantics = sem);
      s->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_VAR0 + loc);
      nir_instr_remove(&pos->instr);
   }
   if (vp)
      nir_instr_remove(&vp->instr);
   if (layer)
      nir_instr_remove(&layer->instr);
   s->info.outputs_written &= ~(VARYING_BIT_VIEWPORT | VARYING_BIT_LAYER);
   nir_progress(true, impl, nir_metadata_none);
}

static void
lower_viewport_index_fs(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info)
{
   if (s->info.stage != MESA_SHADER_FRAGMENT)
      return;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   if (!key->viewports) {
      /* gl_ViewportIndex / gl_Layer read without the emulation: viewport 0, layer 0 */
      nir_builder b0 = nir_builder_create(impl);
      bool any = false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            gl_varying_slot slot = intr->intrinsic == nir_intrinsic_load_input ?
                                   nir_intrinsic_io_semantics(intr).location : VARYING_SLOT_POS;
            if (slot != VARYING_SLOT_VIEWPORT && slot != VARYING_SLOT_LAYER &&
                intr->intrinsic != nir_intrinsic_load_layer_id)
               continue;
            b0.cursor = nir_before_instr(instr);
            nir_def_replace(&intr->def, nir_imm_zero(&b0, intr->def.num_components, intr->def.bit_size));
            any = true;
         }
      }
      nir_progress(any, impl, nir_metadata_control_flow);
      return;
   }
   info->variant_mask |= WGVK_VARIANT_VIEWPORTS;
   uint32_t used = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_load_input || intr->intrinsic == nir_intrinsic_load_interpolated_input) {
            nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
            if (sem.location >= VARYING_SLOT_VAR0 && sem.location < VARYING_SLOT_VAR0 + 32)
               used |= BITFIELD_RANGE(sem.location - VARYING_SLOT_VAR0, MAX2(sem.num_slots, 1));
         }
      }
   }
   if (info->point_coord_location >= 0)
      used |= BITFIELD_BIT(info->point_coord_location);
   int loc = -1;
   for (int i = 15; i >= 0; i--) {
      if (!(used & BITFIELD_BIT(i))) {
         loc = i;
         break;
      }
   }
   if (loc < 0) {
      lfail(info, "viewport-index", "no free varying location for the viewport index");
      return;
   }
   info->viewport_index_location = loc;
   nir_builder bb = nir_builder_at(nir_before_impl(impl));
   nir_builder *b = &bb;
   nir_io_semantics sem = { .location = VARYING_SLOT_VAR0 + loc, .num_slots = 1 };
   nir_def *il = nir_load_input(b, 2, 32, nir_imm_int(b, 0), .base = loc, .component = 0,
                                .dest_type = nir_type_uint32, .io_semantics = sem);
   nir_def *i = nir_channel(b, il, 0), *l = nir_channel(b, il, 1);
   nir_def *fc = nir_load_frag_coord(b);
   nir_def *voff = nir_iadd_imm(b, nir_imul_imm(b, i, 24), offsetof(struct wgvk_driver_uniforms, viewports));
   nir_def *v = du_load(b, 4, voff, info);
   nir_def *sc = nir_i2f32(b, du_load(b, 4, nir_iadd_imm(b, nir_imul_imm(b, i, 16),
                                                         offsetof(struct wgvk_driver_uniforms, scissors)), info));
   nir_def *out = nir_ine(b, l, du_load(b, 1, nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, layer)), info));
   for (unsigned k = 0; k < 2; k++) {
      nir_def *x = nir_channel(b, fc, k);
      nir_def *a0 = nir_channel(b, v, k), *a1 = nir_fadd(b, a0, nir_channel(b, v, 2 + k));
      nir_def *lo = nir_fmax(b, nir_fmin(b, a0, a1), nir_channel(b, sc, k));
      nir_def *hi = nir_fmin(b, nir_fmax(b, a0, a1), nir_fadd(b, nir_channel(b, sc, k), nir_channel(b, sc, 2 + k)));
      out = nir_ior(b, out, nir_ior(b, nir_flt(b, x, lo), nir_fge(b, x, hi)));
   }
   nir_demote_if(b, out);
   /* gl_ViewportIndex / gl_Layer read by the fragment shader */
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         nir_def *r = NULL;
         if (intr->intrinsic == nir_intrinsic_load_input) {
            gl_varying_slot slot = nir_intrinsic_io_semantics(intr).location;
            r = slot == VARYING_SLOT_VIEWPORT ? i : slot == VARYING_SLOT_LAYER ? l : NULL;
         } else if (intr->intrinsic == nir_intrinsic_load_layer_id) {
            r = l;
         }
         if (!r)
            continue;
         b->cursor = nir_before_instr(instr);
         nir_def_replace(&intr->def, nir_pad_vector_imm_int(b, r, 0, intr->def.num_components));
      }
   }
   nir_progress(true, impl, nir_metadata_none);
}

/* ---------------------------------------------------------------- early fragment tests */

static bool
shader_writes_memory(nir_shader *s)
{
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_store_deref:
            case nir_intrinsic_deref_atomic:
            case nir_intrinsic_deref_atomic_swap:
               if (nir_deref_mode_may_be(nir_src_as_deref(intr->src[0]), nir_var_mem_ssbo | nir_var_mem_global))
                  return true;
               break;
            case nir_intrinsic_store_ssbo:
            case nir_intrinsic_ssbo_atomic:
            case nir_intrinsic_ssbo_atomic_swap:
            case nir_intrinsic_store_global:
            case nir_intrinsic_global_atomic:
            case nir_intrinsic_global_atomic_swap:
            case nir_intrinsic_image_deref_store:
            case nir_intrinsic_image_deref_atomic:
            case nir_intrinsic_image_deref_atomic_swap:
            case nir_intrinsic_image_store:
            case nir_intrinsic_image_atomic:
            case nir_intrinsic_image_atomic_swap:
               return true;
            default:
               break;
            }
         }
      }
   }
   return false;
}

/*
 * WGSL has no early_fragment_tests: a fragment shader that writes memory does so before the (late) depth
 * test rejects it. With key.early_depth such a shader first tests itself: the depth at its pixel from the
 * pass's depth attachment (driver binding 938, bound read-only), compare op and format from the driver
 * uniforms, and discards (WGSL discard: no storage writes after it) when the test fails. The pipeline's
 * own depth test still runs after it for the colour outputs.
 */
static void
lower_early_depth(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info)
{
   if (s->info.stage != MESA_SHADER_FRAGMENT || !key->early_depth || !s->info.fs.early_fragment_tests ||
       !shader_writes_memory(s))
      return;
   info->variant_mask |= WGVK_VARIANT_EARLY_DEPTH;
   info->early_depth_test = true;
   bool ms = key->early_depth == 2;
   nir_variable *var = nir_variable_create(s, nir_var_uniform,
                                           glsl_texture_type(ms ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D, false,
                                                             GLSL_TYPE_FLOAT),
                                           "wgvk_depth");
   var->data.descriptor_set = WGVK_DRIVER_SET;
   var->data.binding = WGVK_DRIVER_BINDING_DEPTH;
   var->data.how_declared = nir_var_hidden;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_builder bb = nir_builder_at(nir_before_impl(impl));
   nir_builder *b = &bb;
   nir_def *fc = nir_load_frag_coord(b);
   nir_deref_instr *d = nir_build_deref_var(b, var);
   nir_tex_instr *t = nir_tex_instr_create(s, 3);
   t->op = ms ? nir_texop_txf_ms : nir_texop_txf;
   t->sampler_dim = ms ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D;
   t->dest_type = nir_type_float32;
   t->coord_components = 2;
   t->is_shadow = true;            /* (a depth texture: texture_depth_2d, one component) */
   t->is_new_style_shadow = true;
   t->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &d->def);
   t->src[1] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_f2i32(b, nir_trim_vector(b, fc, 2)));
   t->src[2] = ms ? nir_tex_src_for_ssa(nir_tex_src_ms_index, nir_imm_int(b, 0))
                  : nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(b, 0));
   nir_def_init(&t->instr, &t->def, 1, 32);
   nir_builder_instr_insert(b, &t->instr);
   nir_def *stored = &t->def;
   nir_def *z = nir_channel(b, fc, 2);
   nir_def *op = du_load(b, 1, nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, depth_compare)), info);
   nir_def *max = du_load(b, 1, nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, depth_unorm_max)), info);
   /* unorm formats: compare the integers they store (the fragment's depth rounded as the format rounds it) */
   nir_def *unorm = nir_fgt_imm(b, max, 0.0);
   nir_def *zq = nir_bcsel(b, unorm, nir_fround_even(b, nir_fmul(b, nir_fsat(b, z), max)), z);
   nir_def *sq = nir_bcsel(b, unorm, nir_fround_even(b, nir_fmul(b, stored, max)), stored);
   /* VkCompareOp is a mask: LESS 1, EQUAL 2, GREATER 4 */
   nir_def *pass = nir_ior(b, nir_ior(b, nir_iand(b, nir_flt(b, zq, sq), nir_test_mask(b, op, 1)),
                                         nir_iand(b, nir_feq(b, zq, sq), nir_test_mask(b, op, 2))),
                           nir_iand(b, nir_flt(b, sq, zq), nir_test_mask(b, op, 4)));
   nir_demote_if(b, nir_inot(b, pass));
   nir_progress(true, impl, nir_metadata_none);
}

/* ---------------------------------------------------------------- gl_PrimitiveID written by a GS */

/*
 * A geometry shader may write gl_PrimitiveID (zink's GS for flat shading / polygon modes does): its rast part
 * then stores VARYING_SLOT_PRIMITIVE_ID, which a WebGPU vertex stage cannot output. With
 * key.primitive_id_varying it goes to a generic flat u32 varying the fragment shader (key.
 * primitive_id_from_varying) reads instead of @builtin(primitive_index); without, it is dropped.
 */
static void
lower_primitive_id_vs(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info)
{
   if (s->info.stage != MESA_SHADER_VERTEX || key->vs_before_gs)
      return;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   bool any = false;
   nir_builder b = nir_builder_create(impl);
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_store_output)
            continue;
         gl_varying_slot slot = nir_intrinsic_io_semantics(intr).location;
         if (key->primitive_id_varying && slot == VARYING_SLOT_VAR0 + key->primitive_id_varying - 1) {
            nir_instr_remove(instr);   /* (the fragment shader does not read it there) */
            continue;
         }
         if (slot != VARYING_SLOT_PRIMITIVE_ID)
            continue;
         any = true;
         if (key->primitive_id_varying) {
            unsigned loc = key->primitive_id_varying - 1;
            b.cursor = nir_before_instr(instr);
            nir_io_semantics sem = { .location = VARYING_SLOT_VAR0 + loc, .num_slots = 1 };
            nir_store_output(&b, nir_channel(&b, intr->src[0].ssa, 0), nir_imm_int(&b, 0), .base = loc,
                             .write_mask = 1, .component = 0, .src_type = nir_type_uint32, .io_semantics = sem);
            s->info.outputs_written |= BITFIELD64_BIT(VARYING_SLOT_VAR0 + loc);
         }
         nir_instr_remove(instr);
      }
   }
   info->writes_primitive_id = any;
   s->info.outputs_written &= ~VARYING_BIT_PRIMITIVE_ID;
   nir_progress(true, impl, nir_metadata_control_flow);
}

static void
lower_primitive_id_fs(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info)
{
   if (s->info.stage != MESA_SHADER_FRAGMENT || !key->primitive_id_from_varying)
      return;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   uint32_t used = 0;
   bool reads = false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_load_primitive_id)
            reads = true;
         if (intr->intrinsic == nir_intrinsic_load_input || intr->intrinsic == nir_intrinsic_load_interpolated_input) {
            nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
            if (sem.location == VARYING_SLOT_PRIMITIVE_ID)
               reads = true;
            if (sem.location >= VARYING_SLOT_VAR0 && sem.location < VARYING_SLOT_VAR0 + 32)
               used |= BITFIELD_RANGE(sem.location - VARYING_SLOT_VAR0, MAX2(sem.num_slots, 1));
         }
      }
   }
   if (!reads)
      return;
   if (info->point_coord_location >= 0)
      used |= BITFIELD_BIT(info->point_coord_location);
   if (info->viewport_index_location >= 0)
      used |= BITFIELD_BIT(info->viewport_index_location);
   int loc = -1;
   for (int i = 15; i >= 0 && loc < 0; i--)
      if (!(used & BITFIELD_BIT(i)))
         loc = i;
   if (loc < 0) {
      lfail(info, "primitive-id", "no free varying location for gl_PrimitiveID");
      return;
   }
   info->primitive_id_location = loc;
   nir_builder b = nir_builder_create(impl);
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_primitive_id &&
             !(intr->intrinsic == nir_intrinsic_load_input &&
               nir_intrinsic_io_semantics(intr).location == VARYING_SLOT_PRIMITIVE_ID))
            continue;
         b.cursor = nir_before_instr(instr);
         nir_io_semantics sem = { .location = VARYING_SLOT_VAR0 + loc, .num_slots = 1 };
         nir_def *v = nir_load_input(&b, 1, 32, nir_imm_int(&b, 0), .base = loc, .component = 0,
                                     .dest_type = nir_type_uint32, .io_semantics = sem);
         nir_def_replace(&intr->def, nir_pad_vector_imm_int(&b, v, 0, intr->def.num_components));
      }
   }
   nir_progress(true, impl, nir_metadata_control_flow);
}

void
wgvk_legalize_pre(nir_shader *s, const struct wgvk_shader_key *key, const struct wgvk_compile_caps *caps,
                  struct wgvk_legalize_info *info)
{
   (void)s;
   (void)key;
   (void)caps;
   info->draw_params_offset = 0xffff;
   info->point_coord_location = -1;
   info->viewport_index_location = -1;
   info->primitive_id_location = -1;
}

void
wgvk_legalize_post(nir_shader *s, const struct wgvk_shader_key *key, const struct wgvk_compile_caps *caps,
                   struct wgvk_legalize_info *info)
{
   /* (instructions a pass inserts before the one it visits are not visited again) */
   bool progress = false;
   if (s->info.stage == MESA_SHADER_FRAGMENT)
      progress |= lower_input_attachments(s, info);
   progress |= lower_dynamic_arrays(s, info);
   lower_vertex_input(s, key, info);
   if (key->vs_before_gs && s->info.stage == MESA_SHADER_VERTEX)
      wgvk_poly_vs_before_gs(s, info);
   wgvk_poly_lower(s, key, info);
   lower_view_index(s, info);
   lower_push_constants_and_draw_params(s, key, caps, info);
   NIR_PASS(progress, s, nir_shader_instructions_pass, lower_int_tex, nir_metadata_control_flow, info);
   if (s->info.stage == MESA_SHADER_FRAGMENT)
      progress |= lower_point_coord(s, info);
   lower_viewport_index_vs(s, key, info);
   lower_viewport_index_fs(s, key, info);
   lower_primitive_id_vs(s, key, info);
   lower_primitive_id_fs(s, key, info);
   lower_early_depth(s, key, info);
   lower_formatless_images(s, key, info);
   progress |= lower_texel_buffers(s, key, info);
   lower_image_atomics(s, info);
   lower_odd_bit_sizes(s, caps, info);
   NIR_PASS(progress, s, nir_shader_intrinsics_pass, lower_float_atomic, nir_metadata_none, NULL);
   lower_binding_limits(s, caps, info);
   cleanup(s);
}
