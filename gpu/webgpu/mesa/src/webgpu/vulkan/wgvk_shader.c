/*
 * wgvk shaders: Mesa's common pipelines (vk_pipeline.c) on vk_shader objects
 * whose code is WGSL from T3's wgvk_compile (INTERFACES.md 6). The runtime
 * does SPIR-V -> NIR, VkPipeline, pipeline caches and libraries; wgvk gives it
 * the compile step and binds the result while the queue replays a command
 * buffer (wgvk_cmd.c). The GPURenderPipeline itself is made at draw time from
 * the bound shaders and the dynamic state (wgvk_pipeline.c): WebGPU bakes
 * nearly all of Vulkan's dynamic state into it.
 *
 * Vertex shaders depend on the fragment shader they are drawn with (WebGPU
 * wants the two sides of each location to agree on type and interpolation,
 * Vulkan only reads the fragment side) and on the sign of the viewport's
 * height (keep_clip_y) and on depth clip control's [-1, 1] (z_minus_one). So a vertex shader keeps its NIR and makes variants
 * when a draw needs one. A linked pipeline compiles its fragment shader first
 * and gets the variant it needs right away.
 *
 * Bind group layouts come from the shaders' binding tables, merged over the
 * stages a pipeline has: WebGPU's layout entries need the view dimension,
 * sample type and storage format, which a Vulkan set layout does not say.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "wgvk_private.h"

#include "compiler/nir/nir_serialize.h"
#include "util/blob.h"
#include "vk_format.h"
#include "vk_nir.h"
#define XXH_INLINE_ALL
#include "util/xxhash.h"

static uint64_t layout_hash(const struct wgvk_shader *s);

/* ---- WGSL modules, by hash */

WGPUShaderModule
wgvk_module_get(struct wgvk_device *dev, const struct wgvk_compiled_shader *cs)
{
	struct hash_entry *he = _mesa_hash_table_search(dev->modules, &cs->hash);
	if (he)
		return he->data;
	WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
	src.code = (WGPUStringView){ cs->wgsl, cs->wgsl_len };
	WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	md.nextInChain = &src.chain;
	WGPUShaderModule m = wgpuDeviceCreateShaderModule(dev->wdev, &md);
	uint64_t *key = malloc(sizeof(*key));
	*key = cs->hash;
	_mesa_hash_table_insert(dev->modules, key, m);
	if (getenv("WGVK_DUMP_WGSL"))
		fprintf(stderr, "wgvk: WGSL %016llx:\n%s\n", (unsigned long long)cs->hash, cs->wgsl);
	return m;
}

/* ---- bind group layouts: T1's, from the compiled shaders' binding lists */

static WGPUShaderStage
stage_visibility(mesa_shader_stage s)
{
	return s == MESA_SHADER_VERTEX || s == MESA_SHADER_GEOMETRY ? WGPUShaderStage_Vertex :
	       s == MESA_SHADER_FRAGMENT ? WGPUShaderStage_Fragment : WGPUShaderStage_Compute;
}

/* The bind group layouts of a set of shaders (one pipeline's stages); returns the number of groups. */
unsigned
wgvk_bgls_for_shaders(struct wgvk_device *dev, struct wgvk_shader *const *sh,
		      const struct wgvk_compiled_shader *const *cs, unsigned n, const struct wgvk_unfilterable *uf,
		      struct wgvk_bgl *out[4])
{
	const struct wgvk_compiled_shader *list[4];
	WGPUShaderStage stages[4];
	const struct wgvk_layout_remap *remap = NULL;
	unsigned m = 0, groups = 0;
	for (unsigned s = 0; s < n && m < 4; s++) {
		if (!sh[s])
			continue;
		list[m] = cs[s];
		stages[m++] = stage_visibility(sh[s]->vk.stage);
		remap = remap ? remap : &sh[s]->remap;
		for (unsigned i = 0; i < cs[s]->n_bindings; i++)
			groups = MAX2(groups, cs[s]->bindings[i].group + 1u);
	}
	groups = MIN2(groups, 4);
	for (unsigned g = 0; g < groups; g++) {
		WGPUBindGroupLayoutEntry e[128];
		unsigned k = wgvk_bgl_entries_from_shaders(dev, remap, list, stages, m, g, e, ARRAY_SIZE(e));
		for (unsigned i = 0; uf && i < uf->n; i++) {
			if (uf->e[i].group != g)
				continue;
			for (unsigned j = 0; j < k; j++) {
				if (e[j].binding != uf->e[i].binding)
					continue;
				if (e[j].texture.sampleType == WGPUTextureSampleType_Float)
					e[j].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
				if (e[j].sampler.type == WGPUSamplerBindingType_Filtering)
					e[j].sampler.type = WGPUSamplerBindingType_NonFiltering;
			}
		}
		out[g] = wgvk_bgl_get(dev, e, k);
	}
	return groups;
}

