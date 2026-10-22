#include <stdio.h>
/*
 * wgvk on Mesa's libpoly (track T4): geometry shaders, and the vertex shader before one, as WebGPU compute
 * and vertex shaders. See wgvk_compile.h (struct wgvk_gs_program) and gpu/webgpu/INTERFACES.md 6.2.
 *
 * libpoly works on 64-bit global addresses (its OpenCL helpers, turned into NIR builder code by
 * vtn_bindgen2, load and store through them). WGSL has no pointers into global memory, so an address here
 * is a segment and a byte offset: the high word names a storage binding, the low word is the offset in
 * it. Segment 0 is the poly buffer (driver binding 940: params, vertex shader outputs, the GS index
 * buffer, count buffers, a zero sink), segment 1 the draw's index buffer (936). The driver writes
 * addresses into poly_vertex_params / poly_geometry_params in that form, and every global access becomes
 * a storage buffer access (a branch over the segments when the high word is not a constant).
 */
#include <string.h>
#include "wgvk_poly.h"
#include "wgvk_legalize.h"
#include "nir_builder.h"
#include "poly/cl/libpoly.h"
#include "wgvk_poly_cl.h"
#include "wgvk_tess_spv.h"
#include "compiler/spirv/nir_spirv.h"
#include "poly/geometry.h"
#include "poly/tessellator.h"
#include "poly/nir/poly_nir.h"
#include "util/u_dynarray.h"
#include "nir_xfb_info.h"

#define WGVK_POLY_WORKGROUP 64

static const unsigned segment_binding[] = { WGVK_DRIVER_BINDING_POLY, WGVK_DRIVER_BINDING_INDEX_BUFFER,
                                            WGVK_DRIVER_BINDING_POLY_INDIRECT,
                                            WGVK_DRIVER_BINDING_XFB + 0, WGVK_DRIVER_BINDING_XFB + 1,
                                            WGVK_DRIVER_BINDING_XFB + 2, WGVK_DRIVER_BINDING_XFB + 3 };
#define NUM_SEGMENTS (sizeof(segment_binding) / sizeof(segment_binding[0]))

static nir_def *
driver_word(nir_builder *b, struct wgvk_legalize_info *info, unsigned offset)
{
   info->driver_uniform_bytes = MAX2(info->driver_uniform_bytes, sizeof(struct wgvk_driver_uniforms));
   return wgvk_load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 1,
                               nir_imm_int(b, offset));
}

/* a segment-0 address from a driver uniform word */
static nir_def *
poly_address(nir_builder *b, struct wgvk_legalize_info *info, unsigned offset)
{
   return nir_pack_64_2x32_split(b, driver_word(b, info, offset), nir_imm_int(b, 0));
}

/* The same access on segment `seg` at byte offset `off`. */
static nir_def *
segment_access(nir_builder *b, nir_intrinsic_instr *intr, unsigned seg, nir_def *off)
{
   nir_def *idx = nir_imm_ivec2(b, WGVK_DRIVER_SET << 16 | segment_binding[seg], 0);
   unsigned align = nir_intrinsic_has_align_mul(intr) ? MAX2(nir_intrinsic_align_mul(intr), 1) : 4;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_global:
   case nir_intrinsic_load_global_constant:
      return nir_load_ssbo(b, intr->def.num_components, intr->def.bit_size, idx, off, .align_mul = align,
                           .access = nir_intrinsic_access(intr) & ACCESS_COHERENT);
   case nir_intrinsic_store_global:
      nir_store_ssbo(b, intr->src[0].ssa, idx, off, .write_mask = nir_intrinsic_write_mask(intr),
                     .align_mul = align);
      return NULL;
   case nir_intrinsic_global_atomic:
      return nir_ssbo_atomic(b, intr->def.bit_size, idx, off, intr->src[1].ssa,
                             .atomic_op = nir_intrinsic_atomic_op(intr));
   case nir_intrinsic_global_atomic_swap:
      return nir_ssbo_atomic_swap(b, intr->def.bit_size, idx, off, intr->src[1].ssa, intr->src[2].ssa,
                                  .atomic_op = nir_intrinsic_atomic_op(intr));
   default:
      UNREACHABLE("global access");
   }
}

static nir_def *
segment_tree(nir_builder *b, nir_intrinsic_instr *intr, nir_def *seg, nir_def *off, unsigned lo, unsigned hi)
{
   if (lo == hi)
      return segment_access(b, intr, lo, off);
   unsigned mid = (lo + hi) / 2;
   nir_push_if(b, nir_ule_imm(b, seg, mid));
   nir_def *x = segment_tree(b, intr, seg, off, lo, mid);
   nir_push_else(b, NULL);
   nir_def *y = segment_tree(b, intr, seg, off, mid + 1, hi);
   nir_pop_if(b, NULL);
   return x ? nir_if_phi(b, x, y) : NULL;
}

/* An offset added to an address: 32-bit values widened, constants, and arithmetic on those. */
static bool
offset_like(nir_scalar x, unsigned depth)
{
   if (depth > 8)
      return false;
   if (nir_scalar_is_const(x)) {
      uint64_t v = nir_scalar_as_uint(x);
      return (v >> 32) == 0 || (v >> 32) == 0xffffffffu;
   }
   if (!nir_scalar_is_alu(x))
      return false;
   nir_op op = nir_scalar_alu_op(x);
   switch (op) {
   case nir_op_u2u64:
   case nir_op_i2i64:
      return true;
   case nir_op_iadd:
   case nir_op_imul:
   case nir_op_ishl:
   case nir_op_iand:
      return offset_like(nir_scalar_chase_alu_src(x, 0), depth + 1) &&
             (op == nir_op_ishl || offset_like(nir_scalar_chase_alu_src(x, 1), depth + 1));
   default:
      return false;
   }
}

static nir_def *segment_of_loaded(nir_builder *b, nir_intrinsic_instr *load);

/* The segment (high word) of an address, when it can be found without the carries of 64-bit adds: libpoly
 * adds offsets to a base, and offsets never cross a segment. NULL: take the high word as computed. */
