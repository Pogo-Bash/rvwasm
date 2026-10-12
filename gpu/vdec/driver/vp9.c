/* SPDX-License-Identifier: MIT */
/*
 * VP9 (VP9 Bitstream & Decoding Process Specification v0.6): a chunk is the frame as VA has it (FFmpeg
 * vaapi_vp9.c and GStreamer gstvavp9dec.c pass the whole frame: uncompressed header, compressed header,
 * tiles; superframes are already split, and show_existing_frame packets never reach VA: FFmpeg vp9.c shows
 * the reference itself).
 *
 * A hidden frame (show_frame 0, an alt-ref) gives no output, yet its surface is shown later (through the
 * player's own show_existing_frame). So it is followed by a 1-byte frame of our own, show_existing_frame of
 * a slot the hidden frame refreshed (6.2: frame_marker, profile, show_existing_frame, frame_to_show_map_idx),
 * whose output is the hidden frame's picture, for its surface. libvpx does not count such a frame as shown for
 * the next frame's use_prev_frame_mvs (vp9_decoder.c: last_show_frame and prev_frame are only updated when
 * show_existing_frame is 0), so the decoding of what follows is unchanged.
 */
#include "bits.h"
#include "vdec.h"

/* refresh_frame_flags of a non-shown frame (6.2, uncompressed_header), or -1 */
static int refresh_flags(const uint8_t *p, size_t n, int *profile)
{
	struct br r;
	int frame_type, show, error_res, intra_only = 0;

	br_init(&r, p, n);
	if (br_u(&r, 2) != 2)
		return -1;
	*profile = (int)br_u(&r, 1);
	*profile |= (int)br_u(&r, 1) << 1;
	if (*profile == 3)
		br_u(&r, 1);
	if (br_u(&r, 1))
		return -1;	/* show_existing_frame */
	frame_type = (int)br_u(&r, 1);
	show = (int)br_u(&r, 1);
	error_res = (int)br_u(&r, 1);
	if (frame_type == 0)
		return 0xff;
	if (!show)
		intra_only = (int)br_u(&r, 1);
	if (!error_res)
		br_u(&r, 2);	/* reset_frame_context */
	if (intra_only) {
		if (br_u(&r, 24) != 0x498342)
			return -1;
		if (*profile > 0)
			return -1;	/* (color_config: profile 0 only here) */
	}
	return r.over ? -1 : (int)br_u(&r, 8);
}

static VAStatus pack(struct vcontext *c, struct vbuf **bufs, int nbufs, struct chunks *o)
{
	struct vbuf *pb = find_buf(bufs, nbufs, VAPictureParameterBufferType, -1);
	struct vbuf *sb = find_buf(bufs, nbufs, VASliceParameterBufferType, 0);
	struct vbuf *db = find_buf(bufs, nbufs, VASliceDataBufferType, 0);
	const VADecPictureParameterBufferVP9 *pp;
	const VASliceParameterBufferVP9 *sp;
	const uint8_t *frame;
	int key, profile = 0;

	if (!pb || !sb || !db || pb->size < sizeof(*pp) || sb->size < sizeof(*sp))
		return VA_STATUS_ERROR_INVALID_BUFFER;
	pp = (void *)pb->data;
	sp = (void *)sb->data;
	if ((uint64_t)sp->slice_data_offset + sp->slice_data_size > (uint64_t)db->size * db->num)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	if (pp->profile != 0 || pp->bit_depth != 8 || pp->pic_fields.bits.subsampling_x != 1 ||
	    pp->pic_fields.bits.subsampling_y != 1)
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	frame = db->data + sp->slice_data_offset;
	key = pp->pic_fields.bits.frame_type == 0;
	chunk_begin(o);
	if (buf_put(&o->b, frame, sp->slice_data_size))
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (pp->pic_fields.bits.show_frame) {
		chunk_end(o, key ? CHUNK_KEY : 0);
		return VA_STATUS_SUCCESS;
	}
	chunk_end(o, (key ? CHUNK_KEY : 0) | CHUNK_HIDDEN);
	{
		int refresh = refresh_flags(frame, sp->slice_data_size, &profile), idx = 0;
		uint8_t show;

		if (refresh <= 0) {
			VLOG(c->drv, "VP9: a hidden frame that refreshes no slot\n");
			return VA_STATUS_SUCCESS;
		}
		while (!(refresh & (1 << idx)))
			idx++;
		show = (uint8_t)(0x80 | (profile & 1) << 5 | (profile >> 1) << 4 | 1 << 3 | idx);
		chunk_begin(o);
		buf_byte(&o->b, show);
		chunk_end(o, 0);
	}
	return VA_STATUS_SUCCESS;
}

static void describe(struct vcontext *c, uint32_t *profile, uint32_t *level)
{
	(void)c;
	*profile = 0;
	*level = 51;
}

const struct codec codec_vp9 = { CODEC_VP9, pack, NULL, describe };
