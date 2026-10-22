/*
 * NIR -> WGSL. See nir_to_wgsl.h for the contract.
 *
 * How the output is shaped:
 *   - Every SSA value is a WGSL `let vN` in the scope where NIR defines it. A value used outside
 *     that scope (NIR allows a use anywhere the definition dominates, WGSL only inside the
 *     enclosing block) is a function-scope `var vN` declared at the top instead.
 *   - A phi is a `var vN` too. Each predecessor block assigns its source at its end, through lets
 *     first when the block has several phis, so a phi that reads another phi sees the old value.
 *   - NIR values have a bit size, not a type. Each value gets the WGSL type of the instruction
 *     that makes it (f32 for fadd, i32 for iadd, u32 for loads, ...); a use that wants another
 *     type of the same size gets a bitcast<>. Constants are written directly in the type each use
 *     wants, so most constants cost nothing.
 *   - Buffers are word arrays: uniform buffers array<vec4<u32>, N>, storage buffers array<u32>
 *     (array<atomic<u32>> or array<atomic<i32>> when the shader uses atomics on them). NIR's byte
 *     offsets index them directly. This takes any Vulkan layout (std140, std430, scalar) and
 *     leaves room for the buffer heap later.
 *   - Shader I/O is one vec4 per location, in an input struct parameter and an output struct the
 *     entry point returns, so both sides of an inter-stage interface have the same types.
 *
 * Unsupported constructs do not stop emission: each one adds a "[tag] detail" line to the error,
 * so one run lists everything that blocks a shader.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "nir_to_wgsl.h"
#include "util/hash_table.h"
#include "util/set.h"
#include "util/ralloc.h"
#include "util/string_buffer.h"
#include "util/u_math.h"
#include "util/format/u_format.h"
#include "util/half_float.h"

enum wt { /* WGSL scalar types */
   WT_NONE,
   WT_BOOL,
   WT_I32,
   WT_U32,
   WT_F32,
   WT_F16,
};

static const char *wt_name[] = { "?", "bool", "i32", "u32", "f32", "f16" };

struct io_slot {
   bool used;
   enum wt type;
   unsigned comps;           /* mask of components touched */
   int interp;               /* INTERP_MODE_*, -1 unknown */
   int sampling;             /* 0 center, 1 centroid, 2 sample */
   unsigned blend_src;       /* dual-source index + 1 */
};

struct buf_slot {
   unsigned set, binding, index;
   bool ubo, written, atomic, atomic_signed, atomic_unsigned_minmax;
   uint32_t size;            /* bytes the shader may touch (ubo) */
   char name[32];
};

struct res_var {             /* a texture / sampler / storage image variable, per array element */
   nir_variable *var;
   unsigned index;
   bool tex_used, smp_used, shadow, shadow_mixed, read, written;
   enum wt sample;           /* texture sampled type */
   bool ms;
   char tname[48], sname[48];
};

struct ctx {
   nir_shader *s;
   const struct wgsl_options *o;
   void *mem;
   struct _mesa_string_buffer *out;     /* function body */
   struct _mesa_string_buffer *err;
   struct hash_table *tags;             /* tags already reported */
   unsigned indent;

   enum wt *type;                       /* per SSA index */
   bool *is_var;                        /* declared as a function-scope var */
   struct hash_table *list_of;          /* cf node -> list id */
   unsigned *block_list;                /* block index -> list id */

   struct buf_slot bufs[64];
   unsigned num_bufs;
   struct res_var res[128];
   unsigned num_res;
   struct hash_table *atomic_vars;      /* nir_variable -> (1 unsigned, 2 signed) */
   struct hash_table *struct_names;     /* glsl_type -> name */
   struct _mesa_string_buffer *structs;
   unsigned num_structs;

   struct io_slot in[64], out_slots[64];
   bool in_frag_coord, in_front_face, in_sample_id, in_sample_mask, in_vertex_index,
        in_instance_index, in_prim_id;
   bool out_pos, out_depth, out_sample_mask;
   unsigned out_clip;                   /* clip distance count */
   bool cs_lid, cs_lidx, cs_gid, cs_wgid, cs_nwg, sg_size, sg_inv, sg_num, sg_id;
   bool uses_subgroups, uses_f16, uses_dual_src, uses_derivatives;
   uint32_t pc_size;
   bool helper_imul_high, helper_umul_high, helper_imod, helper_ballot;
   struct set *barrier_nodes;           /* cf nodes with a workgroup barrier inside */
   struct set *uniformize;              /* ifs whose condition must be made uniform */
   uint8_t *uniform;                    /* per SSA index: 0 unknown, 1 busy, 2 uniform, 3 not */
   unsigned num_uconds;
   bool need_inf;
};

/* ---------------------------------------------------------------- output helpers */

static void PRINTFLIKE(2, 3)
P(struct ctx *c, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   _mesa_string_buffer_vprintf(c->out, fmt, ap);
   va_end(ap);
}

static void
IND(struct ctx *c)
{
   for (unsigned i = 0; i < c->indent; i++)
      _mesa_string_buffer_append(c->out, "  ");
}

static void PRINTFLIKE(3, 4)
fail(struct ctx *c, const char *tag, const char *fmt, ...)
{
   char detail[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(detail, sizeof(detail), fmt, ap);
   va_end(ap);
   char key[300];
   snprintf(key, sizeof(key), "[%s] %s", tag, detail);
   if (_mesa_hash_table_search(c->tags, key))
      return;
   _mesa_hash_table_insert(c->tags, ralloc_strdup(c->mem, key), NULL);
   _mesa_string_buffer_printf(c->err, "%s\n", key);
}

/* ---------------------------------------------------------------- types */

static enum wt
wt_of_alu_type(nir_alu_type t, unsigned bit_size)
{
   if (bit_size == 1)
      return WT_BOOL;
   switch (nir_alu_type_get_base_type(t)) {
   case nir_type_float:
      return bit_size == 16 ? WT_F16 : WT_F32;
   case nir_type_int:
      return WT_I32;
   case nir_type_bool:
      return WT_BOOL;
   default:
      return WT_U32;
   }
}

static enum wt
wt_of_glsl_base(enum glsl_base_type b)
{
   switch (b) {
   case GLSL_TYPE_FLOAT: return WT_F32;
   case GLSL_TYPE_FLOAT16: return WT_F16;
   case GLSL_TYPE_INT: return WT_I32;
   case GLSL_TYPE_UINT: return WT_U32;
   case GLSL_TYPE_BOOL: return WT_BOOL;
   default: return WT_NONE;
   }
}

/* "f32", "vec3<f32>" */
static const char *
type_str(struct ctx *c, enum wt t, unsigned n)
{
   if (n == 1)
      return wt_name[t];
   return ralloc_asprintf(c->mem, "vec%u<%s>", n, wt_name[t]);
}

static void
check_bits(struct ctx *c, unsigned bit_size, enum wt t, const char *what)
{
   if (bit_size == 64)
      fail(c, "64-bit", "%s: 64-bit values (lower with nir_lower_int64 / soft fp64)", what);
   else if (bit_size == 8)
      fail(c, "8-bit", "%s: 8-bit values", what);
   else if (bit_size == 16 && t != WT_F16)
      fail(c, "16-bit-int", "%s: 16-bit integers (WGSL has none; nir_lower_bit_size)", what);
   else if (bit_size == 16 && !c->o->shader_f16)
      fail(c, "f16", "%s: 16-bit float without shader-f16", what);
   else if (bit_size == 16)
      c->uses_f16 = true;
}

static enum wt
def_type(struct ctx *c, nir_def *d)
{
   enum wt t = c->type[d->index];
   if (d->bit_size == 1)
      return WT_BOOL;
   if (t == WT_NONE)
      t = d->bit_size == 16 ? WT_F16 : WT_U32;
   return t;
}

/* ---------------------------------------------------------------- constants */

static char *
f32_lit(struct ctx *c, float f)
{
   if (isnan(f) || isinf(f)) {
      c->need_inf = true;
      union { float f; uint32_t u; } u = { f };
      return ralloc_asprintf(c->mem, "bitcast<f32>(wgsl_nonfinite(0x%08xu))", u.u);
   }
   /* WGSL reads a literal as an exact (abstract) value and rejects one outside f32's range, so
    * print the float's exact value: the short form when it is exact, else 17 digits, which are. */
   char buf[64];
   snprintf(buf, sizeof(buf), "%.9g", f);
   if (strtod(buf, NULL) != (double)f)
      snprintf(buf, sizeof(buf), "%.17g", (double)f);
   if (f == 0 && signbit(f))
      snprintf(buf, sizeof(buf), "-0.0");
   bool has_dot = strpbrk(buf, ".epx") != NULL;
   return ralloc_asprintf(c->mem, "%s%sf", buf, has_dot ? "" : ".0");
}

static char *
scalar_lit(struct ctx *c, nir_const_value v, unsigned bit_size, enum wt t)
{
   if (bit_size == 1)
      return ralloc_strdup(c->mem, v.b ? "true" : "false");
   uint32_t u = bit_size == 16 ? v.u16 : v.u32;
   switch (t) {
   case WT_BOOL:
      return ralloc_strdup(c->mem, u ? "true" : "false");
   case WT_I32:
      if ((int32_t)u == INT32_MIN)
         return ralloc_strdup(c->mem, "i32(-2147483647 - 1)");
      return ralloc_asprintf(c->mem, "%di", (int32_t)u);
   case WT_U32:
      return ralloc_asprintf(c->mem, "%uu", u);
   case WT_F32:
      return f32_lit(c, uif(u));
   case WT_F16: {
      float f = _mesa_half_to_float(v.u16);
      if (isnan(f) || isinf(f)) {
         c->need_inf = true;
         return ralloc_asprintf(c->mem, "f16(bitcast<f32>(wgsl_nonfinite(0x%08xu)))", fui(f));
      }
      char *s = f32_lit(c, f);
      s[strlen(s) - 1] = 'h';
      return s;
   }
   default:
      return ralloc_strdup(c->mem, "0");
   }
}

/* ---------------------------------------------------------------- sources */

static const char swz[] = "xyzw";

static bool
src_is_const(nir_src *s)
{
   return nir_def_instr(s->ssa)->type == nir_instr_type_load_const;
}

static bool
src_is_undef(nir_src *s)
{
   return nir_def_instr(s->ssa)->type == nir_instr_type_undef;
}

/*
 * Print an SSA value as `want` with the given components (comp[i] selects the source
 * component for result component i; NULL means 0..n-1). Splats scalars, bitcasts across types.
 */
static void
emit_src_comps(struct ctx *c, nir_def *d, enum wt want, const uint8_t *comp, unsigned n)
{
   uint8_t ident[4] = { 0, 1, 2, 3 };
   if (!comp)
      comp = ident;
   if (want == WT_NONE)
      want = def_type(c, d);
   if (d->bit_size == 1)
      want = WT_BOOL;

   if (nir_def_instr(d)->type == nir_instr_type_load_const) {
      nir_load_const_instr *lc = nir_instr_as_load_const(nir_def_instr(d));
      if (n > 1)
         P(c, "%s(", type_str(c, want, n));
      for (unsigned i = 0; i < n; i++)
         P(c, "%s%s", i ? ", " : "", scalar_lit(c, lc->value[comp[i]], d->bit_size, want));
      if (n > 1)
         P(c, ")");
      return;
   }
   if (nir_def_instr(d)->type == nir_instr_type_undef) {
      P(c, "%s()", type_str(c, want, n));
      return;
   }

   enum wt have = def_type(c, d);
   bool cast = have != want;
   if (cast && (have == WT_BOOL || want == WT_BOOL)) {
      /* bool <-> number: a conversion, not a bitcast (only reached for odd typeless movs) */
      P(c, "%s(", type_str(c, want, n));
   } else if (cast) {
      P(c, "bitcast<%s>(", type_str(c, want, n));
   }
   if (d->num_components == 1 && n > 1) {
      P(c, "%s(v%u)", type_str(c, have, n), d->index);
   } else {
      P(c, "v%u", d->index);
      bool identity = n == d->num_components;
      for (unsigned i = 0; i < n; i++)
         identity = identity && comp[i] == i;
      if (!identity && d->num_components > 1) {
         P(c, ".");
         for (unsigned i = 0; i < n; i++)
            P(c, "%c", swz[comp[i]]);
      }
   }
   if (cast)
      P(c, ")");
}

static void
emit_src(struct ctx *c, nir_src *s, enum wt want)
{
   emit_src_comps(c, s->ssa, want, NULL, s->ssa->num_components);
}

static void
emit_src_comp(struct ctx *c, nir_src *s, enum wt want, unsigned comp)
{
   uint8_t cc = comp;
   emit_src_comps(c, s->ssa, want, &cc, 1);
}

static void
emit_src_n(struct ctx *c, nir_src *s, enum wt want, unsigned n)
{
   emit_src_comps(c, s->ssa, want, NULL, n);
}

/* Start a definition: `let vN = ` or `vN = `. */
static void
def_begin(struct ctx *c, nir_def *d)
{
   IND(c);
   if (c->is_var[d->index])
      P(c, "v%u = ", d->index);
   else
      P(c, "let v%u = ", d->index);
}

/* ---------------------------------------------------------------- scopes */

/* Each statement list (function body, then, else, loop body) gets an id; a value defined in
 * list L is visible to uses in L after it and in lists nested inside L. */
static void
assign_lists(struct ctx *c, struct exec_list *list, unsigned *next, nir_cf_node *parent_node)
{
   unsigned id = (*next)++;
   foreach_list_typed(nir_cf_node, node, node, list) {
      _mesa_hash_table_insert(c->list_of, node, (void *)(uintptr_t)(id + 1));
      switch (node->type) {
      case nir_cf_node_block:
         c->block_list[nir_cf_node_as_block(node)->index] = id;
         break;
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         assign_lists(c, &nif->then_list, next, node);
         assign_lists(c, &nif->else_list, next, node);
         break;
      }
      case nir_cf_node_loop:
         assign_lists(c, &nir_cf_node_as_loop(node)->body, next, node);
         break;
      default:
         break;
      }
   }
}

static unsigned
list_of(struct ctx *c, nir_cf_node *n)
{
   struct hash_entry *e = _mesa_hash_table_search(c->list_of, n);
   return e ? (unsigned)(uintptr_t)e->data - 1 : ~0u;
}

static bool
visible_from(struct ctx *c, unsigned def_list, nir_cf_node *use)
{
   for (nir_cf_node *n = use; n; n = n->parent) {
      if (n->type == nir_cf_node_function)
         return def_list == list_of(c, n) || def_list == 0;
      if (list_of(c, n) == def_list)
         return true;
   }
   return false;
}

static bool
def_needs_var(struct ctx *c, nir_def *d)
{
   nir_block *db = nir_def_instr(d)->block;
   unsigned dl = c->block_list[db->index];
   nir_foreach_use_including_if(src, d) {
      nir_cf_node *at;
      if (nir_src_is_if(src)) {
         at = &nir_src_use_if(src)->cf_node;
      } else {
         nir_instr *ui = nir_src_use_instr(src);
         if (ui->type == nir_instr_type_phi) {
            nir_phi_src *ps = exec_node_data(nir_phi_src, src, src);
            at = &ps->pred->cf_node;
         } else {
            at = &ui->block->cf_node;
         }
      }
      if (!visible_from(c, dl, at))
         return true;
   }
   return false;
}

/* ---------------------------------------------------------------- ALU */

static bool
alu_is_passthrough(nir_op op)
{
   return op == nir_op_mov || nir_op_is_vec(op) || op == nir_op_bcsel;
}

/* The WGSL type an ALU source is read as. */
static enum wt
alu_src_want(struct ctx *c, nir_alu_instr *alu, unsigned i)
{
   const nir_op_info *info = &nir_op_infos[alu->op];
   unsigned bits = alu->src[i].src.ssa->bit_size;
   if (bits == 1)
      return WT_BOOL;
   if (alu_is_passthrough(alu->op))
      return def_type(c, &alu->def);
   return wt_of_alu_type(info->input_types[i], bits);
}

/* Source i of an ALU instruction, swizzled to n components. */
static void
asrc_n(struct ctx *c, nir_alu_instr *alu, unsigned i, enum wt want, unsigned n)
{
   if (want == WT_NONE)
      want = alu_src_want(c, alu, i);
   emit_src_comps(c, alu->src[i].src.ssa, want, alu->src[i].swizzle, n);
}

static unsigned
alu_src_n(nir_alu_instr *alu, unsigned i)
{
   unsigned in = nir_op_infos[alu->op].input_sizes[i];
   return in ? in : alu->def.num_components;
}

static void
asrc(struct ctx *c, nir_alu_instr *alu, unsigned i)
{
   asrc_n(c, alu, i, WT_NONE, alu_src_n(alu, i));
}

static void
asrc_as(struct ctx *c, nir_alu_instr *alu, unsigned i, enum wt want)
{
   asrc_n(c, alu, i, want, alu_src_n(alu, i));
}

static void
binop(struct ctx *c, nir_alu_instr *alu, const char *op)
{
   P(c, "(");
   asrc(c, alu, 0);
   P(c, " %s ", op);
   asrc(c, alu, 1);
   P(c, ")");
}

static void
call(struct ctx *c, nir_alu_instr *alu, const char *fn)
{
   P(c, "%s(", fn);
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      if (i)
         P(c, ", ");
      asrc(c, alu, i);
   }
   P(c, ")");
}

/* Apply a scalar helper per component: vecN<T>(f(a.x, b.x), ...). */
static void
per_comp(struct ctx *c, nir_alu_instr *alu, const char *fn, enum wt t)
{
   unsigned n = alu->def.num_components;
   if (n > 1)
      P(c, "%s(", type_str(c, t, n));
   for (unsigned k = 0; k < n; k++) {
      P(c, "%s%s(", k ? ", " : "", fn);
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
         if (i)
            P(c, ", ");
         uint8_t comp = alu->src[i].swizzle[k];
         emit_src_comps(c, alu->src[i].src.ssa, t, &comp, 1);
      }
      P(c, ")");
   }
   if (n > 1)
      P(c, ")");
}

