// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Collabora
 *
 * Author: Benjamin Gaignard <benjamin.gaignard@collabora.com>
 */

#include <linux/types.h>
#include <media/v4l2-h264.h>
#include <media/v4l2-mem2mem.h>

#include "hantro.h"
#include "hantro_av1.h"
#include "hantro_hw.h"

#define GM_GLOBAL_MODELS_PER_FRAME	7
#define GLOBAL_MODEL_TOTAL_SIZE	(6 * 4 + 4 * 2)
#define GLOBAL_MODEL_SIZE	ALIGN(GM_GLOBAL_MODELS_PER_FRAME * GLOBAL_MODEL_TOTAL_SIZE, 2048)
#define AV1_MAX_TILES		128
#define AV1_TILE_INFO_SIZE	(AV1_MAX_TILES * 16)
#define AV1_INVALID_IDX		-1
#define AV1_TILE_SIZE		ALIGN(32 * 128, 4096)

#define SUPERRES_SCALE_BITS 3

#define DIV_LUT_PREC_BITS 14
#define DIV_LUT_BITS 8
#define DIV_LUT_NUM BIT(DIV_LUT_BITS)
#define WARP_PARAM_REDUCE_BITS 6
#define WARPEDMODEL_PREC_BITS 16

#define AV1_DIV_ROUND_UP_POW2(value, n)			\
({							\
	typeof(n) _n  = n;				\
	typeof(value) _value = value;			\
	(_value + (BIT(_n) >> 1)) >> _n;		\
})

#define AV1_DIV_ROUND_UP_POW2_SIGNED(value, n)				\
({									\
	typeof(n) _n_  = n;						\
	typeof(value) _value_ = value;					\
	(((_value_) < 0) ? -AV1_DIV_ROUND_UP_POW2(-(_value_), (_n_))	\
		: AV1_DIV_ROUND_UP_POW2((_value_), (_n_)));		\
})

static const short div_lut[DIV_LUT_NUM + 1] = {
	16384, 16320, 16257, 16194, 16132, 16070, 16009, 15948, 15888, 15828, 15768,
	15709, 15650, 15592, 15534, 15477, 15420, 15364, 15308, 15252, 15197, 15142,
	15087, 15033, 14980, 14926, 14873, 14821, 14769, 14717, 14665, 14614, 14564,
	14513, 14463, 14413, 14364, 14315, 14266, 14218, 14170, 14122, 14075, 14028,
	13981, 13935, 13888, 13843, 13797, 13752, 13707, 13662, 13618, 13574, 13530,
	13487, 13443, 13400, 13358, 13315, 13273, 13231, 13190, 13148, 13107, 13066,
	13026, 12985, 12945, 12906, 12866, 12827, 12788, 12749, 12710, 12672, 12633,
	12596, 12558, 12520, 12483, 12446, 12409, 12373, 12336, 12300, 12264, 12228,
	12193, 12157, 12122, 12087, 12053, 12018, 11984, 11950, 11916, 11882, 11848,
	11815, 11782, 11749, 11716, 11683, 11651, 11619, 11586, 11555, 11523, 11491,
	11460, 11429, 11398, 11367, 11336, 11305, 11275, 11245, 11215, 11185, 11155,
	11125, 11096, 11067, 11038, 11009, 10980, 10951, 10923, 10894, 10866, 10838,
	10810, 10782, 10755, 10727, 10700, 10673, 10645, 10618, 10592, 10565, 10538,
	10512, 10486, 10460, 10434, 10408, 10382, 10356, 10331, 10305, 10280, 10255,
	10230, 10205, 10180, 10156, 10131, 10107, 10082, 10058, 10034, 10010, 9986,
	9963,  9939,  9916,  9892,  9869,  9846,  9823,  9800,  9777,  9754,  9732,
	9709,  9687,  9664,  9642,  9620,  9598,  9576,  9554,  9533,  9511,  9489,
	9468,  9447,  9425,  9404,  9383,  9362,  9341,  9321,  9300,  9279,  9259,
	9239,  9218,  9198,  9178,  9158,  9138,  9118,  9098,  9079,  9059,  9039,
	9020,  9001,  8981,  8962,  8943,  8924,  8905,  8886,  8867,  8849,  8830,
	8812,  8793,  8775,  8756,  8738,  8720,  8702,  8684,  8666,  8648,  8630,
	8613,  8595,  8577,  8560,  8542,  8525,  8508,  8490,  8473,  8456,  8439,
	8422,  8405,  8389,  8372,  8355,  8339,  8322,  8306,  8289,  8273,  8257,
	8240,  8224,  8208,  8192,
};

