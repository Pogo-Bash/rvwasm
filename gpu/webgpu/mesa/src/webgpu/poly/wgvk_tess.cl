/*
 * wgvk's tessellator entry points (track T4): libpoly's port of the D3D11 reference tessellator, one patch
 * per call: COUNT mode writes the patch's index count, WITH_COUNTS mode its coordinates and indices.
 *
 * This module is not turned into builder code by vtn_bindgen2 like wgvk_poly.cl: OpenCL SPIR-V has no
 * structured control flow, and bindgen's structurizing plus its if-else trees for every indexed private
 * array grow the tessellator past nir_serialize's object limit. wgvk_poly.c imports the SPIR-V at run time
 * (spirv_to_nir as a library, linked into a kernel shader) and keeps the private arrays indexable.
 */
#include "compiler/libcl/libcl_vk.h"
#include "poly/geometry.h"
#include "poly/cl/tessellator.h"

void
wgvk_tess_tri(constant struct poly_tess_params *p, uint patch, uint mode)
{
   poly_tess_tri_process(p, patch, (enum poly_tess_mode)mode);
}

void
wgvk_tess_quad(constant struct poly_tess_params *p, uint patch, uint mode)
{
   poly_tess_quad_process(p, patch, (enum poly_tess_mode)mode);
}

void
wgvk_tess_isoline(constant struct poly_tess_params *p, uint patch, uint mode)
{
   poly_tess_isoline_process(p, patch, (enum poly_tess_mode)mode);
}
