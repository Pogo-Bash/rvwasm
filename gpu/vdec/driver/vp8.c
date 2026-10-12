/* SPDX-License-Identifier: MIT */
/*
 * VP8 (RFC 6386): a chunk is the whole frame. FFmpeg gives VA the frame without its first 3 bytes (the frame
 * tag) and on key frames the 7 after them (start code, sizes) (vaapi_vp8.c: header_size = 3 + 7 * keyframe);
 * they are put back from the parameters, as nvidia-vaapi-driver's vp8.c does: first_part_size =
 * partition_size[0] + ceil(macroblock_offset / 8) (FFmpeg: partition_size[0] = header_partition_size -
 * ceil(macroblock_offset / 8)), show_frame 1 (not in VA; a hidden golden or altref frame then gives a frame
 * too, for its surface). GStreamer (gstvavp8dec.c) leaves the whole frame in the buffer, slice_data_offset
 * past the tag: then it goes as it is.
 */
#include "vdec.h"

static VAStatus pack(struct vcontext *c, struct vbuf **bufs, int nbufs, struct chunks *o)
{
	struct vbuf *pb = find_buf(bufs, nbufs, VAPictureParameterBufferType, -1);
	struct vbuf *sb = find_buf(bufs, nbufs, VASliceParameterBufferType, 0);
	struct vbuf *db = find_buf(bufs, nbufs, VASliceDataBufferType, 0);
	const VAPictureParameterBufferVP8 *pp;
	const VASliceParameterBufferVP8 *sp;
	size_t have;
	int key;

	(void)c;
	if (!pb || !sb || !db || pb->size < sizeof(*pp) || sb->size < sizeof(*sp))
		return VA_STATUS_ERROR_INVALID_BUFFER;
	pp = (void *)pb->data;
	sp = (void *)sb->data;
	key = pp->pic_fields.bits.key_frame == 0;	/* (VA: 0 is a key frame, as the tag's frame_type) */
	have = (size_t)db->size * db->num;
	chunk_begin(o);
	if (sp->slice_data_offset >= (key ? 10u : 3u)) {
		size_t n = (size_t)sp->slice_data_offset + sp->slice_data_size;

		if (buf_put(&o->b, db->data, n < have ? n : have))
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
	} else {
		uint32_t first = sp->partition_size[0] + (sp->macroblock_offset + 7) / 8;
		uint8_t tag[10];
		size_t n = key ? 10 : 3;

		if ((uint64_t)sp->slice_data_offset + sp->slice_data_size > have)
			return VA_STATUS_ERROR_INVALID_BUFFER;
		tag[0] = (uint8_t)((key ? 0 : 1) | (pp->pic_fields.bits.version << 1) | (1 << 4) | ((first & 7) << 5));
		tag[1] = (uint8_t)(first >> 3);
		tag[2] = (uint8_t)(first >> 11);
		if (key) {
			tag[3] = 0x9d;
			tag[4] = 0x01;
			tag[5] = 0x2a;
			tag[6] = (uint8_t)pp->frame_width;
			tag[7] = (uint8_t)((pp->frame_width >> 8) & 0x3f);	/* (horizontal_scale 0) */
			tag[8] = (uint8_t)pp->frame_height;
			tag[9] = (uint8_t)((pp->frame_height >> 8) & 0x3f);
		}
		if (buf_put(&o->b, tag, n) || buf_put(&o->b, db->data + sp->slice_data_offset, sp->slice_data_size))
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	chunk_end(o, key ? CHUNK_KEY : 0);
	return VA_STATUS_SUCCESS;
}

const struct codec codec_vp8 = { CODEC_VP8, pack, NULL, NULL };