static bool
alu_srcs_equal(nir_alu_instr *alu)
{
   if (alu->src[0].src.ssa != alu->src[1].src.ssa)
      return false;
   for (unsigned i = 0; i < alu->def.num_components; i++)
      if (alu->src[0].swizzle[i] != alu->src[1].swizzle[i])
         return false;
   return true;
}

/* (bits & 0x7fffffff) > 0x7f800000, per component */
static void
emit_isnan(struct ctx *c, nir_alu_instr *alu, unsigned i)
{
   unsigned n = alu->def.num_components;
   bool h = alu->src[i].src.ssa->bit_size == 16;
   const char *U = type_str(c, WT_U32, n);
   P(c, "(");
   if (h) {
      P(c, "(bitcast<%s>(%s(", U, type_str(c, WT_F32, n));
      asrc(c, alu, i);
      P(c, "))");
   } else {
      P(c, "(bitcast<%s>(", U);
      asrc(c, alu, i);
      P(c, ")");
   }
   P(c, " & %s(0x7fffffffu)) > %s(0x7f800000u))", U, U);
}

static void
emit_alu(struct ctx *c, nir_alu_instr *alu)
{
   nir_def *d = &alu->def;
   unsigned n = d->num_components;
   enum wt t = def_type(c, d);
   const char *T = type_str(c, t, n);

   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
      check_bits(c, alu->src[i].src.ssa->bit_size, alu_src_want(c, alu, i), nir_op_infos[alu->op].name);
   check_bits(c, d->bit_size, t, nir_op_infos[alu->op].name);

   def_begin(c, d);
   switch (alu->op) {
   case nir_op_mov:
      asrc(c, alu, 0);
      break;
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      P(c, "%s(", T);
      for (unsigned i = 0; i < n; i++) {
         if (i)
            P(c, ", ");
         asrc_n(c, alu, i, t, 1);
      }
      P(c, ")");
      break;
   case nir_op_bcsel:
      P(c, "select(");
      asrc(c, alu, 2);
      P(c, ", ");
      asrc(c, alu, 1);
      P(c, ", ");
      asrc(c, alu, 0);
      P(c, ")");
      break;

   /* float arithmetic */
   case nir_op_fadd: binop(c, alu, "+"); break;
   case nir_op_fsub: binop(c, alu, "-"); break;
   case nir_op_fmul: binop(c, alu, "*"); break;
   case nir_op_fdiv: binop(c, alu, "/"); break;
   case nir_op_frem: binop(c, alu, "%"); break;
   case nir_op_fneg: P(c, "(-"); asrc(c, alu, 0); P(c, ")"); break;
   case nir_op_fabs: call(c, alu, "abs"); break;
   case nir_op_fsat: call(c, alu, "saturate"); break;
   case nir_op_fsign: call(c, alu, "sign"); break;
   case nir_op_ffloor: call(c, alu, "floor"); break;
   case nir_op_fceil: call(c, alu, "ceil"); break;
   case nir_op_ftrunc: call(c, alu, "trunc"); break;
   case nir_op_fround_even: call(c, alu, "round"); break;
   case nir_op_ffract: call(c, alu, "fract"); break;
   case nir_op_fmin: call(c, alu, "min"); break;
   case nir_op_fmax: call(c, alu, "max"); break;
   case nir_op_ffma: call(c, alu, "fma"); break;
   case nir_op_fsqrt: call(c, alu, "sqrt"); break;
   case nir_op_frsq: call(c, alu, "inverseSqrt"); break;
   case nir_op_frcp: P(c, "(%s(1.0) / ", T); asrc(c, alu, 0); P(c, ")"); break;
   case nir_op_fexp2: call(c, alu, "exp2"); break;
   case nir_op_flog2: call(c, alu, "log2"); break;
   case nir_op_fsin: call(c, alu, "sin"); break;
   case nir_op_fcos: call(c, alu, "cos"); break;
   case nir_op_fpow: call(c, alu, "pow"); break;
   case nir_op_fquantize2f16: call(c, alu, "quantizeToF16"); break;
   case nir_op_ldexp: call(c, alu, "ldexp"); break;
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4: call(c, alu, "dot"); break;
   case nir_op_fisfinite:
      P(c, "(abs(");
      asrc(c, alu, 0);
      P(c, ") <= %s(0x1.fffffep+127))", type_str(c, alu_src_want(c, alu, 0), n));
      break;

   /* float comparisons (unordered forms spelled with ordered ones). WGSL lets an implementation
    * assume there are no NaNs, so x != x (SPIR-V's isnan) is spelled as a bit test. */
   case nir_op_feq:
   case nir_op_fneu:
      if (alu_srcs_equal(alu) && d->bit_size == 1) {
         P(c, alu->op == nir_op_fneu ? "(" : "!(");
         emit_isnan(c, alu, 0);
         P(c, ")");
         break;
      }
      /* fneu is true for NaN; spelled !(a == b) it stays so even where != is lowered as an ordered compare */
      if (alu->op == nir_op_fneu) {
         P(c, "!");
         binop(c, alu, "==");
      } else {
         binop(c, alu, "==");
      }
      break;
   case nir_op_flt: binop(c, alu, "<"); break;
   case nir_op_fge: binop(c, alu, ">="); break;
   case nir_op_fltu: P(c, "!"); binop(c, alu, ">="); break;
   case nir_op_fgeu: P(c, "!"); binop(c, alu, "<"); break;
   case nir_op_fequ: P(c, "!("); binop(c, alu, "<"); P(c, " | "); binop(c, alu, ">"); P(c, ")"); break;
   case nir_op_fneo: P(c, "("); binop(c, alu, "<"); P(c, " | "); binop(c, alu, ">"); P(c, ")"); break;
   case nir_op_ford:
      P(c, "((");
      asrc(c, alu, 0); P(c, " == "); asrc(c, alu, 0);
      P(c, ") & (");
      asrc(c, alu, 1); P(c, " == "); asrc(c, alu, 1);
      P(c, "))");
      break;
   case nir_op_funord:
      P(c, "((");
      asrc(c, alu, 0); P(c, " != "); asrc(c, alu, 0);
      P(c, ") | (");
      asrc(c, alu, 1); P(c, " != "); asrc(c, alu, 1);
      P(c, "))");
      break;
   case nir_op_ball_fequal2: case nir_op_ball_fequal3: case nir_op_ball_fequal4:
   case nir_op_ball_iequal2: case nir_op_ball_iequal3: case nir_op_ball_iequal4:
      P(c, "all("); binop(c, alu, "=="); P(c, ")"); break;
   case nir_op_bany_fnequal2: case nir_op_bany_fnequal3: case nir_op_bany_fnequal4:
   case nir_op_bany_inequal2: case nir_op_bany_inequal3: case nir_op_bany_inequal4:
      P(c, "any("); binop(c, alu, "!="); P(c, ")"); break;

   /* integers */
   case nir_op_iadd: binop(c, alu, "+"); break;
   case nir_op_isub: binop(c, alu, "-"); break;
   case nir_op_imul:
   case nir_op_amul: binop(c, alu, "*"); break;
   case nir_op_idiv: binop(c, alu, "/"); break;
   case nir_op_udiv: binop(c, alu, "/"); break;
   case nir_op_irem: binop(c, alu, "%"); break;
   case nir_op_umod: binop(c, alu, "%"); break;
   case nir_op_imod: c->helper_imod = true; per_comp(c, alu, "wgsl_imod", WT_I32); break;
   case nir_op_imul_high: c->helper_imul_high = true; per_comp(c, alu, "wgsl_imul_high", WT_I32); break;
   case nir_op_umul_high: c->helper_umul_high = true; per_comp(c, alu, "wgsl_umul_high", WT_U32); break;
   case nir_op_ineg: P(c, "(-"); asrc(c, alu, 0); P(c, ")"); break;
   case nir_op_iabs: call(c, alu, "abs"); break;
   case nir_op_isign: call(c, alu, "sign"); break;
   case nir_op_imin:
   case nir_op_umin: call(c, alu, "min"); break;
   case nir_op_imax:
   case nir_op_umax: call(c, alu, "max"); break;
   case nir_op_ieq: binop(c, alu, "=="); break;
   case nir_op_ine: binop(c, alu, "!="); break;
   case nir_op_ilt:
   case nir_op_ult: binop(c, alu, "<"); break;
   case nir_op_ige:
   case nir_op_uge: binop(c, alu, ">="); break;
   case nir_op_iand:
      binop(c, alu, "&");
      break;
   case nir_op_ior:
      binop(c, alu, "|");
      break;
   case nir_op_ixor:
      binop(c, alu, d->bit_size == 1 ? "!=" : "^");
      break;
   case nir_op_inot:
      P(c, d->bit_size == 1 ? "(!" : "(~");
      asrc(c, alu, 0);
      P(c, ")");
      break;
   case nir_op_ishl:
   case nir_op_ishr:
   case nir_op_ushr:
      P(c, "(");
      asrc(c, alu, 0);
      P(c, alu->op == nir_op_ishl ? " << (" : " >> (");
      asrc_as(c, alu, 1, WT_U32);
      P(c, " & %s(31u)))", type_str(c, WT_U32, n));
      break;
   case nir_op_urol:
   case nir_op_uror:
      P(c, "((");
      asrc(c, alu, 0);
      P(c, alu->op == nir_op_urol ? " << (" : " >> (");
      asrc_as(c, alu, 1, WT_U32);
      P(c, " & %s(31u))) | (", type_str(c, WT_U32, n));
      asrc(c, alu, 0);
      P(c, alu->op == nir_op_urol ? " >> ((%s(32u) - " : " << ((%s(32u) - ", type_str(c, WT_U32, n));
      asrc_as(c, alu, 1, WT_U32);
      P(c, ") & %s(31u))))", type_str(c, WT_U32, n));
      break;
   case nir_op_bitfield_insert:
      P(c, "insertBits(");
      asrc(c, alu, 0); P(c, ", ");
      asrc(c, alu, 1); P(c, ", ");
      asrc_n(c, alu, 2, WT_U32, 1); P(c, ", ");
      asrc_n(c, alu, 3, WT_U32, 1); P(c, ")");
      if (n > 1)
         fail(c, "alu", "vector bitfield_insert");
      break;
   case nir_op_ubitfield_extract:
   case nir_op_ibitfield_extract:
   case nir_op_ubfe:
   case nir_op_ibfe: {
      bool mask = alu->op == nir_op_ubfe || alu->op == nir_op_ibfe;
      P(c, "extractBits(");
      asrc(c, alu, 0); P(c, ", ");
      asrc_n(c, alu, 1, WT_U32, 1); P(c, mask ? " & 31u, " : ", ");
      asrc_n(c, alu, 2, WT_U32, 1); P(c, mask ? " & 31u)" : ")");
      if (n > 1)
         fail(c, "alu", "vector bitfield_extract");
      break;
   }
   case nir_op_bitfield_reverse: call(c, alu, "reverseBits"); break;
   case nir_op_bit_count: P(c, "bitcast<%s>(countOneBits(", T); asrc(c, alu, 0); P(c, "))"); break;
   case nir_op_ufind_msb: P(c, "bitcast<%s>(firstLeadingBit(", T); asrc(c, alu, 0); P(c, "))"); break;
   case nir_op_ifind_msb: P(c, "bitcast<%s>(firstLeadingBit(", T); asrc(c, alu, 0); P(c, "))"); break;
   case nir_op_find_lsb: P(c, "bitcast<%s>(firstTrailingBit(", T); asrc(c, alu, 0); P(c, "))"); break;
   case nir_op_uclz: P(c, "bitcast<%s>(countLeadingZeros(", T); asrc(c, alu, 0); P(c, "))"); break;

   /* conversions */
   case nir_op_f2i32: case nir_op_f2u32: case nir_op_i2f32: case nir_op_u2f32:
   case nir_op_f2f32: case nir_op_f2f16: case nir_op_f2f16_rtne: case nir_op_f2f16_rtz:
   case nir_op_i2f16: case nir_op_u2f16: case nir_op_i2i32: case nir_op_u2u32:
   case nir_op_b2f32: case nir_op_b2i32: case nir_op_b2f16: case nir_op_f2fmp:
      P(c, "%s(", T);
      asrc(c, alu, 0);
      P(c, ")");
      break;
   case nir_op_b2b1:
   case nir_op_b2b32:
      asrc(c, alu, 0);
      break;
   case nir_op_u2u1:
   case nir_op_i2i1:
      P(c, "(");
      asrc(c, alu, 0);
      P(c, " != %s(0))", type_str(c, alu_src_want(c, alu, 0), n));
      break;

   /* packing */
   case nir_op_pack_half_2x16: call(c, alu, "pack2x16float"); break;
   case nir_op_unpack_half_2x16: call(c, alu, "unpack2x16float"); break;
   case nir_op_pack_unorm_2x16: call(c, alu, "pack2x16unorm"); break;
   case nir_op_pack_snorm_2x16: call(c, alu, "pack2x16snorm"); break;
   case nir_op_unpack_unorm_2x16: call(c, alu, "unpack2x16unorm"); break;
   case nir_op_unpack_snorm_2x16: call(c, alu, "unpack2x16snorm"); break;
   case nir_op_pack_unorm_4x8: call(c, alu, "pack4x8unorm"); break;
   case nir_op_pack_snorm_4x8: call(c, alu, "pack4x8snorm"); break;
   case nir_op_unpack_unorm_4x8: call(c, alu, "unpack4x8unorm"); break;
   case nir_op_unpack_snorm_4x8: call(c, alu, "unpack4x8snorm"); break;
   case nir_op_pack_half_2x16_split:
      P(c, "pack2x16float(vec2<f32>(");
      asrc(c, alu, 0);
      P(c, ", ");
      asrc(c, alu, 1);
      P(c, "))");
      break;
   case nir_op_pack_uvec2_to_uint:
      P(c, "(("); asrc_n(c, alu, 0, WT_U32, 2); P(c, ").x & 0xffffu | (("); asrc_n(c, alu, 0, WT_U32, 2);
      P(c, ").y << 16u))");
      break;
   case nir_op_pack_uvec4_to_uint: {
      P(c, "(");
      for (unsigned k = 0; k < 4; k++) {
         P(c, "%s((", k ? " | " : "");
         asrc_n(c, alu, 0, WT_U32, 4);
         P(c, ").%c & 0xffu) << %uu", swz[k], 8 * k);
      }
      P(c, ")");
      break;
   }
   default:
      fail(c, "alu", "%s", nir_op_infos[alu->op].name);
      P(c, "%s()", T);
      break;
   }
   P(c, ";\n");
}

/* ---------------------------------------------------------------- bindings */

static bool
remap(struct ctx *c, unsigned set, unsigned binding, unsigned index, bool sampler,
      unsigned *group, unsigned *wb)
{
   if (c->o->remap)
      return c->o->remap(c->o->remap_data, set, binding, index, sampler, group, wb);
   if (index != 0)
      return false;
   *group = set;
   *wb = binding + (sampler ? (c->o->combined_sampler_offset ? c->o->combined_sampler_offset : 500) : 0);
   return true;
}

static nir_variable *
find_buffer_var(struct ctx *c, bool ubo, unsigned set, unsigned binding)
{
   nir_foreach_variable_with_modes(var, c->s, ubo ? nir_var_mem_ubo : nir_var_mem_ssbo) {
      if (var->data.descriptor_set == set && var->data.binding == binding)
         return var;
   }
   return NULL;
}

/* The buffer a load_ubo/load_ssbo/store_ssbo/ssbo_atomic index source names. */
static struct buf_slot *
get_buf(struct ctx *c, nir_src *index, bool ubo)
{
   nir_def *d = index->ssa;
   nir_scalar sx = nir_scalar_resolved(d, 0), sy = nir_scalar_resolved(d, 1);
   if (!nir_scalar_is_const(sx)) {
      fail(c, "buffer-index", "buffer descriptor is not a constant (bindless / descriptor indexing)");
      return NULL;
   }
   if (!nir_scalar_is_const(sy)) {
      fail(c, "buffer-array-dynamic", "dynamically indexed array of %s bindings (set %u binding %u)",
           ubo ? "uniform buffer" : "storage buffer", (unsigned)(nir_scalar_as_uint(sx) >> 16), (unsigned)(nir_scalar_as_uint(sx) & 0xffff));
      return NULL;
   }
   unsigned slot = nir_scalar_as_uint(sx), index_v = nir_scalar_as_uint(sy);
   unsigned set = slot >> 16, binding = slot & 0xffff;
   for (unsigned i = 0; i < c->num_bufs; i++) {
      struct buf_slot *b = &c->bufs[i];
      if (b->set == set && b->binding == binding && b->index == index_v && b->ubo == ubo)
         return b;
   }
   if (c->num_bufs == ARRAY_SIZE(c->bufs)) {
      fail(c, "limits", "more than %u buffer bindings", (unsigned)ARRAY_SIZE(c->bufs));
      return NULL;
   }
   struct buf_slot *b = &c->bufs[c->num_bufs++];
   memset(b, 0, sizeof(*b));
   b->set = set;
   b->binding = binding;
   b->index = index_v;
   b->ubo = ubo;
   snprintf(b->name, sizeof(b->name), "%s%u_%u_%u", ubo ? "ub" : "sb", set, binding, index_v);
   nir_variable *var = find_buffer_var(c, ubo, set, binding);
   if (var)
      b->size = glsl_get_explicit_size(glsl_without_array(var->type), false);
   return b;
}

