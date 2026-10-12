/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * HEVC: VA's buffers into an Annex B access unit (ITU-T H.265 (V8, 08/2021)), Main profile.
 *
 * VA's slice data is the whole NAL unit (FFmpeg hevcdec.c: decode_slice(nal->raw_data)). VA does not carry
 * the parameter sets, and two things the slice headers point at in the SPS are not in VA's parameters: the
 * short-term reference picture set candidates (only their number and the bits of an explicit one,
 * st_rps_bits) and the long-term candidates (lt_ref_pic_poc_lsb_sps). So:
 *
 * - VPS 0, SPS 0, PPS with the slices' own ids, made from VA's picture parameters (7.3.2.1-7.3.2.3); the
 *   SPS has no RPS candidates (num_short_term_ref_pic_sets 0, num_long_term_ref_pics_sps 0), seven sub-layers
 *   declared (VA does not say how many the stream has; a slice's TemporalId must fit), no reordering
 *   (sps_max_num_reorder_pics 0, sps_max_latency_increase_plus1 0: a decoder gives each picture as soon as
 *   it is decoded, as for H.264), the scaling lists VA gives (raster order there, FFmpeg hevc/ps.c; up-right
 *   diagonal in the bitstream, 7.3.4 and 6.5.3) and a VUI with the colour (BT.709, full range).
 * - Every slice segment header that has an RPS is written again with it explicit (short_term_ref_pic_set_sps_flag
 *   0, st_ref_pic_set(0) and explicit long-term entries with their MSB), from the pictures VA lists
 *   (ReferenceFrames with their POCs and RPS flags: FFmpeg lists exactly the pictures marked for reference
 *   by the current RPS, i.e. Curr and Foll, vaapi_hevc.c fill_vaapi_reference_frames); everything after the
 *   RPS is copied bit for bit, and the slice data after it byte for byte with its emulation prevention bytes
 *   (the header always ends with a byte that is not 0, its alignment bit, so they stay what they were).
 *   The order of the pictures is the one the spec prescribes for an RPS (S0 by decreasing POC, S1 by
 *   increasing), so the reference lists come out the same; long-term pictures keep the order of the original
 *   header.
 *
 * Not done (the picture fails): 10-bit and 4:2:2/4:4:4 (range extensions), and the long-term case where the
 * original order needs a decreasing MSB cycle.
 */
#include <stdlib.h>
#include <string.h>

#include "bits.h"
#include "vdec.h"

#define MAX_SUB_LAYERS_MINUS1 6

struct hpps {
	int known;
	uint8_t bytes[1024];
	size_t n;
};

struct hevc {
	uint8_t vps[128], sps[2048];
	size_t vps_n, sps_n;
	int sent;
	uint32_t level;
	struct hpps pps[64];
	uint8_t diag4[16], diag8[64];
};

/* 6.5.3: up-right diagonal scan, as raster positions */
static void diag(uint8_t *out, int size)
{
	int i = 0, x = 0, y = 0;

	while (i < size * size) {
		while (y >= 0) {
			if (x < size && y < size)
				out[i++] = (uint8_t)(y * size + x);
			y--;
			x++;
		}
		y = x;
		x = 0;
	}
}

/* Table A.8 (Main tier): level * 30, MaxLumaPs */
static const struct { uint32_t level, max_ps; } levels[] = {
	{ 30, 36864 }, { 60, 122880 }, { 63, 245760 }, { 90, 552960 }, { 93, 983040 }, { 120, 2228224 },
	{ 123, 2228224 }, { 150, 8912896 }, { 153, 8912896 }, { 156, 8912896 }, { 180, 35651584 },
	{ 183, 35651584 }, { 186, 35651584 },
};

static uint32_t level_for(uint32_t w, uint32_t h, uint32_t dpb)
{
	uint64_t ps = (uint64_t)w * h;

	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		uint64_t max = levels[i].max_ps;
		uint32_t side = 0, max_dpb;

		while ((uint64_t)(side + 1) * (side + 1) <= 8 * max)
			side++;
		/* A.4.2: maxDpbPicBuf 6, more for smaller pictures */
		max_dpb = ps <= max / 4 ? 16 : ps <= max / 2 ? 12 : ps <= 3 * max / 4 ? 8 : 6;
		if (ps <= max && w <= side && h <= side && dpb <= max_dpb)
			return levels[i].level;
	}
	return 186;
}

