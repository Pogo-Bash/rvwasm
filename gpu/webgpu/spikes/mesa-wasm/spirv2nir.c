/*
 * Spike S4: Mesa's spirv_to_nir and a driver-style optimisation loop over SPIR-V modules, timed; built natively and
 * with Emscripten from the same Mesa objects, so the wasm/native ratio of the page-side shader front end is known.
 *   spirv2nir FILE.spv...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nir.h"
#include "nir_spirv.h"
#include "compiler/glsl_types.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static gl_shader_stage stage_of(const uint32_t *w, size_t n, char *name)
{
	for (size_t i = 5; i < n;) {
		uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
		if (!len) break;
		if (op == 15) { /* OpEntryPoint */
			strncpy(name, (const char *)&w[i + 3], 63);
			switch (w[i + 1]) {
			case 0: return MESA_SHADER_VERTEX; case 1: return MESA_SHADER_TESS_CTRL; case 2: return MESA_SHADER_TESS_EVAL;
			case 3: return MESA_SHADER_GEOMETRY; case 4: return MESA_SHADER_FRAGMENT; case 5: return MESA_SHADER_COMPUTE;
			}
		}
		i += len;
	}
	return MESA_SHADER_NONE;
}

int main(int argc, char **argv)
{
	glsl_type_singleton_init_or_ref();
	static const nir_shader_compiler_options nopts = {
		.lower_fdiv = true, .lower_flrp32 = true, .lower_fsat = true, .lower_ffma32 = true,
		.lower_uadd_carry = true, .lower_usub_borrow = true, .lower_extract_byte = true, .lower_extract_word = true,
		.lower_insert_byte = true, .lower_insert_word = true, .max_unroll_iterations = 32,
		.lower_int64_options = ~0, .lower_doubles_options = ~0,
	};
	struct spirv_to_nir_options sopts = {
		.environment = NIR_SPIRV_VULKAN,
		.capabilities = NULL, /* NULL: everything spirv_to_nir implements */
		.ubo_addr_format = nir_address_format_32bit_index_offset,
		.ssbo_addr_format = nir_address_format_32bit_index_offset,
		.phys_ssbo_addr_format = nir_address_format_64bit_global,
		.push_const_addr_format = nir_address_format_32bit_offset,
		.shared_addr_format = nir_address_format_32bit_offset,
		.min_ubo_alignment = 16, .min_ssbo_alignment = 16,
	};
	double total = 0; int ok = 0, bad = 0; unsigned instrs = 0;
	for (int a = 1; a < argc; a++) {
		FILE *f = fopen(argv[a], "rb");
		if (!f) { bad++; continue; }
		fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
		uint32_t *w = malloc(sz); fread(w, 1, sz, f); fclose(f);
		char name[64] = "main";
		gl_shader_stage st = stage_of(w, sz / 4, name);
		double t0 = now();
		nir_shader *s = spirv_to_nir(w, sz / 4, NULL, 0, st, name, &sopts, &nopts);
		if (!s) { bad++; free(w); continue; }
		NIR_PASS(_, s, nir_lower_variable_initializers, nir_var_function_temp);
		NIR_PASS(_, s, nir_lower_returns);
		NIR_PASS(_, s, nir_inline_functions);
		nir_remove_non_entrypoints(s);
		NIR_PASS(_, s, nir_lower_vars_to_ssa);
		bool progress;
		int loops = 0;
		do {
			progress = false;
			NIR_PASS(progress, s, nir_copy_prop);
			NIR_PASS(progress, s, nir_opt_dce);
			NIR_PASS(progress, s, nir_opt_cse);
			NIR_PASS(progress, s, nir_opt_algebraic);
			NIR_PASS(progress, s, nir_opt_constant_folding);
			NIR_PASS(progress, s, nir_opt_if, 0);
			NIR_PASS(progress, s, nir_opt_dead_cf);
			NIR_PASS(progress, s, nir_opt_loop_unroll);
		} while (progress && ++loops < 20);
		NIR_PASS(_, s, nir_lower_int64);
		NIR_PASS(_, s, nir_convert_from_ssa, true, false);
		double t = now() - t0;
		total += t; ok++;
		unsigned n = 0;
		nir_foreach_function_impl(impl, s) nir_foreach_block(b, impl) nir_foreach_instr(i, b) n++;
		instrs += n;
		if (getenv("VERBOSE")) printf("%s stage %d %u instrs %.2f ms\n", argv[a], st, n, t * 1000);
		ralloc_free(s); free(w);
	}
	printf("modules ok %d failed %d, %u NIR instructions, %.1f ms total, %.2f ms per module\n", ok, bad, instrs, total * 1000, ok ? total * 1000 / ok : 0);
	return 0;
}