static nir_def *
segment_of(nir_builder *b, nir_scalar a, unsigned depth)
{
   if (depth > 16)
      return NULL;
   if (nir_scalar_is_const(a))
      return nir_imm_int(b, nir_scalar_as_uint(a) >> 32);
   if (nir_scalar_is_intrinsic(a)) {
      nir_intrinsic_instr *ii = nir_def_as_intrinsic(a.def);
      switch (ii->intrinsic) {
      case nir_intrinsic_load_vertex_param_buffer_poly:
      case nir_intrinsic_load_geometry_param_buffer_poly:
      case nir_intrinsic_load_tess_param_buffer_poly:
      case nir_intrinsic_load_ro_sink_address_poly:
      case nir_intrinsic_load_stat_query_address_poly:
      case nir_intrinsic_load_printf_buffer_address:
         return nir_imm_int(b, 0);
      case nir_intrinsic_ro_to_rw_poly:
         return segment_of(b, nir_scalar_chase_movs(nir_get_scalar(ii->src[0].ssa, 0)), depth + 1);
      case nir_intrinsic_load_global:
      case nir_intrinsic_load_global_constant:
         return ii->def.bit_size == 64 ? segment_of_loaded(b, ii) : NULL;
      default:
         return NULL;
      }
   }
   if (nir_def_instr_type(a.def) == nir_instr_type_phi) {
      /* a pointer chosen by control flow (the tessellator's "out or null"): one segment if every
       * incoming one has the same constant segment; a phi already being followed (a loop's pointer
       * increment) does not decide */
      static nir_phi_instr *following[16];
      static unsigned nfollowing;
      nir_phi_instr *phi = nir_def_as_phi(a.def);
      for (unsigned i = 0; i < nfollowing; i++)
         if (following[i] == phi)
            return nir_imm_int(b, ~0u);
      if (nfollowing == 16)
         return NULL;
      following[nfollowing++] = phi;
      int64_t seg = -1;
      bool ok = true;
      nir_foreach_phi_src(src, phi) {
         nir_cursor saved = b->cursor;
         nir_def *x = segment_of(b, nir_scalar_chase_movs(nir_get_scalar(src->src.ssa, a.comp)), depth + 1);
         b->cursor = saved;
         nir_scalar xs = x ? nir_scalar_chase_movs(nir_get_scalar(x, 0)) : (nir_scalar){ 0 };
         if (!x || !nir_scalar_is_const(xs)) {
            ok = false;
            break;
         }
         uint32_t v = nir_scalar_as_uint(xs);
         if (v == ~0u)
            continue;
         if (seg >= 0 && seg != v) {
            ok = false;
            break;
         }
         seg = v;
      }
      nfollowing--;
      return ok && seg >= 0 ? nir_imm_int(b, seg) : NULL;
   }
   if (!nir_scalar_is_alu(a))
      return NULL;
   switch (nir_scalar_alu_op(a)) {
   case nir_op_pack_64_2x32_split: {
      nir_scalar hi = nir_scalar_chase_alu_src(a, 1);
      return nir_scalar_is_const(hi) ? nir_imm_int(b, nir_scalar_as_uint(hi)) : nir_channel(b, hi.def, hi.comp);
   }
   case nir_op_iadd: {
      nir_scalar x = nir_scalar_chase_alu_src(a, 0), y = nir_scalar_chase_alu_src(a, 1);
      if (offset_like(y, 0))
         return segment_of(b, x, depth + 1);
      if (offset_like(x, 0))
         return segment_of(b, y, depth + 1);
      return NULL;
   }
   case nir_op_bcsel: {
      nir_def *x = segment_of(b, nir_scalar_chase_alu_src(a, 1), depth + 1);
      nir_def *y = x ? segment_of(b, nir_scalar_chase_alu_src(a, 2), depth + 1) : NULL;
      if (!y)
         return NULL;
      nir_scalar c = nir_scalar_chase_alu_src(a, 0);
      return nir_bcsel(b, nir_channel(b, c.def, c.comp), x, y);
   }
   default:
      return NULL;
   }
}

/* The segment of a pointer libpoly loads from its params: the vertex params' index buffer is the draw's
 * index buffer (segment 1), every other pointer in them is into the poly buffer. (Transform feedback
 * buffers will be segments of their own.) */
static nir_def *
segment_of_loaded(nir_builder *b, nir_intrinsic_instr *load)
{
   nir_scalar a = nir_scalar_chase_movs(nir_get_scalar(load->src[0].ssa, 0));
   uint64_t off = 0;
   if (nir_scalar_is_alu(a) && nir_scalar_alu_op(a) == nir_op_iadd) {
      nir_scalar x = nir_scalar_chase_alu_src(a, 0), y = nir_scalar_chase_alu_src(a, 1);
      if (nir_scalar_is_const(x)) {
         nir_scalar t = x;
         x = y;
         y = t;
      }
      if (!nir_scalar_is_const(y))
         return NULL;
      off = nir_scalar_as_uint(y);
      a = x;
   }
   if (nir_scalar_is_intrinsic(a)) {
      nir_intrinsic_op op = nir_scalar_intrinsic_op(a);
      if (op == nir_intrinsic_load_vertex_param_buffer_poly)
         return nir_imm_int(b, off == offsetof(struct poly_vertex_params, index_buffer) ? 1 : 0);
      if (op == nir_intrinsic_load_geometry_param_buffer_poly) {
         /* transform feedback buffer pointers: segments 3 + buffer */
         unsigned orig = offsetof(struct poly_geometry_params, xfb_base_original);
         unsigned cur = offsetof(struct poly_geometry_params, xfb_base);
         if (off >= orig && off < orig + 8 * POLY_MAX_SO_BUFFERS)
            return nir_imm_int(b, 3 + (off - orig) / 8);
         if (off >= cur && off < cur + 8 * POLY_MAX_SO_BUFFERS)
            return nir_imm_int(b, 3 + (off - cur) / 8);
         return nir_imm_int(b, 0);
      }
   }
   /* any other pointer kept in the poly buffer points into the poly buffer (the heap's base, ...) */
   nir_def *seg = segment_of(b, nir_scalar_chase_movs(nir_get_scalar(load->src[0].ssa, 0)), 1);
   nir_scalar ss = seg ? nir_scalar_chase_movs(nir_get_scalar(seg, 0)) : (nir_scalar){ 0 };
   if (seg && nir_scalar_is_const(ss) && nir_scalar_as_uint(ss) == 0)
      return nir_imm_int(b, 0);
   return NULL;
}

static bool
is_global_access(nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_global:
   case nir_intrinsic_load_global_constant:
   case nir_intrinsic_store_global:
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
      return true;
   default:
      return false;
   }
}

/* Before anything is lowered: the segment of each global access's address. */
static nir_def *
access_segment(nir_builder *b, nir_intrinsic_instr *intr)
{
   unsigned addr_src = intr->intrinsic == nir_intrinsic_store_global ? 1 : 0;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *addr = intr->src[addr_src].ssa;
   return segment_of(b, nir_scalar_chase_movs(nir_get_scalar(addr, 0)), 0);
}

static bool
lower_global(nir_builder *b, nir_intrinsic_instr *intr, nir_def *seg)
{
   unsigned addr_src = intr->intrinsic == nir_intrinsic_store_global ? 1 : 0;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *addr = intr->src[addr_src].ssa;
   nir_def *off = nir_unpack_64_2x32_split_x(b, addr);
   nir_def *r;
   if (!seg)
      seg = nir_unpack_64_2x32_split_y(b, addr);
   nir_scalar ss = nir_scalar_chase_movs(nir_get_scalar(seg, 0));
   if (nir_scalar_is_const(ss) && nir_scalar_as_uint(ss) < NUM_SEGMENTS)
      r = segment_access(b, intr, nir_scalar_as_uint(ss), off);
   else
      r = segment_tree(b, intr, seg, off, 0, NUM_SEGMENTS - 1);
   if (r)
      nir_def_replace(&intr->def, r);
   else
      nir_instr_remove(&intr->instr);
   return true;
}