static void ptl(struct bw *w, uint32_t level)
{
	bw_u(w, 0, 2);				/* general_profile_space */
	bw_u(w, 0, 1);				/* general_tier_flag: Main */
	bw_u(w, 1, 5);				/* general_profile_idc: Main */
	bw_u(w, 0x60000000, 32);		/* compatible with Main (1) and Main 10 (2) */
	bw_flag(w, 1);				/* general_progressive_source_flag */
	bw_flag(w, 0);				/* general_interlaced_source_flag */
	bw_flag(w, 0);				/* general_non_packed_constraint_flag */
	bw_flag(w, 1);				/* general_frame_only_constraint_flag */
	bw_u(w, 0, 32);				/* 43 reserved bits and general_inbld_flag */
	bw_u(w, 0, 12);
	bw_u(w, level, 8);
	for (int i = 0; i < MAX_SUB_LAYERS_MINUS1; i++) {
		bw_flag(w, 0);			/* sub_layer_profile_present_flag */
		bw_flag(w, 0);			/* sub_layer_level_present_flag */
	}
	for (int i = MAX_SUB_LAYERS_MINUS1; i < 8; i++)
		bw_u(w, 0, 2);			/* reserved_zero_2bits */
}

static void vui(struct bw *w)
{
	bw_flag(w, 0);				/* aspect_ratio_info_present_flag */
	bw_flag(w, 0);				/* overscan_info_present_flag */
	bw_flag(w, 1);				/* video_signal_type_present_flag */
	bw_u(w, 5, 3);				/* video_format */
	bw_flag(w, 1);				/* video_full_range_flag */
	bw_flag(w, 1);				/* colour_description_present_flag */
	bw_u(w, 1, 8);
	bw_u(w, 1, 8);
	bw_u(w, 1, 8);
	bw_flag(w, 0);				/* chroma_loc_info_present_flag */
	bw_flag(w, 0);				/* neutral_chroma_indication_flag */
	bw_flag(w, 0);				/* field_seq_flag */
	bw_flag(w, 0);				/* frame_field_info_present_flag */
	bw_flag(w, 0);				/* default_display_window_flag */
	bw_flag(w, 0);				/* vui_timing_info_present_flag */
	bw_flag(w, 0);				/* bitstream_restriction_flag */
}

static void scaling_list(struct bw *w, const uint8_t *raster, const uint8_t *scan, int n, int dc)
{
	int next = 8;

	bw_flag(w, 1);				/* scaling_list_pred_mode_flag: explicit */
	if (dc >= 0) {
		bw_se(w, dc - 8);		/* scaling_list_dc_coef_minus8 */
		next = dc;
	}
	for (int i = 0; i < n; i++) {
		int v = raster[scan[i]];

		bw_se(w, ((v - next + 128) & 255) - 128);
		next = v;
	}
}