enum hantro_av1_tx_mode {
	HANTRO_AV1_TX_MODE_ONLY_4X4	= 0,
	HANTRO_AV1_TX_MODE_8X8		= 1,
	HANTRO_AV1_TX_MODE_16x16	= 2,
	HANTRO_AV1_TX_MODE_32x32	= 3,
	HANTRO_AV1_TX_MODE_SELECT	= 4,
};

enum hantro_av1_inter_prediction_filter_type {
	HANTRO_AV1_EIGHT_TAP_SMOOTH	= 0,
	HANTRO_AV1_EIGHT_TAP		= 1,
	HANTRO_AV1_EIGHT_TAP_SHARP	= 2,
	HANTRO_AV1_BILINEAR		= 3,
	HANTRO_AV1_SWITCHABLE		= 4,
};

int hantro_av1_get_hardware_tx_mode(enum v4l2_av1_tx_mode tx_mode)
{
	switch (tx_mode) {
	case V4L2_AV1_TX_MODE_ONLY_4X4:
		return HANTRO_AV1_TX_MODE_ONLY_4X4;
	case V4L2_AV1_TX_MODE_LARGEST:
		return HANTRO_AV1_TX_MODE_32x32;
	case V4L2_AV1_TX_MODE_SELECT:
		return HANTRO_AV1_TX_MODE_SELECT;
	}

	return HANTRO_AV1_TX_MODE_32x32;
}

int hantro_av1_get_hardware_mcomp_filt_type(int interpolation_filter)
{
	switch (interpolation_filter) {
	case V4L2_AV1_INTERPOLATION_FILTER_EIGHTTAP:
		return HANTRO_AV1_EIGHT_TAP;
	case V4L2_AV1_INTERPOLATION_FILTER_EIGHTTAP_SMOOTH:
		return HANTRO_AV1_EIGHT_TAP_SMOOTH;
	case V4L2_AV1_INTERPOLATION_FILTER_EIGHTTAP_SHARP:
		return HANTRO_AV1_EIGHT_TAP_SHARP;
	case V4L2_AV1_INTERPOLATION_FILTER_BILINEAR:
		return HANTRO_AV1_BILINEAR;
	case V4L2_AV1_INTERPOLATION_FILTER_SWITCHABLE:
		return HANTRO_AV1_SWITCHABLE;
	}

	return HANTRO_AV1_EIGHT_TAP_SMOOTH;
}

int hantro_av1_get_frame_index(struct hantro_ctx *ctx, int ref)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	const struct v4l2_ctrl_av1_frame *frame = ctrls->frame;
	u64 timestamp;
	int i, idx = frame->ref_frame_idx[ref];

	if (idx >= V4L2_AV1_TOTAL_REFS_PER_FRAME || idx < 0)
		return AV1_INVALID_IDX;

	timestamp = frame->reference_frame_ts[idx];
	for (i = 0; i < AV1_MAX_FRAME_BUF_COUNT; i++) {
		if (!av1_dec->frame_refs[i].used)
			continue;
		if (av1_dec->frame_refs[i].timestamp == timestamp)
			return i;
	}

	return AV1_INVALID_IDX;
}

int hantro_av1_get_order_hint(struct hantro_ctx *ctx, int ref)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	int idx = hantro_av1_get_frame_index(ctx, ref);

	if (idx != AV1_INVALID_IDX)
		return av1_dec->frame_refs[idx].order_hint;

	return 0;
}