struct poly_state {
   const struct wgvk_shader_key *key;
   struct wgvk_legalize_info *info;
};

static bool
lower_poly_instr(nir_builder *b, nir_intrinsic_instr *intr, struct poly_state *st)
{
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *v;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_vertex_param_buffer_poly:
      v = poly_address(b, st->info, offsetof(struct wgvk_driver_uniforms, poly_vertex_params));
      break;
   case nir_intrinsic_load_geometry_param_buffer_poly:
      v = poly_address(b, st->info, offsetof(struct wgvk_driver_uniforms, poly_geometry_params));
      break;
   case nir_intrinsic_load_tess_param_buffer_poly:
      v = poly_address(b, st->info, offsetof(struct wgvk_driver_uniforms, poly_tess_params));
      break;
   case nir_intrinsic_load_ro_sink_address_poly:
   case nir_intrinsic_load_stat_query_address_poly:
      v = poly_address(b, st->info, offsetof(struct wgvk_driver_uniforms, poly_sink));
      break;
   case nir_intrinsic_ro_to_rw_poly:
      v = intr->src[0].ssa;
      break;
   case nir_intrinsic_load_printf_buffer_address:
      /* libpoly's printf (heap overflow): no buffer, nothing printed */
      v = poly_address(b, st->info, offsetof(struct wgvk_driver_uniforms, poly_sink));
      break;
   case nir_intrinsic_load_printf_buffer_size:
      /* the header word only: every message is "too long", nothing past the sink's first words written */
      v = nir_imm_intN_t(b, 8, intr->def.bit_size);
      break;
   case nir_intrinsic_load_provoking_last:
      v = nir_imm_intN_t(b, st->key->provoking_last ? 1 : 0, intr->def.bit_size);
      break;
   case nir_intrinsic_load_rasterization_stream:
      v = nir_imm_intN_t(b, 0, intr->def.bit_size);
      break;
   default:
      return false;
   }
   nir_def_replace(&intr->def, v);
   return true;
}

static bool
has_poly_work(nir_shader *s)
{
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            switch (nir_instr_as_intrinsic(instr)->intrinsic) {
            case nir_intrinsic_load_global:
            case nir_intrinsic_load_global_constant:
            case nir_intrinsic_store_global:
            case nir_intrinsic_global_atomic:
            case nir_intrinsic_global_atomic_swap:
            case nir_intrinsic_load_vertex_param_buffer_poly:
            case nir_intrinsic_load_geometry_param_buffer_poly:
            case nir_intrinsic_load_tess_param_buffer_poly:
            case nir_intrinsic_load_ro_sink_address_poly:
            case nir_intrinsic_load_stat_query_address_poly:
            case nir_intrinsic_ro_to_rw_poly:
            case nir_intrinsic_load_provoking_last:
            case nir_intrinsic_load_rasterization_stream:
            case nir_intrinsic_load_printf_buffer_address:
            case nir_intrinsic_load_printf_buffer_size:
            case nir_intrinsic_load_index_size_poly:
            case nir_intrinsic_load_vs_outputs_poly:
               return true;
            default:
               break;
            }
         }
      }
   }
   return false;
}

/* libpoly's abort() (after a heap overflow): WGSL has no halt; the invocation returns */
static bool
halt_to_return(nir_shader *s)
{
   bool progress = false;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_instr *last = nir_block_last_instr(block);
         if (last && last->type == nir_instr_type_jump && nir_instr_as_jump(last)->type == nir_jump_halt) {
            nir_instr_as_jump(last)->type = nir_jump_return;
            progress = true;
         }
      }
      nir_progress(progress, impl, nir_metadata_none);
   }
   if (progress)
      NIR_PASS(_, s, nir_lower_returns);
   return progress;
}

void
wgvk_poly_lower(nir_shader *s, const struct wgvk_shader_key *key, struct wgvk_legalize_info *info)
{
   halt_to_return(s);
   if (!has_poly_work(s))
      return;
   NIR_PASS(_, s, poly_nir_lower_sysvals);
   struct poly_state st = { key, info };
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   /* collected first: the segment branches split blocks */
   struct util_dynarray list;
   util_dynarray_init(&list, NULL);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block)
         if (instr->type == nir_instr_type_intrinsic)
            util_dynarray_append(&list, nir_instr_as_intrinsic(instr));
   }
   nir_builder b = nir_builder_create(impl);
   bool progress = false;
   /* segments first, on the NIR as libpoly made it (sysvals and pointer loads still recognisable) */
   struct util_dynarray segs;
   util_dynarray_init(&segs, NULL);
   util_dynarray_foreach(&list, nir_intrinsic_instr *, ip) {
      nir_def *seg = is_global_access(*ip) ? access_segment(&b, *ip) : NULL;
      if (is_global_access(*ip) && getenv("WGVK_SEGMENT_DEBUG")) {
         nir_scalar ss = seg ? nir_scalar_chase_movs(nir_get_scalar(seg, 0)) : (nir_scalar){ 0 };
         if (!seg || !nir_scalar_is_const(ss)) {
            fprintf(stderr, "wgvk: segment unknown: ");
            nir_print_instr(&(*ip)->instr, stderr);
            fprintf(stderr, "\n");
            nir_instr *chain[64];
            unsigned nc = 0;
            chain[nc++] = nir_def_instr((*ip)->src[(*ip)->intrinsic == nir_intrinsic_store_global ? 1 : 0].ssa);
            for (unsigned c = 0; c < nc && nc < 60; c++) {
               fprintf(stderr, "   ");
               nir_print_instr(chain[c], stderr);
               fprintf(stderr, "\n");
               if (chain[c]->type == nir_instr_type_alu) {
                  nir_alu_instr *al = nir_instr_as_alu(chain[c]);
                  for (unsigned q = 0; q < nir_op_infos[al->op].num_inputs && nc < 60; q++)
                     chain[nc++] = nir_def_instr(al->src[q].src.ssa);
               }
            }
         }
      }
      util_dynarray_append(&segs, seg);
   }
   unsigned k = 0;
   util_dynarray_foreach(&list, nir_intrinsic_instr *, ip) {
      nir_def *seg = *util_dynarray_element(&segs, nir_def *, k++);
      progress |= is_global_access(*ip) ? lower_global(&b, *ip, seg) : lower_poly_instr(&b, *ip, &st);
   }
   util_dynarray_fini(&segs);
   util_dynarray_fini(&list);
   nir_progress(progress, impl, nir_metadata_none);
   NIR_PASS(_, s, nir_opt_constant_folding);
   NIR_PASS(_, s, nir_opt_algebraic);
   NIR_PASS(_, s, nir_opt_dce);
}

/* Wrap the shader's body in "if (gid.x < grid.x && gid.y < grid.y)", grid.xy at byte `grid_offset` of the
 * params at segment-0 address `params` (workgroups are WGVK_POLY_WORKGROUP wide in x). */