static void make_vps_sps(struct hevc *h, const VAPictureParameterBufferHEVC *pp, const VAIQMatrixBufferHEVC *iq)
{
	struct bw w;
	uint32_t dpb = pp->sps_max_dec_pic_buffering_minus1 + 1u;

	h->level = level_for(pp->pic_width_in_luma_samples, pp->pic_height_in_luma_samples, dpb);
	/* VPS (7.3.2.1) */
	bw_init(&w);
	bw_u(&w, 32 << 1, 8);			/* nal_unit_type 32 */
	bw_u(&w, 1, 8);				/* nuh_layer_id 0, nuh_temporal_id_plus1 1 */
	bw_u(&w, 0, 4);				/* vps_video_parameter_set_id */
	bw_flag(&w, 1);				/* vps_base_layer_internal_flag */
	bw_flag(&w, 1);				/* vps_base_layer_available_flag */
	bw_u(&w, 0, 6);				/* vps_max_layers_minus1 */
	bw_u(&w, MAX_SUB_LAYERS_MINUS1, 3);
	bw_flag(&w, 0);				/* vps_temporal_id_nesting_flag */
	bw_u(&w, 0xffff, 16);
	ptl(&w, h->level);
	bw_flag(&w, 0);				/* vps_sub_layer_ordering_info_present_flag */
	bw_ue(&w, dpb - 1);
	bw_ue(&w, 0);				/* vps_max_num_reorder_pics */
	bw_ue(&w, 0);				/* vps_max_latency_increase_plus1 */
	bw_u(&w, 0, 6);				/* vps_max_layer_id */
	bw_ue(&w, 0);				/* vps_num_layer_sets_minus1 */
	bw_flag(&w, 0);				/* vps_timing_info_present_flag */
	bw_flag(&w, 0);				/* vps_extension_flag */
	bw_trailing(&w);
	h->vps_n = bw_bytes(&w);
	memcpy(h->vps, w.p, h->vps_n);

	/* SPS (7.3.2.2) */
	bw_init(&w);
	bw_u(&w, 33 << 1, 8);
	bw_u(&w, 1, 8);
	bw_u(&w, 0, 4);				/* sps_video_parameter_set_id */
	bw_u(&w, MAX_SUB_LAYERS_MINUS1, 3);
	bw_flag(&w, 0);				/* sps_temporal_id_nesting_flag */
	ptl(&w, h->level);
	bw_ue(&w, 0);				/* sps_seq_parameter_set_id */
	bw_ue(&w, pp->pic_fields.bits.chroma_format_idc);
	bw_ue(&w, pp->pic_width_in_luma_samples);
	bw_ue(&w, pp->pic_height_in_luma_samples);
	bw_flag(&w, 0);				/* conformance_window_flag (the player crops) */
	bw_ue(&w, pp->bit_depth_luma_minus8);
	bw_ue(&w, pp->bit_depth_chroma_minus8);
	bw_ue(&w, pp->log2_max_pic_order_cnt_lsb_minus4);
	bw_flag(&w, 0);				/* sps_sub_layer_ordering_info_present_flag: one set for all */
	bw_ue(&w, dpb - 1);
	bw_ue(&w, 0);				/* sps_max_num_reorder_pics */
	bw_ue(&w, 0);				/* sps_max_latency_increase_plus1 */
	bw_ue(&w, pp->log2_min_luma_coding_block_size_minus3);
	bw_ue(&w, pp->log2_diff_max_min_luma_coding_block_size);
	bw_ue(&w, pp->log2_min_transform_block_size_minus2);
	bw_ue(&w, pp->log2_diff_max_min_transform_block_size);
	bw_ue(&w, pp->max_transform_hierarchy_depth_inter);
	bw_ue(&w, pp->max_transform_hierarchy_depth_intra);
	bw_flag(&w, pp->pic_fields.bits.scaling_list_enabled_flag);
	if (pp->pic_fields.bits.scaling_list_enabled_flag) {
		bw_flag(&w, iq != NULL);	/* sps_scaling_list_data_present_flag (else the defaults) */
		if (iq) {
			for (int m = 0; m < 6; m++)
				scaling_list(&w, iq->ScalingList4x4[m], h->diag4, 16, -1);
			for (int m = 0; m < 6; m++)
				scaling_list(&w, iq->ScalingList8x8[m], h->diag8, 64, -1);
			for (int m = 0; m < 6; m++)
				scaling_list(&w, iq->ScalingList16x16[m], h->diag8, 64, iq->ScalingListDC16x16[m]);
			for (int m = 0; m < 2; m++)
				scaling_list(&w, iq->ScalingList32x32[m], h->diag8, 64, iq->ScalingListDC32x32[m]);
		}
	}
	bw_flag(&w, pp->pic_fields.bits.amp_enabled_flag);
	bw_flag(&w, pp->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag);
	bw_flag(&w, pp->pic_fields.bits.pcm_enabled_flag);
	if (pp->pic_fields.bits.pcm_enabled_flag) {
		bw_u(&w, pp->pcm_sample_bit_depth_luma_minus1, 4);
		bw_u(&w, pp->pcm_sample_bit_depth_chroma_minus1, 4);
		bw_ue(&w, pp->log2_min_pcm_luma_coding_block_size_minus3);
		bw_ue(&w, pp->log2_diff_max_min_pcm_luma_coding_block_size);
		bw_flag(&w, pp->pic_fields.bits.pcm_loop_filter_disabled_flag);
	}
	bw_ue(&w, 0);				/* num_short_term_ref_pic_sets */
	bw_flag(&w, pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag);
	if (pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
		bw_ue(&w, 0);			/* num_long_term_ref_pics_sps */
	bw_flag(&w, pp->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag);
	bw_flag(&w, pp->pic_fields.bits.strong_intra_smoothing_enabled_flag);
	bw_flag(&w, 1);				/* vui_parameters_present_flag */
	vui(&w);
	bw_flag(&w, 0);				/* sps_extension_present_flag */
	bw_trailing(&w);
	h->sps_n = bw_bytes(&w);
	memcpy(h->sps, w.p, h->sps_n);
}

static void make_pps(uint32_t id, const VAPictureParameterBufferHEVC *pp, uint8_t *out, size_t *n)
{
	struct bw w;
	int dbk = pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag ||
		  pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag || pp->pps_beta_offset_div2 ||
		  pp->pps_tc_offset_div2;

	bw_init(&w);
	bw_u(&w, 34 << 1, 8);
	bw_u(&w, 1, 8);
	bw_ue(&w, id);
	bw_ue(&w, 0);				/* pps_seq_parameter_set_id */
	bw_flag(&w, pp->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag);
	bw_flag(&w, pp->slice_parsing_fields.bits.output_flag_present_flag);
	bw_u(&w, pp->num_extra_slice_header_bits, 3);
	bw_flag(&w, pp->pic_fields.bits.sign_data_hiding_enabled_flag);
	bw_flag(&w, pp->slice_parsing_fields.bits.cabac_init_present_flag);
	bw_ue(&w, pp->num_ref_idx_l0_default_active_minus1);
	bw_ue(&w, pp->num_ref_idx_l1_default_active_minus1);
	bw_se(&w, pp->init_qp_minus26);
	bw_flag(&w, pp->pic_fields.bits.constrained_intra_pred_flag);
	bw_flag(&w, pp->pic_fields.bits.transform_skip_enabled_flag);
	bw_flag(&w, pp->pic_fields.bits.cu_qp_delta_enabled_flag);
	if (pp->pic_fields.bits.cu_qp_delta_enabled_flag)
		bw_ue(&w, pp->diff_cu_qp_delta_depth);
	bw_se(&w, pp->pps_cb_qp_offset);
	bw_se(&w, pp->pps_cr_qp_offset);
	bw_flag(&w, pp->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag);
	bw_flag(&w, pp->pic_fields.bits.weighted_pred_flag);
	bw_flag(&w, pp->pic_fields.bits.weighted_bipred_flag);
	bw_flag(&w, pp->pic_fields.bits.transquant_bypass_enabled_flag);
	bw_flag(&w, pp->pic_fields.bits.tiles_enabled_flag);
	bw_flag(&w, pp->pic_fields.bits.entropy_coding_sync_enabled_flag);
	if (pp->pic_fields.bits.tiles_enabled_flag) {
		bw_ue(&w, pp->num_tile_columns_minus1);
		bw_ue(&w, pp->num_tile_rows_minus1);
		bw_flag(&w, 0);			/* uniform_spacing_flag: the sizes as they are */
		for (int i = 0; i < pp->num_tile_columns_minus1 && i < 19; i++)
			bw_ue(&w, pp->column_width_minus1[i]);
		for (int i = 0; i < pp->num_tile_rows_minus1 && i < 21; i++)
			bw_ue(&w, pp->row_height_minus1[i]);
		bw_flag(&w, pp->pic_fields.bits.loop_filter_across_tiles_enabled_flag);
	}
	bw_flag(&w, pp->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag);
	bw_flag(&w, dbk);			/* deblocking_filter_control_present_flag */
	if (dbk) {
		bw_flag(&w, pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag);
		bw_flag(&w, pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag);
		if (!pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
			bw_se(&w, pp->pps_beta_offset_div2);
			bw_se(&w, pp->pps_tc_offset_div2);
		}
	}
	bw_flag(&w, 0);				/* pps_scaling_list_data_present_flag (the SPS has the lists) */
	bw_flag(&w, pp->slice_parsing_fields.bits.lists_modification_present_flag);
	bw_ue(&w, pp->log2_parallel_merge_level_minus2);
	bw_flag(&w, pp->slice_parsing_fields.bits.slice_segment_header_extension_present_flag);
	bw_flag(&w, 0);				/* pps_extension_present_flag */
	bw_trailing(&w);
	*n = bw_bytes(&w);
	memcpy(out, w.p, *n);
}

static int ceil_log2(uint32_t v)
{
	int n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

/* The pictures of the current RPS (VA's ReferenceFrames) in an explicit st_ref_pic_set and long-term list. */
struct rps {
	int nneg, npos, nlt;
	int32_t neg[16], pos[16];	/* POCs, S0 decreasing, S1 increasing */
	int neg_used[16], pos_used[16];
	int32_t lt[16];
	int lt_used[16];
	int total_curr;			/* NumPicTotalCurr */
};

static int cmp_desc(const void *a, const void *b)
{
	const int32_t *x = a, *y = b;

	return *x < *y ? 1 : *x > *y ? -1 : 0;
}

static int cmp_asc(const void *a, const void *b)
{
	return -cmp_desc(a, b);
}

static void rps_of(const VAPictureParameterBufferHEVC *pp, struct rps *r)
{
	int32_t cur = pp->CurrPic.pic_order_cnt;
	struct { int32_t poc; int used; } s0[16], s1[16];
	int n0 = 0, n1 = 0;

	memset(r, 0, sizeof(*r));
	for (int i = 0; i < 15; i++) {
		const VAPictureHEVC *p = &pp->ReferenceFrames[i];

		if (p->flags & VA_PICTURE_HEVC_INVALID || p->picture_id == VA_INVALID_SURFACE)
			continue;
		if (p->flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE) {
			if (r->nlt < 16) {
				r->lt[r->nlt] = p->pic_order_cnt;
				r->lt_used[r->nlt++] = !!(p->flags & VA_PICTURE_HEVC_RPS_LT_CURR);
			}
			r->total_curr += !!(p->flags & VA_PICTURE_HEVC_RPS_LT_CURR);
			continue;
		}
		if (p->pic_order_cnt < cur && n0 < 16) {
			s0[n0].poc = p->pic_order_cnt;
			s0[n0++].used = !!(p->flags & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER));
		} else if (p->pic_order_cnt > cur && n1 < 16) {
			s1[n1].poc = p->pic_order_cnt;
			s1[n1++].used = !!(p->flags & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE | VA_PICTURE_HEVC_RPS_ST_CURR_AFTER));
		}
	}
	/* (sorted with their used flags: pairs, the POC first) */
	qsort(s0, (size_t)n0, sizeof(s0[0]), cmp_desc);
	qsort(s1, (size_t)n1, sizeof(s1[0]), cmp_asc);
	for (int i = 0; i < n0; i++) {
		r->neg[i] = s0[i].poc;
		r->neg_used[i] = s0[i].used;
		r->total_curr += s0[i].used;
	}
	for (int i = 0; i < n1; i++) {
		r->pos[i] = s1[i].poc;
		r->pos_used[i] = s1[i].used;
		r->total_curr += s1[i].used;
	}
	r->nneg = n0;
	r->npos = n1;
}

