/*
 * The NIR shape the WGSL emitter wants: compiler options for spirv_to_nir, the Vulkan descriptor
 * lowering, and the preprocessing every shader goes through before nir_to_wgsl().
 */
#include "nir_to_wgsl.h"
#include "nir_builder.h"
#include "compiler/spirv/nir_spirv.h"

const nir_shader_compiler_options wgsl_nir_options = {
   /* WGSL has none of these; NIR expands them. */
   .lower_fdph = true,
   .lower_fmod = true,              /* WGSL's % truncates; GLSL mod floors */
   .lower_flrp16 = true,
   .lower_flrp32 = true,
   .lower_flrp64 = true,
   .lower_uadd_carry = true,
   .lower_usub_borrow = true,
   .lower_hadd = true,
   .lower_uadd_sat = true,
   .lower_usub_sat = true,
   .lower_iadd_sat = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_insert_byte = true,
   .lower_insert_word = true,
   .lower_pack_64_2x32 = true,
   .lower_pack_64_4x16 = true,
   .lower_pack_32_2x16 = true,
   .lower_pack_32_2x16_split = true,
   .lower_unpack_32_2x16_split = true,
   /* 64-bit pack/unpack stay as such so that umulExtended & co. (umul_2x32_64 -> pack(imul, umul_high))
    * fold back to 32-bit values */
   .lower_mul_2x32_64 = true,
   .lower_fisnormal = true,
   .lower_scmp = true,
   .has_ldexp = true,
   .instance_id_includes_base_index = true, /* WGSL's instance_index includes firstInstance, as Vulkan's InstanceIndex */
   .lower_device_index_to_zero = true,
   .has_cs_global_id = true,
   .vertex_id_zero_based = false,
   .lower_base_vertex = false,
   .lower_helper_invocation = true,
   .max_unroll_iterations = 32,
   .lower_int64_options = 0,
   .lower_doubles_options = 0,
   .support_16bit_alu = true,
   .support_indirect_inputs = 0,
   .support_indirect_outputs = 0,
};

const struct spirv_to_nir_options wgsl_spirv_options = {
   .environment = NIR_SPIRV_VULKAN,
   .ubo_addr_format = nir_address_format_vec2_index_32bit_offset,
   .ssbo_addr_format = nir_address_format_vec2_index_32bit_offset,
   .phys_ssbo_addr_format = nir_address_format_64bit_global,
   .push_const_addr_format = nir_address_format_logical,
   .shared_addr_format = nir_address_format_logical,
   .task_payload_addr_format = nir_address_format_logical,
   .constant_addr_format = nir_address_format_64bit_global,
   .min_ubo_alignment = 16,
   .min_ssbo_alignment = 4,
};

static bool
lower_vk_resource(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   nir_def *res;
   b->cursor = nir_before_instr(&intr->instr);
   switch (intr->intrinsic) {
   case nir_intrinsic_vulkan_resource_index: {
      unsigned slot = nir_intrinsic_desc_set(intr) << 16 | nir_intrinsic_binding(intr);
      res = nir_vec3(b, nir_imm_int(b, slot), intr->src[0].ssa, nir_imm_int(b, 0));
      break;
   }
   case nir_intrinsic_vulkan_resource_reindex:
      res = nir_vec3(b, nir_channel(b, intr->src[0].ssa, 0),
                     nir_iadd(b, nir_channel(b, intr->src[0].ssa, 1), intr->src[1].ssa),
                     nir_channel(b, intr->src[0].ssa, 2));
      break;
   case nir_intrinsic_load_vulkan_descriptor:
      res = intr->src[0].ssa;
      break;
   default:
      return false;
   }
   nir_def_replace(&intr->def, res);
   return true;
}

bool
wgsl_lower_vk_resources(nir_shader *s)
{
   return nir_shader_intrinsics_pass(s, lower_vk_resource, nir_metadata_control_flow, NULL);
}

static unsigned
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static bool
nonconst_offset(const nir_instr *instr, const void *data)
{
   nir_tex_instr *tex = nir_instr_as_tex((nir_instr *)instr);
   int i = nir_tex_instr_src_index(tex, nir_tex_src_offset);
   return i >= 0 && !nir_src_is_const(tex->src[i].src);
}

static void
optimize(nir_shader *s)
{
   bool progress;
   unsigned n = 0;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_split_array_vars, nir_var_function_temp);
      NIR_PASS(progress, s, nir_shrink_vec_array_vars, nir_var_function_temp);
      NIR_PASS(progress, s, nir_opt_deref);
      NIR_PASS(progress, s, nir_lower_vars_to_ssa);
      NIR_PASS(progress, s, nir_opt_copy_prop_vars);
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_lower_flrp, 16 | 32 | 64, false);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_if, nir_opt_if_optimize_phi_true_false);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_opt_cse);
      const nir_opt_peephole_select_options pso = { .limit = 8, .indirect_load_ok = true, .expensive_alu_ok = true };
      NIR_PASS(progress, s, nir_opt_peephole_select, &pso);
      NIR_PASS(progress, s, nir_opt_undef);
      NIR_PASS(progress, s, nir_opt_loop_unroll);
   } while (progress && ++n < 40);
}