static struct res_var *
get_res(struct ctx *c, nir_variable *var, unsigned index)
{
   for (unsigned i = 0; i < c->num_res; i++)
      if (c->res[i].var == var && c->res[i].index == index)
         return &c->res[i];
   if (c->num_res == ARRAY_SIZE(c->res)) {
      fail(c, "limits", "more than %u texture/sampler/image bindings", (unsigned)ARRAY_SIZE(c->res));
      return NULL;
   }
   struct res_var *r = &c->res[c->num_res++];
   memset(r, 0, sizeof(*r));
   r->var = var;
   r->index = index;
   snprintf(r->tname, sizeof(r->tname), "%s%u_%u_%u", var->data.mode == nir_var_image ? "img" : "tex",
            var->data.descriptor_set, var->data.binding, index);
   snprintf(r->sname, sizeof(r->sname), "smp%u_%u_%u", var->data.descriptor_set, var->data.binding, index);
   return r;
}

/* The texture/sampler/image variable (and array element) behind a deref. */
static struct res_var *
res_of_deref(struct ctx *c, nir_deref_instr *deref)
{
   unsigned index = 0;
   if (deref->deref_type == nir_deref_type_array) {
      if (!nir_src_is_const(deref->arr.index)) {
         nir_variable *v = nir_deref_instr_get_variable(deref);
         fail(c, "binding-array-dynamic", "dynamically indexed array of textures/samplers/images (set %u binding %u)",
              v ? v->data.descriptor_set : 0, v ? v->data.binding : 0);
         return NULL;
      }
      index = nir_src_as_uint(deref->arr.index);
      deref = nir_deref_instr_parent(deref);
   }
   if (deref->deref_type != nir_deref_type_var) {
      fail(c, "deref", "texture/image deref that is not a variable or array element");
      return NULL;
   }
   return get_res(c, deref->var, index);
}

/* ---------------------------------------------------------------- memory derefs */

static const char *
var_name(struct ctx *c, nir_variable *var)
{
   const char *prefix = var->data.mode == nir_var_mem_shared ? "w" :
                        var->data.mode == nir_var_shader_temp ? "g" : "l";
   return ralloc_asprintf(c->mem, "%s%u", prefix, var->index);
}

static void
emit_deref(struct ctx *c, nir_deref_instr *deref)
{
   switch (deref->deref_type) {
   case nir_deref_type_var:
      P(c, "%s", var_name(c, deref->var));
      break;
   case nir_deref_type_array: {
      nir_deref_instr *parent = nir_deref_instr_parent(deref);
      emit_deref(c, parent);
      P(c, "[");
      emit_src_n(c, &deref->arr.index, WT_NONE, 1);
      P(c, "]");
      break;
   }
   case nir_deref_type_struct:
      emit_deref(c, nir_deref_instr_parent(deref));
      P(c, ".f%u", deref->strct.index);
      break;
   default:
      fail(c, "deref", "%s deref",
           deref->deref_type == nir_deref_type_cast ? "cast" :
           deref->deref_type == nir_deref_type_ptr_as_array ? "ptr_as_array" : "wildcard");
      P(c, "?");
   }
}

static unsigned
var_atomic_kind(struct ctx *c, nir_variable *var)
{
   struct hash_entry *e = var ? _mesa_hash_table_search(c->atomic_vars, var) : NULL;
   return e ? (unsigned)(uintptr_t)e->data : 0;
}

static bool
deref_is_atomic_var(struct ctx *c, nir_deref_instr *deref)
{
   return var_atomic_kind(c, nir_deref_instr_get_variable(deref)) != 0;
}

static enum wt
deref_leaf_type(struct ctx *c, nir_deref_instr *deref)
{
   const struct glsl_type *t = deref->type;
   enum wt w = wt_of_glsl_base(glsl_get_base_type(t));
   unsigned kind = var_atomic_kind(c, nir_deref_instr_get_variable(deref));
   if (kind && (w == WT_U32 || w == WT_I32))
      w = kind == 2 ? WT_I32 : WT_U32;
   return w;
}

/* ---------------------------------------------------------------- atomics */

static const char *
atomic_fn(struct ctx *c, nir_atomic_op op)
{
   switch (op) {
   case nir_atomic_op_iadd: return "atomicAdd";
   case nir_atomic_op_imin: case nir_atomic_op_umin: return "atomicMin";
   case nir_atomic_op_imax: case nir_atomic_op_umax: return "atomicMax";
   case nir_atomic_op_iand: return "atomicAnd";
   case nir_atomic_op_ior: return "atomicOr";
   case nir_atomic_op_ixor: return "atomicXor";
   case nir_atomic_op_xchg: return "atomicExchange";
   case nir_atomic_op_cmpxchg: return "atomicCompareExchangeWeak";
   default:
      fail(c, "atomic-op", "atomic op %d (float / wrapping atomics)", op);
      return "atomicAdd";
   }
}

/* `ptr` is printed by the callback. The compare-exchange is strong in SPIR-V and weak in WGSL:
 * retry while the weak one fails spuriously. */
static void
emit_atomic(struct ctx *c, nir_intrinsic_instr *intr, enum wt t, void (*ptr)(struct ctx *, void *),
            void *data, nir_src *val, nir_src *cmp)
{
   nir_atomic_op op = nir_intrinsic_atomic_op(intr);
   const char *fn = atomic_fn(c, op);
   if (op == nir_atomic_op_cmpxchg) {
      /* the value is declared as a var (see type pass) */
      IND(c);
      P(c, "loop {\n");
      c->indent++;
      IND(c);
      P(c, "let r = %s(&", fn);
      ptr(c, data);
      P(c, ", ");
      emit_src(c, cmp, t);
      P(c, ", ");
      emit_src(c, val, t);
      P(c, ");\n");
      IND(c);
      P(c, "if (r.exchanged || r.old_value != ");
      emit_src(c, cmp, t);
      P(c, ") { v%u = r.old_value; break; }\n", intr->def.index);
      c->indent--;
      IND(c);
      P(c, "}\n");
      return;
   }
   def_begin(c, &intr->def);
   P(c, "%s(&", fn);
   ptr(c, data);
   P(c, ", ");
   emit_src(c, val, t);
   P(c, ");\n");
}

static void
ptr_deref(struct ctx *c, void *data)
{
   emit_deref(c, data);
}

struct buf_ptr {
   struct buf_slot *b;
   nir_src *offset;
};

static void
ptr_buf(struct ctx *c, void *data)
{
   struct buf_ptr *p = data;
   P(c, "%s[", p->b->name);
   emit_src_n(c, p->offset, WT_U32, 1);
   P(c, " >> 2u]");
}

/* ---------------------------------------------------------------- buffers */

/* A 16-bit load (f16 only: WGSL has no 16-bit integers): component i is half (o_i >> 1) & 1 of the word at
 * o_i >> 2, o_i = off + 2 i. `word` prints the word expression for a byte-offset expression. */
static void
emit_load16(struct ctx *c, nir_intrinsic_instr *intr, nir_src *off, struct buf_slot *b, bool ubo)
{
   nir_def *d = &intr->def;
   unsigned n = d->num_components;
   if (def_type(c, d) != WT_F16) {
      fail(c, "buffer-16bit", "16-bit integer load (WGSL has no 16-bit integers)");
      return;
   }
   def_begin(c, d);
   if (n > 1)
      P(c, "vec%u<f16>(", n);
   for (unsigned i = 0; i < n; i++) {
      P(c, "%sbitcast<vec2<f16>>(", i ? ", " : "");
      if (ubo) {
         P(c, "%s[(", b->name);
         emit_src_n(c, off, WT_U32, 1);
         P(c, " + %uu) >> 4u][((", 2 * i);
         emit_src_n(c, off, WT_U32, 1);
         P(c, " + %uu) >> 2u) & 3u]", 2 * i);
      } else {
         P(c, "%s%s[(", b->atomic ? "atomicLoad(&" : "", b->name);
         emit_src_n(c, off, WT_U32, 1);
         P(c, " + %uu) >> 2u]%s", 2 * i, b->atomic ? ")" : "");
      }
      P(c, ")[((");
      emit_src_n(c, off, WT_U32, 1);
      P(c, " + %uu) >> 1u) & 1u]", 2 * i);
   }
   P(c, n > 1 ? ");\n" : ";\n");
   if (ubo && nir_src_is_const(*off))
      b->size = MAX2(b->size, (unsigned)nir_src_as_uint(*off) + 2 * n);
}

/* Word i of a load at byte offset `off` from a uniform buffer: ub[w >> 2][w & 3]. */
static void
emit_ubo_load(struct ctx *c, nir_intrinsic_instr *intr, struct buf_slot *b)
{
   unsigned n = intr->def.num_components;
   nir_src *off = &intr->src[1];
   if (intr->def.bit_size == 16) {
      emit_load16(c, intr, off, b, true);
      return;
   }
   if (intr->def.bit_size != 32) {
      fail(c, "buffer-bit-size", "%u-bit uniform buffer load", intr->def.bit_size);
      return;
   }
   def_begin(c, &intr->def);
   if (nir_src_is_const(*off) && (nir_src_as_uint(*off) / 4) % 4 + n <= 4) {
      /* all in one vec4: ub[k] or a swizzle of it */
      unsigned w = nir_src_as_uint(*off) / 4;
      P(c, "%s[%u]", b->name, w / 4);
      if (n < 4)
         P(c, ".%.*s", n, swz + w % 4);
      b->size = MAX2(b->size, (w + n) * 4);
      P(c, ";\n");
      return;
   }
   if (n > 1)
      P(c, "vec%u<u32>(", n);
   if (nir_src_is_const(*off)) {
      unsigned w = nir_src_as_uint(*off) / 4;
      for (unsigned i = 0; i < n; i++)
         P(c, "%s%s[%u][%u]", i ? ", " : "", b->name, (w + i) / 4, (w + i) % 4);
      b->size = MAX2(b->size, (w + n) * 4);
   } else {
      unsigned align_mul = nir_intrinsic_align_mul(intr), align_off = nir_intrinsic_align_offset(intr);
      for (unsigned i = 0; i < n; i++) {
         P(c, "%s", i ? ", " : "");
         if (align_mul >= 16 && (align_off % 4) == 0 && (align_off % 16) / 4 + i < 4) {
            P(c, "%s[", b->name);
            emit_src_n(c, off, WT_U32, 1);
            P(c, " >> 4u][%u]", (align_off % 16) / 4 + i);
         } else {
            P(c, "%s[(", b->name);
            emit_src_n(c, off, WT_U32, 1);
            P(c, " + %uu) >> 4u][((", 4 * i);
            emit_src_n(c, off, WT_U32, 1);
            P(c, " + %uu) >> 2u) & 3u]", 4 * i);
         }
      }
   }
   if (n > 1)
      P(c, ")");
   P(c, ";\n");
}

static void
emit_ssbo_word(struct ctx *c, struct buf_slot *b, nir_src *off, unsigned i)
{
   if (nir_src_is_const(*off)) {
      P(c, "%s[%uu]", b->name, (unsigned)(nir_src_as_uint(*off) / 4 + i));
   } else {
      P(c, "%s[(", b->name);
      emit_src_n(c, off, WT_U32, 1);
      P(c, " >> 2u)%s", i ? "" : "]");
      if (i)
         P(c, " + %uu]", i);
   }
}

static void
emit_ssbo_load(struct ctx *c, nir_intrinsic_instr *intr, struct buf_slot *b)
{
   unsigned n = intr->def.num_components;
   if (intr->def.bit_size == 16) {
      emit_load16(c, intr, &intr->src[1], b, false);
      return;
   }
   if (intr->def.bit_size != 32) {
      fail(c, "buffer-bit-size", "%u-bit storage buffer load", intr->def.bit_size);
      return;
   }
   def_begin(c, &intr->def);
   if (n > 1)
      P(c, "vec%u<u32>(", n);
   for (unsigned i = 0; i < n; i++) {
      P(c, "%s", i ? ", " : "");
      if (b->atomic)
         P(c, "%satomicLoad(&", b->atomic_signed ? "bitcast<u32>(" : "");
      emit_ssbo_word(c, b, &intr->src[1], i);
      if (b->atomic)
         P(c, ")%s", b->atomic_signed ? ")" : "");
   }
   if (n > 1)
      P(c, ")");
   P(c, ";\n");
}

static void
emit_ssbo_store(struct ctx *c, nir_intrinsic_instr *intr, struct buf_slot *b)
{
   unsigned mask = nir_intrinsic_write_mask(intr);
   nir_src *val = &intr->src[0];
   enum wt wt_word = b->atomic && b->atomic_signed ? WT_I32 : WT_U32;
   if (val->ssa->bit_size != 32) {
      fail(c, "buffer-bit-size", "%u-bit storage buffer store (needs a read-modify-write: nir_lower_mem_access_bit_sizes)",
           val->ssa->bit_size);
      return;
   }
   u_foreach_bit(i, mask) {
      IND(c);
      if (b->atomic) {
         P(c, "atomicStore(&");
         emit_ssbo_word(c, b, &intr->src[2], i);
         P(c, ", ");
      } else {
         emit_ssbo_word(c, b, &intr->src[2], i);
         P(c, " = ");
      }
      emit_src_comp(c, val, wt_word, i);
      P(c, b->atomic ? ");\n" : ";\n");
   }
}

/* ---------------------------------------------------------------- I/O */

static int
vs_out_location(gl_varying_slot slot)
{
   if (slot >= VARYING_SLOT_VAR0 && slot < VARYING_SLOT_VAR0 + 32)
      return slot - VARYING_SLOT_VAR0;
   return -1;
}

static const char *
interp_attr(struct ctx *c, struct io_slot *s)
{
   if (s->type != WT_F32 && s->type != WT_F16)
      return "@interpolate(flat) ";
   const char *sampling = s->sampling == 1 ? ", centroid" : s->sampling == 2 ? ", sample" : "";
   switch (s->interp) {
   case INTERP_MODE_FLAT:
      return "@interpolate(flat) ";
   case INTERP_MODE_NOPERSPECTIVE:
      return ralloc_asprintf(c->mem, "@interpolate(linear%s) ", sampling);
   default:
      return ralloc_asprintf(c->mem, "@interpolate(perspective%s) ", sampling);
   }
}

/* ---------------------------------------------------------------- textures and images */

static const char *
storage_format_name(enum pipe_format f)
{
   switch (f) {
   case PIPE_FORMAT_R8G8B8A8_UNORM: return "rgba8unorm";
   case PIPE_FORMAT_R8G8B8A8_SNORM: return "rgba8snorm";
   case PIPE_FORMAT_R8G8B8A8_UINT: return "rgba8uint";
   case PIPE_FORMAT_R8G8B8A8_SINT: return "rgba8sint";
   case PIPE_FORMAT_B8G8R8A8_UNORM: return "bgra8unorm";
   case PIPE_FORMAT_R16G16B16A16_UINT: return "rgba16uint";
   case PIPE_FORMAT_R16G16B16A16_SINT: return "rgba16sint";
   case PIPE_FORMAT_R16G16B16A16_FLOAT: return "rgba16float";
   case PIPE_FORMAT_R32_UINT: return "r32uint";
   case PIPE_FORMAT_R32_SINT: return "r32sint";
   case PIPE_FORMAT_R32_FLOAT: return "r32float";
   case PIPE_FORMAT_R32G32_UINT: return "rg32uint";
   case PIPE_FORMAT_R32G32_SINT: return "rg32sint";
   case PIPE_FORMAT_R32G32_FLOAT: return "rg32float";
   case PIPE_FORMAT_R32G32B32A32_UINT: return "rgba32uint";
   case PIPE_FORMAT_R32G32B32A32_SINT: return "rgba32sint";
   case PIPE_FORMAT_R32G32B32A32_FLOAT: return "rgba32float";
   /* texture_formats_tier1 */
   case PIPE_FORMAT_R8_UNORM: return "r8unorm";
   case PIPE_FORMAT_R8_SNORM: return "r8snorm";
   case PIPE_FORMAT_R8_UINT: return "r8uint";
   case PIPE_FORMAT_R8_SINT: return "r8sint";
   case PIPE_FORMAT_R8G8_UNORM: return "rg8unorm";
   case PIPE_FORMAT_R8G8_SNORM: return "rg8snorm";
   case PIPE_FORMAT_R8G8_UINT: return "rg8uint";
   case PIPE_FORMAT_R8G8_SINT: return "rg8sint";
   case PIPE_FORMAT_R16_UNORM: return "r16unorm";
   case PIPE_FORMAT_R16_SNORM: return "r16snorm";
   case PIPE_FORMAT_R16_UINT: return "r16uint";
   case PIPE_FORMAT_R16_SINT: return "r16sint";
   case PIPE_FORMAT_R16_FLOAT: return "r16float";
   case PIPE_FORMAT_R16G16_UNORM: return "rg16unorm";
   case PIPE_FORMAT_R16G16_SNORM: return "rg16snorm";
   case PIPE_FORMAT_R16G16_UINT: return "rg16uint";
   case PIPE_FORMAT_R16G16_SINT: return "rg16sint";
   case PIPE_FORMAT_R16G16_FLOAT: return "rg16float";
   case PIPE_FORMAT_R16G16B16A16_UNORM: return "rgba16unorm";
   case PIPE_FORMAT_R16G16B16A16_SNORM: return "rgba16snorm";
   case PIPE_FORMAT_R10G10B10A2_UINT: return "rgb10a2uint";
   case PIPE_FORMAT_R10G10B10A2_UNORM: return "rgb10a2unorm";
   case PIPE_FORMAT_R11G11B10_FLOAT: return "rg11b10ufloat";
   default: return NULL;
   }
}

static enum wt
image_texel_type(enum pipe_format f)
{
   if (util_format_is_pure_uint(f))
      return WT_U32;
   if (util_format_is_pure_sint(f))
      return WT_I32;
   return WT_F32;
}

/* Coordinates of an image or texel fetch, split as WGSL wants: (coords, array index). */
static unsigned
dim_coords(enum glsl_sampler_dim dim)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D: return 1;
   case GLSL_SAMPLER_DIM_3D:
   case GLSL_SAMPLER_DIM_CUBE: return 3;
   default: return 2;
   }
}

static void
emit_image_coords(struct ctx *c, nir_src *coord, enum glsl_sampler_dim dim, bool arrayed)
{
   unsigned n = dim_coords(dim);
   emit_src_n(c, coord, WT_I32, n);
   if (arrayed) {
      P(c, ", ");
      emit_src_comp(c, coord, WT_I32, n);
   }
}