int hantro_av1_frame_ref(struct hantro_ctx *ctx, u64 timestamp)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	const struct v4l2_ctrl_av1_frame *frame = ctrls->frame;
	int i;

	for (i = 0; i < AV1_MAX_FRAME_BUF_COUNT; i++) {
		int j;

		if (av1_dec->frame_refs[i].used)
			continue;

		av1_dec->frame_refs[i].width = frame->frame_width_minus_1 + 1;
		av1_dec->frame_refs[i].height = frame->frame_height_minus_1 + 1;
		av1_dec->frame_refs[i].mi_cols = DIV_ROUND_UP(frame->frame_width_minus_1 + 1, 8);
		av1_dec->frame_refs[i].mi_rows = DIV_ROUND_UP(frame->frame_height_minus_1 + 1, 8);
		av1_dec->frame_refs[i].timestamp = timestamp;
		av1_dec->frame_refs[i].frame_type = frame->frame_type;
		av1_dec->frame_refs[i].order_hint = frame->order_hint;
		av1_dec->frame_refs[i].vb2_ref = hantro_get_dst_buf(ctx);

		for (j = 0; j < V4L2_AV1_TOTAL_REFS_PER_FRAME; j++)
			av1_dec->frame_refs[i].order_hints[j] = frame->order_hints[j];
		av1_dec->frame_refs[i].used = true;
		av1_dec->current_frame_index = i;

		return i;
	}

	return AV1_INVALID_IDX;
}

static void hantro_av1_frame_unref(struct hantro_ctx *ctx, int idx)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;

	if (idx >= 0)
		av1_dec->frame_refs[idx].used = false;
}

void hantro_av1_clean_refs(struct hantro_ctx *ctx)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;

	int ref, idx;

	for (idx = 0; idx < AV1_MAX_FRAME_BUF_COUNT; idx++) {
		u64 timestamp = av1_dec->frame_refs[idx].timestamp;
		bool used = false;

		if (!av1_dec->frame_refs[idx].used)
			continue;

		for (ref = 0; ref < V4L2_AV1_TOTAL_REFS_PER_FRAME; ref++) {
			if (ctrls->frame->reference_frame_ts[ref] == timestamp)
				used = true;
		}

		if (!used)
			hantro_av1_frame_unref(ctx, idx);
	}
}

size_t hantro_av1_luma_size(struct hantro_ctx *ctx)
{
	return ctx->ref_fmt.plane_fmt[0].bytesperline * ctx->ref_fmt.height;
}

size_t hantro_av1_chroma_size(struct hantro_ctx *ctx)
{
	size_t cr_offset = hantro_av1_luma_size(ctx);

	return ALIGN((cr_offset * 3) / 2, 64);
}

static void hantro_av1_tiles_free(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;

	if (av1_dec->db_data_col.cpu)
		dma_free_coherent(vpu->dev, av1_dec->db_data_col.size,
				  av1_dec->db_data_col.cpu,
				  av1_dec->db_data_col.dma);
	av1_dec->db_data_col.cpu = NULL;

	if (av1_dec->db_ctrl_col.cpu)
		dma_free_coherent(vpu->dev, av1_dec->db_ctrl_col.size,
				  av1_dec->db_ctrl_col.cpu,
				  av1_dec->db_ctrl_col.dma);
	av1_dec->db_ctrl_col.cpu = NULL;

	if (av1_dec->cdef_col.cpu)
		dma_free_coherent(vpu->dev, av1_dec->cdef_col.size,
				  av1_dec->cdef_col.cpu, av1_dec->cdef_col.dma);
	av1_dec->cdef_col.cpu = NULL;

	if (av1_dec->sr_col.cpu)
		dma_free_coherent(vpu->dev, av1_dec->sr_col.size,
				  av1_dec->sr_col.cpu, av1_dec->sr_col.dma);
	av1_dec->sr_col.cpu = NULL;

	if (av1_dec->lr_col.cpu)
		dma_free_coherent(vpu->dev, av1_dec->lr_col.size,
				  av1_dec->lr_col.cpu, av1_dec->lr_col.dma);
	av1_dec->lr_col.cpu = NULL;
}