/*
 * One slice segment NAL unit, its header written again if it has an RPS. `p`, `n`: the NAL as VA has it
 * (escaped). Returns 0, or -1 if this one cannot be done.
 */
static int slice(struct vcontext *c, const VAPictureParameterBufferHEVC *pp, const struct rps *rps,
		 const uint8_t *p, size_t n, struct buf *out, uint32_t *pps_id, int *irap)
{
	static const uint8_t start[4] = { 0, 0, 0, 1 };
	uint8_t *u = malloc(n + 1);
	uint32_t *map = malloc(sizeof(uint32_t) * (n + 1));
	uint32_t type, ctb, pic_ctbs, nst = pp->num_short_term_ref_pic_sets, max_lsb;
	size_t m = 0, a_pos, b_pos, c_pos, data_at;
	int zeros = 0, dependent = 0, slice_type, lsb = 0, r = -1;
	int sps_flag = 0, nlt_sps = 0, nlt_pics = 0;
	int lt_lsb[32], lt_known[32], lt_used_o[32];
	struct br br;
	struct bw w;

	if (!u || !map)
		goto out;
	for (size_t i = 0; i < n; i++) {
		if (zeros >= 2 && p[i] == 3) {
			zeros = 0;
			continue;
		}
		map[m] = (uint32_t)i;
		u[m++] = p[i];
		zeros = p[i] ? 0 : zeros + 1;
	}
	map[m] = (uint32_t)n;
	br_init(&br, u, m);
	br_u(&br, 1);
	type = br_u(&br, 6);
	br_u(&br, 9);				/* nuh_layer_id, nuh_temporal_id_plus1 */
	*irap = type >= 16 && type <= 23;
	if (type > 31)
		goto verbatim;			/* (not a slice: as it is) */
	if (!br_u(&br, 1)) {			/* first_slice_segment_in_pic_flag */
		ctb = 1u << (pp->log2_min_luma_coding_block_size_minus3 + 3 + pp->log2_diff_max_min_luma_coding_block_size);
		pic_ctbs = ((pp->pic_width_in_luma_samples + ctb - 1) / ctb) * ((pp->pic_height_in_luma_samples + ctb - 1) / ctb);
		if (*irap)
			br_u(&br, 1);		/* no_output_of_prior_pics_flag */
		*pps_id = br_ue(&br);
		if (pp->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag)
			dependent = (int)br_u(&br, 1);
		br_u(&br, ceil_log2(pic_ctbs));	/* slice_segment_address */
	} else {
		if (*irap)
			br_u(&br, 1);
		*pps_id = br_ue(&br);
	}
	if (dependent || type == 19 || type == 20)
		goto verbatim;			/* (no RPS in it) */
	br_u(&br, pp->num_extra_slice_header_bits);
	slice_type = (int)br_ue(&br);
	if (pp->slice_parsing_fields.bits.output_flag_present_flag)
		br_u(&br, 1);
	/* (separate_colour_plane_flag: not for Main) */
	max_lsb = 1u << (pp->log2_max_pic_order_cnt_lsb_minus4 + 4);
	lsb = (int)br_u(&br, (int)pp->log2_max_pic_order_cnt_lsb_minus4 + 4);
	a_pos = br.pos;
	sps_flag = (int)br_u(&br, 1);
	if (!sps_flag)
		br_u(&br, 0), br.pos += pp->st_rps_bits;
	else if (nst > 1)
		br_u(&br, ceil_log2(nst));
	if (pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
		if (pp->num_long_term_ref_pic_sps > 0)
			nlt_sps = (int)br_ue(&br);
		nlt_pics = (int)br_ue(&br);
		if (nlt_sps + nlt_pics > 32)
			goto out;
		for (int i = 0; i < nlt_sps + nlt_pics; i++) {
			lt_known[i] = i >= nlt_sps;
			lt_lsb[i] = 0;
			lt_used_o[i] = -1;
			if (i < nlt_sps) {
				if (pp->num_long_term_ref_pic_sps > 1)
					br_u(&br, ceil_log2(pp->num_long_term_ref_pic_sps));
			} else {
				lt_lsb[i] = (int)br_u(&br, (int)pp->log2_max_pic_order_cnt_lsb_minus4 + 4);
				lt_used_o[i] = (int)br_u(&br, 1);
			}
			if (br_u(&br, 1))	/* delta_poc_msb_present_flag */
				br_ue(&br);
		}
	}
	b_pos = br.pos;
	/* The rest, read only to find where the header ends (7.3.6.1). */
	{
		int tmvp = 0, sao_l = 0, sao_c = 0, dbk_disabled = pp->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag;
		int l0 = pp->num_ref_idx_l0_default_active_minus1, l1 = pp->num_ref_idx_l1_default_active_minus1;

		if (pp->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag)
			tmvp = (int)br_u(&br, 1);
		if (pp->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag) {
			sao_l = (int)br_u(&br, 1);
			sao_c = (int)br_u(&br, 1);	/* (ChromaArrayType is 1) */
		}
		if (slice_type == 0 || slice_type == 1) {	/* B, P */
			if (br_u(&br, 1)) {
				l0 = (int)br_ue(&br);
				if (slice_type == 0)
					l1 = (int)br_ue(&br);
			}
			if (pp->slice_parsing_fields.bits.lists_modification_present_flag && rps->total_curr > 1) {
				int bits = ceil_log2((uint32_t)rps->total_curr);

				if (br_u(&br, 1))
					br.pos += (size_t)bits * (l0 + 1);
				if (slice_type == 0 && br_u(&br, 1))
					br.pos += (size_t)bits * (l1 + 1);
			}
			if (slice_type == 0)
				br_u(&br, 1);		/* mvd_l1_zero_flag */
			if (pp->slice_parsing_fields.bits.cabac_init_present_flag)
				br_u(&br, 1);
			if (tmvp) {
				int from_l0 = 1;

				if (slice_type == 0)
					from_l0 = (int)br_u(&br, 1);
				if ((from_l0 && l0 > 0) || (!from_l0 && l1 > 0))
					br_ue(&br);	/* collocated_ref_idx */
			}
			if ((pp->pic_fields.bits.weighted_pred_flag && slice_type == 1) ||
			    (pp->pic_fields.bits.weighted_bipred_flag && slice_type == 0)) {
				/* pred_weight_table (7.3.6.3) */
				br_ue(&br);
				br_se(&br);
				for (int list = 0; list < (slice_type == 0 ? 2 : 1); list++) {
					int k = list ? l1 : l0, lf[16], cf[16];

					for (int i = 0; i <= k && i < 16; i++)
						lf[i] = (int)br_u(&br, 1);
					for (int i = 0; i <= k && i < 16; i++)
						cf[i] = (int)br_u(&br, 1);
					for (int i = 0; i <= k && i < 16; i++) {
						if (lf[i])
							br_se(&br), br_se(&br);
						if (cf[i])
							for (int j = 0; j < 2; j++)
								br_se(&br), br_se(&br);
					}
				}
			}
			br_ue(&br);			/* five_minus_max_num_merge_cand */
		}
		br_se(&br);				/* slice_qp_delta */
		if (pp->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag)
			br_se(&br), br_se(&br);
		{
			int override = 0;

			if (pp->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag)
				override = (int)br_u(&br, 1);
			if (override) {
				dbk_disabled = (int)br_u(&br, 1);
				if (!dbk_disabled)
					br_se(&br), br_se(&br);
			}
		}
		if (pp->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag && (sao_l || sao_c || !dbk_disabled))
			br_u(&br, 1);
		if (pp->pic_fields.bits.tiles_enabled_flag || pp->pic_fields.bits.entropy_coding_sync_enabled_flag) {
			uint32_t k = br_ue(&br);

			if (k) {
				uint32_t len = br_ue(&br) + 1;

				if (len > 32 || k > 4096)
					goto out;
				br.pos += (size_t)len * k;
			}
		}
		if (pp->slice_parsing_fields.bits.slice_segment_header_extension_present_flag) {
			uint32_t k = br_ue(&br);

			br.pos += 8 * (size_t)k;
		}
	}
	c_pos = br.pos;
	if (br.over || c_pos >= 8 * m || !br_u(&br, 1))
		goto bad;			/* (alignment_bit_equal_to_one) */
	while (br.pos & 7)
		if (br_u(&br, 1))
			goto bad;
	data_at = br.pos / 8;

	/* The new header: up to the RPS, the RPS, the rest, the alignment. */
	bw_init(&w);
	bw_copy(&w, u, 0, a_pos);
	bw_flag(&w, 0);				/* short_term_ref_pic_set_sps_flag */
	/* st_ref_pic_set(0): no inter_ref_pic_set_prediction_flag for the first set (7.3.7) */
	{
		int32_t cur = pp->CurrPic.pic_order_cnt, prev = cur;

		bw_ue(&w, (uint32_t)rps->nneg);
		bw_ue(&w, (uint32_t)rps->npos);
		for (int i = 0; i < rps->nneg; i++) {
			bw_ue(&w, (uint32_t)(prev - rps->neg[i] - 1));
			bw_flag(&w, rps->neg_used[i]);
			prev = rps->neg[i];
		}
		prev = cur;
		for (int i = 0; i < rps->npos; i++) {
			bw_ue(&w, (uint32_t)(rps->pos[i] - prev - 1));
			bw_flag(&w, rps->pos_used[i]);
			prev = rps->pos[i];
		}
		if (pp->slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
			int taken[16] = { 0 }, order[32], nl = nlt_sps + nlt_pics;
			uint32_t last_cycle = 0;

			if (nl != rps->nlt) {
				VLOG(c->drv, "HEVC: %d long-term pictures in the header, %d in VA's list\n", nl, rps->nlt);
				goto out;
			}
			/* the original order: explicit entries by their LSB, the SPS's ones by what is left */
			for (int i = 0; i < nl; i++) {
				order[i] = -1;
				for (int k = 0; k < rps->nlt && lt_known[i]; k++) {
					if (!taken[k] && (uint32_t)(rps->lt[k] & (int32_t)(max_lsb - 1)) == (uint32_t)lt_lsb[i] &&
					    rps->lt_used[k] == lt_used_o[i]) {
						order[i] = k;
						taken[k] = 1;
						break;
					}
				}
			}
			for (int i = 0; i < nl; i++)
				for (int k = 0; k < rps->nlt && order[i] < 0; k++)
					if (!taken[k]) {
						order[i] = k;
						taken[k] = 1;
					}
			bw_ue(&w, (uint32_t)nl);	/* num_long_term_pics (num_long_term_ref_pics_sps is 0) */
			for (int i = 0; i < nl; i++) {
				int32_t poc = rps->lt[order[i]];
				uint32_t poc_lsb = (uint32_t)poc & (max_lsb - 1);
				int64_t msb_delta = (int64_t)(cur - lsb) - (int64_t)(poc - (int32_t)poc_lsb);
				uint32_t cycle = (uint32_t)(msb_delta / max_lsb);

				if (msb_delta < 0 || msb_delta % max_lsb || cycle < last_cycle) {
					VLOG(c->drv, "HEVC: a long-term picture this rewriter cannot put in order\n");
					goto out;
				}
				bw_u(&w, poc_lsb, (int)pp->log2_max_pic_order_cnt_lsb_minus4 + 4);
				bw_flag(&w, rps->lt_used[order[i]]);
				bw_flag(&w, 1);		/* delta_poc_msb_present_flag */
				bw_ue(&w, cycle - last_cycle);
				last_cycle = cycle;
			}
		}
	}
	bw_copy(&w, u, b_pos, c_pos - b_pos);
	bw_trailing(&w);			/* byte_alignment(): a 1 and 0s, as rbsp_trailing_bits */
	if (w.over)
		goto out;
	/* start code, the header escaped, then the slice data as it came (with its emulation prevention bytes) */
	if (buf_put(out, start, 4))
		goto out;
	{
		int z = 0;
		size_t hn = bw_bytes(&w);

		for (size_t i = 0; i < hn; i++) {
			if (z >= 2 && w.p[i] <= 3) {
				buf_byte(out, 3);
				z = 0;
			}
			buf_byte(out, w.p[i]);
			z = w.p[i] ? 0 : z + 1;
		}
	}
	if (buf_put(out, p + map[data_at], n - map[data_at]))
		goto out;
	r = 0;
	goto out;
verbatim:
	r = buf_put(out, start, 4) || buf_put(out, p, n) ? -1 : 0;
	goto out;
bad:
	VLOG(c->drv, "HEVC: a slice segment header that does not end where it should (bit %zu of %zu)\n", c_pos, 8 * m);
out:
	free(u);
	free(map);
	return r;
}