/* ---- compute pipelines: one per compute shader, made on first use */

VkResult
wgvk_compute_pipeline_get(struct wgvk_device *dev, struct wgvk_shader *s)
{
	if (s->cpipe)
		return VK_SUCCESS;
	struct wgvk_shader *sh[1] = { s };
	const struct wgvk_compiled_shader *cs[1] = { &s->cs };
	s->cgroups = wgvk_bgls_for_shaders(dev, sh, cs, 1, NULL, s->cbgl);
	WGPUBindGroupLayout bgls[4];
	for (unsigned g = 0; g < s->cgroups; g++)
		bgls[g] = s->cbgl[g]->layout;
	WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	pld.bindGroupLayoutCount = s->cgroups;
	pld.bindGroupLayouts = bgls;
	pld.immediateSize = align(s->cs.immediate_bytes, 4);
	WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev->wdev, &pld);
	WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
	cd.layout = pl;
	cd.compute.module = wgvk_module_get(dev, &s->cs);
	cd.compute.entryPoint = wgvk_sv("main");
	s->cpipe = wgpuDeviceCreateComputePipeline(dev->wdev, &cd);
	wgpuPipelineLayoutRelease(pl);
	dev->stats.pipelines_created++;
	return s->cpipe ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

const struct wgvk_cpipe *
wgvk_cpipe_get(struct wgvk_device *dev, const struct wgvk_compiled_shader *cs, const struct wgvk_layout_remap *remap,
	       const struct wgvk_unfilterable *uf)
{
	uint64_t key = XXH64(remap->b, remap->n * sizeof(remap->b[0]), cs->hash ^ remap->n);
	if (uf && uf->n)
		key = XXH64(uf->e, uf->n * sizeof(uf->e[0]), key);
	struct hash_entry *he = _mesa_hash_table_search(dev->cpipes, &key);
	if (he)
		return he->data;
	struct wgvk_cpipe *p = calloc(1, sizeof(*p));
	p->key = key;
	unsigned groups = 0;
	for (unsigned i = 0; i < cs->n_bindings; i++)
		groups = MAX2(groups, cs->bindings[i].group + 1u);
	p->ngroups = MIN2(groups, 4);
	WGPUShaderStage st = WGPUShaderStage_Compute;
	WGPUBindGroupLayout bgls[4];
	for (unsigned g = 0; g < p->ngroups; g++) {
		WGPUBindGroupLayoutEntry e[128];
		unsigned k = wgvk_bgl_entries_from_shaders(dev, remap, &cs, &st, 1, g, e, ARRAY_SIZE(e));
		for (unsigned i = 0; uf && i < uf->n; i++)
			for (unsigned j = 0; j < k; j++)
				if (uf->e[i].group == g && e[j].binding == uf->e[i].binding) {
					if (e[j].texture.sampleType == WGPUTextureSampleType_Float)
						e[j].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
					if (e[j].sampler.type == WGPUSamplerBindingType_Filtering)
						e[j].sampler.type = WGPUSamplerBindingType_NonFiltering;
				}
		p->bgl[g] = wgvk_bgl_get(dev, e, k);
		bgls[g] = p->bgl[g]->layout;
	}
	p->immediate_bytes = cs->immediate_bytes;
	p->draw_params_offset = cs->legal.draw_params_offset;
	WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	pld.bindGroupLayoutCount = p->ngroups;
	pld.bindGroupLayouts = bgls;
	pld.immediateSize = align(cs->immediate_bytes, 4);
	WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(dev->wdev, &pld);
	WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
	cd.layout = pl;
	cd.compute.module = wgvk_module_get(dev, cs);
	cd.compute.entryPoint = wgvk_sv("main");
	p->pipe = wgpuDeviceCreateComputePipeline(dev->wdev, &cd);
	wgpuPipelineLayoutRelease(pl);
	dev->stats.pipelines_created++;
	_mesa_hash_table_insert(dev->cpipes, &p->key, p);
	return p;
}

