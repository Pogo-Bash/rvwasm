/*
 * wgvk's own OpenCL helpers on top of libpoly (track T4), built like libpoly's (mesa_clc, then
 * vtn_bindgen2 into NIR builder functions, see meson.build).
 */
#include "compiler/libcl/libcl_vk.h"
#include "poly/geometry.h"
#include "poly/prim.h"
#include "poly/tessellator.h"
#include "wgvk_poly_args.h"

/*
 * An indirect draw through a geometry shader: libpoly's setup from the draw command the GPU holds (vertex
 * and geometry params, the heap allocations), plus the workgroup counts of the compute parts for
 * dispatchWorkgroupsIndirect (64-wide workgroups in x), which must not be in the poly buffer those
 * dispatches bind writable: they go to `dispatch` (the indirect buffer, segment 2).
 */
void
wgvk_gs_setup_indirect(global struct wgvk_gs_indirect_args *a, uint64_t index_buffer, constant uint *draw,
                       global struct poly_vertex_params *vp, global struct poly_geometry_params *gp,
                       global struct poly_heap *heap, global uint *dispatch, global uint *draw_params)
{
   /* the draw parameters the vertex shader reads (struct wgvk_draw_params): base vertex, first instance */
   draw_params[0] = a->index_size_B ? draw[3] : draw[2];
   draw_params[1] = a->index_size_B ? draw[4] : draw[3];
   draw_params[2] = a->draw_id;
   poly_gs_setup_indirect(index_buffer, draw, vp, gp, heap, a->vs_outputs, a->index_size_B,
                          a->index_buffer_range_el, a->prim, a->prefix_sum, a->max_indices,
                          (enum poly_gs_shape)a->shape);
   dispatch[0] = (vp->grid[0] + 63) / 64;
   dispatch[1] = vp->grid[1];
   dispatch[2] = 1;
   dispatch[3] = (gp->grid[0] + 63) / 64;
   dispatch[4] = gp->grid[1];
   dispatch[5] = 1;
   /* the xfb part: one invocation per output vertex of the rasterization draw */
   dispatch[6] = (gp->draw.index_count + 63) / 64;
   dispatch[7] = gp->draw.instance_count;
   dispatch[8] = 1;
}

/*
 * Prefix sums of the GS count buffer (transform feedback needs each primitive's place), one counter word at
 * a time. A single invocation: libpoly's workgroup scan wants 32-wide subgroups, which WebGPU does not
 * promise. Slow for large draws; a parallel scan is a later step.
 */
void
wgvk_prefix_sum_geom(global struct poly_geometry_params *p)
{
   uint words = p->count_buffer_stride / 4;
   for (uint w = 0; w < words; w++) {
      uint sum = 0;
      for (uint i = 0; i < p->input_primitives; i++) {
         global uint *ptr = &p->count_buffer[i * words + w];
         sum += *ptr;
         *ptr = sum;
      }
   }
}

/* ---------------------------------------------------------------- tessellation */

/* Prefix sums of the patches' index counts, then the index buffer allocation and the indexed draw the TES
 * part is drawn with (libagx_prefix_sum_tess, one invocation). */
void
wgvk_prefix_sum_tess(global struct poly_tess_params *p)
{
   uint sum = 0;
   for (uint i = 0; i < p->nr_patches; i++) {
      sum += p->counts[i];
      p->counts[i] = sum;
   }
   uint alloc_B = poly_heap_alloc_offs(p->heap, sum * 4, 16);
   p->index_buffer = (global uint32_t *)(((uintptr_t)p->heap->base) + alloc_B);
   global uint32_t *desc = p->out_draws;
   desc[0] = sum;
   desc[1] = 1;
   desc[2] = alloc_B / 4;
   desc[3] = 0;
   desc[4] = 0;
}