static int hantro_av1_tiles_reallocate(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	const struct v4l2_av1_tile_info *tile_info = &ctrls->frame->tile_info;
	unsigned int num_tile_cols = tile_info->tile_cols;
	unsigned int height = ALIGN(ctrls->frame->frame_height_minus_1 + 1, 64);
	unsigned int height_in_sb = height / 64;
	unsigned int stripe_num = ((height + 8) + 63) / 64;
	size_t size;

	if (av1_dec->db_data_col.size >=
	    ALIGN(height * 12 * ctx->bit_depth / 8, 128) * num_tile_cols)
		return 0;

	hantro_av1_tiles_free(ctx);

	size = ALIGN(height * 12 * ctx->bit_depth / 8, 128) * num_tile_cols;
	av1_dec->db_data_col.cpu = dma_alloc_coherent(vpu->dev, size,
						      &av1_dec->db_data_col.dma,
						      GFP_KERNEL);
	if (!av1_dec->db_data_col.cpu)
		goto buffer_allocation_error;
	av1_dec->db_data_col.size = size;

	size = ALIGN(height * 2 * 16 / 4, 128) * num_tile_cols;
	av1_dec->db_ctrl_col.cpu = dma_alloc_coherent(vpu->dev, size,
						      &av1_dec->db_ctrl_col.dma,
						      GFP_KERNEL);
	if (!av1_dec->db_ctrl_col.cpu)
		goto buffer_allocation_error;
	av1_dec->db_ctrl_col.size = size;

	size = ALIGN(height_in_sb * 44 * ctx->bit_depth * 16 / 8, 128) * num_tile_cols;
	av1_dec->cdef_col.cpu = dma_alloc_coherent(vpu->dev, size,
						   &av1_dec->cdef_col.dma,
						   GFP_KERNEL);
	if (!av1_dec->cdef_col.cpu)
		goto buffer_allocation_error;
	av1_dec->cdef_col.size = size;

	size = ALIGN(height_in_sb * (3040 + 1280), 128) * num_tile_cols;
	av1_dec->sr_col.cpu = dma_alloc_coherent(vpu->dev, size,
						 &av1_dec->sr_col.dma,
						 GFP_KERNEL);
	if (!av1_dec->sr_col.cpu)
		goto buffer_allocation_error;
	av1_dec->sr_col.size = size;

	size = ALIGN(stripe_num * 1536 * ctx->bit_depth / 8, 128) * num_tile_cols;
	av1_dec->lr_col.cpu = dma_alloc_coherent(vpu->dev, size,
						 &av1_dec->lr_col.dma,
						 GFP_KERNEL);
	if (!av1_dec->lr_col.cpu)
		goto buffer_allocation_error;
	av1_dec->lr_col.size = size;

	av1_dec->num_tile_cols_allocated = num_tile_cols;
	return 0;

buffer_allocation_error:
	hantro_av1_tiles_free(ctx);
	return -ENOMEM;
}

void hantro_av1_exit(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;

	if (av1_dec->global_model.cpu)
		dma_free_coherent(vpu->dev, av1_dec->global_model.size,
				  av1_dec->global_model.cpu,
				  av1_dec->global_model.dma);
	av1_dec->global_model.cpu = NULL;

	if (av1_dec->tile_info.cpu)
		dma_free_coherent(vpu->dev, av1_dec->tile_info.size,
				  av1_dec->tile_info.cpu,
				  av1_dec->tile_info.dma);
	av1_dec->tile_info.cpu = NULL;

	if (av1_dec->film_grain.cpu)
		dma_free_coherent(vpu->dev, av1_dec->film_grain.size,
				  av1_dec->film_grain.cpu,
				  av1_dec->film_grain.dma);
	av1_dec->film_grain.cpu = NULL;

	if (av1_dec->prob_tbl.cpu)
		dma_free_coherent(vpu->dev, av1_dec->prob_tbl.size,
				  av1_dec->prob_tbl.cpu, av1_dec->prob_tbl.dma);
	av1_dec->prob_tbl.cpu = NULL;

	if (av1_dec->prob_tbl_out.cpu)
		dma_free_coherent(vpu->dev, av1_dec->prob_tbl_out.size,
				  av1_dec->prob_tbl_out.cpu,
				  av1_dec->prob_tbl_out.dma);
	av1_dec->prob_tbl_out.cpu = NULL;

	if (av1_dec->tile_buf.cpu)
		dma_free_coherent(vpu->dev, av1_dec->tile_buf.size,
				  av1_dec->tile_buf.cpu, av1_dec->tile_buf.dma);
	av1_dec->tile_buf.cpu = NULL;

	hantro_av1_tiles_free(ctx);
}