/* ---- vk_shader objects */

static void
shader_destroy(struct vk_device *vk_dev, struct vk_shader *vk_shader, const VkAllocationCallbacks *pAllocator)
{
	struct wgvk_shader *s = container_of(vk_shader, struct wgvk_shader, vk);
	wgvk_compiled_shader_free(&s->cs);
	for (struct wgvk_vs_variant *v = s->variants, *next; v; v = next) {
		next = v->next;
		wgvk_compiled_shader_free(&v->cs);
		free(v);
	}
	for (struct wgvk_gs_variant *v = s->gs_variants, *next; v; v = next) {
		next = v->next;
		wgvk_gs_program_free(&v->prog);
		free(v);
	}
	for (struct wgvk_fs_variant *v = s->fs_variants, *next; v; v = next) {
		next = v->next;
		wgvk_compiled_shader_free(&v->cs);
		free(v);
	}
	if (s->cpipe)
		wgpuComputePipelineRelease(s->cpipe);
	free((void *)s->remap.b);
	free(s->nir_blob);
	vk_shader_free(vk_dev, pAllocator, &s->vk);
}

/* the blob's layout: a new compiler struct size is a new layout */
#define WGVK_SHADER_BLOB_VERSION (5u ^ (uint32_t)sizeof(struct wgvk_legalize_info) << 8 ^ \
				  (uint32_t)sizeof(struct wgvk_shader_binding) << 20)

static bool
shader_serialize(struct vk_device *vk_dev, const struct vk_shader *vk_shader, struct blob *b)
{
	const struct wgvk_shader *s = container_of(vk_shader, struct wgvk_shader, vk);
	blob_write_uint32(b, WGVK_SHADER_BLOB_VERSION);
	blob_write_uint32(b, s->vk.stage);
	blob_write_uint64(b, s->cs.wgsl_len);
	blob_write_bytes(b, s->cs.wgsl, s->cs.wgsl_len);
	blob_write_uint32(b, s->cs.n_bindings);
	blob_write_bytes(b, s->cs.bindings, s->cs.n_bindings * sizeof(s->cs.bindings[0]));
	blob_write_bytes(b, &s->cs.legal, sizeof(s->cs.legal));
	blob_write_uint32(b, s->cs.variant_mask);
	blob_write_uint32(b, s->cs.immediate_bytes);
	blob_write_bytes(b, s->cs.workgroup_size, sizeof(s->cs.workgroup_size));
	blob_write_uint64(b, s->cs.hash);
	blob_write_bytes(b, s->cs.fs_varyings, 32);
	blob_write_uint32(b, s->remap.n);
	blob_write_bytes(b, s->remap.b, s->remap.n * sizeof(s->remap.b[0]));
	blob_write_uint8(b, s->remap.driver_group);
	blob_write_uint16(b, s->remap.push_constant_bytes);
	blob_write_uint16(b, s->remap.immediate_bytes);
	blob_write_uint32(b, s->vs_inputs | (uint32_t)s->vs_writes_psiz << 31);
	blob_write_bytes(b, s->key_fs_varyings, 32);
	blob_write_uint8(b, s->key_keep_clip_y);
	blob_write_uint8(b, s->writes_viewport_index);
	blob_write_uint8(b, s->fs_early);
	blob_write_uint32(b, s->fs_outputs);
	blob_write_uint32(b, s->vs_float_inputs);
	blob_write_uint32(b, s->fs_float_outputs);
	blob_write_uint64(b, s->nir_blob_size);
	blob_write_bytes(b, s->nir_blob, s->nir_blob_size);
	return !b->out_of_memory;
}

