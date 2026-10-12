/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * H.264: VA's picture, IQ matrix and slice buffers into an Annex B access unit (ITU-T H.264 (08/2021)).
 *
 * The slices go as they came (VA's slice data is the whole NAL unit: header byte, slice header, data, with its
 * emulation prevention bytes, no start code: libva va.h VASliceParameterBufferH264, FFmpeg h264dec.c
 * decode_slice(nal->raw_data)). What VA does not carry are the parameter sets; they are made here from the
 * fields VA has (7.3.2.1, 7.3.2.2), which are the ones the slice header's syntax and the decoding depend on:
 *
 * - SPS 0: High profile (100) for every VA profile (a superset of Constrained Baseline and Main for what VA
 *   can carry), level the smallest whose MaxFS and MaxDpbMbs hold the picture and its references (A.3.1,
 *   Table A-1), no cropping (a VA surface has whole macroblocks; the player crops), and a VUI with the
 *   colour (BT.709, full range: what the page tells the browser too) and bitstream_restriction with
 *   max_num_reorder_frames 0, so that a decoder gives each picture as soon as it is decoded (the player
 *   reorders; it waits in vaSyncSurface for that very picture).
 * - PPS with the slice's own pic_parameter_set_id; num_ref_idx_lX_default_active_minus1 from a slice that
 *   does not override them (read from its header); explicit scaling lists from the IQ buffer (VA: raster
 *   order; the bitstream: zig-zag, 7.3.2.1.1.1) unless they are all flat.
 *
 * A key chunk (IDR) carries the SPS and every PPS; other chunks a PPS that is new or changed.
 * Not done (the picture fails, the player falls back): pic_order_cnt_type 1 (its offsets are not in VA),
 * fields and MBAFF (frame_mbs_only_flag 0), other than 4:2:0 8-bit.
 */
#include <stdlib.h>
#include <string.h>

#include "bits.h"
#include "vdec.h"

struct pps_state {
	int known;			/* sent with these bytes */
	uint8_t bytes[1024];
	size_t n;
	int have_l0, have_l1;		/* num_ref_idx defaults learnt */
	uint8_t l0, l1;
	int used;			/* ever used */
};

struct h264 {
	uint8_t sps[256];
	size_t sps_n;
	int sps_sent;
	uint32_t level;
	struct pps_state pps[256];
};

static const uint8_t zz4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t zz8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
	28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
	54, 47, 55, 62, 63,
};

/* Table A-1: level_idc, MaxFS, MaxDpbMbs */
static const struct { uint32_t level, max_fs, max_dpb_mbs; } levels[] = {
	{ 30, 1620, 8100 }, { 31, 3600, 18000 }, { 32, 5120, 20480 }, { 40, 8192, 32768 }, { 42, 8704, 34816 },
	{ 50, 22080, 110400 }, { 51, 36864, 184320 }, { 60, 139264, 696320 },
};

static uint32_t level_for(uint32_t mbs, uint32_t refs, uint32_t w_mbs, uint32_t h_mbs)
{
	if (refs < 1)
		refs = 1;
	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		uint32_t side = 0;

		/* (A.3.1 h, i: each side at most sqrt(8 * MaxFS) macroblocks) */
		while ((side + 1) * (side + 1) <= 8 * levels[i].max_fs)
			side++;
		if (mbs <= levels[i].max_fs && w_mbs <= side && h_mbs <= side && levels[i].max_dpb_mbs / mbs >= refs)
			return levels[i].level;
	}
	return 62;
}

static void scaling_list(struct bw *w, const uint8_t *raster, const uint8_t *zz, int n)
{
	int last = 8;

	for (int j = 0; j < n; j++) {
		int v = raster[zz[j]];
		int delta = ((v - last + 128) & 255) - 128;

		bw_se(w, delta);
		last = v;
	}
}