static void
emit_image(struct ctx *c, nir_intrinsic_instr *intr)
{
   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   struct res_var *r = res_of_deref(c, deref);
   if (!r)
      return;
   enum glsl_sampler_dim dim = nir_intrinsic_image_dim(intr);
   bool arrayed = nir_intrinsic_image_array(intr);
   if (dim == GLSL_SAMPLER_DIM_CUBE || dim == GLSL_SAMPLER_DIM_MS || dim == GLSL_SAMPLER_DIM_BUF ||
       dim == GLSL_SAMPLER_DIM_SUBPASS || dim == GLSL_SAMPLER_DIM_SUBPASS_MS) {
      fail(c, dim == GLSL_SAMPLER_DIM_BUF ? "texel-buffer" : "storage-image-dim",
           "storage image of dim %d (WebGPU storage textures are 1D/2D/2D-array/3D)", dim);
      return;
   }
   enum pipe_format f = r->var->data.image.format;
   enum wt texel = image_texel_type(f);
   switch (intr->intrinsic) {
   case nir_intrinsic_image_deref_load:
      r->read = true;
      def_begin(c, &intr->def);
      P(c, "textureLoad(%s, ", r->tname);
      emit_image_coords(c, &intr->src[1], dim, arrayed);
      P(c, ")");
      if (intr->def.num_components < 4)
         P(c, ".%.*s", intr->def.num_components, swz);
      P(c, ";\n");
      break;
   case nir_intrinsic_image_deref_store:
      r->written = true;
      IND(c);
      P(c, "textureStore(%s, ", r->tname);
      emit_image_coords(c, &intr->src[1], dim, arrayed);
      P(c, ", ");
      emit_src_n(c, &intr->src[3], texel, 4);
      P(c, ");\n");
      break;
   case nir_intrinsic_image_deref_size: {
      unsigned n = intr->def.num_components;
      def_begin(c, &intr->def);
      unsigned nd = dim_coords(dim);
      if (n == 1 && nd == 1)
         P(c, "textureDimensions(%s)", r->tname);
      else {
         P(c, "vec%u<u32>(textureDimensions(%s)", n, r->tname);
         if (arrayed)
            P(c, ", textureNumLayers(%s)", r->tname);
         P(c, ")");
      }
      P(c, ";\n");
      break;
   }
   default:
      fail(c, "image-op", "%s", nir_intrinsic_infos[intr->intrinsic].name);
   }
}

/* ---------------------------------------------------------------- intrinsics */

static void
emit_sysval(struct ctx *c, nir_intrinsic_instr *intr, const char *expr)
{
   def_begin(c, &intr->def);
   P(c, "%s;\n", expr);
}

static const char *
subgroup_op_name(struct ctx *c, nir_intrinsic_instr *intr)
{
   nir_op op = nir_intrinsic_reduction_op(intr);
   bool reduce = intr->intrinsic == nir_intrinsic_reduce;
   bool incl = intr->intrinsic == nir_intrinsic_inclusive_scan;
   if (reduce && nir_intrinsic_cluster_size(intr) != 0) {
      fail(c, "subgroup", "clustered reduction (cluster size %u)", nir_intrinsic_cluster_size(intr));
      return "subgroupAdd";
   }
   switch (op) {
   case nir_op_iadd: case nir_op_fadd:
      return reduce ? "subgroupAdd" : incl ? "subgroupInclusiveAdd" : "subgroupExclusiveAdd";
   case nir_op_imul: case nir_op_fmul:
      return reduce ? "subgroupMul" : incl ? "subgroupInclusiveMul" : "subgroupExclusiveMul";
   default:
      break;
   }
   if (!reduce) {
      fail(c, "subgroup", "scan with %s", nir_op_infos[op].name);
      return "subgroupAdd";
   }
   switch (op) {
   case nir_op_imin: case nir_op_umin: case nir_op_fmin: return "subgroupMin";
   case nir_op_imax: case nir_op_umax: case nir_op_fmax: return "subgroupMax";
   case nir_op_iand: return "subgroupAnd";
   case nir_op_ior: return "subgroupOr";
   case nir_op_ixor: return "subgroupXor";
   default:
      fail(c, "subgroup", "reduction with %s", nir_op_infos[op].name);
      return "subgroupAdd";
   }
}

static void
emit_barrier(struct ctx *c, nir_intrinsic_instr *intr)
{
   mesa_scope exec = nir_intrinsic_execution_scope(intr);
   nir_variable_mode modes = nir_intrinsic_memory_modes(intr);
   if (c->s->info.stage != MESA_SHADER_COMPUTE || exec < SCOPE_WORKGROUP) {
      /* WGSL has no memory-only barrier and no barriers outside compute. Within one invocation
       * (and for helper-less fragment work) program order already holds. */
      IND(c);
      P(c, "// memory barrier (modes 0x%x) dropped\n", modes);
      return;
   }
   bool any = false;
   if (modes & nir_var_mem_shared) {
      IND(c);
      P(c, "workgroupBarrier();\n");
      any = true;
   }
   if (modes & (nir_var_mem_ssbo | nir_var_mem_global)) {
      IND(c);
      P(c, "storageBarrier();\n");
      any = true;
   }
   if (modes & nir_var_image) {
      IND(c);
      P(c, "textureBarrier();\n");
      any = true;
   }
   if (!any) {
      IND(c);
      P(c, "workgroupBarrier();\n");
   }
}

static void
emit_intrinsic(struct ctx *c, nir_intrinsic_instr *intr)
{
   nir_def *d = &intr->def;
   if (nir_intrinsic_infos[intr->intrinsic].has_dest)
      check_bits(c, d->bit_size, def_type(c, d), nir_intrinsic_infos[intr->intrinsic].name);

   switch (intr->intrinsic) {
   /* --- buffers */
   case nir_intrinsic_load_ubo: {
      struct buf_slot *b = get_buf(c, &intr->src[0], true);
      if (b)
         emit_ubo_load(c, intr, b);
      break;
   }
   case nir_intrinsic_load_ssbo: {
      struct buf_slot *b = get_buf(c, &intr->src[0], false);
      if (b)
         emit_ssbo_load(c, intr, b);
      break;
   }
   case nir_intrinsic_store_ssbo: {
      struct buf_slot *b = get_buf(c, &intr->src[1], false);
      if (b)
         emit_ssbo_store(c, intr, b);
      break;
   }
   case nir_intrinsic_ssbo_atomic:
   case nir_intrinsic_ssbo_atomic_swap: {
      struct buf_slot *b = get_buf(c, &intr->src[0], false);
      if (!b)
         break;
      struct buf_ptr p = { b, &intr->src[1] };
      emit_atomic(c, intr, b->atomic_signed ? WT_I32 : WT_U32, ptr_buf, &p,
                  intr->intrinsic == nir_intrinsic_ssbo_atomic_swap ? &intr->src[3] : &intr->src[2],
                  &intr->src[2]);
      break;
   }
   case nir_intrinsic_get_ssbo_size: {
      struct buf_slot *b = get_buf(c, &intr->src[0], false);
      if (!b)
         break;
      def_begin(c, d);
      P(c, "arrayLength(&%s) * 4u;\n", b->name);
      break;
   }
   case nir_intrinsic_load_push_constant: {
      if (!nir_src_is_const(intr->src[0])) {
         fail(c, "push-constant-dynamic", "push constants read at a dynamic offset (move them to a UBO)");
         break;
      }
      unsigned w = (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[0])) / 4;
      unsigned n = d->num_components;
      if (d->bit_size != 32) {
         fail(c, "buffer-bit-size", "%u-bit push constant load", d->bit_size);
         break;
      }
      c->pc_size = MAX2(c->pc_size, (((w + n) * 4 + 15) & ~15u));
      def_begin(c, d);
      if (n > 1)
         P(c, "vec%u<u32>(", n);
      for (unsigned i = 0; i < n; i++)
         P(c, "%swgsl_pc.m%u[%u]", i ? ", " : "", (w + i) / 4, (w + i) % 4);
      P(c, n > 1 ? ");\n" : ";\n");
      break;
   }

   /* --- variables (function, private, workgroup) */
   case nir_intrinsic_load_deref: {
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
      def_begin(c, d);
      if (deref_is_atomic_var(c, deref) && glsl_type_is_integer(deref->type)) {
         P(c, "atomicLoad(&");
         emit_deref(c, deref);
         P(c, ");\n");
      } else {
         emit_deref(c, deref);
         P(c, ";\n");
      }
      break;
   }
   case nir_intrinsic_store_deref: {
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
      unsigned mask = nir_intrinsic_write_mask(intr);
      unsigned n = glsl_get_vector_elements(deref->type);
      enum wt t = deref_leaf_type(c, deref);
      bool atomic = deref_is_atomic_var(c, deref) && glsl_type_is_integer(deref->type);
      if (!glsl_type_is_vector_or_scalar(deref->type)) {
         fail(c, "deref", "store of a whole array/struct");
         break;
      }
      if (mask == BITFIELD_MASK(n) || n == 1) {
         IND(c);
         if (atomic) {
            P(c, "atomicStore(&");
            emit_deref(c, deref);
            P(c, ", ");
         } else {
            emit_deref(c, deref);
            P(c, " = ");
         }
         emit_src_n(c, &intr->src[1], t, n);
         P(c, atomic ? ");\n" : ";\n");
      } else {
         u_foreach_bit(i, mask) {
            IND(c);
            emit_deref(c, deref);
            P(c, ".%c = ", swz[i]);
            emit_src_comp(c, &intr->src[1], t, i);
            P(c, ";\n");
         }
      }
      break;
   }
   case nir_intrinsic_deref_atomic:
   case nir_intrinsic_deref_atomic_swap: {
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
      emit_atomic(c, intr, deref_leaf_type(c, deref), ptr_deref, deref,
                  intr->intrinsic == nir_intrinsic_deref_atomic_swap ? &intr->src[2] : &intr->src[1],
                  &intr->src[1]);
      break;
   }

   /* --- images */
   case nir_intrinsic_image_deref_load:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_image_deref_size:
      emit_image(c, intr);
      break;
   case nir_intrinsic_image_deref_atomic:
   case nir_intrinsic_image_deref_atomic_swap:
      fail(c, "image-atomic", "image atomics (WebGPU has none: r32 images in a storage buffer)");
      break;
   case nir_intrinsic_image_deref_samples:
      fail(c, "image-op", "image samples query");
      break;

   /* --- shader I/O */
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input: {
      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      unsigned comp = nir_intrinsic_component(intr);
      enum wt t = wt_of_alu_type(nir_intrinsic_dest_type(intr), d->bit_size);
      unsigned n = d->num_components;
      const char *base = NULL;
      int loc = -1;
      if (c->s->info.stage == MESA_SHADER_VERTEX) {
         loc = sem.location - VERT_ATTRIB_GENERIC0;
         base = "a";
      } else if (sem.location == VARYING_SLOT_PRIMITIVE_ID) {
         emit_sysval(c, intr, "wi.prim_id");
         break;
      } else {
         loc = vs_out_location(sem.location);
         base = "v";
      }
      if (loc < 0 || loc >= 32) {
         fail(c, "io", "input slot %s", gl_varying_slot_name_for_stage(sem.location, c->s->info.stage));
         break;
      }
      enum wt have = c->in[loc].type;
      def_begin(c, d);
      if (have != t)
         P(c, "bitcast<%s>(", type_str(c, t, n));
      P(c, "wi.%s%d", base, loc);
      P(c, ".%.*s", n, swz + comp);
      P(c, have != t ? ");\n" : ";\n");
      break;
   }
   case nir_intrinsic_store_output: {
      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      unsigned comp = nir_intrinsic_component(intr);
      unsigned mask = nir_intrinsic_write_mask(intr);
      nir_src *val = &intr->src[0];
      if (c->s->info.stage == MESA_SHADER_VERTEX) {
         if (sem.location == VARYING_SLOT_POS) {
            if (comp == 0 && mask == 0xf) {
               IND(c);
               P(c, "wo.pos = ");
               emit_src_n(c, val, WT_F32, 4);
               P(c, ";\n");
               break;
            }
            u_foreach_bit(i, mask) {
               IND(c);
               P(c, "wo.pos.%c = ", swz[comp + i]);
               emit_src_comp(c, val, WT_F32, i);
               P(c, ";\n");
            }
            break;
         }
         if (sem.location == VARYING_SLOT_PSIZ) {
            IND(c);
            P(c, "// gl_PointSize dropped (WebGPU points are 1 px)\n");
            break;
         }
         if (sem.location == VARYING_SLOT_CLIP_DIST0 || sem.location == VARYING_SLOT_CLIP_DIST1) {
            unsigned base = (sem.location - VARYING_SLOT_CLIP_DIST0) * 4 + comp;
            if (c->s->info.cull_distance_array_size)
               fail(c, "cull-distance", "gl_CullDistance (WebGPU has clip distances only)");
            u_foreach_bit(i, mask) {
               if (base + i >= c->s->info.clip_distance_array_size)
                  continue;
               IND(c);
               P(c, "wo.clip[%u] = ", base + i);
               emit_src_comp(c, val, WT_F32, i);
               P(c, ";\n");
            }
            break;
         }
         int loc = vs_out_location(sem.location);
         if (loc < 0) {
            fail(c, "io", "vertex output %s", gl_varying_slot_name_for_stage(sem.location, MESA_SHADER_VERTEX));
            break;
         }
         if (comp == 0 && mask == 0xf) {
            IND(c);
            P(c, "wo.v%d = ", loc);
            emit_src_n(c, val, c->out_slots[loc].type, 4);
            P(c, ";\n");
            break;
         }
         u_foreach_bit(i, mask) {
            IND(c);
            P(c, "wo.v%d.%c = ", loc, swz[comp + i]);
            emit_src_comp(c, val, c->out_slots[loc].type, i);
            P(c, ";\n");
         }
      } else if (c->s->info.stage == MESA_SHADER_FRAGMENT) {
         if (sem.location == FRAG_RESULT_DEPTH) {
            IND(c);
            P(c, "wo.depth = ");
            emit_src_comp(c, val, WT_F32, 0);
            P(c, ";\n");
         } else if (sem.location == FRAG_RESULT_SAMPLE_MASK) {
            IND(c);
            P(c, "wo.mask = ");
            emit_src_comp(c, val, WT_U32, 0);
            P(c, ";\n");
         } else if (sem.location >= FRAG_RESULT_DATA0 && sem.location < FRAG_RESULT_DATA0 + 8) {
            int loc = sem.location - FRAG_RESULT_DATA0;
            const char *suffix = sem.dual_source_blend_index ? "_1" : "";
            if (comp == 0 && mask == 0xf) {
               IND(c);
               P(c, "wo.c%d%s = ", loc, suffix);
               emit_src_n(c, val, c->out_slots[loc + (sem.dual_source_blend_index ? 8 : 0)].type, 4);
               P(c, ";\n");
               break;
            }
            u_foreach_bit(i, mask) {
               IND(c);
               P(c, "wo.c%d%s.%c = ", loc, suffix, swz[comp + i]);
               emit_src_comp(c, val, c->out_slots[loc + (sem.dual_source_blend_index ? 8 : 0)].type, i);
               P(c, ";\n");
            }
         } else {
            fail(c, "io", "fragment output %s", gl_frag_result_name(sem.location));
         }
      } else {
         fail(c, "stage", "outputs in stage %s", mesa_shader_stage_name(c->s->info.stage));
      }
      break;
   }
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_sample:
      break; /* read by load_interpolated_input */
   case nir_intrinsic_load_barycentric_at_offset:
   case nir_intrinsic_load_barycentric_at_sample:
      fail(c, "interpolate-at", "interpolateAtOffset/AtSample");
      break;

   /* --- system values */
   case nir_intrinsic_load_vertex_id: emit_sysval(c, intr, "wi.vertex_index"); break;
   case nir_intrinsic_load_instance_id: emit_sysval(c, intr, "wi.instance_index"); break;
   case nir_intrinsic_load_base_vertex:
   case nir_intrinsic_load_first_vertex:
   case nir_intrinsic_load_base_instance:
   case nir_intrinsic_load_draw_id:
      fail(c, "draw-params", "%s (no WGSL builtin: pass through immediates)", nir_intrinsic_infos[intr->intrinsic].name);
      break;
   case nir_intrinsic_load_frag_coord: emit_sysval(c, intr, "wi.frag_coord"); break;
   case nir_intrinsic_load_front_face: emit_sysval(c, intr, "wi.front_facing"); break;
   case nir_intrinsic_load_sample_id: emit_sysval(c, intr, "wi.sample_index"); break;
   case nir_intrinsic_load_sample_mask_in: emit_sysval(c, intr, "wi.sample_mask"); break;
   case nir_intrinsic_load_primitive_id: emit_sysval(c, intr, "wi.prim_id"); break;
   case nir_intrinsic_load_point_coord:
      fail(c, "point-coord", "gl_PointCoord (WebGPU points are 1 px; lower to a quad)");
      break;
   case nir_intrinsic_load_sample_pos:
      fail(c, "sample-pos", "gl_SamplePosition");
      break;
   case nir_intrinsic_load_helper_invocation:
   case nir_intrinsic_is_helper_invocation:
      fail(c, "helper-invocation", "gl_HelperInvocation");
      break;
   case nir_intrinsic_load_local_invocation_id: emit_sysval(c, intr, "lid"); break;
   case nir_intrinsic_load_local_invocation_index: emit_sysval(c, intr, "lidx"); break;
   case nir_intrinsic_load_global_invocation_id: emit_sysval(c, intr, "gid"); break;
   case nir_intrinsic_load_workgroup_id: emit_sysval(c, intr, "wgid"); break;
   case nir_intrinsic_load_num_workgroups: emit_sysval(c, intr, "nwg"); break;
   case nir_intrinsic_load_workgroup_size:
      def_begin(c, d);
      P(c, "vec3<u32>(%uu, %uu, %uu);\n", c->s->info.workgroup_size[0], c->s->info.workgroup_size[1],
        c->s->info.workgroup_size[2]);
      break;
   case nir_intrinsic_load_subgroup_size:
      emit_sysval(c, intr, c->s->info.stage == MESA_SHADER_COMPUTE ? "sg_size" : "wi.sg_size");
      break;
   case nir_intrinsic_load_subgroup_invocation:
      emit_sysval(c, intr, c->s->info.stage == MESA_SHADER_COMPUTE ? "sg_inv" : "wi.sg_inv");
      break;
   case nir_intrinsic_ballot_bitfield_extract:
      def_begin(c, d);
      P(c, "((");
      emit_src(c, &intr->src[0], WT_U32);
      P(c, ")[");
      emit_src(c, &intr->src[1], WT_U32);
      P(c, " >> 5u] >> (");
      emit_src(c, &intr->src[1], WT_U32);
      P(c, " & 31u) & 1u) != 0u;\n");
      break;
   case nir_intrinsic_ballot_bit_count_reduce:
      def_begin(c, d);
      P(c, "dot(countOneBits(");
      emit_src(c, &intr->src[0], WT_U32);
      P(c, "), vec4<u32>(1u));\n");
      break;
   case nir_intrinsic_ballot_bit_count_inclusive:
   case nir_intrinsic_ballot_bit_count_exclusive:
      c->helper_ballot = true;
      def_begin(c, d);
      P(c, "wgsl_ballot_count_below(");
      emit_src(c, &intr->src[0], WT_U32);
      P(c, ", %s%s);\n", c->s->info.stage == MESA_SHADER_COMPUTE ? "sg_inv" : "wi.sg_inv",
        intr->intrinsic == nir_intrinsic_ballot_bit_count_inclusive ? " + 1u" : "");
      break;
   case nir_intrinsic_ballot_find_lsb:
   case nir_intrinsic_ballot_find_msb:
      c->helper_ballot = true;
      def_begin(c, d);
      P(c, "%s(", intr->intrinsic == nir_intrinsic_ballot_find_lsb ? "wgsl_ballot_lsb" : "wgsl_ballot_msb");
      emit_src(c, &intr->src[0], WT_U32);
      P(c, ");\n");
      break;
   case nir_intrinsic_load_num_subgroups: emit_sysval(c, intr, "sg_num"); break;
   case nir_intrinsic_load_subgroup_id: emit_sysval(c, intr, "sg_id"); break;

   /* --- fragment */
   case nir_intrinsic_demote:
   case nir_intrinsic_terminate:
      IND(c);
      P(c, "discard;\n");
      break;
   case nir_intrinsic_demote_if:
   case nir_intrinsic_terminate_if:
      IND(c);
      P(c, "if (");
      emit_src(c, &intr->src[0], WT_BOOL);
      P(c, ") { discard; }\n");
      break;
   case nir_intrinsic_ddx: case nir_intrinsic_ddx_fine: case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy: case nir_intrinsic_ddy_fine: case nir_intrinsic_ddy_coarse: {
      static const char *names[] = { "dpdx", "dpdxFine", "dpdxCoarse", "dpdy", "dpdyFine", "dpdyCoarse" };
      unsigned k = intr->intrinsic == nir_intrinsic_ddx ? 0 : intr->intrinsic == nir_intrinsic_ddx_fine ? 1 :
                   intr->intrinsic == nir_intrinsic_ddx_coarse ? 2 : intr->intrinsic == nir_intrinsic_ddy ? 3 :
                   intr->intrinsic == nir_intrinsic_ddy_fine ? 4 : 5;
      if (c->s->info.stage != MESA_SHADER_FRAGMENT)
         fail(c, "derivative-stage", "derivatives outside the fragment stage");
      c->uses_derivatives = true;
      def_begin(c, d);
      P(c, "%s(", names[k]);
      emit_src(c, &intr->src[0], def_type(c, d));
      P(c, ");\n");
      break;
   }

   /* --- barriers */
   case nir_intrinsic_barrier:
      emit_barrier(c, intr);
      break;

   /* --- subgroups */
   case nir_intrinsic_ballot:
      def_begin(c, d);
      P(c, "subgroupBallot(");
      emit_src(c, &intr->src[0], WT_BOOL);
      P(c, ");\n");
      if (d->num_components != 4)
         fail(c, "subgroup", "ballot that is not a vec4");
      break;
   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_all:
      def_begin(c, d);
      P(c, "%s(", intr->intrinsic == nir_intrinsic_vote_any ? "subgroupAny" : "subgroupAll");
      emit_src(c, &intr->src[0], WT_BOOL);
      P(c, ");\n");
      break;
   case nir_intrinsic_elect:
      emit_sysval(c, intr, "subgroupElect()");
      break;
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_read_invocation:
   case nir_intrinsic_shuffle:
   case nir_intrinsic_shuffle_xor:
   case nir_intrinsic_shuffle_up:
   case nir_intrinsic_shuffle_down:
   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan:
   case nir_intrinsic_exclusive_scan:
   case nir_intrinsic_quad_broadcast:
   case nir_intrinsic_quad_swap_horizontal:
   case nir_intrinsic_quad_swap_vertical:
   case nir_intrinsic_quad_swap_diagonal: {
      const char *fn;
      bool second = true;
      switch (intr->intrinsic) {
      case nir_intrinsic_read_first_invocation: fn = "subgroupBroadcastFirst"; second = false; break;
      case nir_intrinsic_read_invocation:
      case nir_intrinsic_shuffle: fn = "subgroupShuffle"; break;
      case nir_intrinsic_shuffle_xor: fn = "subgroupShuffleXor"; break;
      case nir_intrinsic_shuffle_up: fn = "subgroupShuffleUp"; break;
      case nir_intrinsic_shuffle_down: fn = "subgroupShuffleDown"; break;
      case nir_intrinsic_quad_broadcast: fn = "quadBroadcast"; break;
      case nir_intrinsic_quad_swap_horizontal: fn = "quadSwapX"; second = false; break;
      case nir_intrinsic_quad_swap_vertical: fn = "quadSwapY"; second = false; break;
      case nir_intrinsic_quad_swap_diagonal: fn = "quadSwapDiagonal"; second = false; break;
      default: fn = subgroup_op_name(c, intr); second = false; break;
      }
      enum wt t = def_type(c, d);
      if (t == WT_BOOL) {
         def_begin(c, d);
         P(c, "(%s(%s(", fn, type_str(c, WT_U32, d->num_components));
         emit_src(c, &intr->src[0], WT_BOOL);
         P(c, ")");
      } else {
         def_begin(c, d);
         P(c, "%s(", fn);
         emit_src(c, &intr->src[0], t);
      }
      if (second) {
         P(c, ", ");
         emit_src(c, &intr->src[1], WT_U32);
      }
      P(c, t == WT_BOOL ? ") != %s(0u));\n" : ");\n", type_str(c, WT_U32, d->num_components));
      break;
   }

   default:
      fail(c, "intrinsic", "%s", nir_intrinsic_infos[intr->intrinsic].name);
      break;
   }
}