static VkResult
shader_get_executable_properties(struct vk_device *device, const struct vk_shader *shader, uint32_t *count,
				 VkPipelineExecutablePropertiesKHR *props)
{
	*count = 0;
	return VK_SUCCESS;
}

static VkResult
shader_get_executable_statistics(struct vk_device *device, const struct vk_shader *shader, uint32_t index,
				 uint32_t *count, VkPipelineExecutableStatisticKHR *stats)
{
	*count = 0;
	return VK_SUCCESS;
}

static VkResult
shader_get_internal_representations(struct vk_device *device, const struct vk_shader *shader, uint32_t index,
				    uint32_t *count, VkPipelineExecutableInternalRepresentationKHR *ir)
{
	*count = 0;
	return VK_SUCCESS;
}

static const struct vk_shader_ops wgvk_shader_ops = {
	.destroy = shader_destroy,
	.serialize = shader_serialize,
	.get_executable_properties = shader_get_executable_properties,
	.get_executable_statistics = shader_get_executable_statistics,
	.get_executable_internal_representations = shader_get_internal_representations,
};

static VkResult
shader_deserialize(struct vk_device *vk_dev, struct blob_reader *b, uint32_t binary_version,
		   const VkAllocationCallbacks *pAllocator, struct vk_shader **out)
{
	if (blob_read_uint32(b) != WGVK_SHADER_BLOB_VERSION)
		return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
	mesa_shader_stage stage = blob_read_uint32(b);
	struct wgvk_shader *s = vk_shader_zalloc(vk_dev, &wgvk_shader_ops, stage, pAllocator, sizeof(*s));
	if (!s)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	s->cs.wgsl_len = blob_read_uint64(b);
	s->cs.wgsl = calloc(1, s->cs.wgsl_len + 1);
	blob_copy_bytes(b, s->cs.wgsl, s->cs.wgsl_len);
	s->cs.n_bindings = blob_read_uint32(b);
	s->cs.bindings = calloc(MAX2(s->cs.n_bindings, 1), sizeof(s->cs.bindings[0]));
	blob_copy_bytes(b, s->cs.bindings, s->cs.n_bindings * sizeof(s->cs.bindings[0]));
	blob_copy_bytes(b, &s->cs.legal, sizeof(s->cs.legal));
	s->cs.variant_mask = blob_read_uint32(b);
	s->cs.immediate_bytes = blob_read_uint32(b);
	blob_copy_bytes(b, s->cs.workgroup_size, sizeof(s->cs.workgroup_size));
	s->cs.hash = blob_read_uint64(b);
	blob_copy_bytes(b, s->cs.fs_varyings, 32);
	s->remap.n = blob_read_uint32(b);
	struct wgvk_binding_remap *rb = calloc(MAX2(s->remap.n, 1), sizeof(*rb));
	blob_copy_bytes(b, rb, s->remap.n * sizeof(*rb));
	s->remap.b = rb;
	s->remap.driver_group = blob_read_uint8(b);
	s->remap.push_constant_bytes = blob_read_uint16(b);
	s->remap.immediate_bytes = blob_read_uint16(b);
	s->vs_inputs = blob_read_uint32(b);
	s->vs_writes_psiz = s->vs_inputs >> 31;
	s->vs_inputs &= ~(1u << 31);
	blob_copy_bytes(b, s->key_fs_varyings, 32);
	s->key_keep_clip_y = blob_read_uint8(b);
	s->writes_viewport_index = blob_read_uint8(b);
	s->fs_early = blob_read_uint8(b);
	s->fs_outputs = blob_read_uint32(b);
	s->vs_float_inputs = blob_read_uint32(b);
	s->fs_float_outputs = blob_read_uint32(b);
	s->nir_blob_size = blob_read_uint64(b);
	if (s->nir_blob_size) {
		s->nir_blob = malloc(s->nir_blob_size);
		blob_copy_bytes(b, s->nir_blob, s->nir_blob_size);
	}
	s->layout_hash = layout_hash(s);
	if (b->overrun) {
		shader_destroy(vk_dev, &s->vk, pAllocator);
		return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
	}
	*out = &s->vk;
	return VK_SUCCESS;
}

