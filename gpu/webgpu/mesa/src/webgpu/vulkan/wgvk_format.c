/*
 * wgvk's format table (wgvk_format.h, INTERFACES.md 4.5).
 *
 * Two halves:
 *   - a fixed map VkFormat -> (WebGPU texture format, WebGPU vertex format,
 *     flags, swizzle);
 *   - per device, WebGPU's capabilities of each texture format (render,
 *     blend, multisample, resolve, storage, filtering), from Dawn's
 *     Format.cpp (c943ea32), which implements the spec's capability tables
 *     and the features that extend them (core-features-and-limits,
 *     texture-formats-tier1/2, float32-filterable/-blendable,
 *     rg11b10ufloat-renderable, bgra8unorm-storage, depth32float-stencil8,
 *     the compression features, unorm16-texture-formats); and the Vulkan
 *     format features that follow.
 *
 * Planned emulations are advertised now, as the design table says (marked
 * "(T3b)" or "(T2)" below): texel buffers decode in the shader, storage
 * images without a format are shader variants, image atomics on r32 go
 * through a buffer, depth24 copies go through a shader.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "wgvk_private.h"
#include "wgvk_format.h"

#include "util/format/u_format.h"
#include "vk_format.h"

#define SWZ_ID { WGPUComponentSwizzle_R, WGPUComponentSwizzle_G, WGPUComponentSwizzle_B, WGPUComponentSwizzle_A }

struct map {
	VkFormat vk;
	WGPUTextureFormat wf;
	WGPUVertexFormat vf;
	uint16_t flags;
	WGPUComponentSwizzle swz[4];
};

#define T(vk, wf) { VK_FORMAT_##vk, WGPUTextureFormat_##wf, 0, 0, SWZ_ID }
#define TV(vk, wf, vf) { VK_FORMAT_##vk, WGPUTextureFormat_##wf, WGPUVertexFormat_##vf, 0, SWZ_ID }
#define V(vk, vf) { VK_FORMAT_##vk, WGPUTextureFormat_Undefined, WGPUVertexFormat_##vf, 0, SWZ_ID }
#define TF(vk, wf, fl) { VK_FORMAT_##vk, WGPUTextureFormat_##wf, 0, fl, SWZ_ID }
#define TS(vk, wf, r, g, b, a) { VK_FORMAT_##vk, WGPUTextureFormat_##wf, 0, WGVK_FMT_SWIZZLED, \
	{ WGPUComponentSwizzle_##r, WGPUComponentSwizzle_##g, WGPUComponentSwizzle_##b, WGPUComponentSwizzle_##a } }
#define C(vk, wf) { VK_FORMAT_##vk, WGPUTextureFormat_##wf, 0, WGVK_FMT_COMPRESSED, SWZ_ID }

static const struct map map[] = {
	/* 8 bits per component */
	TV(R8_UNORM, R8Unorm, Unorm8), TV(R8_SNORM, R8Snorm, Snorm8), TV(R8_UINT, R8Uint, Uint8),
	TV(R8_SINT, R8Sint, Sint8),
	TV(R8G8_UNORM, RG8Unorm, Unorm8x2), TV(R8G8_SNORM, RG8Snorm, Snorm8x2), TV(R8G8_UINT, RG8Uint, Uint8x2),
	TV(R8G8_SINT, RG8Sint, Sint8x2),
	TV(R8G8B8A8_UNORM, RGBA8Unorm, Unorm8x4), TV(R8G8B8A8_SNORM, RGBA8Snorm, Snorm8x4),
	TV(R8G8B8A8_UINT, RGBA8Uint, Uint8x4), TV(R8G8B8A8_SINT, RGBA8Sint, Sint8x4), T(R8G8B8A8_SRGB, RGBA8UnormSrgb),
	/* A8B8G8R8_*_PACK32 is R8G8B8A8 in memory on a little-endian machine */
	TV(A8B8G8R8_UNORM_PACK32, RGBA8Unorm, Unorm8x4), TV(A8B8G8R8_SNORM_PACK32, RGBA8Snorm, Snorm8x4),
	TV(A8B8G8R8_UINT_PACK32, RGBA8Uint, Uint8x4), TV(A8B8G8R8_SINT_PACK32, RGBA8Sint, Sint8x4),
	T(A8B8G8R8_SRGB_PACK32, RGBA8UnormSrgb),
	TV(B8G8R8A8_UNORM, BGRA8Unorm, Unorm8x4BGRA), T(B8G8R8A8_SRGB, BGRA8UnormSrgb),
	/* B8G8R8A8 integer: RGBA8 storage read through a swizzle */
	TS(B8G8R8A8_UINT, RGBA8Uint, B, G, R, A), TS(B8G8R8A8_SINT, RGBA8Sint, B, G, R, A),
	TS(B8G8R8A8_SNORM, RGBA8Snorm, B, G, R, A),
	/* packed */
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, WGPUTextureFormat_RGB10A2Unorm, WGPUVertexFormat_Unorm10_10_10_2, 0, SWZ_ID },
	T(A2B10G10R10_UINT_PACK32, RGB10A2Uint),
	{ VK_FORMAT_A2B10G10R10_SNORM_PACK32, 0, WGPUVertexFormat_Snorm10_10_10_2, 0, SWZ_ID },
	/* the rest of GL's 2_10_10_10_rev vertex formats (and 3-component 8/16-bit ones): pulled (T3b) */
	{ VK_FORMAT_A2B10G10R10_USCALED_PACK32, 0, 0, WGVK_FMT_VERTEX_PULL, SWZ_ID },
	{ VK_FORMAT_A2B10G10R10_SSCALED_PACK32, 0, 0, WGVK_FMT_VERTEX_PULL, SWZ_ID },
	{ VK_FORMAT_A2B10G10R10_SINT_PACK32, 0, 0, WGVK_FMT_VERTEX_PULL, SWZ_ID },
	{ VK_FORMAT_A2R10G10B10_SNORM_PACK32, 0, 0, WGVK_FMT_VERTEX_PULL, SWZ_ID },
	{ VK_FORMAT_A2R10G10B10_USCALED_PACK32, 0, 0, WGVK_FMT_VERTEX_PULL, SWZ_ID },
	{ VK_FORMAT_A2R10G10B10_SSCALED_PACK32, 0, 0, WGVK_FMT_VERTEX_PULL, SWZ_ID },
	{ VK_FORMAT_A2R10G10B10_SINT_PACK32, 0, 0, WGVK_FMT_VERTEX_PULL, SWZ_ID },
	{ VK_FORMAT_A2R10G10B10_UNORM_PACK32, WGPUTextureFormat_RGB10A2Unorm, 0, WGVK_FMT_SWIZZLED | WGVK_FMT_VERTEX_PULL,
	  { WGPUComponentSwizzle_B, WGPUComponentSwizzle_G, WGPUComponentSwizzle_R, WGPUComponentSwizzle_A } },
	{ VK_FORMAT_A2R10G10B10_UINT_PACK32, WGPUTextureFormat_RGB10A2Uint, 0, WGVK_FMT_SWIZZLED | WGVK_FMT_VERTEX_PULL,
	  { WGPUComponentSwizzle_B, WGPUComponentSwizzle_G, WGPUComponentSwizzle_R, WGPUComponentSwizzle_A } },
	T(B10G11R11_UFLOAT_PACK32, RG11B10Ufloat), T(E5B9G9R9_UFLOAT_PACK32, RGB9E5Ufloat),
	/* 16 bits */
	TV(R16_UNORM, R16Unorm, Unorm16), TV(R16_SNORM, R16Snorm, Snorm16), TV(R16_UINT, R16Uint, Uint16),
	TV(R16_SINT, R16Sint, Sint16), TV(R16_SFLOAT, R16Float, Float16),
	TV(R16G16_UNORM, RG16Unorm, Unorm16x2), TV(R16G16_SNORM, RG16Snorm, Snorm16x2),
	TV(R16G16_UINT, RG16Uint, Uint16x2), TV(R16G16_SINT, RG16Sint, Sint16x2), TV(R16G16_SFLOAT, RG16Float, Float16x2),
	TV(R16G16B16A16_UNORM, RGBA16Unorm, Unorm16x4), TV(R16G16B16A16_SNORM, RGBA16Snorm, Snorm16x4),
	TV(R16G16B16A16_UINT, RGBA16Uint, Uint16x4), TV(R16G16B16A16_SINT, RGBA16Sint, Sint16x4),
	TV(R16G16B16A16_SFLOAT, RGBA16Float, Float16x4),
	/* 32 bits */
	TV(R32_UINT, R32Uint, Uint32), TV(R32_SINT, R32Sint, Sint32), TV(R32_SFLOAT, R32Float, Float32),
	TV(R32G32_UINT, RG32Uint, Uint32x2), TV(R32G32_SINT, RG32Sint, Sint32x2), TV(R32G32_SFLOAT, RG32Float, Float32x2),
	V(R32G32B32_UINT, Uint32x3), V(R32G32B32_SINT, Sint32x3), V(R32G32B32_SFLOAT, Float32x3),
	TV(R32G32B32A32_UINT, RGBA32Uint, Uint32x4), TV(R32G32B32A32_SINT, RGBA32Sint, Sint32x4),
	TV(R32G32B32A32_SFLOAT, RGBA32Float, Float32x4),
	/* depth / stencil */
	TF(D16_UNORM, Depth16Unorm, WGVK_FMT_DEPTH),
	TF(X8_D24_UNORM_PACK32, Depth24Plus, WGVK_FMT_DEPTH | WGVK_FMT_DEPTH_NO_COPY),
	TF(D32_SFLOAT, Depth32Float, WGVK_FMT_DEPTH | WGVK_FMT_DEPTH_NO_COPY_DST),
	TF(S8_UINT, Stencil8, WGVK_FMT_STENCIL),
	TF(D24_UNORM_S8_UINT, Depth24PlusStencil8, WGVK_FMT_DEPTH | WGVK_FMT_STENCIL | WGVK_FMT_DEPTH_NO_COPY),
	TF(D32_SFLOAT_S8_UINT, Depth32FloatStencil8, WGVK_FMT_DEPTH | WGVK_FMT_STENCIL | WGVK_FMT_DEPTH_NO_COPY_DST),
	/* compressed: BC (RGB variants are the RGBA format with alpha forced to one) */
	{ VK_FORMAT_BC1_RGB_UNORM_BLOCK, WGPUTextureFormat_BC1RGBAUnorm, 0, WGVK_FMT_COMPRESSED | WGVK_FMT_SWIZZLED,
	  { WGPUComponentSwizzle_R, WGPUComponentSwizzle_G, WGPUComponentSwizzle_B, WGPUComponentSwizzle_One } },
	{ VK_FORMAT_BC1_RGB_SRGB_BLOCK, WGPUTextureFormat_BC1RGBAUnormSrgb, 0, WGVK_FMT_COMPRESSED | WGVK_FMT_SWIZZLED,
	  { WGPUComponentSwizzle_R, WGPUComponentSwizzle_G, WGPUComponentSwizzle_B, WGPUComponentSwizzle_One } },
	C(BC1_RGBA_UNORM_BLOCK, BC1RGBAUnorm), C(BC1_RGBA_SRGB_BLOCK, BC1RGBAUnormSrgb),
	C(BC2_UNORM_BLOCK, BC2RGBAUnorm), C(BC2_SRGB_BLOCK, BC2RGBAUnormSrgb),
	C(BC3_UNORM_BLOCK, BC3RGBAUnorm), C(BC3_SRGB_BLOCK, BC3RGBAUnormSrgb),
	C(BC4_UNORM_BLOCK, BC4RUnorm), C(BC4_SNORM_BLOCK, BC4RSnorm),
	C(BC5_UNORM_BLOCK, BC5RGUnorm), C(BC5_SNORM_BLOCK, BC5RGSnorm),
	C(BC6H_UFLOAT_BLOCK, BC6HRGBUfloat), C(BC6H_SFLOAT_BLOCK, BC6HRGBFloat),
	C(BC7_UNORM_BLOCK, BC7RGBAUnorm), C(BC7_SRGB_BLOCK, BC7RGBAUnormSrgb),
	/* ETC2 / EAC */
	C(ETC2_R8G8B8_UNORM_BLOCK, ETC2RGB8Unorm), C(ETC2_R8G8B8_SRGB_BLOCK, ETC2RGB8UnormSrgb),
	C(ETC2_R8G8B8A1_UNORM_BLOCK, ETC2RGB8A1Unorm), C(ETC2_R8G8B8A1_SRGB_BLOCK, ETC2RGB8A1UnormSrgb),
	C(ETC2_R8G8B8A8_UNORM_BLOCK, ETC2RGBA8Unorm), C(ETC2_R8G8B8A8_SRGB_BLOCK, ETC2RGBA8UnormSrgb),
	C(EAC_R11_UNORM_BLOCK, EACR11Unorm), C(EAC_R11_SNORM_BLOCK, EACR11Snorm),
	C(EAC_R11G11_UNORM_BLOCK, EACRG11Unorm), C(EAC_R11G11_SNORM_BLOCK, EACRG11Snorm),
	/* ASTC LDR */
	C(ASTC_4x4_UNORM_BLOCK, ASTC4x4Unorm), C(ASTC_4x4_SRGB_BLOCK, ASTC4x4UnormSrgb),
	C(ASTC_5x4_UNORM_BLOCK, ASTC5x4Unorm), C(ASTC_5x4_SRGB_BLOCK, ASTC5x4UnormSrgb),
	C(ASTC_5x5_UNORM_BLOCK, ASTC5x5Unorm), C(ASTC_5x5_SRGB_BLOCK, ASTC5x5UnormSrgb),
	C(ASTC_6x5_UNORM_BLOCK, ASTC6x5Unorm), C(ASTC_6x5_SRGB_BLOCK, ASTC6x5UnormSrgb),
	C(ASTC_6x6_UNORM_BLOCK, ASTC6x6Unorm), C(ASTC_6x6_SRGB_BLOCK, ASTC6x6UnormSrgb),
	C(ASTC_8x5_UNORM_BLOCK, ASTC8x5Unorm), C(ASTC_8x5_SRGB_BLOCK, ASTC8x5UnormSrgb),
	C(ASTC_8x6_UNORM_BLOCK, ASTC8x6Unorm), C(ASTC_8x6_SRGB_BLOCK, ASTC8x6UnormSrgb),
	C(ASTC_8x8_UNORM_BLOCK, ASTC8x8Unorm), C(ASTC_8x8_SRGB_BLOCK, ASTC8x8UnormSrgb),
	C(ASTC_10x5_UNORM_BLOCK, ASTC10x5Unorm), C(ASTC_10x5_SRGB_BLOCK, ASTC10x5UnormSrgb),
	C(ASTC_10x6_UNORM_BLOCK, ASTC10x6Unorm), C(ASTC_10x6_SRGB_BLOCK, ASTC10x6UnormSrgb),
	C(ASTC_10x8_UNORM_BLOCK, ASTC10x8Unorm), C(ASTC_10x8_SRGB_BLOCK, ASTC10x8UnormSrgb),
	C(ASTC_10x10_UNORM_BLOCK, ASTC10x10Unorm), C(ASTC_10x10_SRGB_BLOCK, ASTC10x10UnormSrgb),
	C(ASTC_12x10_UNORM_BLOCK, ASTC12x10Unorm), C(ASTC_12x10_SRGB_BLOCK, ASTC12x10UnormSrgb),
	C(ASTC_12x12_UNORM_BLOCK, ASTC12x12Unorm), C(ASTC_12x12_SRGB_BLOCK, ASTC12x12UnormSrgb),
};