static VAStatus pack(struct vcontext *c, struct vbuf **bufs, int nbufs, struct chunks *o)
{
	struct hevc *h = c->state;
	struct vbuf *pb = find_buf(bufs, nbufs, VAPictureParameterBufferType, -1);
	struct vbuf *qb = find_buf(bufs, nbufs, VAIQMatrixBufferType, -1);
	const VAPictureParameterBufferHEVC *pp;
	const VAIQMatrixBufferHEVC *iq = qb && qb->size >= sizeof(VAIQMatrixBufferHEVC) ? (void *)qb->data : NULL;
	uint8_t vps_was[128], sps_was[2048], bytes[1024];
	size_t vps_was_n, sps_was_n, n;
	struct buf slices = { 0 };
	struct rps rps;
	int key = -1, used[64] = { 0 }, any = 0;

	if (!h) {
		h = c->state = calloc(1, sizeof(*h));
		if (!h)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		diag(h->diag4, 4);
		diag(h->diag8, 8);
	}
	if (!pb || pb->size < sizeof(VAPictureParameterBufferHEVC))
		return VA_STATUS_ERROR_INVALID_BUFFER;
	pp = (void *)pb->data;
	if (pp->pic_fields.bits.chroma_format_idc != 1 || pp->bit_depth_luma_minus8 || pp->bit_depth_chroma_minus8 ||
	    pp->pic_fields.bits.separate_colour_plane_flag)
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	rps_of(pp, &rps);
	for (int i = 0; i < nbufs; i++) {
		struct vbuf *spb = bufs[i], *db;

		if (spb->type != VASliceParameterBufferType)
			continue;
		db = find_buf(bufs + i, nbufs - i, VASliceDataBufferType, 0);
		if (!db || spb->size < sizeof(VASliceParameterBufferHEVC))
			goto bad_buffer;
		for (uint32_t e = 0; e < spb->num; e++) {
			const VASliceParameterBufferHEVC *sp = (void *)(spb->data + e * spb->size);
			uint32_t pps_id = 0;
			int irap = 0;

			if ((uint64_t)sp->slice_data_offset + sp->slice_data_size > (uint64_t)db->size * db->num ||
			    sp->slice_data_flag != VA_SLICE_DATA_FLAG_ALL)
				goto bad_buffer;
			if (slice(c, pp, &rps, db->data + sp->slice_data_offset, sp->slice_data_size, &slices, &pps_id, &irap)) {
				free(slices.p);
				return VA_STATUS_ERROR_DECODING_ERROR;
			}
			if (key < 0)
				key = irap;
			if (pps_id < 64)
				used[pps_id] = 1;
			any = 1;
		}
	}
	if (!any)
		goto bad_buffer;
	chunk_begin(o);
	vps_was_n = h->vps_n;
	sps_was_n = h->sps_n;
	memcpy(vps_was, h->vps, h->vps_n);
	memcpy(sps_was, h->sps, h->sps_n);
	make_vps_sps(h, pp, iq);
	if (key || !h->sent || vps_was_n != h->vps_n || sps_was_n != h->sps_n || memcmp(vps_was, h->vps, h->vps_n) ||
	    memcmp(sps_was, h->sps, h->sps_n)) {
		nal_put(&o->b, h->vps, h->vps_n);
		nal_put(&o->b, h->sps, h->sps_n);
		h->sent = 1;
	}
	for (int id = 0; id < 64; id++) {
		struct hpps *ps = &h->pps[id];

		if (used[id]) {
			make_pps((uint32_t)id, pp, bytes, &n);
			if (key || !ps->known || n != ps->n || memcmp(bytes, ps->bytes, n)) {
				nal_put(&o->b, bytes, n);
				memcpy(ps->bytes, bytes, n);
				ps->n = n;
				ps->known = 1;
			}
		} else if (key && ps->known) {
			nal_put(&o->b, ps->bytes, ps->n);
		}
	}
	if (buf_put(&o->b, slices.p, slices.n)) {
		free(slices.p);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}
	free(slices.p);
	chunk_end(o, key > 0 ? CHUNK_KEY : 0);
	return VA_STATUS_SUCCESS;
bad_buffer:
	free(slices.p);
	return VA_STATUS_ERROR_INVALID_BUFFER;
}

static void destroy(struct vcontext *c)
{
	free(c->state);
	c->state = NULL;
}

static void describe(struct vcontext *c, uint32_t *profile, uint32_t *level)
{
	struct hevc *h = c->state;

	*profile = 1;
	*level = h ? h->level : 120;
}

const struct codec codec_hevc = { CODEC_HEVC, pack, destroy, describe };