static void make_sps(struct h264 *h, const VAPictureParameterBufferH264 *pp)
{
	struct bw w;
	uint32_t w_mbs = pp->picture_width_in_mbs_minus1 + 1u, h_mbs = pp->picture_height_in_mbs_minus1 + 1u;

	h->level = level_for(w_mbs * h_mbs, pp->num_ref_frames, w_mbs, h_mbs);
	bw_init(&w);
	bw_u(&w, 0x67, 8);			/* nal_ref_idc 3, nal_unit_type 7 */
	bw_u(&w, 100, 8);			/* profile_idc: High */
	bw_u(&w, 0, 8);				/* constraint_set0..5, reserved_zero_2bits */
	bw_u(&w, h->level, 8);
	bw_ue(&w, 0);				/* seq_parameter_set_id */
	bw_ue(&w, pp->seq_fields.bits.chroma_format_idc);
	bw_ue(&w, pp->bit_depth_luma_minus8);
	bw_ue(&w, pp->bit_depth_chroma_minus8);
	bw_flag(&w, 0);				/* qpprime_y_zero_transform_bypass_flag */
	bw_flag(&w, 0);				/* seq_scaling_matrix_present_flag (the PPS has them) */
	bw_ue(&w, pp->seq_fields.bits.log2_max_frame_num_minus4);
	bw_ue(&w, pp->seq_fields.bits.pic_order_cnt_type);
	if (pp->seq_fields.bits.pic_order_cnt_type == 0)
		bw_ue(&w, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
	bw_ue(&w, pp->num_ref_frames);
	bw_flag(&w, pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
	bw_ue(&w, w_mbs - 1);
	bw_ue(&w, h_mbs - 1);			/* pic_height_in_map_units_minus1 (frames only) */
	bw_flag(&w, 1);				/* frame_mbs_only_flag */
	bw_flag(&w, pp->seq_fields.bits.direct_8x8_inference_flag);
	bw_flag(&w, 0);				/* frame_cropping_flag */
	bw_flag(&w, 1);				/* vui_parameters_present_flag */
	/* VUI (E.1.1) */
	bw_flag(&w, 0);				/* aspect_ratio_info_present_flag */
	bw_flag(&w, 0);				/* overscan_info_present_flag */
	bw_flag(&w, 1);				/* video_signal_type_present_flag */
	bw_u(&w, 5, 3);				/* video_format: unspecified */
	bw_flag(&w, 1);				/* video_full_range_flag */
	bw_flag(&w, 1);				/* colour_description_present_flag */
	bw_u(&w, 1, 8);				/* colour_primaries: BT.709 */
	bw_u(&w, 1, 8);				/* transfer_characteristics */
	bw_u(&w, 1, 8);				/* matrix_coefficients */
	bw_flag(&w, 0);				/* chroma_loc_info_present_flag */
	bw_flag(&w, 0);				/* timing_info_present_flag */
	bw_flag(&w, 0);				/* nal_hrd_parameters_present_flag */
	bw_flag(&w, 0);				/* vcl_hrd_parameters_present_flag */
	bw_flag(&w, 0);				/* pic_struct_present_flag */
	bw_flag(&w, 1);				/* bitstream_restriction_flag */
	bw_flag(&w, 1);				/* motion_vectors_over_pic_boundaries_flag */
	bw_ue(&w, 0);				/* max_bytes_per_pic_denom */
	bw_ue(&w, 0);				/* max_bits_per_mb_denom */
	bw_ue(&w, 15);				/* log2_max_mv_length_horizontal */
	bw_ue(&w, 15);				/* log2_max_mv_length_vertical */
	bw_ue(&w, 0);				/* max_num_reorder_frames */
	bw_ue(&w, pp->num_ref_frames ? pp->num_ref_frames : 1);	/* max_dec_frame_buffering */
	bw_trailing(&w);
	h->sps_n = bw_bytes(&w);
	memcpy(h->sps, w.p, h->sps_n);
}

static int flat(const VAIQMatrixBufferH264 *iq, int with8x8)
{
	for (int i = 0; i < 6; i++)
		for (int j = 0; j < 16; j++)
			if (iq->ScalingList4x4[i][j] != 16)
				return 0;
	if (with8x8)
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 64; j++)
				if (iq->ScalingList8x8[i][j] != 16)
					return 0;
	return 1;
}

static void make_pps(struct pps_state *s, uint32_t id, const VAPictureParameterBufferH264 *pp,
		     const VAIQMatrixBufferH264 *iq, uint8_t *out, size_t *n)
{
	struct bw w;
	int t8 = pp->pic_fields.bits.transform_8x8_mode_flag;

	bw_init(&w);
	bw_u(&w, 0x68, 8);			/* nal_ref_idc 3, nal_unit_type 8 */
	bw_ue(&w, id);
	bw_ue(&w, 0);				/* seq_parameter_set_id */
	bw_flag(&w, pp->pic_fields.bits.entropy_coding_mode_flag);
	bw_flag(&w, pp->pic_fields.bits.pic_order_present_flag);
	bw_ue(&w, 0);				/* num_slice_groups_minus1 */
	bw_ue(&w, s->have_l0 ? s->l0 : 0);
	bw_ue(&w, s->have_l1 ? s->l1 : 0);
	bw_flag(&w, pp->pic_fields.bits.weighted_pred_flag);
	bw_u(&w, pp->pic_fields.bits.weighted_bipred_idc, 2);
	bw_se(&w, pp->pic_init_qp_minus26);
	bw_se(&w, pp->pic_init_qs_minus26);
	bw_se(&w, pp->chroma_qp_index_offset);
	bw_flag(&w, pp->pic_fields.bits.deblocking_filter_control_present_flag);
	bw_flag(&w, pp->pic_fields.bits.constrained_intra_pred_flag);
	bw_flag(&w, pp->pic_fields.bits.redundant_pic_cnt_present_flag);
	bw_flag(&w, t8);
	if (!iq || flat(iq, t8)) {
		bw_flag(&w, 0);			/* pic_scaling_matrix_present_flag */
	} else {
		bw_flag(&w, 1);
		for (int i = 0; i < 6 + 2 * t8; i++) {
			bw_flag(&w, 1);		/* pic_scaling_list_present_flag[i] */
			if (i < 6)
				scaling_list(&w, iq->ScalingList4x4[i], zz4, 16);
			else
				scaling_list(&w, iq->ScalingList8x8[i - 6], zz8, 64);
		}
	}
	bw_se(&w, pp->second_chroma_qp_index_offset);
	bw_trailing(&w);
	*n = bw_bytes(&w);
	memcpy(out, w.p, *n);
}

/* What a slice header says up to num_ref_idx_active_override_flag (7.3.3). */
struct slice_head {
	uint32_t nal_type, slice_type, pps_id;
	int override, l0, l1, ok;
};

static void read_slice(const VAPictureParameterBufferH264 *pp, const VASliceParameterBufferH264 *sp,
		       const uint8_t *nal, size_t n, struct slice_head *out)
{
	uint8_t rbsp[96];
	struct br r;
	uint32_t type;

	memset(out, 0, sizeof(*out));
	if (!n)
		return;
	br_init(&r, rbsp, nal_unescape(nal, n < sizeof(rbsp) ? n : sizeof(rbsp), rbsp));
	out->nal_type = br_u(&r, 8) & 0x1f;
	br_ue(&r);				/* first_mb_in_slice */
	type = br_ue(&r) % 5;
	out->slice_type = type;
	out->pps_id = br_ue(&r);
	/* (separate_colour_plane_flag: 4:4:4 only, not here) */
	br_u(&r, pp->seq_fields.bits.log2_max_frame_num_minus4 + 4);	/* frame_num */
	/* (frame_mbs_only_flag is 1 here: no field_pic_flag) */
	if (out->nal_type == 5)
		br_ue(&r);			/* idr_pic_id */
	if (pp->seq_fields.bits.pic_order_cnt_type == 0) {
		br_u(&r, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pp->pic_fields.bits.pic_order_present_flag)
			br_se(&r);		/* delta_pic_order_cnt_bottom */
	}
	if (pp->pic_fields.bits.redundant_pic_cnt_present_flag)
		br_ue(&r);
	if (type == 1)
		br_u(&r, 1);			/* direct_spatial_mv_pred_flag */
	if (type == 0 || type == 3 || type == 1) {
		out->override = (int)br_u(&r, 1);
		out->l0 = sp->num_ref_idx_l0_active_minus1;
		out->l1 = sp->num_ref_idx_l1_active_minus1;
	} else {
		out->override = 1;		/* (I and SI slices say nothing about the defaults) */
	}
	out->ok = !r.over && out->pps_id < 256;
}

static VAStatus pack(struct vcontext *c, struct vbuf **bufs, int nbufs, struct chunks *o)
{
	struct h264 *h = c->state;
	struct vbuf *pb = find_buf(bufs, nbufs, VAPictureParameterBufferType, -1);
	struct vbuf *qb = find_buf(bufs, nbufs, VAIQMatrixBufferType, -1);
	const VAPictureParameterBufferH264 *pp;
	const VAIQMatrixBufferH264 *iq = qb && qb->size >= sizeof(VAIQMatrixBufferH264) ? (void *)qb->data : NULL;
	uint8_t sps_was[256];
	size_t sps_was_n;
	int key = 0, nslices = 0, pps_used[256];
	uint8_t bytes[1024];
	size_t n;

	if (!h) {
		h = c->state = calloc(1, sizeof(*h));
		if (!h)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	if (!pb || pb->size < sizeof(VAPictureParameterBufferH264))
		return VA_STATUS_ERROR_INVALID_BUFFER;
	pp = (void *)pb->data;
	if (!pp->seq_fields.bits.frame_mbs_only_flag || pp->pic_fields.bits.field_pic_flag) {
		vlog(c->drv, "H.264: fields and MBAFF are not done (frame_mbs_only_flag 0)\n");
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}
	if (pp->seq_fields.bits.pic_order_cnt_type == 1) {
		vlog(c->drv, "H.264: pic_order_cnt_type 1 is not done (its offsets are not in VA's parameters)\n");
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	}
	if (pp->seq_fields.bits.chroma_format_idc != 1 || pp->bit_depth_luma_minus8 || pp->bit_depth_chroma_minus8)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	/* The slices' headers: which PPS, whether IDR, the defaults they show. */
	memset(pps_used, 0, sizeof(pps_used));
	for (int i = 0, k = 0; i < nbufs; i++) {
		struct vbuf *spb = bufs[i], *db;

		if (spb->type != VASliceParameterBufferType)
			continue;
		db = find_buf(bufs + i, nbufs - i, VASliceDataBufferType, 0);
		if (!db || spb->size < sizeof(VASliceParameterBufferH264))
			return VA_STATUS_ERROR_INVALID_BUFFER;
		for (uint32_t e = 0; e < spb->num; e++, k++) {
			const VASliceParameterBufferH264 *sp = (void *)(spb->data + e * spb->size);
			struct slice_head sh;
			struct pps_state *ps;

			if ((uint64_t)sp->slice_data_offset + sp->slice_data_size > (uint64_t)db->size * db->num ||
			    sp->slice_data_flag != VA_SLICE_DATA_FLAG_ALL)
				return VA_STATUS_ERROR_INVALID_BUFFER;
			read_slice(pp, sp, db->data + sp->slice_data_offset, sp->slice_data_size, &sh);
			if (!sh.ok)
				return VA_STATUS_ERROR_DECODING_ERROR;
			if (k == 0)
				key = sh.nal_type == 5;
			ps = &h->pps[sh.pps_id];
			pps_used[sh.pps_id] = 1;
			if (!sh.override) {
				ps->have_l0 = 1;
				ps->l0 = (uint8_t)sh.l0;
				if (sh.slice_type == 1) {
					ps->have_l1 = 1;
					ps->l1 = (uint8_t)sh.l1;
				}
			}
			nslices++;
		}
	}
	if (!nslices)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	chunk_begin(o);
	sps_was_n = h->sps_n;
	memcpy(sps_was, h->sps, h->sps_n);
	make_sps(h, pp);
	if (key || !h->sps_sent || sps_was_n != h->sps_n || memcmp(sps_was, h->sps, h->sps_n)) {
		nal_put(&o->b, h->sps, h->sps_n);
		h->sps_sent = 1;
	}
	for (int id = 0; id < 256; id++) {
		struct pps_state *ps = &h->pps[id];

		if (pps_used[id]) {
			make_pps(ps, (uint32_t)id, pp, iq, bytes, &n);
			ps->used = 1;
			if (key || !ps->known || n != ps->n || memcmp(bytes, ps->bytes, n)) {
				nal_put(&o->b, bytes, n);
				memcpy(ps->bytes, bytes, n);
				ps->n = n;
				ps->known = 1;
			}
		} else if (key && ps->known) {
			/* (a decoder started again at this key chunk must know every PPS) */
			nal_put(&o->b, ps->bytes, ps->n);
		}
	}
	for (int i = 0; i < nbufs; i++) {
		struct vbuf *spb = bufs[i], *db;

		if (spb->type != VASliceParameterBufferType)
			continue;
		db = find_buf(bufs + i, nbufs - i, VASliceDataBufferType, 0);
		for (uint32_t e = 0; e < spb->num; e++) {
			const VASliceParameterBufferH264 *sp = (void *)(spb->data + e * spb->size);
			static const uint8_t start[4] = { 0, 0, 0, 1 };

			if (buf_put(&o->b, start, 4) ||
			    buf_put(&o->b, db->data + sp->slice_data_offset, sp->slice_data_size))
				return VA_STATUS_ERROR_ALLOCATION_FAILED;
		}
	}
	chunk_end(o, key ? CHUNK_KEY : 0);
	return VA_STATUS_SUCCESS;
}

static void destroy(struct vcontext *c)
{
	free(c->state);
	c->state = NULL;
}

static void describe(struct vcontext *c, uint32_t *profile, uint32_t *level)
{
	struct h264 *h = c->state;

	*profile = 100;
	*level = h ? h->level : 40;
}

const struct codec codec_h264 = { CODEC_H264, pack, destroy, describe };