/* ---- WebGPU's capabilities per texture format (Dawn src/dawn/native/Format.cpp) */

#define R WGVK_CAP_RENDER
#define B WGVK_CAP_BLEND
#define M WGVK_CAP_MULTISAMPLE
#define RS WGVK_CAP_RESOLVE
#define SW WGVK_CAP_STORAGE_W
#define SR WGVK_CAP_STORAGE_R
#define SRW WGVK_CAP_STORAGE_RW
#define F WGVK_CAP_FILTER

static unsigned
wgpu_caps(const struct wgvk_wgpu_features *f, WGPUTextureFormat wf)
{
	unsigned c = WGVK_CAP_SAMPLE;
	switch (wf) {
	case WGPUTextureFormat_R8Unorm:
		c |= R | M | RS | B | F;
		if (f->tier1) c |= SW | SR;
		if (f->tier2) c |= SRW;
		return c;
	case WGPUTextureFormat_RG8Unorm:
		c |= R | M | RS | B | F;
		if (f->tier1) c |= SW | SR;
		return c;
	case WGPUTextureFormat_R8Snorm:
	case WGPUTextureFormat_RG8Snorm:
		c |= F;
		if (f->tier1) c |= R | M | RS | B | SW | SR;
		return c;
	case WGPUTextureFormat_RGBA8Snorm:
		c |= F | SW | SR;
		if (f->tier1) c |= R | M | RS | B;
		return c;
	case WGPUTextureFormat_RGBA8Unorm:
		c |= F | SW | SR | R | M | RS | B;
		if (f->tier2) c |= SRW;
		return c;
	case WGPUTextureFormat_RGBA8UnormSrgb:
		return c | F | R | M | RS | B;
	case WGPUTextureFormat_BGRA8Unorm:
		c |= F | R | M | RS | B;
		if (f->bgra8_storage) c |= SW;
		return c;
	case WGPUTextureFormat_BGRA8UnormSrgb:
		c |= F;
		if (f->core) c |= R | M | RS | B;
		return c;
	case WGPUTextureFormat_R16Float:
		c |= F | R | M | RS | B;
		if (f->tier1) c |= SW | SR;
		if (f->tier2) c |= SRW;
		return c;
	case WGPUTextureFormat_RG16Float:
	case WGPUTextureFormat_RGB10A2Unorm:
		c |= F | R | M | RS | B;
		if (f->tier1) c |= SW | SR;
		return c;
	case WGPUTextureFormat_R8Uint:
	case WGPUTextureFormat_R8Sint:
	case WGPUTextureFormat_R16Uint:
	case WGPUTextureFormat_R16Sint:
		c |= R;
		if (f->core) c |= M;
		if (f->tier1) c |= SW | SR;
		if (f->tier2) c |= SRW;
		return c;
	case WGPUTextureFormat_RG8Uint:
	case WGPUTextureFormat_RG8Sint:
	case WGPUTextureFormat_RG16Uint:
	case WGPUTextureFormat_RG16Sint:
	case WGPUTextureFormat_RGB10A2Uint:
		c |= R;
		if (f->core) c |= M;
		if (f->tier1) c |= SW | SR;
		return c;
	case WGPUTextureFormat_R32Uint:
	case WGPUTextureFormat_R32Sint:
		return c | R | SW | SR | SRW;
	case WGPUTextureFormat_R32Float:
		c |= R | SW | SR | SRW;
		if (f->core) c |= M;
		if (f->float32_blendable) c |= B;
		if (f->float32_filterable) c |= F;
		return c;
	case WGPUTextureFormat_RGBA8Uint:
	case WGPUTextureFormat_RGBA8Sint:
		c |= R | SW | SR;
		if (f->core) c |= M;
		if (f->tier2) c |= SRW;
		return c;
	case WGPUTextureFormat_RG32Uint:
	case WGPUTextureFormat_RG32Sint:
		c |= R;
		if (f->core) c |= SW | SR;
		return c;
	case WGPUTextureFormat_RG32Float:
		c |= R;
		if (f->core) c |= SW | SR;
		if (f->float32_blendable) c |= B;
		if (f->float32_filterable) c |= F;
		return c;
	case WGPUTextureFormat_RGBA16Uint:
	case WGPUTextureFormat_RGBA16Sint:
		c |= R | SW | SR;
		if (f->core) c |= M;
		if (f->tier2) c |= SRW;
		return c;
	case WGPUTextureFormat_RGBA16Float:
		c |= F | R | SW | SR | RS | B;
		if (f->core) c |= M;
		if (f->tier2) c |= SRW;
		return c;
	case WGPUTextureFormat_RGBA32Uint:
	case WGPUTextureFormat_RGBA32Sint:
		c |= R | SW | SR;
		if (f->tier2) c |= SRW;
		return c;
	case WGPUTextureFormat_RGBA32Float:
		c |= R | SW | SR;
		if (f->tier2) c |= SRW;
		if (f->float32_blendable) c |= B;
		if (f->float32_filterable) c |= F;
		return c;
	case WGPUTextureFormat_RG11B10Ufloat:
		c |= F;
		if (f->tier1) c |= SW | SR;
		if (f->rg11b10_renderable) c |= R | M | RS | B;
		return c;
	case WGPUTextureFormat_RGB9E5Ufloat:
		return c | F;
	case WGPUTextureFormat_R16Unorm:
	case WGPUTextureFormat_RG16Unorm:
	case WGPUTextureFormat_RGBA16Unorm:
	case WGPUTextureFormat_R16Snorm:
	case WGPUTextureFormat_RG16Snorm:
	case WGPUTextureFormat_RGBA16Snorm: {
		bool unorm = wf == WGPUTextureFormat_R16Unorm || wf == WGPUTextureFormat_RG16Unorm ||
			     wf == WGPUTextureFormat_RGBA16Unorm;
		if (!f->tier1 && !(f->unorm16 && unorm))
			return 0;
		if (f->tier1) c |= R | M | B | SW | SR;
		if (f->unorm16 && unorm) c |= R | M | RS | F;
		return c;
	}
	case WGPUTextureFormat_Depth16Unorm:
	case WGPUTextureFormat_Depth24Plus:
	case WGPUTextureFormat_Depth24PlusStencil8:
	case WGPUTextureFormat_Depth32Float:
	case WGPUTextureFormat_Stencil8:
		return c | R | M;
	case WGPUTextureFormat_Depth32FloatStencil8:
		return f->depth32_stencil8 ? c | R | M : 0;
	default:
		break;
	}
	/* compressed */
	if (wf >= WGPUTextureFormat_BC1RGBAUnorm && wf <= WGPUTextureFormat_BC7RGBAUnormSrgb)
		return f->bc ? c | F : 0;
	if (wf >= WGPUTextureFormat_ETC2RGB8Unorm && wf <= WGPUTextureFormat_EACRG11Snorm)
		return f->etc2 ? c | F : 0;
	if (wf >= WGPUTextureFormat_ASTC4x4Unorm && wf <= WGPUTextureFormat_ASTC12x12UnormSrgb)
		return f->astc ? c | F : 0;
	return 0;
}