static void
make_compute(nir_shader *s, bool vertex_params, struct wgvk_legalize_info *info)
{
   s->info.stage = MESA_SHADER_COMPUTE;
   memset(&s->info.cs, 0, sizeof(s->info.cs));
   s->info.workgroup_size[0] = WGVK_POLY_WORKGROUP;
   s->info.workgroup_size[1] = 1;
   s->info.workgroup_size[2] = 1;
   s->info.workgroup_size_variable = false;
   s->xfb_info = NULL;
   s->info.has_transform_feedback_varyings = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_cf_list body;
   nir_cf_extract(&body, nir_before_impl(impl), nir_after_impl(impl));
   nir_builder b = nir_builder_at(nir_before_impl(impl));
   nir_def *params = vertex_params ? nir_load_vertex_param_buffer_poly(&b) : nir_load_geometry_param_buffer_poly(&b);
   unsigned grid = vertex_params ? offsetof(struct poly_vertex_params, grid) :
                                   offsetof(struct poly_geometry_params, grid);
   nir_def *g = nir_load_global_constant(&b, 2, 32, nir_iadd_imm(&b, params, grid), .align_mul = 4);
   nir_def *id = nir_load_global_invocation_id(&b, 32);
   nir_def *in = nir_iand(&b, nir_ult(&b, nir_channel(&b, id, 0), nir_channel(&b, g, 0)),
                          nir_ult(&b, nir_channel(&b, id, 1), nir_channel(&b, g, 1)));
   nir_push_if(&b, in);
   b.cursor = nir_cf_reinsert(&body, b.cursor);
   nir_pop_if(&b, NULL);
   nir_progress(true, impl, nir_metadata_none);
   (void)info;
}

/* ---------------------------------------------------------------- the vertex shader before a GS */

static bool
lower_vs_ids(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   b->cursor = nir_before_instr(&intr->instr);
   if (intr->intrinsic == nir_intrinsic_load_vertex_id_zero_base) {
      nir_def_replace(&intr->def, nir_channel(b, nir_load_global_invocation_id(b, 32), 0));
      return true;
   }
   return false;
}

/* Vulkan's InstanceIndex includes the first instance; libpoly's (the compute grid's y) does not. */
static bool
add_base_instance(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_instance_id || data == &intr->def)
      return false;
   b->cursor = nir_after_instr(&intr->instr);
   nir_def *v = nir_iadd(b, &intr->def, nir_load_base_instance(b));
   nir_def_rewrite_uses_after(&intr->def, v);
   return true;
}

/* Draw parameters of an indirect draw come from the GPU: the setup part wrote them to the poly buffer at
 * driver uniform poly_draw_params; 0 there means a direct draw (the immediates as before). */
static bool
gpu_draw_params(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   unsigned word;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_first_vertex:
   case nir_intrinsic_load_base_vertex:
      word = 0;
      break;
   case nir_intrinsic_load_base_instance:
      word = 1;
      break;
   case nir_intrinsic_load_draw_id:
      word = 2;
      break;
   default:
      return false;
   }
   b->cursor = nir_after_instr(&intr->instr);
   nir_def *at = wgvk_load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 1,
                                      nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, poly_draw_params)));
   nir_def *gpu = nir_load_global_constant(b, 1, 32, nir_pack_64_2x32_split(b, nir_iadd_imm(b, at, word * 4),
                                                                            nir_imm_int(b, 0)), .align_mul = 4);
   nir_def *v = nir_bcsel(b, nir_ine_imm(b, at, 0), gpu, &intr->def);
   nir_def_rewrite_uses_after(&intr->def, v);
   return true;
}

void
wgvk_poly_vs_before_gs(nir_shader *s, struct wgvk_legalize_info *info)
{
   s->info.outputs_written = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_output)
               s->info.outputs_written |= BITFIELD64_BIT(nir_intrinsic_io_semantics(intr).location);
         }
      }
   }
   /* gl_PointSize is not something the GS can read back meaningfully; keep it (libpoly lays out by mask) */
   info->vs_outputs = s->info.outputs_written;
   nir_shader_intrinsics_pass(s, add_base_instance, nir_metadata_control_flow, NULL);
   NIR_PASS(_, s, poly_nir_lower_vs_before_gs);
   NIR_PASS(_, s, nir_shader_intrinsics_pass, lower_vs_ids, nir_metadata_control_flow, NULL);
   make_compute(s, true, info);
   s->xfb_info = NULL;
   NIR_PASS(_, s, poly_nir_lower_sw_vs);
   NIR_PASS(_, s, nir_shader_intrinsics_pass, gpu_draw_params, nir_metadata_control_flow, NULL);
}

/* ---------------------------------------------------------------- geometry shaders */

static unsigned
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* gl_ViewportMask (no use without multiple-viewport hardware); gl_ViewportIndex and gl_Layer go on to the
 * rast part, where the legalisation emulates them (key.viewport_index_varying) or drops them. */
static bool
drop_viewport(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   gl_varying_slot loc = nir_intrinsic_io_semantics(intr).location;
   if (loc != VARYING_SLOT_VIEWPORT_MASK)
      return false;
   nir_instr_remove(&intr->instr);
   return true;
}

static void
gs_lower_io(nir_shader *s)
{
   nir_function_impl *entry = nir_shader_get_entrypoint(s);
   NIR_PASS(_, s, nir_lower_io_vars_to_temporaries, entry, nir_var_shader_out);
   NIR_PASS(_, s, nir_split_var_copies);
   NIR_PASS(_, s, nir_lower_global_vars_to_local);
   NIR_PASS(_, s, nir_lower_var_copies);
   NIR_PASS(_, s, nir_split_struct_vars, nir_var_function_temp);
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   NIR_PASS(_, s, nir_lower_io_array_vars_to_elements_no_indirects, false);
   nir_assign_io_var_locations(s, nir_var_shader_out);
   nir_assign_io_var_locations(s, nir_var_shader_in);
   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size_vec4, 0);
   NIR_PASS(_, s, nir_lower_system_values);
   NIR_PASS(_, s, nir_shader_intrinsics_pass, drop_viewport, nir_metadata_control_flow, NULL);
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_opt_cse);
   } while (progress);
   nir_shader_gather_info(s, entry);
}

/* Is anything left in a compute part but its bounds check? (main GS of a static topology) */
static bool
has_side_effects(nir_shader *s)
{
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_info *ii = &nir_intrinsic_infos[nir_instr_as_intrinsic(instr)->intrinsic];
            if (!(ii->flags & NIR_INTRINSIC_CAN_ELIMINATE))
               return true;
         }
      }
   }
   return false;
}

/* The xfb part: the rast part as compute, one invocation per output vertex of the rasterization draw (gid.x
 * < draw.index_count, gid.y < draw.instance_count): its vertex ID read from the index buffer (indexed
 * shapes; restarts skipped) or first_vertex + gid.x. It keeps only the transform feedback stores. */
