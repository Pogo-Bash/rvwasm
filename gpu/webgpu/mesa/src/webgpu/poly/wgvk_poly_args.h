/*
 * Shared by wgvk_poly.cl and the driver: what an indirect GS draw's setup reads (in the poly buffer).
 */
#ifndef WGVK_POLY_ARGS_H
#define WGVK_POLY_ARGS_H

#include "compiler/libcl/libcl.h"

struct wgvk_gs_indirect_args {
   uint64_t vs_outputs;           /* the vertex shader's legal.vs_outputs */
   uint32_t index_size_B;         /* 0: not indexed */
   uint32_t index_buffer_range_el;
   uint32_t prim;                 /* enum mesa_prim of the draw */
   uint32_t prefix_sum;           /* wgvk_gs_program.prefix_sum */
   uint32_t max_indices;
   uint32_t shape;                /* wgvk_gs_program.shape */
   uint32_t draw_id;              /* DrawIndex of this draw (the setup writes it to the draw parameters) */
} PACKED;

#endif