#undef R
#undef B
#undef M
#undef RS
#undef SW
#undef SR
#undef SRW
#undef F

WGPUTextureFormat
wgvk_wgpu_srgb_partner(WGPUTextureFormat f)
{
	switch (f) {
	case WGPUTextureFormat_RGBA8Unorm: return WGPUTextureFormat_RGBA8UnormSrgb;
	case WGPUTextureFormat_RGBA8UnormSrgb: return WGPUTextureFormat_RGBA8Unorm;
	case WGPUTextureFormat_BGRA8Unorm: return WGPUTextureFormat_BGRA8UnormSrgb;
	case WGPUTextureFormat_BGRA8UnormSrgb: return WGPUTextureFormat_BGRA8Unorm;
	default: break;
	}
	/* the compressed formats come in (unorm, srgb) pairs, unorm first */
	if ((f >= WGPUTextureFormat_BC1RGBAUnorm && f <= WGPUTextureFormat_BC3RGBAUnormSrgb) ||
	    (f >= WGPUTextureFormat_BC7RGBAUnorm && f <= WGPUTextureFormat_BC7RGBAUnormSrgb) ||
	    (f >= WGPUTextureFormat_ETC2RGB8Unorm && f <= WGPUTextureFormat_ETC2RGBA8UnormSrgb) ||
	    (f >= WGPUTextureFormat_ASTC4x4Unorm && f <= WGPUTextureFormat_ASTC12x12UnormSrgb)) {
		unsigned base = f >= WGPUTextureFormat_ASTC4x4Unorm ? WGPUTextureFormat_ASTC4x4Unorm :
				f >= WGPUTextureFormat_ETC2RGB8Unorm ? WGPUTextureFormat_ETC2RGB8Unorm :
				f >= WGPUTextureFormat_BC7RGBAUnorm ? WGPUTextureFormat_BC7RGBAUnorm :
								      WGPUTextureFormat_BC1RGBAUnorm;
		return (f - base) % 2 ? f - 1 : f + 1;
	}
	return WGPUTextureFormat_Undefined;
}