static bool
xfb_ids(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   nir_def **ids = data;
   if (intr->intrinsic == nir_intrinsic_load_vertex_id && &intr->def != ids[0]) {
      nir_def_replace(&intr->def, ids[1]);
      return true;
   }
   if (intr->intrinsic == nir_intrinsic_load_instance_id && &intr->def != ids[2]) {
      nir_def_replace(&intr->def, ids[3]);
      return true;
   }
   if (intr->intrinsic == nir_intrinsic_store_output) {
      nir_instr_remove(&intr->instr);
      return true;
   }
   return false;
}

static nir_shader *
make_xfb_part(nir_shader *rast, bool indexed)
{
   nir_shader *s = nir_shader_clone(NULL, rast);
   s->info.stage = MESA_SHADER_COMPUTE;
   memset(&s->info.cs, 0, sizeof(s->info.cs));
   s->info.workgroup_size[0] = WGVK_POLY_WORKGROUP;
   s->info.workgroup_size[1] = s->info.workgroup_size[2] = 1;
   s->info.outputs_written = 0;
   s->xfb_info = NULL;
   s->info.has_transform_feedback_varyings = false;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_cf_list body;
   nir_cf_extract(&body, nir_before_impl(impl), nir_after_impl(impl));
   nir_builder b = nir_builder_at(nir_before_impl(impl));
   nir_def *gp = nir_load_geometry_param_buffer_poly(&b);
   nir_def *draw = nir_load_global_constant(&b, 3, 32, nir_iadd_imm(&b, gp, offsetof(struct poly_geometry_params, draw)),
                                            .align_mul = 4);
   nir_def *gid = nir_load_global_invocation_id(&b, 32);
   nir_def *x = nir_channel(&b, gid, 0), *y = nir_channel(&b, gid, 1);
   nir_def *in = nir_iand(&b, nir_ult(&b, x, nir_channel(&b, draw, 0)), nir_ult(&b, y, nir_channel(&b, draw, 1)));
   nir_push_if(&b, in);
   nir_def *vid;
   if (indexed) {
      /* the poly buffer is the index buffer (u32), first_index from its start */
      nir_def *addr = nir_pack_64_2x32_split(&b, nir_ishl_imm(&b, nir_iadd(&b, nir_channel(&b, draw, 2), x), 2),
                                             nir_imm_int(&b, 0));
      vid = nir_load_global_constant(&b, 1, 32, addr, .align_mul = 4);
   } else {
      vid = nir_iadd(&b, nir_channel(&b, draw, 2), x);
   }
   nir_push_if(&b, nir_ine_imm(&b, vid, ~0u));
   nir_def *raw_v = nir_load_vertex_id(&b), *raw_i = nir_load_instance_id(&b);
   b.cursor = nir_cf_reinsert(&body, b.cursor);
   nir_pop_if(&b, NULL);
   nir_pop_if(&b, NULL);
   nir_def *ids[4] = { raw_v, vid, raw_i, y };
   nir_shader_intrinsics_pass(s, xfb_ids, nir_metadata_none, ids);
   nir_instr_remove(nir_def_instr(raw_v));
   nir_instr_remove(nir_def_instr(raw_i));
   nir_progress(true, impl, nir_metadata_none);
   return s;
}

/* The rast part that draws: without the transform feedback stores (they are the xfb part's). */
static bool
strip_global_stores(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_global)
      return false;
   nir_instr_remove(&intr->instr);
   return true;
}

nir_shader *
wgvk_poly_prefix_sum_shader(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, opts, "gs count prefix sum");
   b.shader->info.workgroup_size[0] = b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   wgvk_prefix_sum_geom(&b, nir_load_geometry_param_buffer_poly(&b));
   return b.shader;
}

bool
wgvk_poly_lower_gs(nir_shader *gs, nir_shader *parts[WGVK_GS_PARTS], struct wgvk_gs_program *out)
{
   /* an application GS with transform feedback outputs */
   if (gs->info.has_transform_feedback_varyings && !gs->xfb_info)
      nir_gather_xfb_info_with_varyings(gs, NULL, NULL);
   if (gs->xfb_info)
      for (unsigned i = 0; i < 4; i++)
         out->xfb_stride[i] = gs->xfb_info->buffers[i].stride;
   gs_lower_io(gs);
   out->input_primitive = gs->info.gs.input_primitive;
   struct poly_gs_info info;
   nir_shader *count = NULL, *rast = NULL, *pre = NULL;
   poly_nir_lower_gs(gs, &count, &rast, &pre, &info);
   out->shape = info.shape;
   out->mode = info.mode;
   out->count_words = info.count_words;
   out->prefix_sum = info.prefix_sum;
   out->xfb = info.xfb;
   out->max_indices = info.max_indices;
   memcpy(out->topology, info.topology, sizeof(out->topology));

   parts[WGVK_GS_RAST] = rast;
   parts[WGVK_GS_COUNT] = count;
   parts[WGVK_GS_PRE] = pre;
   parts[WGVK_GS_MAIN] = NULL;
   parts[WGVK_GS_XFB] = NULL;
   parts[WGVK_GS_PREFIX] = NULL;
   if (info.xfb) {
      parts[WGVK_GS_XFB] = make_xfb_part(rast, poly_gs_indexed(info.shape));
      NIR_PASS(_, rast, nir_shader_intrinsics_pass, strip_global_stores, nir_metadata_control_flow, NULL);
      NIR_PASS(_, rast, nir_opt_dce);
   }
   if (info.prefix_sum)
      parts[WGVK_GS_PREFIX] = wgvk_poly_prefix_sum_shader(rast->options);
   struct wgvk_legalize_info dummy;
   memset(&dummy, 0, sizeof(dummy));
   if (count)
      make_compute(count, false, &dummy);
   if (pre) {
      pre->info.workgroup_size[0] = pre->info.workgroup_size[1] = pre->info.workgroup_size[2] = 1;
      ralloc_steal(NULL, pre);
   }
   make_compute(gs, false, &dummy);
   /* the main part writes the index buffer of a dynamic topology; otherwise it does nothing */
   NIR_PASS(_, gs, nir_opt_dce);
   NIR_PASS(_, gs, nir_opt_dead_cf);
   if (has_side_effects(gs))
      parts[WGVK_GS_MAIN] = gs;
   else
      ralloc_free(gs);
   return true;
}

/* ---------------------------------------------------------------- vertex shaders that write memory */

/* Does a vertex shader (from spirv_to_nir: derefs) store to buffers or images? WebGPU's vertex stage has
 * read-only storage only. */
