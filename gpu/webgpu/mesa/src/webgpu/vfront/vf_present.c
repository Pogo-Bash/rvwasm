/*
 * vfront's present (T7, INTERFACES.md 9): a virgl resource that a window shows (its backing file handed to the
 * window server as a dma-buf: wlserver's slot format 2) is drawn by the page from zink's image of it, with no
 * readback. rvgpu asks this resolver for the texture when the page's Worker wants the frame.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rvgpu.h"
#include "vfront.h"
#include "wgvk.h"
#include "zink/zink_public.h"

static bool
vf_present_resolve(void *front, uint32_t res, struct rvgpu_present_image *out)
{
	(void)front;
	struct pipe_resource *pres = vf_res_pipe(res);
	if (!pres)
		return false;
	vf_flush_pending();	/* (uploads into it that no flush has taken yet) */
	uint64_t v = zink_resource_get_vk_image(pres);
	if (!v)
		return false;
	VkImage image;
	memcpy(&image, &v, sizeof(image));
	out->tex = wgvk_image_present_texture(image, &out->width, &out->height);
	/* Row 0 of zink's image is row 0 of the Gallium resource, which for a window's buffer is the top of the
	 * picture (Mesa's state tracker draws window framebuffers upside down for that; the backing's rows, which
	 * a readback fills, are in the same order). VIRGL_RESOURCE_Y_0_TOP only matters to vrend, whose GL
	 * textures keep rows the other way round. */
	out->flags = RVGPU_PRESENT_Y_0_TOP;
	return out->tex != NULL;
}

void vf_present_init(void);

void
vf_present_init(void)
{
	rvgpu_present_add_resolver(vf_present_resolve, NULL);
}