int hantro_av1_init(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;

	memset(av1_dec, 0, sizeof(*av1_dec));

	av1_dec->global_model.cpu = dma_alloc_coherent(vpu->dev, GLOBAL_MODEL_SIZE,
						       &av1_dec->global_model.dma,
						       GFP_KERNEL);
	if (!av1_dec->global_model.cpu)
		return -ENOMEM;
	av1_dec->global_model.size = GLOBAL_MODEL_SIZE;

	av1_dec->tile_info.cpu = dma_alloc_coherent(vpu->dev, AV1_TILE_INFO_SIZE,
						    &av1_dec->tile_info.dma,
						    GFP_KERNEL);
	if (!av1_dec->tile_info.cpu)
		return -ENOMEM;
	av1_dec->tile_info.size = AV1_TILE_INFO_SIZE;

	av1_dec->film_grain.cpu = dma_alloc_coherent(vpu->dev,
						     ALIGN(sizeof(struct hantro_av1_film_grain), 2048),
						     &av1_dec->film_grain.dma,
						     GFP_KERNEL);
	if (!av1_dec->film_grain.cpu)
		return -ENOMEM;
	av1_dec->film_grain.size = ALIGN(sizeof(struct hantro_av1_film_grain), 2048);

	av1_dec->prob_tbl.cpu = dma_alloc_coherent(vpu->dev,
						   ALIGN(sizeof(struct av1cdfs), 2048),
						   &av1_dec->prob_tbl.dma,
						   GFP_KERNEL);
	if (!av1_dec->prob_tbl.cpu)
		return -ENOMEM;
	av1_dec->prob_tbl.size = ALIGN(sizeof(struct av1cdfs), 2048);

	av1_dec->prob_tbl_out.cpu = dma_alloc_coherent(vpu->dev,
						       ALIGN(sizeof(struct av1cdfs), 2048),
						       &av1_dec->prob_tbl_out.dma,
						       GFP_KERNEL);
	if (!av1_dec->prob_tbl_out.cpu)
		return -ENOMEM;
	av1_dec->prob_tbl_out.size = ALIGN(sizeof(struct av1cdfs), 2048);
	av1_dec->cdfs = &av1_dec->default_cdfs;
	av1_dec->cdfs_ndvc = &av1_dec->default_cdfs_ndvc;

	hantro_av1_set_default_cdfs(av1_dec->cdfs, av1_dec->cdfs_ndvc);

	av1_dec->tile_buf.cpu = dma_alloc_coherent(vpu->dev,
						   AV1_TILE_SIZE,
						   &av1_dec->tile_buf.dma,
						   GFP_KERNEL);
	if (!av1_dec->tile_buf.cpu)
		return -ENOMEM;
	av1_dec->tile_buf.size = AV1_TILE_SIZE;

	return 0;
}

int hantro_av1_prepare_run(struct hantro_ctx *ctx)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;

	ctrls->sequence = hantro_get_ctrl(ctx, V4L2_CID_STATELESS_AV1_SEQUENCE);
	if (WARN_ON(!ctrls->sequence))
		return -EINVAL;

	ctrls->tile_group_entry =
	    hantro_get_ctrl(ctx, V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY);
	if (WARN_ON(!ctrls->tile_group_entry))
		return -EINVAL;

	ctrls->frame = hantro_get_ctrl(ctx, V4L2_CID_STATELESS_AV1_FRAME);
	if (WARN_ON(!ctrls->frame))
		return -EINVAL;

	ctrls->film_grain =
	    hantro_get_ctrl(ctx, V4L2_CID_STATELESS_AV1_FILM_GRAIN);

	return hantro_av1_tiles_reallocate(ctx);
}

static int hantro_av1_get_msb(u32 n)
{
	if (n == 0)
		return 0;
	return 31 ^ __builtin_clz(n);
}