bool
wgvk_poly_vs_writes_memory(nir_shader *s)
{
   if (s->info.stage != MESA_SHADER_VERTEX)
      return false;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_store_deref:
            case nir_intrinsic_deref_atomic:
            case nir_intrinsic_deref_atomic_swap: {
               nir_deref_instr *d = nir_src_as_deref(intr->src[0]);
               if (nir_deref_mode_may_be(d, nir_var_mem_ssbo | nir_var_mem_global))
                  return true;
               break;
            }
            case nir_intrinsic_store_ssbo:
            case nir_intrinsic_ssbo_atomic:
            case nir_intrinsic_ssbo_atomic_swap:
            case nir_intrinsic_image_deref_store:
            case nir_intrinsic_image_deref_atomic:
            case nir_intrinsic_image_deref_atomic_swap:
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
 * The vertex shader that rasterizes what a vertex shader run as compute wrote: vertex v of instance i
 * (non-indexed draw of the original count, first vertex and instance 0) reads outputs[i * count + v]
 * from libpoly's vertex output buffer and passes them on.
 */
nir_shader *
wgvk_poly_passthrough_vs(const nir_shader_compiler_options *opts, uint64_t outputs, unsigned clip_size)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, opts, "vs passthrough");
   nir_def *vid = nir_load_vertex_id(&b);
   nir_def *iid = nir_load_instance_id(&b);
   nir_def *vp = nir_load_vertex_param_buffer_poly(&b);
   nir_def *linear = nir_iadd(&b, nir_imul(&b, iid, poly_input_vertices(&b, vp)), vid);
   u_foreach_bit64(slot, outputs) {
      if (slot == VARYING_SLOT_PSIZ)
         continue;
      nir_def *addr = poly_vertex_output_address(&b, vp, nir_imm_int64(&b, outputs), linear, nir_imm_int(&b, slot));
      nir_def *v = nir_load_global_constant(&b, 4, 32, addr, .align_mul = 16);
      nir_io_semantics sem = { .location = slot, .num_slots = 1 };
      nir_store_output(&b, v, nir_imm_int(&b, 0), .write_mask = 0xf, .component = 0,
                       .src_type = slot == VARYING_SLOT_POS || slot == VARYING_SLOT_CLIP_DIST0 ||
                                   slot == VARYING_SLOT_CLIP_DIST1 ? nir_type_float32 : nir_type_uint32,
                       .io_semantics = sem);
   }
   b.shader->info.outputs_written = outputs & ~VARYING_BIT_PSIZ;
   b.shader->info.clip_distance_array_size = clip_size;
   return b.shader;
}

/* ---------------------------------------------------------------- indirect draws through a GS */

static nir_def *
driver_address(nir_builder *b, unsigned field, unsigned segment)
{
   nir_def *w = wgvk_load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 1,
                                     nir_imm_int(b, field));
   return nir_pack_64_2x32_split(b, w, nir_imm_int(b, segment));
}

nir_shader *
wgvk_poly_gs_setup_shader(const nir_shader_compiler_options *opts)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, opts, "gs indirect setup");
   b.shader->info.workgroup_size[0] = b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   wgvk_gs_setup_indirect(&b, driver_address(&b, offsetof(struct wgvk_driver_uniforms, poly_setup_args), 0),
                          driver_address(&b, offsetof(struct wgvk_driver_uniforms, index_buffer_offset), 1),
                          driver_address(&b, offsetof(struct wgvk_driver_uniforms, poly_indirect_draw), 2),
                          nir_load_vertex_param_buffer_poly(&b), nir_load_geometry_param_buffer_poly(&b),
                          driver_address(&b, offsetof(struct wgvk_driver_uniforms, poly_heap), 0),
                          driver_address(&b, offsetof(struct wgvk_driver_uniforms, poly_indirect_dispatch), 2),
                          driver_address(&b, offsetof(struct wgvk_driver_uniforms, poly_draw_params), 0));
   return b.shader;
}

/* ---------------------------------------------------------------- transform feedback without a GS */

nir_shader *
wgvk_poly_xfb_passthrough_gs(const nir_shader_compiler_options *opts, const void *xfb_info_, uint64_t outputs,
                             unsigned clip_size, unsigned prim)
{
   const nir_xfb_info *xfb = xfb_info_;
   size_t key_size = poly_passthrough_gs_key_size(xfb->output_count);
   struct poly_passthrough_gs_key *key = calloc(1, key_size);
   key->prim = u_decomposed_prim(prim);
   key->outputs = outputs & ~VARYING_BIT_PSIZ;
   key->clip_distance_array_size = clip_size;
   for (unsigned i = 0; i < NUM_TOTAL_VARYING_SLOTS; i++) {
      key->output_components[i] = 4;
      key->output_types[i] = nir_type_uint32;
   }
   for (unsigned i = 0; i < MAX_XFB_BUFFERS; i++)
      key->xfb_stride[i] = xfb->buffers[i].stride;
   memcpy(&key->xfb_info, xfb, nir_xfb_info_size(xfb->output_count));
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_GEOMETRY, opts, "xfb passthrough gs");
   poly_nir_passthrough_gs(&b, key);
   free(key);
   b.shader->info.io_lowered = true;
   return b.shader;
}

/* ---------------------------------------------------------------- tessellation */

static void
lower_io_tess(nir_shader *s)
{
   nir_function_impl *entry = nir_shader_get_entrypoint(s);
   /* (a TES that reads back what it wrote, gl_Position.y += ...: outputs through temporaries; a TCS's
    * outputs stay memory other invocations read) */
   NIR_PASS(_, s, nir_lower_io_vars_to_temporaries, entry,
            s->info.stage == MESA_SHADER_TESS_CTRL ? 0 : nir_var_shader_out);
   NIR_PASS(_, s, nir_split_var_copies);
   NIR_PASS(_, s, nir_lower_global_vars_to_local);
   NIR_PASS(_, s, nir_lower_var_copies);
   NIR_PASS(_, s, nir_split_struct_vars, nir_var_function_temp);
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   nir_assign_io_var_locations(s, nir_var_shader_out);
   nir_assign_io_var_locations(s, nir_var_shader_in);
   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size_vec4, 0);
   NIR_PASS(_, s, nir_lower_system_values);
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
   } while (progress);
   nir_shader_gather_info(s, entry);
}

/* A patch's invocations are one workgroup here (libpoly assumed one subgroup) */
static bool
widen_barriers(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_barrier)
      return false;
   bool changed = false;
   if (nir_intrinsic_execution_scope(intr) == SCOPE_SUBGROUP) {
      nir_intrinsic_set_execution_scope(intr, SCOPE_WORKGROUP);
      changed = true;
   }
   if (nir_intrinsic_memory_scope(intr) == SCOPE_SUBGROUP) {
      nir_intrinsic_set_memory_scope(intr, SCOPE_WORKGROUP);
      changed = true;
   }
   return changed;
}

void
wgvk_poly_lower_tcs(nir_shader *s, struct wgvk_legalize_info *info)
{
   lower_io_tess(s);
   info->tess.tcs_vertices_out = s->info.tess.tcs_vertices_out;
   info->tess.nr_patch_outputs = util_last_bit(s->info.patch_outputs_written);
   info->tess.output_stride = poly_tcs_output_stride(s);
   info->tess.per_vertex_outputs = poly_tcs_per_vertex_outputs(s);
   NIR_PASS(_, s, poly_nir_lower_tcs, false);
   NIR_PASS(_, s, nir_shader_intrinsics_pass, widen_barriers, nir_metadata_all, NULL);
   unsigned n = s->info.tess.tcs_vertices_out;
   s->info.stage = MESA_SHADER_COMPUTE;
   memset(&s->info.cs, 0, sizeof(s->info.cs));
   s->info.workgroup_size[0] = MAX2(n, 1);
   s->info.workgroup_size[1] = s->info.workgroup_size[2] = 1;
   s->info.workgroup_size_variable = false;
   s->xfb_info = NULL;
}