bool
wgvk_wgpu_format_is_depth_stencil(WGPUTextureFormat f)
{
	return f >= WGPUTextureFormat_Stencil8 && f <= WGPUTextureFormat_Depth32FloatStencil8;
}

/* Texel buffers: storage buffers decoded/encoded in the shader (T3b); the
 * plain color formats of 8, 16 and 32 bits per component and the 10:10:10:2
 * ones. */
static bool
texel_buffer_format(VkFormat vk)
{
	enum pipe_format p = vk_format_to_pipe_format(vk);
	const struct util_format_description *d = util_format_description(p);
	if (!d || d->layout != UTIL_FORMAT_LAYOUT_PLAIN || d->colorspace != UTIL_FORMAT_COLORSPACE_RGB)
		return false;
	if (vk == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || vk == VK_FORMAT_A2B10G10R10_UINT_PACK32 ||
	    vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32)
		return true;
	if (!d->is_array)
		return false;
	unsigned bits = d->channel[0].size;
	return bits == 8 || bits == 16 || bits == 32;
}

static void
compute_features(const struct wgvk_physical_device *pdev, struct wgvk_format *f, VkFormat vk)
{
	VkFormatFeatureFlags2 o = 0, buf = 0;
	if (f->vf || (f->flags & WGVK_FMT_VERTEX_PULL))
		buf |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
	if (texel_buffer_format(vk)) {
		buf |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT |
		       VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
		       VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
		if (vk == VK_FORMAT_R32_UINT || vk == VK_FORMAT_R32_SINT)
			buf |= VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;
	}
	unsigned c = f->caps;
	if (c & WGVK_CAP_SAMPLE) {
		o |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
		     VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_2_BLIT_SRC_BIT;
		if (c & WGVK_CAP_FILTER)
			o |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		if (f->flags & WGVK_FMT_DEPTH) {
			/* comparison sampling filters natively; plain linear depth sampling is a shader
			 * fix-up (T3b), which GL needs */
			o |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT |
			     VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		}
		if (f->flags & (WGVK_FMT_DEPTH | WGVK_FMT_STENCIL)) {
			if (c & WGVK_CAP_RENDER)
				o |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
		} else if ((c & WGVK_CAP_RENDER) && !(f->flags & WGVK_FMT_SWIZZLED)) {
			o |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_BLIT_DST_BIT;
			if (c & WGVK_CAP_BLEND)
				o |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT;
		}
		/* storage: the WebGPU format must be a storage format; read-write or not is the shader's
		 * business (T3: read-only / write-only / read_write access) */
		if ((c & (WGVK_CAP_STORAGE_W | WGVK_CAP_STORAGE_RW)) && !(f->flags & WGVK_FMT_SWIZZLED)) {
			o |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
			if (c & (WGVK_CAP_STORAGE_R | WGVK_CAP_STORAGE_RW))
				o |= VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT;
			/* image atomics: r32 images used atomically live in a buffer (design, M2) */
			if (vk == VK_FORMAT_R32_UINT || vk == VK_FORMAT_R32_SINT)
				o |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_ATOMIC_BIT;
		}
	}
	f->optimal = o;
	/* linear images: copies only (their bytes are the memory; T2 moves them to and from the texture) */
	f->linear = (o && !(f->flags & (WGVK_FMT_DEPTH | WGVK_FMT_STENCIL | WGVK_FMT_COMPRESSED)))
			    ? (VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT)
			    : 0;
	f->buffer = buf;
	(void)pdev;
}