static short hantro_av1_resolve_divisor_32(u32 d, short *shift)
{
	int f;
	u64 e;

	*shift = hantro_av1_get_msb(d);
	/* e is obtained from D after resetting the most significant 1 bit. */
	e = d - ((u32)1 << *shift);
	/* Get the most significant DIV_LUT_BITS (8) bits of e into f */
	if (*shift > DIV_LUT_BITS)
		f = AV1_DIV_ROUND_UP_POW2(e, *shift - DIV_LUT_BITS);
	else
		f = e << (DIV_LUT_BITS - *shift);
	if (f > DIV_LUT_NUM)
		return -1;
	*shift += DIV_LUT_PREC_BITS;
	/* Use f as lookup into the precomputed table of multipliers */
	return div_lut[f];
}

static void hantro_av1_get_shear_params(const u32 *params, s64 *alpha,
					s64 *beta, s64 *gamma, s64 *delta)
{
	const int *mat = params;
	short shift;
	short y;
	long long gv, dv;

	if (mat[2] <= 0)
		return;

	*alpha = clamp_val(mat[2] - (1 << WARPEDMODEL_PREC_BITS), S16_MIN, S16_MAX);
	*beta = clamp_val(mat[3], S16_MIN, S16_MAX);

	y = hantro_av1_resolve_divisor_32(abs(mat[2]), &shift) * (mat[2] < 0 ? -1 : 1);

	gv = ((long long)mat[4] * (1 << WARPEDMODEL_PREC_BITS)) * y;

	*gamma = clamp_val((int)AV1_DIV_ROUND_UP_POW2_SIGNED(gv, shift), S16_MIN, S16_MAX);

	dv = ((long long)mat[3] * mat[4]) * y;
	*delta = clamp_val(mat[5] -
		(int)AV1_DIV_ROUND_UP_POW2_SIGNED(dv, shift) - (1 << WARPEDMODEL_PREC_BITS),
		S16_MIN, S16_MAX);

	*alpha = AV1_DIV_ROUND_UP_POW2_SIGNED(*alpha, WARP_PARAM_REDUCE_BITS)
		 * (1 << WARP_PARAM_REDUCE_BITS);
	*beta = AV1_DIV_ROUND_UP_POW2_SIGNED(*beta, WARP_PARAM_REDUCE_BITS)
		* (1 << WARP_PARAM_REDUCE_BITS);
	*gamma = AV1_DIV_ROUND_UP_POW2_SIGNED(*gamma, WARP_PARAM_REDUCE_BITS)
		 * (1 << WARP_PARAM_REDUCE_BITS);
	*delta = AV1_DIV_ROUND_UP_POW2_SIGNED(*delta, WARP_PARAM_REDUCE_BITS)
		* (1 << WARP_PARAM_REDUCE_BITS);
}

void hantro_av1_set_global_model(struct hantro_ctx *ctx)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	const struct v4l2_ctrl_av1_frame *frame = ctrls->frame;
	const struct v4l2_av1_global_motion *gm = &frame->global_motion;
	u8 *dst = av1_dec->global_model.cpu;
	int ref_frame, i;

	memset(dst, 0, GLOBAL_MODEL_SIZE);
	for (ref_frame = 0; ref_frame < V4L2_AV1_REFS_PER_FRAME; ++ref_frame) {
		s64 alpha = 0, beta = 0, gamma = 0, delta = 0;

		for (i = 0; i < 6; ++i) {
			if (i == 2)
				*(s32 *)dst =
					gm->params[V4L2_AV1_REF_LAST_FRAME + ref_frame][3];
			else if (i == 3)
				*(s32 *)dst =
					gm->params[V4L2_AV1_REF_LAST_FRAME + ref_frame][2];
			else
				*(s32 *)dst =
					gm->params[V4L2_AV1_REF_LAST_FRAME + ref_frame][i];
			dst += 4;
		}

		if (gm->type[V4L2_AV1_REF_LAST_FRAME + ref_frame] <= V4L2_AV1_WARP_MODEL_AFFINE)
			hantro_av1_get_shear_params(&gm->params[V4L2_AV1_REF_LAST_FRAME + ref_frame][0],
						    &alpha, &beta, &gamma, &delta);

		*(s16 *)dst = alpha;
		dst += 2;
		*(s16 *)dst = beta;
		dst += 2;
		*(s16 *)dst = gamma;
		dst += 2;
		*(s16 *)dst = delta;
		dst += 2;
	}
}