/* ---------------------------------------------------------------- texture instructions */

static nir_src *
tex_src(nir_tex_instr *tex, nir_tex_src_type type)
{
   int i = nir_tex_instr_src_index(tex, type);
   return i >= 0 ? &tex->src[i].src : NULL;
}

static struct res_var *
tex_res(struct ctx *c, nir_tex_instr *tex, bool sampler)
{
   nir_src *s = tex_src(tex, sampler ? nir_tex_src_sampler_deref : nir_tex_src_texture_deref);
   if (!s) {
      if (sampler)
         return NULL;
      fail(c, "texture-index", "texture without a deref (bindless or lowered index)");
      return NULL;
   }
   return res_of_deref(c, nir_src_as_deref(*s));
}

/* Float coordinates + array index (rounded as GLSL does). */
static void
emit_tex_coords(struct ctx *c, nir_tex_instr *tex, nir_src *coord, bool integer)
{
   unsigned n = tex->coord_components - (tex->is_array ? 1 : 0);
   emit_src_n(c, coord, integer ? WT_I32 : WT_F32, n);
   if (tex->is_array) {
      P(c, ", ");
      if (integer) {
         emit_src_comp(c, coord, WT_I32, n);
      } else {
         P(c, "i32(floor(");
         emit_src_comp(c, coord, WT_F32, n);
         P(c, " + 0.5f))");
      }
   }
}

static void
emit_tex_offset(struct ctx *c, nir_tex_instr *tex)
{
   nir_src *off = tex_src(tex, nir_tex_src_offset);
   if (!off)
      return;
   unsigned n = off->ssa->num_components;
   for (unsigned i = 0; i < n; i++) {
      int v = nir_src_comp_as_int(*off, i);
      if (v < -8 || v > 7)
         fail(c, "tex-offset", "texel offset %d outside WGSL's -8..7", v);
   }
   P(c, ", ");
   emit_src(c, off, WT_I32);
}

static void
emit_tex(struct ctx *c, nir_tex_instr *tex)
{
   nir_def *d = &tex->def;
   struct res_var *tr = tex_res(c, tex, false);
   struct res_var *sr = tex_res(c, tex, true);
   if (!tr)
      return;
   check_bits(c, d->bit_size, def_type(c, d), "texture result");
   enum wt rt = wt_of_alu_type(tex->dest_type, 32);
   tr->tex_used = true;
   if (tr->var->data.mode != nir_var_uniform)
      fail(c, "texture", "texture op on a non-uniform variable");
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_BUF) {
      fail(c, "texel-buffer", "texel buffer (lower to a storage buffer)");
      return;
   }
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_SUBPASS || tex->sampler_dim == GLSL_SAMPLER_DIM_SUBPASS_MS) {
      fail(c, "input-attachment", "subpass input");
      return;
   }
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_RECT || tex->sampler_dim == GLSL_SAMPLER_DIM_EXTERNAL) {
      fail(c, "texture-dim", "rect/external texture");
      return;
   }
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_MS && tex->is_array) {
      fail(c, "texture-dim", "multisampled array texture");
      return;
   }
   if (tex->is_sparse) {
      fail(c, "sparse", "sparse residency");
      return;
   }
   bool size_query = tex->op == nir_texop_txs || tex->op == nir_texop_query_levels ||
                     tex->op == nir_texop_texture_samples;
   bool cast_result = false;
   if (!size_query) {
      /* SPIR-V lets the signedness of an integer texture differ between reads */
      if (tr->sample == WT_NONE)
         tr->sample = rt;
      else if (tr->sample != rt && (tr->sample == WT_F32 || rt == WT_F32))
         fail(c, "texture-type", "texture read as both float and integer");
      else if (tr->sample != rt)
         cast_result = true;
   }
   if (tex->is_shadow) {
      tr->shadow = true;
      if (sr)
         sr->shadow = true;
   }
   if (tex->sampler_dim == GLSL_SAMPLER_DIM_MS)
      tr->ms = true;

   nir_src *coord = tex_src(tex, nir_tex_src_coord);
   nir_src *lod = tex_src(tex, nir_tex_src_lod);
   nir_src *bias = tex_src(tex, nir_tex_src_bias);
   nir_src *cmp = tex_src(tex, nir_tex_src_comparator);
   nir_src *ddx = tex_src(tex, nir_tex_src_ddx), *ddy = tex_src(tex, nir_tex_src_ddy);
   if (tex_src(tex, nir_tex_src_min_lod))
      fail(c, "tex-min-lod", "minimum LOD clamp (sparse / ARB_sparse_texture_clamp)");
   if (tex_src(tex, nir_tex_src_texture_offset) || tex_src(tex, nir_tex_src_sampler_offset))
      fail(c, "binding-array-dynamic", "texture/sampler index offset");

   bool shadow = tex->is_shadow;
   bool sampling = tex->op == nir_texop_tex || tex->op == nir_texop_txb || tex->op == nir_texop_txl ||
                   tex->op == nir_texop_txd || tex->op == nir_texop_tg4;
   if (sampling) {
      if (!sr) {
         fail(c, "sampler", "sampling without a sampler");
         return;
      }
      sr->smp_used = true;
      if (rt != WT_F32 && tex->op != nir_texop_tg4) {
         fail(c, "int-texture-sample", "sampling an integer texture with a sampler (WGSL samples only float textures)");
         return;
      }
   }
   /* components the WGSL call returns: 4, 1 for depth compares, or exactly the NIR value (queries) */
   /* a depth texture (compared somewhere in the shader) returns f32 from sample/load, vec4 from gather */
   bool depth_tex = tr->shadow;
   if (depth_tex && !shadow && sr && sr->shadow && sampling)
      fail(c, "shadow-sample", "one sampler used both with and without depth comparison");
   if (depth_tex && !shadow && (tex->op == nir_texop_txb || tex->op == nir_texop_txd))
      fail(c, "shadow-sample", "bias/gradient sampling of a depth texture (WGSL has neither)");
   unsigned n_out = size_query ? d->num_components : (shadow || depth_tex) && tex->op != nir_texop_tg4 ? 1 : 4;
   def_begin(c, d);
   bool splat = n_out == 1 && d->num_components > 1;
   if (splat)
      P(c, "%s(", type_str(c, def_type(c, d), d->num_components));
   if (cast_result)
      P(c, "bitcast<vec4<%s>>(", wt_name[rt]);
   switch (tex->op) {
   case nir_texop_tex:
   case nir_texop_txb:
   case nir_texop_txl:
   case nir_texop_txd: {
      const char *fn;
      if (shadow) {
         if (tex->op == nir_texop_txb || tex->op == nir_texop_txd)
            fail(c, "shadow-sample", "shadow sampling with %s (WGSL has textureSampleCompare and ...Level(0) only)",
                 tex->op == nir_texop_txb ? "bias" : "gradients");
         if (tex->op == nir_texop_txl && !(nir_src_is_const(*lod) && nir_src_as_float(*lod) == 0.0))
            fail(c, "shadow-sample", "shadow sampling at a non-zero LOD");
         fn = tex->op == nir_texop_tex && c->s->info.stage == MESA_SHADER_FRAGMENT ? "textureSampleCompare" :
                                                                                      "textureSampleCompareLevel";
      } else {
         fn = tex->op == nir_texop_tex ? "textureSample" : tex->op == nir_texop_txb ? "textureSampleBias" :
              tex->op == nir_texop_txl ? "textureSampleLevel" : "textureSampleGrad";
      }
      if ((tex->op == nir_texop_tex || tex->op == nir_texop_txb) && c->s->info.stage != MESA_SHADER_FRAGMENT)
         fail(c, "implicit-lod", "implicit-LOD sampling outside the fragment stage");
      P(c, "%s(%s, %s, ", fn, tr->tname, sr->sname);
      emit_tex_coords(c, tex, coord, false);
      if (shadow) {
         P(c, ", ");
         emit_src_n(c, cmp, WT_F32, 1);
      } else if (tex->op == nir_texop_txb) {
         P(c, ", ");
         emit_src_n(c, bias, WT_F32, 1);
      } else if (tex->op == nir_texop_txl && depth_tex) {
         P(c, ", i32(");
         emit_src_n(c, lod, WT_F32, 1);
         P(c, ")");
      } else if (tex->op == nir_texop_txl) {
         P(c, ", ");
         emit_src_n(c, lod, WT_F32, 1);
      } else if (tex->op == nir_texop_txd) {
         P(c, ", ");
         emit_src(c, ddx, WT_F32);
         P(c, ", ");
         emit_src(c, ddy, WT_F32);
      }
      if (tex->sampler_dim != GLSL_SAMPLER_DIM_CUBE && tex->sampler_dim != GLSL_SAMPLER_DIM_3D)
         emit_tex_offset(c, tex);
      else if (tex_src(tex, nir_tex_src_offset))
         fail(c, "tex-offset", "offset on a cube texture");
      P(c, ")");
      break;
   }
   case nir_texop_tg4:
      if (shadow) {
         P(c, "textureGatherCompare(%s, %s, ", tr->tname, sr->sname);
         emit_tex_coords(c, tex, coord, false);
         P(c, ", ");
         emit_src_n(c, cmp, WT_F32, 1);
      } else {
         if (depth_tex)
            P(c, "textureGather(%s, %s, ", tr->tname, sr->sname);
         else
            P(c, "textureGather(%u, %s, %s, ", tex->component, tr->tname, sr->sname);
         emit_tex_coords(c, tex, coord, false);
      }
      emit_tex_offset(c, tex);
      P(c, ")");
      break;
   case nir_texop_txf:
   case nir_texop_txf_ms: {
      P(c, "textureLoad(%s, ", tr->tname);
      emit_tex_coords(c, tex, coord, true);
      P(c, ", ");
      if (tex->op == nir_texop_txf_ms)
         emit_src_n(c, tex_src(tex, nir_tex_src_ms_index), WT_I32, 1);
      else if (lod)
         emit_src_n(c, lod, WT_I32, 1);
      else
         P(c, "0");
      P(c, ")");
      if (tex_src(tex, nir_tex_src_offset))
         fail(c, "tex-offset", "texel fetch with offset");
      break;
   }
   case nir_texop_txs: {
      unsigned n = d->num_components;
      unsigned nd = n - (tex->is_array ? 1 : 0);
      bool ms = tex->sampler_dim == GLSL_SAMPLER_DIM_MS;
      if (n > 1)
         P(c, "vec%u<u32>(", n);
      P(c, "textureDimensions(%s", tr->tname);
      if (lod && !ms) {
         P(c, ", ");
         emit_src_n(c, lod, WT_U32, 1);
      }
      P(c, ")");
      if (nd == 1 && dim_coords(tex->sampler_dim) > 1)
         P(c, ".x");
      if (tex->is_array)
         P(c, ", textureNumLayers(%s)", tr->tname);
      if (n > 1)
         P(c, ")");
      break;
   }
   case nir_texop_query_levels:
      P(c, "textureNumLevels(%s)", tr->tname);
      break;
   case nir_texop_texture_samples:
      P(c, "textureNumSamples(%s)", tr->tname);
      break;
   case nir_texop_lod:
      fail(c, "query-lod", "textureQueryLod (no WGSL equivalent)");
      P(c, "vec4<f32>()");
      break;
   default:
      fail(c, "tex-op", "texture op %d", tex->op);
      P(c, "vec4<f32>()");
      break;
   }
   if (cast_result)
      P(c, ")");
   if (splat)
      P(c, ")");
   else if (n_out == 4 && d->num_components < 4)
      P(c, ".%.*s", d->num_components, swz);
   P(c, ";\n");
}

