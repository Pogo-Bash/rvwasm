/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * AV1: VA's buffers into a temporal unit (AV1 Bitstream & Decoding Process Specification, 1.0.0 with Errata 1).
 *
 * VA has no headers at all: FFmpeg gives it the tile data of each tile group (av1dec.c: the tile group OBU's
 * payload after its header, with the tile sizes in it; vaapi_av1.c: each tile's offset and size) and the frame
 * header's values (VADecPictureParameterBufferAV1, from CBS's parsed header, its inferred values filled in). So a
 * chunk is made whole: a temporal delimiter, a sequence header (with key frames), a frame header OBU written from
 * VA's values (5.9), and the tile group OBUs (5.11.1) around the tile data as it came.
 *
 * One value of the frame header is not in VA: refresh_frame_flags, which slots the frame goes into (the next
 * frame's ref_frame_map would tell, too late for a player that waits for this one first). It is not needed: the
 * decoder's slots are ours. Every frame goes into a slot of ours (the shown key frame into all), chosen among those
 * whose picture the stream itself no longer keeps (VA's ref_frame_map says what it keeps), and each frame's
 * ref_frame_idx[] is written to name our slots that hold the pictures VA's ref_frame_map[ref_frame_idx[]] names.
 * What a slot carries besides the picture (order hint, CDFs, loop filter deltas, segmentation, global motion,
 * film grain) travels with it, so the decoding is the stream's. (With eight distinct pictures kept and a ninth
 * coming, one has to go: the one with the oldest order hint not referenced now; a later reference to it fails.)
 *
 * A hidden frame (show_frame 0) gives no output; if it is showable, a frame header of our own showing it
 * (show_existing_frame, 7.21) follows at once, whose output is the hidden frame's picture for its surface: the
 * player shows it later through FFmpeg's own show_existing_frame, which never reaches VA. A frame's picture is
 * the display surface's (current_display_picture: with film grain FFmpeg decodes into another surface, the
 * reference without grain, and wants the grained picture in this one; the page's decoder applies film grain).
 *
 * Choices that do not change the decoding: no frame ids, frame_size_override_flag 1 with the size explicit,
 * render size = frame size, explicit references (no short signaling), all loop filter deltas sent (VA's values are
 * the ones in effect), update_grain 1, the sequence header's enable_* flags on where VA says per frame what is used
 * (warped motion, ref frame MVs, superres, restoration, screen content tools, integer MV: SELECT) and
 * separate_uv_delta_q 1.
 */
#include <stdlib.h>
#include <string.h>

#include "bits.h"
#include "vdec.h"

#define OBU_SEQUENCE_HEADER 1
#define OBU_TEMPORAL_DELIMITER 2
#define OBU_FRAME_HEADER 3
#define OBU_TILE_GROUP 4
#define KEY_FRAME 0
#define INTER_FRAME 1
#define INTRA_ONLY_FRAME 2
#define SWITCH_FRAME 3
#define PRIMARY_REF_NONE 7
#define WARPEDMODEL_PREC_BITS 16

struct slot {
	int valid;
	VASurfaceID surface;		/* the reference picture (VA's current_frame) */
	VASurfaceID display;		/* where its picture goes when shown */
	uint8_t order_hint, frame_type, showable;
	int32_t gm[7][6];		/* its global motion parameters (PrevGmParams of frames that load from it) */
};

struct av1 {
	struct slot slots[8];
	uint8_t seq[64];
	size_t seq_n;
	int seq_sent;
	uint32_t level;
};

/* ns(n) (4.10.7) */
static void bw_ns(struct bw *w, uint32_t n, uint32_t v)
{
	int wbits = 0;
	uint32_t m;

	while ((n >> wbits) > 1)
		wbits++;
	wbits++;
	m = (1u << wbits) - n;
	if (v < m) {
		bw_u(w, v, wbits - 1);
	} else {
		v += m;
		bw_u(w, v >> 1, wbits - 1);
		bw_u(w, v & 1, 1);
	}
}

static void bw_su(struct bw *w, int32_t v, int n)
{
	bw_u(w, (uint32_t)v & ((1u << n) - 1), n);
}

/* encode_subexp, the inverse of decode_subexp (5.9.26) */
static void bw_subexp(struct bw *w, uint32_t num, uint32_t v)
{
	uint32_t mk = 0, k = 3;

	for (int i = 0;; i++) {
		uint32_t b2 = i ? k + i - 1 : k, a = 1u << b2;

		if (num <= mk + 3 * a) {
			bw_ns(w, num - mk, v - mk);
			return;
		}
		if (v >= mk + a) {
			bw_u(w, 1, 1);
			mk += a;
		} else {
			bw_u(w, 0, 1);
			bw_u(w, v - mk, (int)b2);
			return;
		}
	}
}

/* recenter, the inverse of inverse_recenter (5.9.27) */
static uint32_t recenter(int32_t r, int32_t x)
{
	if (x > 2 * r)
		return (uint32_t)x;
	if (x >= r)
		return (uint32_t)((x - r) * 2);
	return (uint32_t)((r - x) * 2 - 1);
}

/* the inverse of decode_signed_subexp_with_ref (5.9.24, 5.9.25) */
static void bw_signed_subexp_ref(struct bw *w, int32_t low, int32_t high, int32_t r, int32_t x)
{
	int32_t mx = high - low;

	r -= low;
	x -= low;
	if ((r << 1) <= mx)
		bw_subexp(w, (uint32_t)mx, recenter(r, x));
	else
		bw_subexp(w, (uint32_t)mx, recenter(mx - 1 - r, mx - 1 - x));
}

static int tile_log2(int blk, int target)
{
	int k = 0;

	while ((blk << k) < target)
		k++;
	return k;
}

static void obu(struct buf *b, int type, const uint8_t *p, size_t n)
{
	uint8_t h[9];
	size_t k;

	h[0] = (uint8_t)(type << 3 | 1 << 1);	/* obu_has_size_field */
	k = 1 + leb128(h + 1, n);
	buf_put(b, h, k);
	buf_put(b, p, n);
}

static void make_seq(struct av1 *a, const VADecPictureParameterBufferAV1 *pp, uint32_t max_w, uint32_t max_h)
{
	struct bw w;
	uint64_t size = (uint64_t)max_w * max_h;

	a->level = size <= 2359296 ? 9 : size <= 8912896 ? 13 : 17;	/* 4.1, 5.1, 6.1 */
	bw_init(&w);
	bw_u(&w, pp->profile, 3);			/* seq_profile */
	bw_flag(&w, pp->seq_info_fields.fields.still_picture);
	bw_flag(&w, 0);					/* reduced_still_picture_header */
	bw_flag(&w, 0);					/* timing_info_present_flag */
	bw_flag(&w, 0);					/* initial_display_delay_present_flag */
	bw_u(&w, 0, 5);					/* operating_points_cnt_minus_1 */
	bw_u(&w, 0, 12);				/* operating_point_idc[0] */
	bw_u(&w, a->level, 5);
	if (a->level > 7)
		bw_flag(&w, 0);				/* seq_tier */
	bw_u(&w, 15, 4);				/* frame_width_bits_minus_1 */
	bw_u(&w, 15, 4);
	bw_u(&w, max_w - 1, 16);
	bw_u(&w, max_h - 1, 16);
	bw_flag(&w, 0);					/* frame_id_numbers_present_flag */
	bw_flag(&w, pp->seq_info_fields.fields.use_128x128_superblock);
	bw_flag(&w, pp->seq_info_fields.fields.enable_filter_intra);
	bw_flag(&w, pp->seq_info_fields.fields.enable_intra_edge_filter);
	bw_flag(&w, pp->seq_info_fields.fields.enable_interintra_compound);
	bw_flag(&w, pp->seq_info_fields.fields.enable_masked_compound);
	bw_flag(&w, 1);					/* enable_warped_motion (allow_warped_motion says) */
	bw_flag(&w, pp->seq_info_fields.fields.enable_dual_filter);
	bw_flag(&w, pp->seq_info_fields.fields.enable_order_hint);
	if (pp->seq_info_fields.fields.enable_order_hint) {
		bw_flag(&w, pp->seq_info_fields.fields.enable_jnt_comp);
		bw_flag(&w, 1);				/* enable_ref_frame_mvs (use_ref_frame_mvs says) */
	}
	bw_flag(&w, 1);					/* seq_choose_screen_content_tools */
	bw_flag(&w, 1);					/* seq_choose_integer_mv */
	if (pp->seq_info_fields.fields.enable_order_hint)
		bw_u(&w, pp->order_hint_bits_minus_1, 3);
	bw_flag(&w, 1);					/* enable_superres (use_superres says) */
	bw_flag(&w, pp->seq_info_fields.fields.enable_cdef);
	bw_flag(&w, 1);					/* enable_restoration (lr_type says) */
	/* color_config (5.5.2): 8 bit 4:2:0 */
	bw_flag(&w, 0);					/* high_bitdepth */
	bw_flag(&w, pp->seq_info_fields.fields.mono_chrome);
	bw_flag(&w, 0);					/* color_description_present_flag */
	bw_flag(&w, pp->seq_info_fields.fields.color_range);
	if (!pp->seq_info_fields.fields.mono_chrome) {
		bw_u(&w, 0, 2);				/* chroma_sample_position: unknown */
		bw_flag(&w, 1);				/* separate_uv_delta_q */
	}
	bw_flag(&w, pp->seq_info_fields.fields.film_grain_params_present);
	bw_trailing(&w);
	a->seq_n = bw_bytes(&w);
	memcpy(a->seq, w.p, a->seq_n);
}

static int32_t relative_dist(const VADecPictureParameterBufferAV1 *pp, int32_t x, int32_t y)
{
	int32_t bits = pp->order_hint_bits_minus_1 + 1, diff = x - y, m = 1 << (bits - 1);

	if (!pp->seq_info_fields.fields.enable_order_hint)
		return 0;
	return (diff & (m - 1)) - (diff & m);
}

/* Whether skip_mode_present is coded (5.9.22): from the references' order hints */
static int skip_mode_allowed(const VADecPictureParameterBufferAV1 *pp, const uint8_t hints[7], int intra)
{
	int fwd = -1, bwd = -1, fwd2 = -1;
	int32_t fh = 0, bh = 0, f2h = 0, cur = pp->order_hint;

	if (intra || !pp->mode_control_fields.bits.reference_select || !pp->seq_info_fields.fields.enable_order_hint)
		return 0;
	for (int i = 0; i < 7; i++) {
		int32_t h = hints[i];

		if (relative_dist(pp, h, cur) < 0) {
			if (fwd < 0 || relative_dist(pp, h, fh) > 0) {
				fwd = i;
				fh = h;
			}
		} else if (relative_dist(pp, h, cur) > 0) {
			if (bwd < 0 || relative_dist(pp, h, bh) < 0) {
				bwd = i;
				bh = h;
			}
		}
	}
	if (fwd < 0)
		return 0;
	if (bwd >= 0)
		return 1;
	for (int i = 0; i < 7; i++) {
		int32_t h = hints[i];

		if (relative_dist(pp, h, fh) < 0 && (fwd2 < 0 || relative_dist(pp, h, f2h) > 0)) {
			fwd2 = i;
			f2h = h;
		}
	}
	return fwd2 >= 0;
}

static void gm_defaults(int32_t gm[7][6])
{
	for (int r = 0; r < 7; r++)
		for (int i = 0; i < 6; i++)
			gm[r][i] = i % 3 == 2 ? 1 << WARPEDMODEL_PREC_BITS : 0;
}

/* global_motion_params (5.9.24): VA's parameters, coded against the primary reference frame's */
static void global_motion(struct bw *w, const VADecPictureParameterBufferAV1 *pp, int32_t prev[7][6], int32_t out[7][6])
{
	int hp = pp->pic_info_fields.bits.allow_high_precision_mv;

	gm_defaults(out);
	for (int ref = 0; ref < 7; ref++) {
		const VAWarpedMotionParamsAV1 *m = &pp->wm[ref];
		int type = (int)m->wmtype;
		static const int order[6] = { 2, 3, 4, 5, 0, 1 };

		bw_flag(w, type != 0);			/* is_global */
		if (type) {
			bw_flag(w, type == 2);		/* is_rot_zoom */
			if (type != 2)
				bw_flag(w, type == 1);	/* is_translation */
		}
		for (int k = 0; k < 6; k++) {
			int idx = order[k], abs_bits = 12, prec_bits = 15, prec_diff, round, sub;
			int32_t mx, r, x;

			if ((idx >= 2 && type < 2) || (idx >= 4 && type != 3) || (idx < 2 && type < 1))
				continue;
			if (idx < 2) {
				if (type == 1) {
					abs_bits = 9 - !hp;
					prec_bits = 3 - !hp;
				} else {
					abs_bits = 12;
					prec_bits = 6;
				}
			}
			prec_diff = WARPEDMODEL_PREC_BITS - prec_bits;
			round = idx % 3 == 2 ? 1 << WARPEDMODEL_PREC_BITS : 0;
			sub = idx % 3 == 2 ? 1 << prec_bits : 0;
			mx = 1 << abs_bits;
			r = (prev[ref][idx] >> prec_diff) - sub;
			x = (m->wmmat[idx] - round) >> prec_diff;
			bw_signed_subexp_ref(w, -mx, mx + 1, r, x);
			out[ref][idx] = (x << prec_diff) + round;
		}
		if (type == 2) {
			out[ref][4] = -out[ref][3];
			out[ref][5] = out[ref][2];
		}
	}
}

static void film_grain(struct bw *w, const VADecPictureParameterBufferAV1 *pp, int frame_type)
{
	const VAFilmGrainStructAV1 *g = &pp->film_grain_info;
	int mono = pp->seq_info_fields.fields.mono_chrome, csfl = g->film_grain_info_fields.bits.chroma_scaling_from_luma;
	int lag = g->film_grain_info_fields.bits.ar_coeff_lag, pos = 2 * lag * (lag + 1), pos_c;

	bw_flag(w, g->film_grain_info_fields.bits.apply_grain);
	if (!g->film_grain_info_fields.bits.apply_grain)
		return;
	bw_u(w, g->grain_seed, 16);
	if (frame_type == INTER_FRAME)
		bw_flag(w, 1);				/* update_grain */
	bw_u(w, g->num_y_points, 4);
	for (int i = 0; i < g->num_y_points; i++) {
		bw_u(w, g->point_y_value[i], 8);
		bw_u(w, g->point_y_scaling[i], 8);
	}
	if (!mono)
		bw_flag(w, csfl);
	if (!(mono || csfl || g->num_y_points == 0)) {
		bw_u(w, g->num_cb_points, 4);
		for (int i = 0; i < g->num_cb_points; i++) {
			bw_u(w, g->point_cb_value[i], 8);
			bw_u(w, g->point_cb_scaling[i], 8);
		}
		bw_u(w, g->num_cr_points, 4);
		for (int i = 0; i < g->num_cr_points; i++) {
			bw_u(w, g->point_cr_value[i], 8);
			bw_u(w, g->point_cr_scaling[i], 8);
		}
	}
	bw_u(w, g->film_grain_info_fields.bits.grain_scaling_minus_8, 2);
	bw_u(w, (uint32_t)lag, 2);
	pos_c = pos;
	if (g->num_y_points) {
		pos_c = pos + 1;
		for (int i = 0; i < pos; i++)
			bw_u(w, (uint32_t)(g->ar_coeffs_y[i] + 128), 8);
	}
	if (csfl || g->num_cb_points)
		for (int i = 0; i < pos_c; i++)
			bw_u(w, (uint32_t)(g->ar_coeffs_cb[i] + 128), 8);
	if (csfl || g->num_cr_points)
		for (int i = 0; i < pos_c; i++)
			bw_u(w, (uint32_t)(g->ar_coeffs_cr[i] + 128), 8);
	bw_u(w, g->film_grain_info_fields.bits.ar_coeff_shift_minus_6, 2);
	bw_u(w, g->film_grain_info_fields.bits.grain_scale_shift, 2);
	if (g->num_cb_points) {
		bw_u(w, g->cb_mult, 8);
		bw_u(w, g->cb_luma_mult, 8);
		bw_u(w, g->cb_offset, 9);
	}
	if (g->num_cr_points) {
		bw_u(w, g->cr_mult, 8);
		bw_u(w, g->cr_luma_mult, 8);
		bw_u(w, g->cr_offset, 9);
	}
	bw_flag(w, g->film_grain_info_fields.bits.overlap_flag);
	bw_flag(w, g->film_grain_info_fields.bits.clip_to_restricted_range);
}

/* Is surface `s` kept by the stream now (VA's ref_frame_map of this frame)? */
static int kept(const VADecPictureParameterBufferAV1 *pp, VASurfaceID s)
{
	for (int i = 0; i < 8; i++)
		if (pp->ref_frame_map[i] == s)
			return 1;
	return 0;
}

static VAStatus pack(struct vcontext *c, struct vbuf **bufs, int nbufs, struct chunks *o)
{
	struct av1 *a = c->state;
	struct vbuf *pb = find_buf(bufs, nbufs, VAPictureParameterBufferType, -1);
	const VADecPictureParameterBufferAV1 *pp;
	int ft, show, intra, err_res, primary, ours[7] = { 0 }, slot, shown_key;
	uint8_t hints[7] = { 0 };
	uint32_t refresh;
	int32_t prev[7][6], gm_out[7][6];
	VASurfaceID cur, disp;
	struct bw w;
	int mi_cols, mi_rows, sb_cols, sb_rows, sb_shift, sb128, cols_log2 = 0, rows_log2 = 0, tile_bytes = 4;
	int coded_lossless = 1, upscaled_w, frame_w, frame_h;

	if (!a) {
		a = c->state = calloc(1, sizeof(*a));
		if (!a)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	if (!pb || pb->size < sizeof(VADecPictureParameterBufferAV1))
		return VA_STATUS_ERROR_INVALID_BUFFER;
	pp = (void *)pb->data;
	if (pp->profile != 0 || pp->bit_depth_idx != 0 || pp->pic_info_fields.bits.large_scale_tile ||
	    pp->anchor_frames_num)
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	ft = pp->pic_info_fields.bits.frame_type;
	show = pp->pic_info_fields.bits.show_frame;
	intra = ft == KEY_FRAME || ft == INTRA_ONLY_FRAME;
	shown_key = ft == KEY_FRAME && show;
	err_res = ft == SWITCH_FRAME || shown_key ? 1 : pp->pic_info_fields.bits.error_resilient_mode;
	primary = intra || err_res ? PRIMARY_REF_NONE : pp->primary_ref_frame;
	cur = pp->current_frame;
	disp = pp->current_display_picture != VA_INVALID_SURFACE && surface_slot(c->drv, pp->current_display_picture) ?
	       pp->current_display_picture : cur;
	if (shown_key)
		memset(a->slots, 0, sizeof(a->slots));
	/* the references: our slots that hold the pictures VA names */
	if (!intra) {
		for (int i = 0; i < 7; i++) {
			VASurfaceID s = pp->ref_frame_map[pp->ref_frame_idx[i] & 7];

			ours[i] = -1;
			for (int k = 0; k < 8; k++)
				if (a->slots[k].valid && a->slots[k].surface == s)
					ours[i] = k;
			if (ours[i] < 0) {
				VLOG(c->drv, "AV1: reference %d (surface %u) is not in a slot of ours\n", i, s);
				return VA_STATUS_ERROR_DECODING_ERROR;
			}
			hints[i] = a->slots[ours[i]].order_hint;
		}
	}
	/* where this frame goes: a free slot, one the stream keeps no more, one of a picture in two slots, the oldest */
	slot = -1;
	if (!shown_key) {
		for (int k = 0; k < 8 && slot < 0; k++)
			if (!a->slots[k].valid)
				slot = k;
		for (int k = 0; k < 8 && slot < 0; k++)
			if (!kept(pp, a->slots[k].surface))
				slot = k;
		for (int k = 0; k < 8 && slot < 0; k++)
			for (int j = 0; j < k && slot < 0; j++)
				if (a->slots[j].surface == a->slots[k].surface)
					slot = k;
		if (slot < 0) {
			int32_t best = 0;

			for (int k = 0; k < 8; k++) {
				int referenced = 0;

				for (int i = 0; i < 7 && !intra; i++)
					referenced |= ours[i] == k;
				if (!referenced && (slot < 0 || relative_dist(pp, a->slots[k].order_hint, best) < 0)) {
					slot = k;
					best = a->slots[k].order_hint;
				}
			}
			VLOG(c->drv, "AV1: eight pictures kept and a ninth: the one in slot %d goes\n", slot);
		}
		if (slot < 0)
			slot = 0;
	}
	refresh = shown_key ? 0xff : 1u << slot;
	if (primary == PRIMARY_REF_NONE)
		gm_defaults(prev);
	else
		memcpy(prev, a->slots[ours[primary]].gm, sizeof(prev));

	/* sizes (5.9.5-5.9.8) and tiles (5.9.15) */
	sb128 = pp->seq_info_fields.fields.use_128x128_superblock;
	upscaled_w = pp->frame_width_minus1 + 1;
	frame_h = pp->frame_height_minus1 + 1;
	frame_w = pp->pic_info_fields.bits.use_superres ?
		  (upscaled_w * 8 + pp->superres_scale_denominator / 2) / pp->superres_scale_denominator : upscaled_w;
	mi_cols = 2 * ((frame_w + 7) >> 3);
	mi_rows = 2 * ((frame_h + 7) >> 3);
	sb_cols = sb128 ? (mi_cols + 31) >> 5 : (mi_cols + 15) >> 4;
	sb_rows = sb128 ? (mi_rows + 31) >> 5 : (mi_rows + 15) >> 4;
	sb_shift = sb128 ? 5 : 4;
	if (!a->seq_n || shown_key) {
		uint8_t was[64];
		size_t was_n = a->seq_n;

		memcpy(was, a->seq, was_n);
		make_seq(a, pp, (uint32_t)(c->width > upscaled_w ? c->width : upscaled_w),
			 (uint32_t)(c->height > frame_h ? c->height : frame_h));
		if (was_n != a->seq_n || memcmp(was, a->seq, was_n))
			a->seq_sent = 0;
	}
	for (int seg = 0; seg < 8; seg++) {
		int q = pp->base_qindex;

		if (pp->seg_info.segment_info_fields.bits.enabled && (pp->seg_info.feature_mask[seg] & 1)) {
			q += pp->seg_info.feature_data[seg][0];
			q = q < 0 ? 0 : q > 255 ? 255 : q;
		}
		if (!(q == 0 && !pp->y_dc_delta_q && !pp->u_dc_delta_q && !pp->u_ac_delta_q && !pp->v_dc_delta_q && !pp->v_ac_delta_q))
			coded_lossless = 0;
	}

	/* ---- uncompressed_header (5.9.2) */
	bw_init(&w);
	bw_flag(&w, 0);					/* show_existing_frame */
	bw_u(&w, (uint32_t)ft, 2);
	bw_flag(&w, show);
	if (!show)
		bw_flag(&w, pp->pic_info_fields.bits.showable_frame);
	if (!(ft == SWITCH_FRAME || shown_key))
		bw_flag(&w, err_res);
	bw_flag(&w, pp->pic_info_fields.bits.disable_cdf_update);
	bw_flag(&w, pp->pic_info_fields.bits.allow_screen_content_tools);
	if (pp->pic_info_fields.bits.allow_screen_content_tools)
		bw_flag(&w, pp->pic_info_fields.bits.force_integer_mv);
	if (ft != SWITCH_FRAME)
		bw_flag(&w, 1);				/* frame_size_override_flag */
	if (pp->seq_info_fields.fields.enable_order_hint)
		bw_u(&w, pp->order_hint, pp->order_hint_bits_minus_1 + 1);
	if (!(intra || err_res))
		bw_u(&w, pp->primary_ref_frame, 3);
	if (!(ft == SWITCH_FRAME || shown_key))
		bw_u(&w, refresh, 8);
	if ((!intra || refresh != 0xff) && err_res && pp->seq_info_fields.fields.enable_order_hint)
		for (int k = 0; k < 8; k++)
			bw_u(&w, a->slots[k].valid ? a->slots[k].order_hint : 0, pp->order_hint_bits_minus_1 + 1);
	if (intra) {
		bw_u(&w, (uint32_t)upscaled_w - 1, 16);	/* frame_size(): frame_width_minus_1 */
		bw_u(&w, (uint32_t)frame_h - 1, 16);
		bw_flag(&w, pp->pic_info_fields.bits.use_superres);
		if (pp->pic_info_fields.bits.use_superres)
			bw_u(&w, pp->superres_scale_denominator - 9u, 3);
		bw_flag(&w, 0);				/* render_and_frame_size_different */
		if (pp->pic_info_fields.bits.allow_screen_content_tools && upscaled_w == frame_w)
			bw_flag(&w, pp->pic_info_fields.bits.allow_intrabc);
	} else {
		if (pp->seq_info_fields.fields.enable_order_hint)
			bw_flag(&w, 0);			/* frame_refs_short_signaling */
		for (int i = 0; i < 7; i++)
			bw_u(&w, (uint32_t)ours[i], 3);
		if (!err_res)
			for (int i = 0; i < 7; i++)
				bw_flag(&w, 0);		/* found_ref: frame_size_with_refs() with the size explicit */
		bw_u(&w, (uint32_t)upscaled_w - 1, 16);
		bw_u(&w, (uint32_t)frame_h - 1, 16);
		bw_flag(&w, pp->pic_info_fields.bits.use_superres);
		if (pp->pic_info_fields.bits.use_superres)
			bw_u(&w, pp->superres_scale_denominator - 9u, 3);
		bw_flag(&w, 0);				/* render_and_frame_size_different */
		if (!pp->pic_info_fields.bits.force_integer_mv)
			bw_flag(&w, pp->pic_info_fields.bits.allow_high_precision_mv);
		bw_flag(&w, pp->interp_filter == 4);	/* is_filter_switchable */
		if (pp->interp_filter != 4)
			bw_u(&w, pp->interp_filter, 2);
		bw_flag(&w, pp->pic_info_fields.bits.is_motion_mode_switchable);
		if (!err_res && pp->seq_info_fields.fields.enable_order_hint)	/* (enable_ref_frame_mvs needs order hints) */
			bw_flag(&w, pp->pic_info_fields.bits.use_ref_frame_mvs);
	}
	if (!pp->pic_info_fields.bits.disable_cdf_update)
		bw_flag(&w, pp->pic_info_fields.bits.disable_frame_end_update_cdf);
	/* tile_info */
	{
		int max_w_sb = 4096 >> (sb_shift + 2), max_area_sb = (4096 * 2304) >> (2 * (sb_shift + 2));
		int min_cols = tile_log2(max_w_sb, sb_cols), max_cols = tile_log2(1, sb_cols < 64 ? sb_cols : 64);
		int max_rows = tile_log2(1, sb_rows < 64 ? sb_rows : 64);
		int min_tiles = tile_log2(max_area_sb, sb_rows * sb_cols);

		if (min_cols > min_tiles)
			min_tiles = min_cols;
		bw_flag(&w, pp->pic_info_fields.bits.uniform_tile_spacing_flag);
		if (pp->pic_info_fields.bits.uniform_tile_spacing_flag) {
			for (cols_log2 = min_cols; cols_log2 < max_cols; cols_log2++) {
				int tw = (sb_cols + (1 << cols_log2) - 1) >> cols_log2;

				if ((sb_cols + tw - 1) / tw >= pp->tile_cols)
					break;
				bw_flag(&w, 1);		/* increment_tile_cols_log2 */
			}
			if (cols_log2 < max_cols)
				bw_flag(&w, 0);
			{
				int min_rows = min_tiles - cols_log2 > 0 ? min_tiles - cols_log2 : 0;

				for (rows_log2 = min_rows; rows_log2 < max_rows; rows_log2++) {
					int th = (sb_rows + (1 << rows_log2) - 1) >> rows_log2;

					if ((sb_rows + th - 1) / th >= pp->tile_rows)
						break;
					bw_flag(&w, 1);
				}
				if (rows_log2 < max_rows)
					bw_flag(&w, 0);
			}
		} else {
			int start = 0, widest = 0;

			for (int i = 0; i < pp->tile_cols && i < 63; i++) {
				int mw = sb_cols - start < max_w_sb ? sb_cols - start : max_w_sb, s = pp->width_in_sbs_minus_1[i] + 1;

				bw_ns(&w, (uint32_t)mw, pp->width_in_sbs_minus_1[i]);
				widest = s > widest ? s : widest;
				start += s;
			}
			cols_log2 = tile_log2(1, pp->tile_cols);
			{
				int area = min_tiles > 0 ? (sb_rows * sb_cols) >> (min_tiles + 1) : sb_rows * sb_cols;
				int max_h_sb = area / (widest ? widest : 1) > 1 ? area / (widest ? widest : 1) : 1;

				start = 0;
				for (int i = 0; i < pp->tile_rows && i < 63; i++) {
					int mh = sb_rows - start < max_h_sb ? sb_rows - start : max_h_sb;

					bw_ns(&w, (uint32_t)mh, pp->height_in_sbs_minus_1[i]);
					start += pp->height_in_sbs_minus_1[i] + 1;
				}
			}
			rows_log2 = tile_log2(1, pp->tile_rows);
		}
		/* TileSizeBytes: what is between two tiles of one group in VA's buffers */
		for (int i = 0; i < nbufs; i++) {
			struct vbuf *sb = bufs[i];

			if (sb->type != VASliceParameterBufferType || sb->num < 2)
				continue;
			{
				const VASliceParameterBufferAV1 *t0 = (void *)sb->data, *t1 = (void *)(sb->data + sb->size);

				tile_bytes = (int)(t1->slice_data_offset - (t0->slice_data_offset + t0->slice_data_size));
				break;
			}
		}
		if (tile_bytes < 1 || tile_bytes > 4)
			tile_bytes = 4;
		if (cols_log2 > 0 || rows_log2 > 0) {
			bw_u(&w, pp->context_update_tile_id, rows_log2 + cols_log2);
			bw_u(&w, (uint32_t)tile_bytes - 1, 2);
		}
	}
	/* quantization_params */
	bw_u(&w, pp->base_qindex, 8);
#define DELTA_Q(v) do { bw_flag(&w, (v) != 0); if (v) bw_su(&w, (v), 7); } while (0)
	DELTA_Q(pp->y_dc_delta_q);
	if (!pp->seq_info_fields.fields.mono_chrome) {
		int diff = pp->u_dc_delta_q != pp->v_dc_delta_q || pp->u_ac_delta_q != pp->v_ac_delta_q;

		bw_flag(&w, diff);			/* diff_uv_delta */
		DELTA_Q(pp->u_dc_delta_q);
		DELTA_Q(pp->u_ac_delta_q);
		if (diff) {
			DELTA_Q(pp->v_dc_delta_q);
			DELTA_Q(pp->v_ac_delta_q);
		}
	}
	bw_flag(&w, pp->qmatrix_fields.bits.using_qmatrix);
	if (pp->qmatrix_fields.bits.using_qmatrix) {
		bw_u(&w, pp->qmatrix_fields.bits.qm_y, 4);
		bw_u(&w, pp->qmatrix_fields.bits.qm_u, 4);
		if (!pp->seq_info_fields.fields.mono_chrome)
			bw_u(&w, pp->qmatrix_fields.bits.qm_v, 4);
	}
	/* segmentation_params (as the stream had them: data not sent is the primary reference's) */
	bw_flag(&w, pp->seg_info.segment_info_fields.bits.enabled);
	if (pp->seg_info.segment_info_fields.bits.enabled) {
		int update_data = 1;

		if (primary != PRIMARY_REF_NONE) {
			bw_flag(&w, pp->seg_info.segment_info_fields.bits.update_map);
			if (pp->seg_info.segment_info_fields.bits.update_map)
				bw_flag(&w, pp->seg_info.segment_info_fields.bits.temporal_update);
			update_data = pp->seg_info.segment_info_fields.bits.update_data;
			bw_flag(&w, update_data);
		}
		if (update_data) {
			static const int bits[8] = { 8, 6, 6, 6, 6, 3, 0, 0 }, sign[8] = { 1, 1, 1, 1, 1, 0, 0, 0 };

			for (int i = 0; i < 8; i++)
				for (int j = 0; j < 8; j++) {
					int on = (pp->seg_info.feature_mask[i] >> j) & 1;

					bw_flag(&w, on);
					if (on && bits[j]) {
						if (sign[j])
							bw_su(&w, pp->seg_info.feature_data[i][j], 1 + bits[j]);
						else
							bw_u(&w, (uint32_t)pp->seg_info.feature_data[i][j], bits[j]);
					}
				}
		}
	}
	/* delta_q_params, delta_lf_params */
	if (pp->base_qindex > 0)
		bw_flag(&w, pp->mode_control_fields.bits.delta_q_present_flag);
	if (pp->mode_control_fields.bits.delta_q_present_flag) {
		bw_u(&w, pp->mode_control_fields.bits.log2_delta_q_res, 2);
		if (!pp->pic_info_fields.bits.allow_intrabc)
			bw_flag(&w, pp->mode_control_fields.bits.delta_lf_present_flag);
		if (pp->mode_control_fields.bits.delta_lf_present_flag) {
			bw_u(&w, pp->mode_control_fields.bits.log2_delta_lf_res, 2);
			bw_flag(&w, pp->mode_control_fields.bits.delta_lf_multi);
		}
	}
	/* loop_filter_params */
	if (!(coded_lossless || pp->pic_info_fields.bits.allow_intrabc)) {
		bw_u(&w, pp->filter_level[0], 6);
		bw_u(&w, pp->filter_level[1], 6);
		if (!pp->seq_info_fields.fields.mono_chrome && (pp->filter_level[0] || pp->filter_level[1])) {
			bw_u(&w, pp->filter_level_u, 6);
			bw_u(&w, pp->filter_level_v, 6);
		}
		bw_u(&w, pp->loop_filter_info_fields.bits.sharpness_level, 3);
		bw_flag(&w, pp->loop_filter_info_fields.bits.mode_ref_delta_enabled);
		if (pp->loop_filter_info_fields.bits.mode_ref_delta_enabled) {
			bw_flag(&w, 1);			/* loop_filter_delta_update: all, the values in effect */
			for (int i = 0; i < 8; i++) {
				bw_flag(&w, 1);
				bw_su(&w, pp->ref_deltas[i], 7);
			}
			for (int i = 0; i < 2; i++) {
				bw_flag(&w, 1);
				bw_su(&w, pp->mode_deltas[i], 7);
			}
		}
	}
	/* cdef_params */
	if (!(coded_lossless || pp->pic_info_fields.bits.allow_intrabc || !pp->seq_info_fields.fields.enable_cdef)) {
		bw_u(&w, pp->cdef_damping_minus_3, 2);
		bw_u(&w, pp->cdef_bits, 2);
		for (int i = 0; i < (1 << pp->cdef_bits); i++) {
			bw_u(&w, pp->cdef_y_strengths[i] >> 2, 4);
			bw_u(&w, pp->cdef_y_strengths[i] & 3, 2);
			if (!pp->seq_info_fields.fields.mono_chrome) {
				bw_u(&w, pp->cdef_uv_strengths[i] >> 2, 4);
				bw_u(&w, pp->cdef_uv_strengths[i] & 3, 2);
			}
		}
	}
	/* lr_params (VA's types are the FrameRestorationType values: back to lr_type with Remap_Lr_Type's inverse) */
	if (!((coded_lossless && upscaled_w == frame_w) || pp->pic_info_fields.bits.allow_intrabc)) {
		static const uint8_t to_lr_type[4] = { 0, 2, 3, 1 };	/* NONE, WIENER, SGRPROJ, SWITCHABLE */
		int types[3] = { pp->loop_restoration_fields.bits.yframe_restoration_type,
				 pp->loop_restoration_fields.bits.cbframe_restoration_type,
				 pp->loop_restoration_fields.bits.crframe_restoration_type };
		int planes = pp->seq_info_fields.fields.mono_chrome ? 1 : 3, uses = 0, chroma = 0;

		for (int i = 0; i < planes; i++) {
			bw_u(&w, to_lr_type[types[i] & 3], 2);
			if (types[i]) {
				uses = 1;
				chroma |= i > 0;
			}
		}
		if (uses) {
			int shift = pp->loop_restoration_fields.bits.lr_unit_shift;

			if (sb128) {
				bw_u(&w, (uint32_t)(shift > 1), 1);
			} else {
				bw_u(&w, (uint32_t)(shift > 0), 1);
				if (shift > 0)
					bw_u(&w, (uint32_t)(shift > 1), 1);
			}
			if (chroma)
				bw_flag(&w, pp->loop_restoration_fields.bits.lr_uv_shift);
		}
	}
	/* read_tx_mode */
	if (!coded_lossless)
		bw_flag(&w, pp->mode_control_fields.bits.tx_mode == 2);	/* tx_mode_select */
	/* frame_reference_mode */
	if (!intra)
		bw_flag(&w, pp->mode_control_fields.bits.reference_select);
	if (skip_mode_allowed(pp, hints, intra))
		bw_flag(&w, pp->mode_control_fields.bits.skip_mode_present);
	if (!(intra || err_res))
		bw_flag(&w, pp->pic_info_fields.bits.allow_warped_motion);
	bw_flag(&w, pp->mode_control_fields.bits.reduced_tx_set_used);
	if (!intra)
		global_motion(&w, pp, prev, gm_out);
	else
		gm_defaults(gm_out);
	if (pp->seq_info_fields.fields.film_grain_params_present && (show || pp->pic_info_fields.bits.showable_frame))
		film_grain(&w, pp, ft);
	bw_trailing(&w);
	if (w.over)
		return VA_STATUS_ERROR_DECODING_ERROR;

	/* ---- the temporal unit */
	chunk_begin(o);
	{
		static const uint8_t td[2] = { OBU_TEMPORAL_DELIMITER << 3 | 1 << 1, 0 };

		buf_put(&o->b, td, 2);
	}
	if (shown_key || !a->seq_sent) {
		obu(&o->b, OBU_SEQUENCE_HEADER, a->seq, a->seq_n);
		a->seq_sent = 1;
	}
	obu(&o->b, OBU_FRAME_HEADER, w.p, bw_bytes(&w));
	for (int i = 0; i < nbufs; i++) {
		struct vbuf *sb = bufs[i], *db;
		const VASliceParameterBufferAV1 *first, *last;
		struct bw t;
		size_t start, end;
		int tiles = pp->tile_cols * pp->tile_rows, bits = cols_log2 + rows_log2;
		uint8_t hdr[9];
		size_t hn;

		if (sb->type != VASliceParameterBufferType)
			continue;
		db = find_buf(bufs + i, nbufs - i, VASliceDataBufferType, 0);
		if (!db || !sb->num || sb->size < sizeof(VASliceParameterBufferAV1))
			return VA_STATUS_ERROR_INVALID_BUFFER;
		first = (void *)sb->data;
		last = (void *)(sb->data + (size_t)sb->size * (sb->num - 1));
		start = first->slice_data_offset;
		end = (size_t)last->slice_data_offset + last->slice_data_size;
		if (end > (size_t)db->size * db->num || start > end)
			return VA_STATUS_ERROR_INVALID_BUFFER;
		/* tile_group_obu: tile_start_and_end_present_flag, tg_start, tg_end, byte_alignment, then the tiles */
		bw_init(&t);
		if (tiles > 1) {
			bw_flag(&t, 1);
			bw_u(&t, (uint32_t)(first->tile_row * pp->tile_cols + first->tile_column), bits);
			bw_u(&t, (uint32_t)(last->tile_row * pp->tile_cols + last->tile_column), bits);
		}
		hn = bw_bytes(&t);
		hdr[0] = OBU_TILE_GROUP << 3 | 1 << 1;
		{
			size_t k = 1 + leb128(hdr + 1, hn + (end - start));

			buf_put(&o->b, hdr, k);
		}
		buf_put(&o->b, t.p, hn);
		buf_put(&o->b, db->data + start, end - start);
	}
	o->c[o->n].slot = (uint32_t)disp;
	chunk_end(o, (shown_key ? CHUNK_KEY : 0) | (show ? 0 : CHUNK_HIDDEN));

	/* this frame into its slots */
	for (int k = 0; k < 8; k++) {
		if (!(refresh & (1u << k)))
			continue;
		a->slots[k].valid = 1;
		a->slots[k].surface = cur;
		a->slots[k].display = disp;
		a->slots[k].order_hint = pp->order_hint;
		a->slots[k].frame_type = (uint8_t)ft;
		a->slots[k].showable = pp->pic_info_fields.bits.showable_frame;
		memcpy(a->slots[k].gm, gm_out, sizeof(gm_out));
	}
	/* a hidden frame that will be shown: shown now, for its surface (show_existing_frame, not of a key frame) */
	if (!show && pp->pic_info_fields.bits.showable_frame && ft != KEY_FRAME) {
		bw_init(&w);
		bw_flag(&w, 1);				/* show_existing_frame */
		bw_u(&w, (uint32_t)slot, 3);		/* frame_to_show_map_idx */
		bw_trailing(&w);
		chunk_begin(o);
		{
			static const uint8_t td[2] = { OBU_TEMPORAL_DELIMITER << 3 | 1 << 1, 0 };

			buf_put(&o->b, td, 2);
		}
		obu(&o->b, OBU_FRAME_HEADER, w.p, bw_bytes(&w));
		o->c[o->n].slot = (uint32_t)disp;
		chunk_end(o, 0);
	}
	return VA_STATUS_SUCCESS;
}

static void destroy(struct vcontext *c)
{
	free(c->state);
	c->state = NULL;
}

static void describe(struct vcontext *c, uint32_t *profile, uint32_t *level)
{
	struct av1 *a = c->state;

	*profile = 0;
	*level = a ? a->level : 13;
}

const struct codec codec_av1 = { CODEC_AV1, pack, destroy, describe };