int hantro_av1_tile_log2(int target)
{
	int k;

	/*
	 * returns the smallest value for k such that 1 << k is greater
	 * than or equal to target
	 */
	for (k = 0; (1 << k) < target; k++);

	return k;
}

int hantro_av1_get_dist(struct hantro_ctx *ctx, int a, int b)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	int bits = ctrls->sequence->order_hint_bits - 1;
	int diff, m;

	if (!ctrls->sequence->order_hint_bits)
		return 0;

	diff = a - b;
	m = 1 << bits;
	diff = (diff & (m - 1)) - (diff & m);

	return diff;
}

void hantro_av1_set_frame_sign_bias(struct hantro_ctx *ctx)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	const struct v4l2_ctrl_av1_frame *frame = ctrls->frame;
	const struct v4l2_ctrl_av1_sequence *sequence = ctrls->sequence;
	int i;

	if (!sequence->order_hint_bits || IS_INTRA(frame->frame_type)) {
		for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++)
			av1_dec->ref_frame_sign_bias[i] = 0;

		return;
	}
	// Identify the nearest forward and backward references.
	for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME - 1; i++) {
		if (hantro_av1_get_frame_index(ctx, i) >= 0) {
			int rel_off =
			    hantro_av1_get_dist(ctx,
						hantro_av1_get_order_hint(ctx, i),
						frame->order_hint);
			av1_dec->ref_frame_sign_bias[i + 1] = (rel_off <= 0) ? 0 : 1;
		}
	}
}

void hantro_av1_init_scaling_function(const u8 *values, const u8 *scaling,
				      u8 num_points, u8 *scaling_lut)
{
	int i, point;

	if (num_points == 0) {
		memset(scaling_lut, 0, 256);
		return;
	}

	for (point = 0; point < num_points - 1; point++) {
		int x;
		s32 delta_y = scaling[point + 1] - scaling[point];
		s32 delta_x = values[point + 1] - values[point];
		s64 delta =
		    delta_x ? delta_y * ((65536 + (delta_x >> 1)) /
					 delta_x) : 0;

		for (x = 0; x < delta_x; x++) {
			scaling_lut[values[point] + x] =
			    scaling[point] +
			    (s32)((x * delta + 32768) >> 16);
		}
	}

	for (i = values[num_points - 1]; i < 256; i++)
		scaling_lut[i] = scaling[num_points - 1];
}

void hantro_av1_set_tile_info(struct hantro_ctx *ctx)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	const struct v4l2_av1_tile_info *tile_info = &ctrls->frame->tile_info;
	const struct v4l2_ctrl_av1_tile_group_entry *group_entry =
	    ctrls->tile_group_entry;
	u8 *dst = av1_dec->tile_info.cpu;
	int tile0, tile1;

	memset(dst, 0, av1_dec->tile_info.size);

	for (tile0 = 0; tile0 < tile_info->tile_cols; tile0++) {
		for (tile1 = 0; tile1 < tile_info->tile_rows; tile1++) {
			int tile_id = tile1 * tile_info->tile_cols + tile0;
			u32 start, end;
			u32 y0 =
			    tile_info->height_in_sbs_minus_1[tile1] + 1;
			u32 x0 = tile_info->width_in_sbs_minus_1[tile0] + 1;

			/* tile size in SB units (width,height) */
			*dst++ = x0;
			*dst++ = 0;
			*dst++ = 0;
			*dst++ = 0;
			*dst++ = y0;
			*dst++ = 0;
			*dst++ = 0;
			*dst++ = 0;

			/* tile start position */
			start = group_entry[tile_id].tile_offset - group_entry[0].tile_offset;
			*dst++ = start & 255;
			*dst++ = (start >> 8) & 255;
			*dst++ = (start >> 16) & 255;
			*dst++ = (start >> 24) & 255;

			/* number of bytes in tile data */
			end = start + group_entry[tile_id].tile_size;
			*dst++ = end & 255;
			*dst++ = (end >> 8) & 255;
			*dst++ = (end >> 16) & 255;
			*dst++ = (end >> 24) & 255;
		}
	}
}