/* ---------------------------------------------------------------- the scan before emission */

static enum wt
known_type(struct ctx *c, nir_def *d)
{
   if (d->bit_size == 1)
      return WT_BOOL;
   nir_instr_type it = nir_def_instr(d)->type;
   if (it == nir_instr_type_load_const || it == nir_instr_type_undef)
      return WT_NONE;
   return c->type[d->index];
}

/* mov / vecN / bcsel take the type their sources agree on, u32 when they do not. */
static enum wt
passthrough_type(struct ctx *c, nir_alu_instr *alu)
{
   enum wt t = WT_NONE;
   unsigned first = alu->op == nir_op_bcsel ? 1 : 0;
   for (unsigned i = first; i < nir_op_infos[alu->op].num_inputs; i++) {
      enum wt s = known_type(c, alu->src[i].src.ssa);
      if (s == WT_NONE)
         continue;
      if (t == WT_NONE)
         t = s;
      else if (t != s)
         return alu->def.bit_size == 16 ? WT_F16 : WT_U32;
   }
   return t;
}

static enum wt
intrinsic_type(struct ctx *c, nir_intrinsic_instr *intr)
{
   nir_def *d = &intr->def;
   switch (intr->intrinsic) {
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
      return wt_of_alu_type(nir_intrinsic_dest_type(intr), d->bit_size);
   case nir_intrinsic_load_frag_coord:
      return WT_F32;
   case nir_intrinsic_load_front_face:
   case nir_intrinsic_vote_any:
   case nir_intrinsic_vote_all:
   case nir_intrinsic_elect:
      return WT_BOOL;
   case nir_intrinsic_load_deref: {
      nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
      return deref_leaf_type(c, deref);
   }
   case nir_intrinsic_deref_atomic:
   case nir_intrinsic_deref_atomic_swap:
      return deref_leaf_type(c, nir_src_as_deref(intr->src[0]));
   case nir_intrinsic_ddx: case nir_intrinsic_ddx_fine: case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy: case nir_intrinsic_ddy_fine: case nir_intrinsic_ddy_coarse:
      return d->bit_size == 16 ? WT_F16 : WT_F32;
   case nir_intrinsic_image_deref_load: {
      nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
      return var ? image_texel_type(var->data.image.format) : WT_F32;
   }
   case nir_intrinsic_read_first_invocation:
   case nir_intrinsic_read_invocation:
   case nir_intrinsic_shuffle:
   case nir_intrinsic_shuffle_xor:
   case nir_intrinsic_shuffle_up:
   case nir_intrinsic_shuffle_down:
   case nir_intrinsic_reduce:
   case nir_intrinsic_inclusive_scan:
   case nir_intrinsic_exclusive_scan:
   case nir_intrinsic_quad_broadcast:
   case nir_intrinsic_quad_swap_horizontal:
   case nir_intrinsic_quad_swap_vertical:
   case nir_intrinsic_quad_swap_diagonal: {
      if (intr->intrinsic == nir_intrinsic_reduce || intr->intrinsic == nir_intrinsic_inclusive_scan ||
          intr->intrinsic == nir_intrinsic_exclusive_scan) {
         nir_alu_type t = nir_op_infos[nir_intrinsic_reduction_op(intr)].output_type;
         return wt_of_alu_type(t, d->bit_size);
      }
      enum wt s = known_type(c, intr->src[0].ssa);
      return s != WT_NONE ? s : d->bit_size == 16 ? WT_F16 : WT_U32;
   }
   default:
      return d->bit_size == 16 ? WT_F16 : WT_U32;
   }
}

static void
note_io(struct ctx *c, nir_intrinsic_instr *intr)
{
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   mesa_shader_stage st = c->s->info.stage;
   if (intr->intrinsic == nir_intrinsic_store_output) {
      enum wt t = wt_of_alu_type(nir_intrinsic_src_type(intr), intr->src[0].ssa->bit_size);
      int loc = -1;
      if (st == MESA_SHADER_VERTEX) {
         if (sem.location == VARYING_SLOT_POS)
            c->out_pos = true;
         else if (sem.location == VARYING_SLOT_CLIP_DIST0 || sem.location == VARYING_SLOT_CLIP_DIST1)
            c->out_clip = MAX2(c->out_clip, c->s->info.clip_distance_array_size);
         else
            loc = vs_out_location(sem.location);
      } else if (st == MESA_SHADER_FRAGMENT) {
         if (sem.location == FRAG_RESULT_DEPTH)
            c->out_depth = true;
         else if (sem.location == FRAG_RESULT_SAMPLE_MASK)
            c->out_sample_mask = true;
         else if (sem.location >= FRAG_RESULT_DATA0 && sem.location < FRAG_RESULT_DATA0 + 8) {
            loc = sem.location - FRAG_RESULT_DATA0;
            if (sem.dual_source_blend_index) {
               loc += 8;
               c->uses_dual_src = true;
            }
         }
      }
      if (loc >= 0 && loc < 64) {
         struct io_slot *s = &c->out_slots[loc];
         if (!s->used) {
            static const enum wt key_types[] = { WT_NONE, WT_F32, WT_I32, WT_U32, WT_F16 };
            s->used = true;
            s->type = t;
            if (st == MESA_SHADER_VERTEX && loc < WGSL_MAX_VARYINGS && c->o->varying_type[loc] &&
                c->o->varying_type[loc] < 5)
               s->type = key_types[c->o->varying_type[loc]];
            if (st == MESA_SHADER_FRAGMENT && loc < 8 && c->o->fs_out_type[loc] && c->o->fs_out_type[loc] < 4)
               s->type = key_types[c->o->fs_out_type[loc]];
            s->interp = -1;
         }
         s->comps |= nir_intrinsic_write_mask(intr) << nir_intrinsic_component(intr);
      }
      return;
   }
   /* inputs */
   int loc;
   if (st == MESA_SHADER_VERTEX)
      loc = sem.location - VERT_ATTRIB_GENERIC0;
   else if (sem.location == VARYING_SLOT_PRIMITIVE_ID) {
      c->in_prim_id = true;
      return;
   } else
      loc = vs_out_location(sem.location);
   if (loc < 0 || loc >= 32)
      return;
   struct io_slot *s = &c->in[loc];
   enum wt t = wt_of_alu_type(nir_intrinsic_dest_type(intr), intr->def.bit_size);
   int interp = INTERP_MODE_FLAT, sampling = 0;
   if (intr->intrinsic == nir_intrinsic_load_interpolated_input) {
      nir_intrinsic_instr *bary = nir_src_as_intrinsic(intr->src[0]);
      if (bary) {
         interp = nir_intrinsic_interp_mode(bary);
         sampling = bary->intrinsic == nir_intrinsic_load_barycentric_centroid ? 1 :
                    bary->intrinsic == nir_intrinsic_load_barycentric_sample ? 2 : 0;
      }
   }
   if (!s->used) {
      s->used = true;
      s->type = t;
      s->interp = interp;
      s->sampling = sampling;
   } else if (st == MESA_SHADER_FRAGMENT && (s->interp != interp || s->sampling != sampling)) {
      fail(c, "mixed-interpolation", "location %d read with two interpolations", loc);
   }
}

static void
note_atomic_var(struct ctx *c, nir_intrinsic_instr *intr)
{
   nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));
   if (!var)
      return;
   nir_atomic_op op = nir_intrinsic_atomic_op(intr);
   unsigned kind = var_atomic_kind(c, var);
   bool sgn = op == nir_atomic_op_imin || op == nir_atomic_op_imax;
   bool usgn = op == nir_atomic_op_umin || op == nir_atomic_op_umax;
   unsigned want = sgn ? 2 : 1;
   if (kind == 2 && usgn)
      fail(c, "atomic-sign", "signed and unsigned min/max atomics on one variable");
   if (kind == 0 || want == 2)
      _mesa_hash_table_insert(c->atomic_vars, var, (void *)(uintptr_t)want);
}

static void
scan_instr(struct ctx *c, nir_instr *instr, bool first_pass)
{
   switch (instr->type) {
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      enum wt t;
      if (alu_is_passthrough(alu->op))
         t = passthrough_type(c, alu);
      else
         t = wt_of_alu_type(nir_op_infos[alu->op].output_type, alu->def.bit_size);
      c->type[alu->def.index] = t;
      break;
   }
   case nir_instr_type_tex: {
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      if (first_pass && tex->is_shadow) {
         /* a texture compared anywhere is a texture_depth_* everywhere */
         int ti = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
         int si = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);
         struct res_var *tr = ti >= 0 ? res_of_deref(c, nir_src_as_deref(tex->src[ti].src)) : NULL;
         struct res_var *sr = si >= 0 ? res_of_deref(c, nir_src_as_deref(tex->src[si].src)) : NULL;
         if (tr)
            tr->shadow = true;
         if (sr)
            sr->shadow = true;
      }
      if (tex->op == nir_texop_txs || tex->op == nir_texop_query_levels || tex->op == nir_texop_texture_samples)
         c->type[tex->def.index] = WT_U32;
      else
         c->type[tex->def.index] = wt_of_alu_type(tex->dest_type, tex->def.bit_size);
      break;
   }
   case nir_instr_type_phi: {
      nir_phi_instr *phi = nir_instr_as_phi(instr);
      enum wt t = WT_NONE;
      nir_foreach_phi_src(ps, phi) {
         enum wt s = known_type(c, ps->src.ssa);
         if (s == WT_NONE)
            continue;
         if (t == WT_NONE)
            t = s;
         else if (s != t)
            t = phi->def.bit_size == 16 ? WT_F16 : WT_U32;
      }
      c->type[phi->def.index] = t;
      break;
   }
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (first_pass) {
         switch (intr->intrinsic) {
         case nir_intrinsic_load_input:
         case nir_intrinsic_load_interpolated_input:
         case nir_intrinsic_store_output:
            note_io(c, intr);
            break;
         case nir_intrinsic_deref_atomic:
         case nir_intrinsic_deref_atomic_swap:
            note_atomic_var(c, intr);
            break;
         case nir_intrinsic_ssbo_atomic:
         case nir_intrinsic_ssbo_atomic_swap:
         case nir_intrinsic_store_ssbo: {
            bool st = intr->intrinsic == nir_intrinsic_store_ssbo;
            struct buf_slot *b = get_buf(c, &intr->src[st ? 1 : 0], false);
            if (!b)
               break;
            b->written = true;
            if (!st) {
               nir_atomic_op op = nir_intrinsic_atomic_op(intr);
               b->atomic = true;
               if (op == nir_atomic_op_imin || op == nir_atomic_op_imax)
                  b->atomic_signed = true;
               if (op == nir_atomic_op_umin || op == nir_atomic_op_umax)
                  b->atomic_unsigned_minmax = true;
            }
            break;
         }
         case nir_intrinsic_load_vertex_id: c->in_vertex_index = true; break;
         case nir_intrinsic_load_instance_id: c->in_instance_index = true; break;
         case nir_intrinsic_load_frag_coord: c->in_frag_coord = true; break;
         case nir_intrinsic_load_front_face: c->in_front_face = true; break;
         case nir_intrinsic_load_sample_id: c->in_sample_id = true; break;
         case nir_intrinsic_load_sample_mask_in: c->in_sample_mask = true; break;
         case nir_intrinsic_load_primitive_id: c->in_prim_id = true; break;
         case nir_intrinsic_load_local_invocation_id: c->cs_lid = true; break;
         case nir_intrinsic_load_local_invocation_index: c->cs_lidx = true; break;
         case nir_intrinsic_load_global_invocation_id: c->cs_gid = true; break;
         case nir_intrinsic_load_workgroup_id: c->cs_wgid = true; break;
         case nir_intrinsic_load_num_workgroups: c->cs_nwg = true; break;
         case nir_intrinsic_ballot_bit_count_inclusive:
         case nir_intrinsic_ballot_bit_count_exclusive:
            c->sg_inv = true;
            c->uses_subgroups = true;
            break;
         case nir_intrinsic_load_subgroup_size: c->sg_size = true; c->uses_subgroups = true; break;
         case nir_intrinsic_load_subgroup_invocation: c->sg_inv = true; c->uses_subgroups = true; break;
         case nir_intrinsic_load_num_subgroups: c->sg_num = true; c->uses_subgroups = true; break;
         case nir_intrinsic_load_subgroup_id: c->sg_id = true; c->uses_subgroups = true; break;
         case nir_intrinsic_ballot: case nir_intrinsic_vote_any: case nir_intrinsic_vote_all:
         case nir_intrinsic_elect: case nir_intrinsic_read_first_invocation:
         case nir_intrinsic_read_invocation: case nir_intrinsic_shuffle: case nir_intrinsic_shuffle_xor:
         case nir_intrinsic_shuffle_up: case nir_intrinsic_shuffle_down: case nir_intrinsic_reduce:
         case nir_intrinsic_inclusive_scan: case nir_intrinsic_exclusive_scan:
         case nir_intrinsic_quad_broadcast: case nir_intrinsic_quad_swap_horizontal:
         case nir_intrinsic_quad_swap_vertical: case nir_intrinsic_quad_swap_diagonal:
            c->uses_subgroups = true;
            break;
         default:
            break;
         }
      }
      if (!nir_intrinsic_infos[intr->intrinsic].has_dest)
         break;
      enum wt t = intrinsic_type(c, intr);
      if (intr->intrinsic == nir_intrinsic_ssbo_atomic || intr->intrinsic == nir_intrinsic_ssbo_atomic_swap) {
         nir_scalar sx = nir_scalar_resolved(intr->src[0].ssa, 0);
         t = WT_U32;
         if (!first_pass && nir_scalar_is_const(sx)) {
            struct buf_slot *b = get_buf(c, &intr->src[0], false);
            if (b && b->atomic_signed)
               t = WT_I32;
         }
      }
      c->type[intr->def.index] = t;
      if (intr->intrinsic == nir_intrinsic_ssbo_atomic_swap || intr->intrinsic == nir_intrinsic_deref_atomic_swap)
         c->is_var[intr->def.index] = true;
      break;
   }
   default:
      break;
   }
}

static void
scan(struct ctx *c, nir_function_impl *impl)
{
   for (unsigned pass = 0; pass < 3; pass++) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block)
            scan_instr(c, instr, pass == 0);
      }
   }
   for (unsigned i = 0; i < c->num_bufs; i++) {
      struct buf_slot *b = &c->bufs[i];
      if (b->atomic_signed && b->atomic_unsigned_minmax)
         fail(c, "atomic-sign", "signed and unsigned min/max atomics on one storage buffer");
   }
}

/* ---------------------------------------------------------------- barriers and uniformity
 *
 * WGSL refuses a barrier unless it can prove the control flow around it uniform. Vulkan only
 * requires it to be uniform at run time, so SPIR-V often guards barriers with conditions WGSL
 * cannot prove uniform (a count read from a read_write storage buffer, an early return on an
 * invocation id that is in fact the same for the whole workgroup, ...). For every `if` around a
 * barrier, and every `if` with a break or continue inside a loop with a barrier, a condition not
 * obviously uniform is passed through workgroup memory and read back with workgroupUniformLoad,
 * which WGSL knows to be uniform. Vulkan already demands that it is, so the value is unchanged.
 */

static bool
def_uniform(struct ctx *c, nir_def *d)
{
   uint8_t *u = &c->uniform[d->index];
   if (*u >= 2)
      return *u == 2;
   if (*u == 1)
      return false; /* a cycle (only through phis, which are not uniform here anyway) */
   *u = 1;
   bool r = false;
   nir_instr *instr = nir_def_instr(d);
   switch (instr->type) {
   case nir_instr_type_load_const:
   case nir_instr_type_undef:
      r = true;
      break;
   case nir_instr_type_alu: {
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      r = true;
      for (unsigned i = 0; r && i < nir_op_infos[alu->op].num_inputs; i++)
         r = def_uniform(c, alu->src[i].src.ssa);
      break;
   }
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      switch (intr->intrinsic) {
      case nir_intrinsic_load_workgroup_id:
      case nir_intrinsic_load_num_workgroups:
      case nir_intrinsic_load_workgroup_size:
      case nir_intrinsic_load_subgroup_size:
      case nir_intrinsic_load_num_subgroups:
         r = true;
         break;
      case nir_intrinsic_load_push_constant:
      case nir_intrinsic_load_ubo:
         r = true;
         for (unsigned i = 0; r && i < nir_intrinsic_infos[intr->intrinsic].num_srcs; i++)
            r = def_uniform(c, intr->src[i].ssa);
         break;
      case nir_intrinsic_load_ssbo: {
         struct buf_slot *b = NULL;
         nir_scalar sx = nir_scalar_resolved(intr->src[0].ssa, 0), sy = nir_scalar_resolved(intr->src[0].ssa, 1);
         if (nir_scalar_is_const(sx) && nir_scalar_is_const(sy))
            b = get_buf(c, &intr->src[0], false);
         r = b && !b->written && !b->atomic && def_uniform(c, intr->src[1].ssa);
         break;
      }
      default:
         break;
      }
      break;
   }
   default:
      break;
   }
   *u = r ? 2 : 3;
   return r;
}