/* The binding remap (INTERFACES 4.2) from the set layouts the runtime hands the compile (T1 builds it). */
static VkResult
build_remap(struct wgvk_device *dev, struct wgvk_shader *s, const struct vk_shader_compile_info *info)
{
	uint32_t pc = 0;
	for (uint32_t i = 0; i < info->push_constant_range_count; i++)
		pc = MAX2(pc, info->push_constant_ranges[i].offset + info->push_constant_ranges[i].size);
	return wgvk_layout_remap_build(dev->pdev, info->set_layouts, info->set_layout_count, pc, &s->remap);
}

/* What the bind group layouts depend on beyond the WGSL: which buffers take dynamic offsets. */
static uint64_t
layout_hash(const struct wgvk_shader *s)
{
	uint64_t h = XXH64(&s->cs.immediate_bytes, sizeof(s->cs.immediate_bytes), s->cs.hash);
	for (unsigned i = 0; i < s->cs.n_bindings; i++) {
		const struct wgvk_shader_binding *b = &s->cs.bindings[i];
		uint8_t dyn = 0;
		for (unsigned j = 0; j < s->remap.n; j++)
			if (s->remap.b[j].set == b->vk_set && s->remap.b[j].binding == b->vk_binding)
				dyn = s->remap.b[j].flags & WGVK_REMAP_DYNAMIC;
		h = XXH64(&dyn, 1, h);
	}
	return h;
}