static bool
uses_subgroups(nir_shader *s)
{
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const char *name = nir_intrinsic_infos[nir_instr_as_intrinsic(instr)->intrinsic].name;
            if (!name)
               continue;
            if (strstr(name, "subgroup") || strstr(name, "ballot") || strstr(name, "vote") ||
                strstr(name, "shuffle") || strstr(name, "quad") || strstr(name, "reduce") ||
                strstr(name, "scan") || strstr(name, "elect") || strstr(name, "first_invocation") ||
                strstr(name, "read_invocation"))
               return true;
         }
      }
   }
   return false;
}

void
wgsl_preprocess_nir(nir_shader *s, const struct wgsl_options *o)
{
   mesa_shader_stage stage = s->info.stage;
   nir_function_impl *entry = nir_shader_get_entrypoint(s);

   NIR_PASS(_, s, nir_lower_continue_constructs);
   NIR_PASS(_, s, nir_lower_io_vars_to_temporaries, entry, nir_var_shader_out);
   NIR_PASS(_, s, nir_split_var_copies);
   NIR_PASS(_, s, nir_lower_global_vars_to_local);
   NIR_PASS(_, s, nir_lower_var_copies);
   NIR_PASS(_, s, nir_split_struct_vars, nir_var_function_temp);
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   NIR_PASS(_, s, nir_lower_frexp);

   if (uses_subgroups(s)) {
      const nir_lower_subgroups_options sg = {
         .subgroup_size = 0,
         .ballot_bit_size = 32,
         .ballot_components = 4,      /* WGSL's subgroupBallot is a vec4<u32> */
         .lower_subgroup_masks = true,
         .lower_vote_feq = true,
         .lower_vote_ieq = true,
         .lower_quad_broadcast_dynamic = true,
         .lower_inverse_ballot = true,
         .lower_rotate_to_shuffle = true,
         .lower_shuffle_to_32bit = true,
         .lower_to_scalar = true,      /* the vector path of lower_shuffle_to_32bit makes invalid NIR (main 35b085c4) */
      };
      NIR_PASS(_, s, nir_lower_subgroups, &sg);
   }
   NIR_PASS(_, s, nir_lower_system_values);
   const nir_lower_compute_system_values_options csv = { 0 };
   NIR_PASS(_, s, nir_lower_compute_system_values, &csv);

   optimize(s);

   /* Shader I/O: one vec4 slot per location, constant offsets folded into the base. */
   if (stage == MESA_SHADER_FRAGMENT)
      NIR_PASS(_, s, nir_lower_io_array_vars_to_elements_no_indirects, true);
   else
      NIR_PASS(_, s, nir_lower_io_array_vars_to_elements_no_indirects, false);
   nir_assign_io_var_locations(s, nir_var_shader_out);
   if (stage != MESA_SHADER_VERTEX)
      nir_assign_io_var_locations(s, nir_var_shader_in);
   else
      nir_foreach_shader_in_variable(var, s)
         var->data.driver_location = var->data.location;
   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size_vec4,
            stage == MESA_SHADER_FRAGMENT ? nir_lower_io_use_interpolated_input_intrinsics : 0);

   /* Buffers: byte offsets into a (slot, element) pair. */
   NIR_PASS(_, s, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);
   NIR_PASS(_, s, nir_lower_explicit_io, nir_var_mem_ubo | nir_var_mem_ssbo,
            nir_address_format_vec2_index_32bit_offset);
   NIR_PASS(_, s, wgsl_lower_vk_resources);

   const nir_lower_tex_options tex = {
      .lower_txp = ~0u,
      .lower_txp_array = true,
      .lower_txf_offset = true,
      .lower_offset_filter = nonconst_offset,
      .lower_1d = true,                /* 1D images are 2D images of height 1 in wgvk */
      .lower_1d_shadow = true,
      .lower_tg4_offsets = true,
      .lower_invalid_implicit_lod = true,
   };
   NIR_PASS(_, s, nir_lower_tex, &tex);

   optimize(s);
   NIR_PASS(_, s, nir_opt_move, nir_move_const_undef);
   nir_rematerialize_derefs_in_use_blocks_impl(nir_shader_get_entrypoint(s));
   NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp | nir_var_shader_temp, NULL);
   NIR_PASS(_, s, nir_opt_dce);
   nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
}