void
wgvk_poly_lower_tes(nir_shader *s, struct wgvk_legalize_info *info)
{
   lower_io_tess(s);
   info->tess.domain = s->info.tess._primitive_mode;
   info->tess.spacing = s->info.tess.spacing;
   info->tess.ccw = s->info.tess.ccw;
   info->tess.point_mode = s->info.tess.point_mode;
   NIR_PASS(_, s, poly_nir_lower_tes, true);
}

/* The tessellator library (wgvk_tess.cl's SPIR-V), as Mesa's drivers import OpenCL libraries: functions
 * with physical addressing for global/constant memory and private memory in scratch, which becomes indexable
 * function arrays again (the WGSL emitter indexes them). */
static nir_shader *
tess_library(const nir_shader_compiler_options *opts)
{
   const struct spirv_to_nir_options so = {
      .environment = NIR_SPIRV_OPENCL,
      .shared_addr_format = nir_address_format_62bit_generic,
      .global_addr_format = nir_address_format_62bit_generic,
      .temp_addr_format = nir_address_format_62bit_generic,
      .constant_addr_format = nir_address_format_64bit_global,
      .create_library = true,
   };
   nir_shader *lib = spirv_to_nir((const uint32_t *)wgvk_tess_spv, sizeof(wgvk_tess_spv) / 4, NULL,
                                  MESA_SHADER_KERNEL, "library", &so, opts);
   if (!lib)
      return NULL;
   nir_fixup_is_exported(lib);
   NIR_PASS(_, lib, nir_lower_system_values);
   NIR_PASS(_, lib, nir_lower_calls_to_builtins);
   NIR_PASS(_, lib, nir_lower_printf, &(const struct nir_lower_printf_options){ .hash_format_strings = true });
   NIR_PASS(_, lib, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, lib, nir_lower_returns);
   NIR_PASS(_, lib, nir_inline_functions);
   return lib;
}

static bool
scratch_to_global(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   bool load = intr->intrinsic == nir_intrinsic_load_scratch;
   if (!load && intr->intrinsic != nir_intrinsic_store_scratch)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *base = wgvk_load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 1,
                                        nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, poly_scratch)));
   nir_def *inv = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_def *stride = nir_imm_int(b, ALIGN_POT(b->shader->scratch_size, 16));
   nir_def *off = nir_iadd(b, nir_iadd(b, base, nir_imul(b, inv, stride)), intr->src[load ? 0 : 1].ssa);
   nir_def *addr = nir_pack_64_2x32_split(b, off, nir_imm_int(b, 0));
   unsigned align = MAX2(nir_intrinsic_align_mul(intr), 1);
   if (load) {
      nir_def *v = nir_load_global(b, intr->def.num_components, intr->def.bit_size, addr, .align_mul = align);
      nir_def_replace(&intr->def, v);
   } else {
      nir_store_global(b, intr->src[0].ssa, addr, .write_mask = nir_intrinsic_write_mask(intr), .align_mul = align);
      nir_instr_remove(&intr->instr);
   }
   return true;
}

/* Global accesses at a constant OpenCL generic address tagged private (top bits 0b10 in
 * nir_address_format_62bit_generic): explicit IO folds some private reads through const pointers into
 * global loads of such an address. They are scratch too. */
static bool
tagged_scratch_to_global(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   bool store = intr->intrinsic == nir_intrinsic_store_global;
   if (!store && intr->intrinsic != nir_intrinsic_load_global && intr->intrinsic != nir_intrinsic_load_global_constant)
      return false;
   nir_scalar a = nir_scalar_chase_movs(nir_get_scalar(intr->src[store ? 1 : 0].ssa, 0));
   if (!nir_scalar_is_const(a) || (nir_scalar_as_uint(a) >> 62) != 2)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *base = wgvk_load_driver_ubo(b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 1,
                                        nir_imm_int(b, offsetof(struct wgvk_driver_uniforms, poly_scratch)));
   nir_def *inv = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_def *stride = nir_imm_int(b, ALIGN_POT(b->shader->scratch_size, 16));
   nir_def *off = nir_iadd_imm(b, nir_iadd(b, base, nir_imul(b, inv, stride)), (uint32_t)nir_scalar_as_uint(a));
   nir_src_rewrite(&intr->src[store ? 1 : 0], nir_pack_64_2x32_split(b, off, nir_imm_int(b, 0)));
   return true;
}

static bool
lower_udiv_aligned_4(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if (alu->op != nir_op_udiv_aligned_4)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def_replace(&alu->def, nir_ushr_imm(b, nir_ssa_for_alu_src(b, alu, 0), 2));
   return true;
}

/* Atomics through OpenCL generic pointers (libpoly casts its global counters to atomic_uint *): only global
 * memory has them here, so pin those derefs to it (nir_lower_explicit_io has no scratch atomics). */
static bool
pin_atomic_derefs(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_deref_atomic && intr->intrinsic != nir_intrinsic_deref_atomic_swap)
      return false;
   bool progress = false;
   for (nir_deref_instr *d = nir_src_as_deref(intr->src[0]); d; d = nir_deref_instr_parent(d)) {
      if (d->modes != nir_var_mem_global && (d->modes & nir_var_mem_global)) {
         d->modes = nir_var_mem_global;
         progress = true;
      }
      if (d->deref_type == nir_deref_type_cast || d->deref_type == nir_deref_type_var)
         break;
   }
   return progress;
}