#define FORMAT_TABLE_SIZE (VK_FORMAT_ASTC_12x12_SRGB_BLOCK + 1)

void
wgvk_format_table_init(struct wgvk_physical_device *pdev)
{
	memset(pdev->formats, 0, sizeof(pdev->formats));
	for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
		const struct map *m = &map[i];
		struct wgvk_format *f = &pdev->formats[m->vk];
		f->wf = m->wf;
		f->vf = m->vf;
		f->flags = m->flags;
		memcpy(f->swizzle, m->swz, sizeof(f->swizzle));
		if (m->wf != WGPUTextureFormat_Undefined) {
			f->caps = wgpu_caps(&pdev->wf, m->wf);
			if (!f->caps)
				f->wf = WGPUTextureFormat_Undefined;	/* the device lacks the feature */
		}
		/* swizzled formats need texture-component-swizzle to be sampled right */
		if ((f->flags & WGVK_FMT_SWIZZLED) && !pdev->wf.swizzle) {
			f->wf = WGPUTextureFormat_Undefined;
			f->caps = 0;
		}
		if (!f->wf && !f->vf && !(f->flags & WGVK_FMT_VERTEX_PULL))
			continue;
		enum pipe_format p = vk_format_to_pipe_format(m->vk);
		f->block_bytes = util_format_get_blocksize(p);
		f->block_w = util_format_get_blockwidth(p);
		f->block_h = util_format_get_blockheight(p);
		compute_features(pdev, f, m->vk);
	}
	/* D32S8 without depth32float-stencil8: depth24plus-stencil8, less precision */
	struct wgvk_format *d32s8 = &pdev->formats[VK_FORMAT_D32_SFLOAT_S8_UINT];
	if (d32s8->wf == WGPUTextureFormat_Undefined) {
		d32s8->wf = WGPUTextureFormat_Depth24PlusStencil8;
		d32s8->flags = WGVK_FMT_DEPTH | WGVK_FMT_STENCIL | WGVK_FMT_DEPTH_NO_COPY | WGVK_FMT_SUBSTITUTE;
		d32s8->caps = wgpu_caps(&pdev->wf, d32s8->wf);
		compute_features(pdev, d32s8, VK_FORMAT_D32_SFLOAT_S8_UINT);
	}
}

const struct wgvk_format *
wgvk_format_get(const struct wgvk_physical_device *pdev, VkFormat f)
{
	if ((unsigned)f >= FORMAT_TABLE_SIZE)
		return NULL;
	const struct wgvk_format *e = &pdev->formats[f];
	return e->wf || e->vf || (e->flags & WGVK_FMT_VERTEX_PULL) ? e : NULL;
}

/* The skeleton's map (no device): kept for wgvk_cmd.c until T2 replaces it. */
WGPUTextureFormat
wgvk_format_to_wgpu(VkFormat f, uint32_t *bpp)
{
	for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
		if (map[i].vk == f && map[i].wf) {
			*bpp = util_format_get_blocksize(vk_format_to_pipe_format(f));
			return map[i].wf;
		}
	}
	*bpp = 0;
	return WGPUTextureFormat_Undefined;
}