static bool
mark_barriers(struct ctx *c, struct exec_list *list)
{
   bool any = false;
   foreach_list_typed(nir_cf_node, node, node, list) {
      bool has = false;
      switch (node->type) {
      case nir_cf_node_block:
         nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
            if (instr->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_barrier &&
                nir_intrinsic_execution_scope(nir_instr_as_intrinsic(instr)) >= SCOPE_WORKGROUP)
               has = true;
         }
         break;
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         bool a = mark_barriers(c, &nif->then_list);
         bool b = mark_barriers(c, &nif->else_list);
         has = a || b;
         break;
      }
      case nir_cf_node_loop:
         has = mark_barriers(c, &nir_cf_node_as_loop(node)->body);
         break;
      default:
         break;
      }
      if (has)
         _mesa_set_add(c->barrier_nodes, node);
      any = any || has;
   }
   return any;
}

static bool
list_has_jump(struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block: {
         nir_instr *last = nir_block_last_instr(nir_cf_node_as_block(node));
         if (last && last->type == nir_instr_type_jump)
            return true;
         break;
      }
      case nir_cf_node_if:
         if (list_has_jump(&nir_cf_node_as_if(node)->then_list) ||
             list_has_jump(&nir_cf_node_as_if(node)->else_list))
            return true;
         break;
      default:
         break; /* jumps inside a nested loop target that loop */
      }
   }
   return false;
}

static void
mark_uniformize(struct ctx *c, struct exec_list *list, bool in_barrier_loop)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      if (node->type == nir_cf_node_if) {
         nir_if *nif = nir_cf_node_as_if(node);
         bool need = _mesa_set_search(c->barrier_nodes, node) ||
                     (in_barrier_loop && (list_has_jump(&nif->then_list) || list_has_jump(&nif->else_list)));
         if (need && !def_uniform(c, nif->condition.ssa))
            _mesa_set_add(c->uniformize, nif);
         mark_uniformize(c, &nif->then_list, in_barrier_loop);
         mark_uniformize(c, &nif->else_list, in_barrier_loop);
      } else if (node->type == nir_cf_node_loop) {
         mark_uniformize(c, &nir_cf_node_as_loop(node)->body,
                         _mesa_set_search(c->barrier_nodes, node) != NULL);
      }
   }
}

/* ---------------------------------------------------------------- control flow */

static void emit_cf_list(struct ctx *c, struct exec_list *list);

static void
emit_phi_copies(struct ctx *c, nir_block *block)
{
   for (unsigned k = 0; k < 2; k++) {
      nir_block *succ = block->successors[k];
      if (!succ)
         continue;
      unsigned nphi = 0;
      nir_foreach_phi(phi, succ)
         nphi++;
      if (!nphi)
         continue;
      /* read every source before writing any phi */
      nir_foreach_phi(phi, succ) {
         nir_phi_src *ps = nir_phi_get_src_from_block(phi, block);
         if (!ps || src_is_undef(&ps->src))
            continue;
         IND(c);
         if (nphi > 1) {
            P(c, "let p%u_%u = ", phi->def.index, block->index);
            emit_src(c, &ps->src, def_type(c, &phi->def));
            P(c, ";\n");
         } else {
            P(c, "v%u = ", phi->def.index);
            emit_src(c, &ps->src, def_type(c, &phi->def));
            P(c, ";\n");
         }
      }
      if (nphi > 1) {
         nir_foreach_phi(phi, succ) {
            nir_phi_src *ps = nir_phi_get_src_from_block(phi, block);
            if (!ps || src_is_undef(&ps->src))
               continue;
            IND(c);
            P(c, "v%u = p%u_%u;\n", phi->def.index, phi->def.index, block->index);
         }
      }
   }
}

static void
emit_block(struct ctx *c, nir_block *block)
{
   nir_jump_instr *jump = NULL;
   nir_foreach_instr(instr, block) {
      switch (instr->type) {
      case nir_instr_type_alu:
         emit_alu(c, nir_instr_as_alu(instr));
         break;
      case nir_instr_type_intrinsic:
         emit_intrinsic(c, nir_instr_as_intrinsic(instr));
         break;
      case nir_instr_type_tex:
         emit_tex(c, nir_instr_as_tex(instr));
         break;
      case nir_instr_type_jump:
         jump = nir_instr_as_jump(instr);
         break;
      case nir_instr_type_call:
         fail(c, "call", "function call (inline first)");
         break;
      case nir_instr_type_load_const:
      case nir_instr_type_undef:
      case nir_instr_type_deref:
      case nir_instr_type_phi:
         break;
      default:
         fail(c, "instr", "instruction type %d", instr->type);
      }
   }
   emit_phi_copies(c, block);
   if (jump) {
      IND(c);
      switch (jump->type) {
      case nir_jump_break: P(c, "break;\n"); break;
      case nir_jump_continue: P(c, "continue;\n"); break;
      case nir_jump_return: P(c, "return wo;\n"); fail(c, "cf", "return (lower returns first)"); break;
      case nir_jump_halt: P(c, "return wo;\n"); fail(c, "cf", "halt"); break;
      default: fail(c, "cf", "unstructured control flow (goto)"); break;
      }
   }
}

static void
emit_cf_list(struct ctx *c, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         emit_block(c, nir_cf_node_as_block(node));
         break;
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         if (_mesa_set_search(c->uniformize, nif)) {
            unsigned u = c->num_uconds++;
            IND(c);
            P(c, "wgsl_uc = select(0u, 1u, ");
            emit_src(c, &nif->condition, WT_BOOL);
            P(c, ");\n");
            IND(c);
            P(c, "let u%u = workgroupUniformLoad(&wgsl_uc) != 0u;\n", u);
            IND(c);
            P(c, "workgroupBarrier();\n");
            IND(c);
            P(c, "if (u%u) {\n", u);
         } else {
            IND(c);
            P(c, "if (");
            emit_src(c, &nif->condition, WT_BOOL);
            P(c, ") {\n");
         }
         c->indent++;
         emit_cf_list(c, &nif->then_list);
         /* An empty else block still carries the phi copies for the merge block, so it is emitted
          * into a side buffer and kept when anything came out. */
         struct _mesa_string_buffer *then_out = c->out;
         c->out = _mesa_string_buffer_create(c, 256);
         emit_cf_list(c, &nif->else_list);
         struct _mesa_string_buffer *else_out = c->out;
         c->out = then_out;
         c->indent--;
         if (else_out->length) {
            IND(c);
            P(c, "} else {\n");
            _mesa_string_buffer_append(c->out, else_out->buf);
         }
         IND(c);
         P(c, "}\n");
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *loop = nir_cf_node_as_loop(node);
         if (nir_loop_has_continue_construct(loop))
            fail(c, "cf", "loop with a continue construct (nir_lower_continue_constructs)");
         IND(c);
         P(c, "loop {\n");
         c->indent++;
         emit_cf_list(c, &loop->body);
         c->indent--;
         IND(c);
         P(c, "}\n");
         break;
      }
      default:
         fail(c, "cf", "cf node %d", node->type);
      }
   }
}

/* ---------------------------------------------------------------- declarations */

static const char *
glsl_to_wgsl(struct ctx *c, const struct glsl_type *t, unsigned atomic_kind)
{
   if (glsl_type_is_array(t)) {
      unsigned len = glsl_get_length(t);
      const char *e = glsl_to_wgsl(c, glsl_get_array_element(t), atomic_kind);
      if (len == 0) {
         fail(c, "type", "unsized array in function/private/workgroup memory");
         len = 1;
      }
      return ralloc_asprintf(c->mem, "array<%s, %u>", e, len);
   }
   if (glsl_type_is_struct(t)) {
      char key[64];
      snprintf(key, sizeof(key), "%p/%u", (void *)t, atomic_kind);
      struct hash_entry *he = _mesa_hash_table_search(c->struct_names, key);
      if (he)
         return he->data;
      struct _mesa_string_buffer *body = _mesa_string_buffer_create(c->mem, 256);
      for (unsigned i = 0; i < glsl_get_length(t); i++)
         _mesa_string_buffer_printf(body, "  f%u: %s,\n", i,
                                    glsl_to_wgsl(c, glsl_get_struct_field(t, i), atomic_kind));
      char *name = ralloc_asprintf(c->mem, "S%u", c->num_structs++);
      _mesa_string_buffer_printf(c->structs, "struct %s {\n%s};\n", name, body->buf);
      _mesa_hash_table_insert(c->struct_names, ralloc_strdup(c->mem, key), name);
      return name;
   }
   enum wt w = wt_of_glsl_base(glsl_get_base_type(t));
   if (w == WT_NONE) {
      fail(c, "type", "variable of type %s", glsl_get_type_name(t));
      return "u32";
   }
   check_bits(c, glsl_get_bit_size(t), w, "variable");
   if (glsl_type_is_matrix(t))
      return ralloc_asprintf(c->mem, "mat%ux%u<%s>", glsl_get_matrix_columns(t), glsl_get_vector_elements(t), wt_name[w]);
   if (atomic_kind && (w == WT_U32 || w == WT_I32) && glsl_type_is_scalar(t))
      return atomic_kind == 2 ? "atomic<i32>" : "atomic<u32>";
   if (atomic_kind && (w == WT_U32 || w == WT_I32))
      fail(c, "atomic", "vector in a variable used atomically");
   return type_str(c, w, glsl_get_vector_elements(t));
}

static const char *
texture_type(struct ctx *c, struct res_var *r)
{
   const struct glsl_type *t = glsl_without_array(r->var->type);
   enum glsl_sampler_dim dim = glsl_get_sampler_dim(t);
   bool arrayed = glsl_sampler_type_is_array(t);
   enum wt st = r->sample == WT_NONE ? WT_F32 : r->sample;
   if (dim == GLSL_SAMPLER_DIM_1D)
      dim = GLSL_SAMPLER_DIM_2D; /* nir_lower_tex lower_1d */
   if (r->shadow) {
      switch (dim) {
      case GLSL_SAMPLER_DIM_2D: return arrayed ? "texture_depth_2d_array" : "texture_depth_2d";
      case GLSL_SAMPLER_DIM_CUBE: return arrayed ? "texture_depth_cube_array" : "texture_depth_cube";
      default: break;
      }
      fail(c, "texture-dim", "shadow texture of dim %d", dim);
      return "texture_depth_2d";
   }
   const char *base;
   switch (dim) {
   case GLSL_SAMPLER_DIM_2D: base = arrayed ? "texture_2d_array" : "texture_2d"; break;
   case GLSL_SAMPLER_DIM_3D: base = "texture_3d"; break;
   case GLSL_SAMPLER_DIM_CUBE: base = arrayed ? "texture_cube_array" : "texture_cube"; break;
   case GLSL_SAMPLER_DIM_MS: base = "texture_multisampled_2d"; break;
   default:
      fail(c, "texture-dim", "texture of dim %d", dim);
      base = "texture_2d";
   }
   return ralloc_asprintf(c->mem, "%s<%s>", base, wt_name[st]);
}

static void
add_binding(struct ctx *c, struct wgsl_result *res, struct wgsl_binding *b)
{
   res->bindings = reralloc(ralloc_parent(c), res->bindings, struct wgsl_binding, res->num_bindings + 1);
   res->bindings[res->num_bindings++] = *b;
}

static void
emit_resources(struct ctx *c, struct _mesa_string_buffer *o, struct wgsl_result *res)
{
   for (unsigned i = 0; i < c->num_bufs; i++) {
      struct buf_slot *b = &c->bufs[i];
      unsigned g, wb;
      if (!remap(c, b->set, b->binding, b->index, false, &g, &wb)) {
         fail(c, "binding-array", "element %u of %s binding (set %u binding %u) needs the binding remap",
              b->index, b->ubo ? "uniform buffer" : "storage buffer", b->set, b->binding);
         continue;
      }
      nir_variable *bvar = find_buffer_var(c, b->ubo, b->set, b->binding);
      struct wgsl_binding wbd = { .group = g, .binding = wb, .vk_set = b->set, .vk_binding = b->binding,
                                  .vk_index = b->index, .atomic = b->atomic,
                                  .vk_array_size = bvar && glsl_type_is_array(bvar->type) ? glsl_get_aoa_size(bvar->type) : 1 };
      if (b->ubo) {
         unsigned n = MAX2(1, DIV_ROUND_UP(b->size, 16));
         _mesa_string_buffer_printf(o, "@group(%u) @binding(%u) var<uniform> %s: array<vec4<u32>, %u>;\n",
                                    g, wb, b->name, n);
         wbd.kind = WGSL_BINDING_UNIFORM_BUFFER;
         wbd.min_size = n * 16;
      } else {
         bool rw = b->written || b->atomic;
         if (rw && c->s->info.stage == MESA_SHADER_VERTEX)
            fail(c, "vertex-storage-write", "vertex shader writes a storage buffer (run it as compute)");
         _mesa_string_buffer_printf(o, "@group(%u) @binding(%u) var<storage, %s> %s: array<%s>;\n", g, wb,
                                    rw ? "read_write" : "read", b->name,
                                    b->atomic ? (b->atomic_signed ? "atomic<i32>" : "atomic<u32>") : "u32");
         wbd.kind = rw ? WGSL_BINDING_STORAGE_BUFFER : WGSL_BINDING_READONLY_STORAGE_BUFFER;
         wbd.min_size = 4;
      }
      add_binding(c, res, &wbd);
   }
   for (unsigned i = 0; i < c->num_res; i++) {
      struct res_var *r = &c->res[i];
      nir_variable *var = r->var;
      unsigned set = var->data.descriptor_set, binding = var->data.binding;
      const struct glsl_type *t = glsl_without_array(var->type);
      unsigned g, wb;
      if (var->data.mode == nir_var_image) {
         if (!remap(c, set, binding, r->index, false, &g, &wb)) {
            fail(c, "binding-array", "element %u of image binding (set %u binding %u) needs the binding remap",
                 r->index, set, binding);
            continue;
         }
         enum pipe_format f = var->data.image.format;
         const char *fname = storage_format_name(f);
         if (f == PIPE_FORMAT_NONE)
            fail(c, "storage-format-none", "storage image without a format (variant per bound format)");
         else if (!fname)
            fail(c, "storage-format", "storage image format %s", util_format_short_name(f));
         if (!fname)
            fname = "r32uint";
         bool rd = r->read && !(var->data.access & ACCESS_NON_READABLE);
         bool wr = r->written;
         const char *access = rd && wr ? "read_write" : wr ? "write" : "read";
         enum glsl_sampler_dim dim = glsl_get_sampler_dim(t);
         bool arrayed = glsl_sampler_type_is_array(t);
         const char *tn = dim == GLSL_SAMPLER_DIM_3D ? "texture_storage_3d" :
                          arrayed ? "texture_storage_2d_array" : "texture_storage_2d";
         _mesa_string_buffer_printf(o, "@group(%u) @binding(%u) var %s: %s<%s, %s>;\n", g, wb, r->tname, tn,
                                    fname, access);
         struct wgsl_binding wbd = { .kind = WGSL_BINDING_STORAGE_TEXTURE, .group = g, .binding = wb,
                                     .vk_set = set, .vk_binding = binding, .vk_index = r->index,
                                     .vk_array_size = glsl_type_is_array(var->type) ? glsl_get_aoa_size(var->type) : 1,
                                     .dim = dim, .arrayed = arrayed, .format = f,
                                     .access = (rd ? 0 : ACCESS_NON_READABLE) | (wr ? 0 : ACCESS_NON_WRITEABLE) };
         add_binding(c, res, &wbd);
         continue;
      }
      unsigned array_size = glsl_type_is_array(var->type) ? glsl_get_aoa_size(var->type) : 1;
      bool combined = glsl_type_is_sampler(t) && !glsl_type_is_bare_sampler(t);
      bool is_tex = combined || glsl_type_is_texture(t);
      if (is_tex && r->tex_used) {
         if (!remap(c, set, binding, r->index, false, &g, &wb)) {
            fail(c, "binding-array", "element %u of texture binding (set %u binding %u) needs the binding remap",
                 r->index, set, binding);
         } else {
            _mesa_string_buffer_printf(o, "@group(%u) @binding(%u) var %s: %s;\n", g, wb, r->tname,
                                       texture_type(c, r));
            enum glsl_sampler_dim dim = glsl_get_sampler_dim(t);
            struct wgsl_binding wbd = { .kind = WGSL_BINDING_TEXTURE, .group = g, .binding = wb,
                                        .vk_set = set, .vk_binding = binding, .vk_index = r->index,
                                        .vk_array_size = array_size,
                                        .dim = dim == GLSL_SAMPLER_DIM_1D ? GLSL_SAMPLER_DIM_2D : dim,
                                        .arrayed = glsl_sampler_type_is_array(t),
                                        .multisampled = dim == GLSL_SAMPLER_DIM_MS,
                                        .sample_type = r->shadow ? WGSL_SAMPLE_DEPTH :
                                                       r->sample == WT_I32 ? WGSL_SAMPLE_SINT :
                                                       r->sample == WT_U32 ? WGSL_SAMPLE_UINT : WGSL_SAMPLE_FLOAT };
            add_binding(c, res, &wbd);
         }
      }
      if ((combined || glsl_type_is_bare_sampler(t)) && r->smp_used) {
         if (!remap(c, set, binding, r->index, combined, &g, &wb)) {
            fail(c, "binding-array", "element %u of sampler binding (set %u binding %u) needs the binding remap",
                 r->index, set, binding);
            continue;
         }
         _mesa_string_buffer_printf(o, "@group(%u) @binding(%u) var %s: %s;\n", g, wb, r->sname,
                                    r->shadow ? "sampler_comparison" : "sampler");
         struct wgsl_binding wbd = { .kind = r->shadow ? WGSL_BINDING_COMPARISON_SAMPLER : WGSL_BINDING_SAMPLER,
                                     .group = g, .binding = wb, .vk_set = set, .vk_binding = binding,
                                     .vk_index = r->index, .vk_array_size = array_size, .from_combined = combined };
         add_binding(c, res, &wbd);
      }
   }
   /* the same WGSL binding twice means the remap folded two descriptors together */
   for (unsigned i = 0; i < res->num_bindings; i++)
      for (unsigned j = i + 1; j < res->num_bindings; j++)
         if (res->bindings[i].group == res->bindings[j].group && res->bindings[i].binding == res->bindings[j].binding)
            fail(c, "binding-collision", "two resources at @group(%u) @binding(%u)", res->bindings[i].group,
                 res->bindings[i].binding);
}