static nir_shader *
import_and_link(nir_shader *s)
{
   static nir_shader *lib;
   if (!lib)
      lib = tess_library(s->options);
   if (!lib)
      return s;
   NIR_PASS(_, s, nir_link_shader_functions, lib);
   NIR_PASS(_, s, nir_inline_functions);
   if (!nir_shader_fully_linked(s)) {
      nir_foreach_function(f, s)
         if (!f->impl)
            fprintf(stderr, "wgvk: tessellator: %s not linked\n", f->name);
   }
   nir_remove_non_entrypoints(s);
   NIR_PASS(_, s, nir_lower_variable_initializers, ~0);
   nir_lower_constant_to_temp(s);
   NIR_PASS(_, s, nir_lower_vec3_to_vec4, nir_var_shader_temp | nir_var_function_temp | nir_var_mem_shared | nir_var_mem_global | nir_var_mem_constant);
   NIR_PASS(_, s, nir_split_var_copies);
   NIR_PASS(_, s, nir_lower_var_copies);
   NIR_PASS(_, s, nir_lower_vars_to_explicit_types, nir_var_shader_temp | nir_var_function_temp | nir_var_mem_shared | nir_var_mem_global | nir_var_mem_constant,
            glsl_get_cl_type_size_align);
   NIR_PASS(_, s, nir_lower_memcpy);
   NIR_PASS(_, s, nir_lower_explicit_io, nir_var_mem_constant, nir_address_format_64bit_global);
   NIR_PASS(_, s, nir_opt_deref);
   NIR_PASS(_, s, nir_lower_convert_alu_types, NULL);
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   /* (pins generic pointers to their modes where it can: an atomic on scratch does not exist) */
   bool progress;
   unsigned n = 0;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_deref);
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_copy_prop_vars);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_lower_vars_to_ssa);
   } while (progress && ++n < 20);
   NIR_PASS(_, s, nir_shader_intrinsics_pass, pin_atomic_derefs, nir_metadata_all, NULL);
   /* (private variables nothing reads any more take no scratch: the layout is then the same in the native
    * and the Emscripten build, whose optimisations leave different dead variables behind) */
   NIR_PASS(_, s, nir_remove_dead_derefs);
   NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp | nir_var_shader_temp, NULL);
   /* (the layout below places every private variable again: the space of those removed is not kept) */
   s->scratch_size = 0;
   /* OpenCL's generic pointers: global, private and local through one address format (global addresses
    * have their top bits clear, as wgvk's segment:offset addresses do); private accesses become scratch,
    * then function arrays again */
   NIR_PASS(_, s, nir_lower_vars_to_explicit_types, nir_var_shader_temp | nir_var_function_temp | nir_var_mem_shared | nir_var_mem_global,
            glsl_get_cl_type_size_align);
   if (getenv("WGVK_PRINT_TESS_NIR")) {
      nir_foreach_function_temp_variable(v, nir_shader_get_entrypoint(s))
         fprintf(stderr, "wgvk: tess private %s at %u, %u bytes\n", v->name ? v->name : "?", v->data.driver_location,
                 glsl_get_explicit_size(v->type, false));
      nir_foreach_variable_with_modes(v, s, nir_var_shader_temp)
         fprintf(stderr, "wgvk: tess shader_temp %s at %u, %u bytes\n", v->name ? v->name : "?", v->data.driver_location,
                 glsl_get_explicit_size(v->type, false));
   }
   NIR_PASS(_, s, nir_lower_explicit_io, nir_var_shader_temp | nir_var_function_temp | nir_var_mem_shared | nir_var_mem_global, nir_address_format_62bit_generic);
   /* private memory: the tessellator's context is ~10 KiB, past WGSL's 8 KiB for function variables; it
    * lives in the poly buffer instead, scratch_size bytes per invocation at driver uniform poly_scratch */
   NIR_PASS(_, s, nir_shader_intrinsics_pass, scratch_to_global, nir_metadata_control_flow, NULL);
   NIR_PASS(_, s, nir_remove_dead_derefs);
   NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp | nir_var_shader_temp, NULL);
   NIR_PASS(_, s, nir_lower_system_values);
   NIR_PASS(_, s, nir_shader_alu_pass, lower_udiv_aligned_4, nir_metadata_control_flow, NULL);
   /* (wgvk_compile reports it as legal.scratch_bytes) */
   s->scratch_size = ALIGN_POT(s->scratch_size, 16);
   NIR_PASS(_, s, nir_opt_copy_prop);
   NIR_PASS(_, s, nir_opt_constant_folding);
   NIR_PASS(_, s, nir_opt_dce);
   NIR_PASS(_, s, nir_shader_intrinsics_pass, tagged_scratch_to_global, nir_metadata_control_flow, NULL);
   if (getenv("WGVK_PRINT_TESS_NIR"))
      nir_print_shader(s, stderr);
   return s;
}

/* Private memory starts zeroed, as WGSL's function variables do (the tessellator reads some of its context
 * before writing it, harmlessly on a GPU whose private memory it owns; here the count kernel's leftovers
 * would be read). */
static void
zero_scratch(nir_shader *s)
{
   unsigned bytes = ALIGN_POT(s->scratch_size, 16);
   if (!bytes)
      return;
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_builder b = nir_builder_at(nir_before_impl(impl));
   nir_def *base = wgvk_load_driver_ubo(&b, WGVK_DRIVER_BINDING_UNIFORMS, sizeof(struct wgvk_driver_uniforms), 1,
                                        nir_imm_int(&b, offsetof(struct wgvk_driver_uniforms, poly_scratch)));
   nir_def *inv = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *start = nir_iadd(&b, base, nir_imul_imm(&b, inv, bytes));
   nir_def *zero = nir_imm_zero(&b, 4, 32);
   nir_variable *i = nir_local_variable_create(impl, glsl_uint_type(), "zero_i");
   nir_store_var(&b, i, nir_imm_int(&b, 0), 1);
   nir_push_loop(&b);
   {
      nir_def *o = nir_load_var(&b, i);
      nir_break_if(&b, nir_uge_imm(&b, o, bytes));
      nir_store_global(&b, zero, nir_pack_64_2x32_split(&b, nir_iadd(&b, start, o), nir_imm_int(&b, 0)), .align_mul = 16);
      nir_store_var(&b, i, nir_iadd_imm(&b, o, 16), 1);
   }
   nir_pop_loop(&b, NULL);
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
}

static nir_shader *
tess_kernel(const nir_shader_compiler_options *opts, unsigned domain, unsigned which)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, opts, "tessellator");
   nir_def *p = nir_load_tess_param_buffer_poly(&b);
   if (which == 1) {
      b.shader->info.workgroup_size[0] = b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
      wgvk_prefix_sum_tess(&b, p);
      return b.shader;
   }
   b.shader->info.workgroup_size[0] = WGVK_POLY_WORKGROUP;
   b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   nir_def *patch = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *n = nir_load_global_constant(&b, 1, 32, nir_iadd_imm(&b, p, offsetof(struct poly_tess_params, nr_patches)),
                                         .align_mul = 4);
   nir_push_if(&b, nir_ult(&b, patch, n));
   nir_def *mode = nir_imm_int(&b, which == 0 ? POLY_TESS_MODE_COUNT : POLY_TESS_MODE_WITH_COUNTS);
   const char *fn = domain == TESS_PRIMITIVE_QUADS ? "wgvk_tess_quad" :
                    domain == TESS_PRIMITIVE_ISOLINES ? "wgvk_tess_isoline" : "wgvk_tess_tri";
   /* a call into the imported library; linked and inlined below */
   nir_function *f = nir_function_create(b.shader, fn);
   f->num_params = 3;
   f->params = ralloc_array(b.shader, nir_parameter, 3);
   f->params[0] = (nir_parameter){ .num_components = 1, .bit_size = 64 };
   f->params[1] = (nir_parameter){ .num_components = 1, .bit_size = 32 };
   f->params[2] = (nir_parameter){ .num_components = 1, .bit_size = 32 };
   nir_def *args[3] = { p, patch, mode };
   nir_build_call(&b, f, 3, args);
   nir_pop_if(&b, NULL);
   nir_shader *k = import_and_link(b.shader);
   zero_scratch(k);
   return k;
}

nir_shader *
wgvk_poly_tess_kernel(const nir_shader_compiler_options *opts, unsigned domain, unsigned which)
{
   return tess_kernel(opts, domain, which);
}