bool hantro_av1_is_lossless(struct hantro_ctx *ctx)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	const struct v4l2_ctrl_av1_frame *frame = ctrls->frame;
	const struct v4l2_av1_segmentation *segmentation = &frame->segmentation;
	const struct v4l2_av1_quantization *quantization = &frame->quantization;
	int i;

	for (i = 0; i < V4L2_AV1_MAX_SEGMENTS; i++) {
		int qindex = quantization->base_q_idx;

		if (segmentation->feature_enabled[i] &
		    V4L2_AV1_SEGMENT_FEATURE_ENABLED(V4L2_AV1_SEG_LVL_ALT_Q)) {
			qindex += segmentation->feature_data[i][V4L2_AV1_SEG_LVL_ALT_Q];
		}
		qindex = clamp(qindex, 0, 255);

		if (qindex ||
		    quantization->delta_q_y_dc ||
		    quantization->delta_q_u_dc ||
		    quantization->delta_q_u_ac ||
		    quantization->delta_q_v_dc ||
		    quantization->delta_q_v_ac)
			return false;
	}

	return true;
}

void hantro_av1_update_prob(struct hantro_ctx *ctx)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	const struct v4l2_ctrl_av1_frame *frame = ctrls->frame;
	bool frame_is_intra = IS_INTRA(frame->frame_type);
	struct av1cdfs *out_cdfs = (struct av1cdfs *)av1_dec->prob_tbl_out.cpu;
	int i;

	if (frame->flags & V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF)
		return;

	for (i = 0; i < NUM_REF_FRAMES; i++) {
		if (frame->refresh_frame_flags & BIT(i)) {
			struct mvcdfs stored_mv_cdf;

			hantro_av1_get_cdfs(ctx, i);
			stored_mv_cdf = av1_dec->cdfs->mv_cdf;
			*av1_dec->cdfs = *out_cdfs;
			if (frame_is_intra) {
				av1_dec->cdfs->mv_cdf = stored_mv_cdf;
				*av1_dec->cdfs_ndvc = out_cdfs->mv_cdf;
			}
			hantro_av1_store_cdfs(ctx, frame->refresh_frame_flags);
			break;
		}
	}
}

void hantro_av1_set_prob(struct hantro_ctx *ctx)
{
	struct hantro_av1_dec_hw_ctx *av1_dec = &ctx->av1_dec;
	struct hantro_av1_dec_ctrls *ctrls = &av1_dec->ctrls;
	const struct v4l2_ctrl_av1_frame *frame = ctrls->frame;
	const struct v4l2_av1_quantization *quantization = &frame->quantization;
	bool error_resilient_mode =
	    !!(frame->flags & V4L2_AV1_FRAME_FLAG_ERROR_RESILIENT_MODE);
	bool frame_is_intra = IS_INTRA(frame->frame_type);

	if (error_resilient_mode || frame_is_intra ||
	    frame->primary_ref_frame == AV1_PRIMARY_REF_NONE) {
		av1_dec->cdfs = &av1_dec->default_cdfs;
		av1_dec->cdfs_ndvc = &av1_dec->default_cdfs_ndvc;
		hantro_av1_default_coeff_probs(quantization->base_q_idx,
					       av1_dec->cdfs);
	} else {
		hantro_av1_get_cdfs(ctx, frame->ref_frame_idx[frame->primary_ref_frame]);
	}
	hantro_av1_store_cdfs(ctx, frame->refresh_frame_flags);

	memcpy(av1_dec->prob_tbl.cpu, av1_dec->cdfs, sizeof(struct av1cdfs));

	if (frame_is_intra) {
		int mv_offset = offsetof(struct av1cdfs, mv_cdf);
		/* Overwrite MV context area with intrabc MV context */
		memcpy(av1_dec->prob_tbl.cpu + mv_offset, av1_dec->cdfs_ndvc,
		       sizeof(struct mvcdfs));
	}
}