static void
emit_io_structs(struct ctx *c, struct _mesa_string_buffer *o, const char **params, const char **ret)
{
   mesa_shader_stage st = c->s->info.stage;
   *params = "";
   *ret = "";
   if (st == MESA_SHADER_COMPUTE) {
      struct _mesa_string_buffer *p = _mesa_string_buffer_create(c->mem, 256);
      const char *sep = "";
#define PARAM(cond, decl) if (cond) { _mesa_string_buffer_printf(p, "%s%s", sep, decl); sep = ", "; }
      PARAM(c->cs_lid, "@builtin(local_invocation_id) lid: vec3<u32>");
      PARAM(c->cs_lidx, "@builtin(local_invocation_index) lidx: u32");
      PARAM(c->cs_gid, "@builtin(global_invocation_id) gid: vec3<u32>");
      PARAM(c->cs_wgid, "@builtin(workgroup_id) wgid: vec3<u32>");
      PARAM(c->cs_nwg, "@builtin(num_workgroups) nwg: vec3<u32>");
      PARAM(c->sg_size, "@builtin(subgroup_size) sg_size: u32");
      PARAM(c->sg_inv, "@builtin(subgroup_invocation_id) sg_inv: u32");
      PARAM(c->sg_num, "@builtin(num_subgroups) sg_num: u32");
      PARAM(c->sg_id, "@builtin(subgroup_id) sg_id: u32");
#undef PARAM
      *params = p->buf;
      return;
   }
   struct _mesa_string_buffer *in = _mesa_string_buffer_create(c->mem, 256);
   struct _mesa_string_buffer *out = _mesa_string_buffer_create(c->mem, 256);
   if (st == MESA_SHADER_VERTEX) {
      for (unsigned i = 0; i < 32; i++)
         if (c->in[i].used)
            _mesa_string_buffer_printf(in, "  @location(%u) a%u: vec4<%s>,\n", i, i, wt_name[c->in[i].type]);
      if (c->in_vertex_index)
         _mesa_string_buffer_printf(in, "  @builtin(vertex_index) vertex_index: u32,\n");
      if (c->in_instance_index)
         _mesa_string_buffer_printf(in, "  @builtin(instance_index) instance_index: u32,\n");
      _mesa_string_buffer_printf(out, "  @builtin(position) pos: vec4<f32>,\n");
      for (unsigned i = 0; i < 32; i++) {
         struct io_slot *s = &c->out_slots[i];
         if (!s->used && c->o->varying_type[i] && c->o->varying_type[i] < 5) {
            /* the fragment shader reads it, the vertex shader does not write it: declare it (zero) */
            static const enum wt key_types[] = { WT_NONE, WT_F32, WT_I32, WT_U32, WT_F16 };
            s->used = true;
            s->type = key_types[c->o->varying_type[i]];
            s->interp = -1;
         }
         if (!s->used)
            continue;
         struct io_slot eff = *s;
         nir_foreach_shader_out_variable(var, c->s) {
            if (var->data.location == VARYING_SLOT_VAR0 + (int)i) {
               eff.interp = var->data.interpolation;
               eff.sampling = var->data.sample ? 2 : var->data.centroid ? 1 : 0;
            }
         }
         if (i < WGSL_MAX_VARYINGS && c->o->varying_interp[i])
            eff.interp = c->o->varying_interp[i] - 1;
         if (i < WGSL_MAX_VARYINGS && c->o->varying_sampling[i])
            eff.sampling = c->o->varying_sampling[i] - 1;
         _mesa_string_buffer_printf(out, "  @location(%u) %sv%u: vec4<%s>,\n", i, interp_attr(c, &eff), i,
                                    wt_name[s->type]);
      }
      if (c->out_clip) {
         if (!c->o->clip_distances)
            fail(c, "clip-distances", "clip distances without the clip-distances feature");
         _mesa_string_buffer_printf(out, "  @builtin(clip_distances) clip: array<f32, %u>,\n", c->out_clip);
      }
   } else if (st == MESA_SHADER_FRAGMENT) {
      if (c->in_frag_coord)
         _mesa_string_buffer_printf(in, "  @builtin(position) frag_coord: vec4<f32>,\n");
      if (c->in_front_face)
         _mesa_string_buffer_printf(in, "  @builtin(front_facing) front_facing: bool,\n");
      if (c->in_sample_id)
         _mesa_string_buffer_printf(in, "  @builtin(sample_index) sample_index: u32,\n");
      if (c->in_sample_mask)
         _mesa_string_buffer_printf(in, "  @builtin(sample_mask) sample_mask: u32,\n");
      if (c->sg_size)
         _mesa_string_buffer_printf(in, "  @builtin(subgroup_size) sg_size: u32,\n");
      if (c->sg_inv)
         _mesa_string_buffer_printf(in, "  @builtin(subgroup_invocation_id) sg_inv: u32,\n");
      if (c->sg_num || c->sg_id)
         fail(c, "subgroup", "num_subgroups / subgroup_id outside compute");
      if (c->in_prim_id) {
         if (!c->o->primitive_index)
            fail(c, "primitive-id", "gl_PrimitiveID without the primitive-index feature");
         _mesa_string_buffer_printf(in, "  @builtin(primitive_index) prim_id: u32,\n");
      }
      for (unsigned i = 0; i < 32; i++)
         if (c->in[i].used)
            _mesa_string_buffer_printf(in, "  @location(%u) %sv%u: vec4<%s>,\n", i, interp_attr(c, &c->in[i]), i,
                                       wt_name[c->in[i].type]);
      for (unsigned i = 0; i < 8; i++) {
         if (c->out_slots[i].used)
            _mesa_string_buffer_printf(out, "  @location(%u)%s c%u: vec4<%s>,\n", i,
                                       c->out_slots[i + 8].used ? " @blend_src(0)" : "", i,
                                       wt_name[c->out_slots[i].type]);
         if (c->out_slots[i + 8].used)
            _mesa_string_buffer_printf(out, "  @location(%u) @blend_src(1) c%u_1: vec4<%s>,\n", i, i,
                                       wt_name[c->out_slots[i + 8].type]);
      }
      if (c->uses_dual_src && !c->o->dual_source_blending)
         fail(c, "dual-source", "dual-source blending without the feature");
      if (c->out_depth)
         _mesa_string_buffer_printf(out, "  @builtin(frag_depth) depth: f32,\n");
      if (c->out_sample_mask)
         _mesa_string_buffer_printf(out, "  @builtin(sample_mask) mask: u32,\n");
   }
   if (in->length) {
      _mesa_string_buffer_printf(o, "struct In {\n%s};\n", in->buf);
      *params = "wi: In";
   }
   if (out->length) {
      _mesa_string_buffer_printf(o, "struct Out {\n%s};\n", out->buf);
      *ret = " -> Out";
   }
}

static void
emit_helpers(struct ctx *c, struct _mesa_string_buffer *o)
{
   if (c->need_inf)
      _mesa_string_buffer_printf(o, "fn wgsl_nonfinite(bits: u32) -> u32 { return bits; }\n");
   if (c->helper_ballot)
      _mesa_string_buffer_printf(o,
         "fn wgsl_ballot_count_below(v: vec4<u32>, n: u32) -> u32 {\n"
         "  var r = 0u;\n"
         "  for (var k = 0u; k < 4u; k++) {\n"
         "    let lo = k * 32u;\n"
         "    let m = select(select((1u << ((n - lo) & 31u)) - 1u, 0xffffffffu, n >= lo + 32u), 0u, n <= lo);\n"
         "    r += countOneBits(v[k] & m);\n"
         "  }\n"
         "  return r;\n"
         "}\n"
         "fn wgsl_ballot_lsb(v: vec4<u32>) -> u32 {\n"
         "  for (var k = 0u; k < 4u; k++) { if (v[k] != 0u) { return k * 32u + firstTrailingBit(v[k]); } }\n"
         "  return 0xffffffffu;\n"
         "}\n"
         "fn wgsl_ballot_msb(v: vec4<u32>) -> u32 {\n"
         "  for (var k = 4u; k > 0u; k--) { if (v[k - 1u] != 0u) { return (k - 1u) * 32u + firstLeadingBit(v[k - 1u]); } }\n"
         "  return 0xffffffffu;\n"
         "}\n");
   if (c->helper_imod)
      _mesa_string_buffer_printf(o,
         "fn wgsl_imod(a: i32, b: i32) -> i32 {\n"
         "  let r = a %% b;\n"
         "  return select(r, r + b, r != 0i && ((r < 0i) != (b < 0i)));\n"
         "}\n");
   if (c->helper_umul_high || c->helper_imul_high)
      _mesa_string_buffer_printf(o,
         "fn wgsl_umul_high(a: u32, b: u32) -> u32 {\n"
         "  let a0 = a & 0xffffu; let a1 = a >> 16u; let b0 = b & 0xffffu; let b1 = b >> 16u;\n"
         "  let m1 = a1 * b0; let m2 = a0 * b1;\n"
         "  let mid = ((a0 * b0) >> 16u) + (m1 & 0xffffu) + (m2 & 0xffffu);\n"
         "  return a1 * b1 + (m1 >> 16u) + (m2 >> 16u) + (mid >> 16u);\n"
         "}\n");
   if (c->helper_imul_high)
      _mesa_string_buffer_printf(o,
         "fn wgsl_imul_high(a: i32, b: i32) -> i32 {\n"
         "  var h = wgsl_umul_high(bitcast<u32>(a), bitcast<u32>(b));\n"
         "  if (a < 0i) { h = h - bitcast<u32>(b); }\n"
         "  if (b < 0i) { h = h - bitcast<u32>(a); }\n"
         "  return bitcast<i32>(h);\n"
         "}\n");
}

/* ---------------------------------------------------------------- entry */

bool
nir_to_wgsl(nir_shader *s, const struct wgsl_options *o, void *mem_ctx, struct wgsl_result *res)
{
   memset(res, 0, sizeof(*res));
   struct ctx *c = rzalloc(mem_ctx, struct ctx);
   c->s = s;
   c->o = o;
   c->mem = c;
   c->err = _mesa_string_buffer_create(c, 256);
   c->tags = _mesa_hash_table_create(c, _mesa_hash_string, _mesa_key_string_equal);
   c->list_of = _mesa_pointer_hash_table_create(c);
   c->atomic_vars = _mesa_pointer_hash_table_create(c);
   c->struct_names = _mesa_hash_table_create(c, _mesa_hash_string, _mesa_key_string_equal);
   c->structs = _mesa_string_buffer_create(c, 256);
   c->out = _mesa_string_buffer_create(c, 4096);

   mesa_shader_stage st = s->info.stage;
   if (st != MESA_SHADER_VERTEX && st != MESA_SHADER_FRAGMENT && st != MESA_SHADER_COMPUTE)
      fail(c, "stage", "%s shader (geometry/tessellation run through libpoly)", mesa_shader_stage_name(st));
   if (exec_list_length(&s->functions) != 1)
      fail(c, "call", "more than one function (inline first)");

   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_index_ssa_defs(impl);
   nir_index_blocks(impl);
   nir_shader_index_vars(s, nir_var_all & ~nir_var_function_temp);
   nir_function_impl_index_vars(impl);
   c->type = rzalloc_array(c, enum wt, impl->ssa_alloc);
   c->is_var = rzalloc_array(c, bool, impl->ssa_alloc);
   c->block_list = rzalloc_array(c, unsigned, impl->num_blocks);
   unsigned next_list = 0;
   assign_lists(c, &impl->body, &next_list, NULL);

   scan(c, impl);

   /* which values must be function-scope vars */
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_def *d = nir_instr_def(instr);
         if (!d || instr->type == nir_instr_type_load_const || instr->type == nir_instr_type_undef ||
             instr->type == nir_instr_type_deref)
            continue;
         if (instr->type == nir_instr_type_phi || def_needs_var(c, d))
            c->is_var[d->index] = true;
      }
   }

   c->barrier_nodes = _mesa_pointer_set_create(c);
   c->uniformize = _mesa_pointer_set_create(c);
   c->uniform = rzalloc_array(c, uint8_t, impl->ssa_alloc);
   if (st == MESA_SHADER_COMPUTE && mark_barriers(c, &impl->body))
      mark_uniformize(c, &impl->body, false);

   /* the body (this also collects bindings and features) */
   c->indent = 1;
   emit_cf_list(c, &impl->body);

   struct _mesa_string_buffer *m = _mesa_string_buffer_create(c, 8192);
   struct _mesa_string_buffer *decl = _mesa_string_buffer_create(c, 2048);
   const char *params, *ret;
   emit_io_structs(c, decl, &params, &ret);
   emit_resources(c, decl, res);
   if (c->pc_size) {
      if (c->pc_size > o->immediate_size)
         fail(c, "push-constant-size", "%u bytes of push constants, %u bytes of immediates", c->pc_size,
              o->immediate_size);
      _mesa_string_buffer_printf(decl, "struct PushConstants {\n");
      for (unsigned i = 0; i < c->pc_size / 16; i++)
         _mesa_string_buffer_printf(decl, "  m%u: vec4<u32>,\n", i);
      _mesa_string_buffer_printf(decl, "};\nvar<immediate> wgsl_pc: PushConstants;\n");
      res->push_constant_size = c->pc_size;
   }
   if (c->num_uconds)
      _mesa_string_buffer_printf(decl, "var<workgroup> wgsl_uc: u32;\n");
   nir_foreach_variable_with_modes(var, s, nir_var_mem_shared | nir_var_shader_temp) {
      _mesa_string_buffer_printf(decl, "var<%s> %s: %s;\n",
                                 var->data.mode == nir_var_mem_shared ? "workgroup" : "private",
                                 var_name(c, var), glsl_to_wgsl(c, var->type, var_atomic_kind(c, var)));
   }

   /* header: enables and diagnostics */
   if (c->uses_f16)
      _mesa_string_buffer_printf(m, "enable f16;\n");
   if (c->uses_subgroups) {
      if (!o->subgroups)
         fail(c, "subgroups", "subgroup operations without the subgroups feature");
      _mesa_string_buffer_printf(m, "enable subgroups;\n");
   }
   if (c->out_clip && o->clip_distances)
      _mesa_string_buffer_printf(m, "enable clip_distances;\n");
   if (c->uses_dual_src && o->dual_source_blending)
      _mesa_string_buffer_printf(m, "enable dual_source_blending;\n");
   if (c->in_prim_id && o->primitive_index)
      _mesa_string_buffer_printf(m, "enable primitive_index;\n");
   if (st == MESA_SHADER_FRAGMENT)
      _mesa_string_buffer_printf(m, "diagnostic(off, derivative_uniformity);\n");
   if (c->uses_subgroups)
      _mesa_string_buffer_printf(m, "diagnostic(off, subgroup_uniformity);\n");
   emit_helpers(c, m);
   _mesa_string_buffer_append(m, c->structs->buf);
   _mesa_string_buffer_append(m, decl->buf);

   /* the entry point */
   if (st == MESA_SHADER_COMPUTE) {
      if (s->info.workgroup_size_variable)
         fail(c, "workgroup-size", "variable workgroup size (specialise first)");
      res->workgroup_size[0] = s->info.workgroup_size[0];
      res->workgroup_size[1] = s->info.workgroup_size[1];
      res->workgroup_size[2] = s->info.workgroup_size[2];
      _mesa_string_buffer_printf(m, "@compute @workgroup_size(%u, %u, %u)\n", MAX2(1, s->info.workgroup_size[0]),
                                 MAX2(1, s->info.workgroup_size[1]), MAX2(1, s->info.workgroup_size[2]));
   } else {
      _mesa_string_buffer_printf(m, "@%s\n", st == MESA_SHADER_VERTEX ? "vertex" : "fragment");
   }
   _mesa_string_buffer_printf(m, "fn main(%s)%s {\n", params, ret);
   if (*ret)
      _mesa_string_buffer_printf(m, "  var wo: Out;\n");
   nir_foreach_function_temp_variable(var, impl)
      _mesa_string_buffer_printf(m, "  var %s: %s;\n", var_name(c, var), glsl_to_wgsl(c, var->type, 0));
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_def *d = nir_instr_def(instr);
         if (d && instr->type != nir_instr_type_deref && c->is_var[d->index])
            _mesa_string_buffer_printf(m, "  var v%u: %s;\n", d->index,
                                       type_str(c, def_type(c, d), d->num_components));
      }
   }
   _mesa_string_buffer_append(m, c->out->buf);
   if (st == MESA_SHADER_VERTEX && !o->keep_clip_y)
      _mesa_string_buffer_printf(m, "  wo.pos.y = -wo.pos.y;   // Vulkan clip space has y down, WebGPU y up\n");
   if (*ret)
      _mesa_string_buffer_printf(m, "  return wo;\n");
   _mesa_string_buffer_printf(m, "}\n");

   if (st == MESA_SHADER_FRAGMENT) {
      for (unsigned i = 0; i < 32; i++) {
         struct io_slot *in = &c->in[i];
         if (!in->used)
            continue;
         unsigned type = in->type == WT_F32 ? 1 : in->type == WT_I32 ? 2 : in->type == WT_U32 ? 3 : 4;
         unsigned interp = in->type != WT_F32 && in->type != WT_F16 ? INTERP_MODE_FLAT :
                           in->interp < 0 ? INTERP_MODE_SMOOTH : in->interp;
         if (interp == INTERP_MODE_NONE)
            interp = INTERP_MODE_SMOOTH;
         res->fs_varyings[i] = 0x80 | (interp & 3) | (in->sampling & 3) << 2 | type << 4;
      }
   }
   if (c->err->length) {
      res->error = ralloc_strdup(mem_ctx, c->err->buf);
      return false;
   }
   res->wgsl = ralloc_strdup(mem_ctx, m->buf);
   return true;
}