static VkResult
compile_one(struct wgvk_device *dev, struct vk_shader_compile_info *info, const uint8_t fs_varyings[32],
	    const VkAllocationCallbacks *pAllocator, struct wgvk_shader **out)
{
	nir_shader *nir = info->nir;
	struct wgvk_shader *s = vk_shader_zalloc(&dev->vk, &wgvk_shader_ops, nir->info.stage, pAllocator, sizeof(*s));
	if (!s) {
		ralloc_free(nir);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	if (build_remap(dev, s, info) != VK_SUCCESS) {
		ralloc_free(nir);
		shader_destroy(&dev->vk, &s->vk, pAllocator);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	if (nir->info.stage == MESA_SHADER_VERTEX) {
		nir_foreach_shader_out_variable(var, nir)
			s->vs_writes_psiz |= var->data.location == VARYING_SLOT_PSIZ;
		/* the attribute locations the shader declares (WebGPU wants each in the vertex state) */
		nir_foreach_shader_in_variable(var, nir) {
			if (var->data.location < VERT_ATTRIB_GENERIC0)
				continue;
			unsigned slots = glsl_count_attribute_slots(var->type, true);
			enum glsl_base_type bt = glsl_get_base_type(glsl_without_array_or_matrix(var->type));
			for (unsigned i = 0; i < slots; i++) {
				s->vs_inputs |= 1u << (var->data.location - VERT_ATTRIB_GENERIC0 + i);
				if (bt == GLSL_TYPE_FLOAT || bt == GLSL_TYPE_FLOAT16)
					s->vs_float_inputs |= 1u << (var->data.location - VERT_ATTRIB_GENERIC0 + i);
			}
		}
		struct blob b;
		blob_init(&b);
		nir_serialize(&b, nir, false);
		blob_finish_get_buffer(&b, &s->nir_blob, &s->nir_blob_size);
		if (fs_varyings)
			memcpy(s->key_fs_varyings, fs_varyings, 32);
	}
	if (nir->info.stage == MESA_SHADER_FRAGMENT) {
		/* the colour locations it writes (WebGPU wants writeMask 0 on the others), and whether it relies on
		 * early fragment tests for its memory writes (INTERFACES 6.1); the NIR again for variants */
		nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
		nir_foreach_shader_out_variable(var, nir) {
			int loc = var->data.location;
			enum glsl_base_type bt = glsl_get_base_type(glsl_without_array_or_matrix(var->type));
			bool fl = bt == GLSL_TYPE_FLOAT || bt == GLSL_TYPE_FLOAT16;
			if (loc == FRAG_RESULT_COLOR) {
				s->fs_outputs = ~0u;
				if (fl)
					s->fs_float_outputs = ~0u;
			} else if (loc >= FRAG_RESULT_DATA0 && loc < FRAG_RESULT_DATA0 + 8) {
				uint32_t m = BITFIELD_RANGE(loc - FRAG_RESULT_DATA0,
							    MAX2(glsl_count_attribute_slots(var->type, false), 1));
				s->fs_outputs |= m;
				if (fl)
					s->fs_float_outputs |= m;
			}
		}
		s->fs_early = nir->info.fs.early_fragment_tests && nir->info.writes_memory;
		struct blob b;
		blob_init(&b);
		nir_serialize(&b, nir, false);
		blob_finish_get_buffer(&b, &s->nir_blob, &s->nir_blob_size);
	}
	if (nir->info.stage == MESA_SHADER_GEOMETRY) {
		/* libpoly's parts (T4), made per draw state (wgvk_shader_gs_variant); the first one now, to report
		 * what cannot be compiled at pipeline creation */
		struct blob b;
		blob_init(&b);
		nir_serialize(&b, nir, false);
		blob_finish_get_buffer(&b, &s->nir_blob, &s->nir_blob_size);
		ralloc_free(nir);
		struct wgvk_gs_key gk = { 0 };
		if (fs_varyings)
			memcpy(gk.fs_varyings, fs_varyings, 32);
		const struct wgvk_gs_variant *gv = wgvk_shader_gs_variant(dev, s, &gk);
		if (!gv) {
			shader_destroy(&dev->vk, &s->vk, pAllocator);
			return VK_ERROR_UNKNOWN;
		}
		s->writes_viewport_index = gv->prog.part[WGVK_GS_RAST].legal.writes_viewport_index;
		*out = s;
		return VK_SUCCESS;
	}
	struct wgvk_shader_key key = { .layout = &s->remap };
	memcpy(key.fs_varyings, s->key_fs_varyings, 32);
	VkResult r = wgvk_compile(dev, nir, &key, &s->cs);
	if (r != VK_SUCCESS) {
		wgvk_log("%s shader not compiled: %s", _mesa_shader_stage_to_string(s->vk.stage),
			 s->cs.error ? s->cs.error : "?\n");
		shader_destroy(&dev->vk, &s->vk, pAllocator);
		return r == VK_ERROR_FEATURE_NOT_PRESENT ? VK_ERROR_UNKNOWN : r;
	}
	if (s->vk.stage == MESA_SHADER_VERTEX)
		s->writes_viewport_index = s->cs.legal.writes_viewport_index;
	s->layout_hash = layout_hash(s);
	*out = s;
	return VK_SUCCESS;
}

static VkResult
compile_shaders(struct vk_device *vk_dev, uint32_t count, struct vk_shader_compile_info *infos,
		const struct vk_graphics_pipeline_state *state, const struct vk_features *features,
		const VkAllocationCallbacks *pAllocator, struct vk_shader **out)
{
	struct wgvk_device *dev = container_of(vk_dev, struct wgvk_device, vk);
	/* The fragment shader first: its inputs are the vertex shader's key (INTERFACES 6). */
	uint8_t fs_varyings[32] = { 0 };
	bool have_fs = false;
	for (uint32_t i = 0; i < count; i++)
		if (infos[i].stage == MESA_SHADER_FRAGMENT) {
			wgvk_gather_fs_varyings(infos[i].nir, fs_varyings);
			have_fs = true;
		}
	VkResult r = VK_SUCCESS;
	uint32_t i;
	for (i = 0; i < count; i++) {
		struct wgvk_shader *s = NULL;
		if (infos[i].stage != MESA_SHADER_VERTEX && infos[i].stage != MESA_SHADER_FRAGMENT &&
		    infos[i].stage != MESA_SHADER_COMPUTE && infos[i].stage != MESA_SHADER_GEOMETRY) {
			wgvk_log("%s shaders need T4 (libpoly)\n", _mesa_shader_stage_to_string(infos[i].stage));
			ralloc_free(infos[i].nir);
			r = VK_ERROR_FEATURE_NOT_PRESENT;
			out[i] = NULL;
			continue;
		}
		VkResult ri = compile_one(dev, &infos[i], have_fs ? fs_varyings : NULL, pAllocator, &s);
		if (ri != VK_SUCCESS) {
			r = ri;
			out[i] = NULL;
			continue;
		}
		out[i] = &s->vk;
	}
	if (r != VK_SUCCESS) {
		for (i = 0; i < count; i++)
			if (out[i])
				shader_destroy(vk_dev, out[i], pAllocator);
	}
	return r;
}

/* The vertex shader's WGSL for a fragment shader's inputs and a viewport sign. */
const struct wgvk_compiled_shader *
wgvk_shader_vs_variant(struct wgvk_device *dev, struct wgvk_shader *vs, const struct wgvk_vs_key *k)
{
	struct wgvk_vs_key base;
	memset(&base, 0, sizeof(base));
	memcpy(base.fs_varyings, vs->key_fs_varyings, 32);
	base.keep_clip_y = vs->key_keep_clip_y;
	if (!memcmp(&base, k, sizeof(base)))
		return &vs->cs;
	for (struct wgvk_vs_variant *v = vs->variants; v; v = v->next)
		if (!memcmp(&v->key, k, sizeof(*k)))
			return &v->cs;
	if (!vs->nir_blob)
		return &vs->cs;
	struct blob_reader br;
	blob_reader_init(&br, vs->nir_blob, vs->nir_blob_size);
	nir_shader *nir = nir_deserialize(NULL, wgvk_nir_options(), &br);
	/* VK_EXT_depth_clip_control's [-1, 1] clip-space depth (zink asks for it): WebGPU clips z to [0, w], so
	 * the vertex shader moves z to (z + w) / 2 */
	if (k->z_minus_one)
		NIR_PASS(_, nir, nir_lower_clip_halfz);
	struct wgvk_vs_variant *v = calloc(1, sizeof(*v));
	v->key = *k;
	struct wgvk_shader_key key = {
		.layout = &vs->remap, .keep_clip_y = k->keep_clip_y, .vertex_pulling_mask = k->pull_mask,
		.point_size = k->point_mode, .point_coord_varying = k->point_coord_varying,
		.vi = (k->pull_mask || k->point_mode) ? &v->key.vi : NULL, .vs_before_gs = k->vs_before_gs,
		.viewport_index_varying = k->viewport_index_varying,
	};
	memcpy(key.fs_varyings, k->fs_varyings, 32);
	if (wgvk_compile(dev, nir, &key, &v->cs) != VK_SUCCESS) {
		wgvk_log("vertex shader variant not compiled: %s", v->cs.error ? v->cs.error : "?\n");
		wgvk_compiled_shader_free(&v->cs);
		free(v);
		return &vs->cs;
	}
	v->next = vs->variants;
	vs->variants = v;
	return &v->cs;
}

const struct wgvk_gs_variant *
wgvk_shader_gs_variant(struct wgvk_device *dev, struct wgvk_shader *gs, const struct wgvk_gs_key *k)
{
	for (struct wgvk_gs_variant *v = gs->gs_variants; v; v = v->next)
		if (!memcmp(&v->key, k, sizeof(*k)))
			return v->failed ? NULL : v;
	if (!gs->nir_blob)
		return NULL;
	struct blob_reader br;
	blob_reader_init(&br, gs->nir_blob, gs->nir_blob_size);
	nir_shader *nir = nir_deserialize(NULL, wgvk_nir_options(), &br);
	if (k->z_minus_one)
		NIR_PASS(_, nir, nir_lower_clip_halfz);
	struct wgvk_gs_variant *v = calloc(1, sizeof(*v));
	v->key = *k;
	struct wgvk_shader_key key = { .layout = &gs->remap, .keep_clip_y = k->keep_clip_y,
				       .viewport_index_varying = k->viewport_index_varying };
	memcpy(key.fs_varyings, k->fs_varyings, 32);
	if (wgvk_compile_gs(wgvk_device_compile_caps(dev), nir, &key, &v->prog) != VK_SUCCESS) {
		wgvk_log("geometry shader not compiled: %s", v->prog.error ? v->prog.error : "?\n");
		v->failed = true;
	} else {
		const struct wgvk_compiled_shader *r = &v->prog.part[WGVK_GS_RAST];
		v->layout_hash = XXH64(&r->immediate_bytes, sizeof(r->immediate_bytes), r->hash);
	}
	v->next = gs->gs_variants;
	gs->gs_variants = v;
	return v->failed ? NULL : v;
}

/* The fragment shader's WGSL for viewport / layer emulation and the early depth test (zero key: fs->cs). */
const struct wgvk_compiled_shader *
wgvk_shader_fs_variant(struct wgvk_device *dev, struct wgvk_shader *fs, const struct wgvk_fs_key *k)
{
	static const uint8_t no_types[8];
	if (!k->viewports && !k->early_depth && !memcmp(k->out_type, no_types, 8))
		return &fs->cs;
	for (struct wgvk_fs_variant *v = fs->fs_variants; v; v = v->next)
		if (!memcmp(&v->key, k, sizeof(*k)))
			return v->cs.wgsl ? &v->cs : &fs->cs;
	if (!fs->nir_blob)
		return &fs->cs;
	struct blob_reader br;
	blob_reader_init(&br, fs->nir_blob, fs->nir_blob_size);
	nir_shader *nir = nir_deserialize(NULL, wgvk_nir_options(), &br);
	struct wgvk_fs_variant *v = calloc(1, sizeof(*v));
	v->key = *k;
	struct wgvk_shader_key key = { .layout = &fs->remap, .viewports = k->viewports, .early_depth = k->early_depth };
	memcpy(key.fs_out_type, k->out_type, 8);
	if (wgvk_compile(dev, nir, &key, &v->cs) != VK_SUCCESS) {
		wgvk_log("fragment shader variant not compiled: %s", v->cs.error ? v->cs.error : "?\n");
		wgvk_compiled_shader_free(&v->cs);
		memset(&v->cs, 0, sizeof(v->cs));	/* (kept: not tried again) */
	}
	v->next = fs->fs_variants;
	fs->fs_variants = v;
	return v->cs.wgsl ? &v->cs : &fs->cs;
}

/* ---- the device's shader ops */

static const nir_shader_compiler_options *
get_nir_options(struct vk_physical_device *pdev, mesa_shader_stage stage, const struct vk_pipeline_robustness_state *rs)
{
	return wgvk_nir_options();
}

static struct spirv_to_nir_options
get_spirv_options(struct vk_physical_device *pdev, mesa_shader_stage stage,
		  const struct vk_pipeline_robustness_state *rs)
{
	return *wgvk_spirv_options();
}

static void
preprocess_nir(struct vk_physical_device *pdev, nir_shader *nir, const struct vk_pipeline_robustness_state *rs)
{
	/* wgvk_compile does its own lowering (T3) */
}

static void
hash_state(struct vk_physical_device *pdev, const struct vk_graphics_pipeline_state *state,
	   const struct vk_features *features, VkShaderStageFlags stages, blake3_hash out)
{
	/* nothing of the state goes into the WGSL: what does is chosen at draw time (variants) */
	memset(out, 0, sizeof(blake3_hash));
}

void wgvk_cmd_bind_shaders(struct vk_command_buffer *cmd, uint32_t count, const mesa_shader_stage *stages,
			   struct vk_shader **const shaders);

const struct vk_device_shader_ops wgvk_device_shader_ops = {
	.get_nir_options = get_nir_options,
	.get_spirv_options = get_spirv_options,
	.preprocess_nir = preprocess_nir,
	.hash_state = hash_state,
	.compile = compile_shaders,
	.deserialize = shader_deserialize,
	.cmd_set_dynamic_graphics_state = vk_cmd_set_dynamic_graphics_state,
	.cmd_bind_shaders = wgvk_cmd_bind_shaders,
};
