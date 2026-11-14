// SPDX-License-Identifier: GPL-2.0-only
/*
 * HEVC parameter sets of the RKVENC v1 encoder. One struct
 * rkvenc_hevc_seq feeds both the slice header registers and the
 * VPS/SPS/PPS writer.
 */

#include <linux/string.h>

#include "rkvenc.h"
#include "rkvenc-nal.h"

#define HEVC_NAL_TRAIL_R 1
#define HEVC_NAL_IDR_W_RADL 19

static const u16 rkvenc_hevc_level_idc[] = {
	30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186,
};

static u16 rkvenc_hevc_level(u32 ctrl)
{
	if (ctrl >= ARRAY_SIZE(rkvenc_hevc_level_idc))
		return 186;
	return rkvenc_hevc_level_idc[ctrl];
}

int rkvenc_hevc_setup(struct rkvenc_ctx *ctx)
{
	struct rkvenc_hevc_seq *seq = &ctx->hevc_seq;

	memset(seq, 0, sizeof(*seq));
	seq->width = ctx->width;
	seq->height = ctx->height;
	seq->level_idc = rkvenc_hevc_level(ctx->c.level);
	seq->log2_max_poc_lsb_m4 = 4;
	seq->pic_init_qp = ctx->c.p_frame_qp;
	seq->max_merge_cand = ctx->c.hevc_max_merge ?: 2;
	seq->sao = ctx->c.hevc_sao;
	seq->deblock_disabled = ctx->c.lf_mode ==
				V4L2_MPEG_VIDEO_HEVC_LOOP_FILTER_MODE_DISABLED;
	seq->beta_offset = ctx->c.hevc_beta_offset;
	seq->tc_offset = ctx->c.hevc_tc_offset;
	seq->strong_intra_smoothing = ctx->c.hevc_strong_intra;
	seq->cabac_init_present = ctx->c.hevc_cabac_init;

	return rkvenc_hevc_headers_write(seq, ctx->hdr, RKVENC_HDR_MAX);
}

int rkvenc_hevc_write_headers(struct rkvenc_ctx *ctx, u8 *buf, u32 size)
{
	if (ctx->hdr_len > size)
		return -ENOSPC;
	memcpy(buf, ctx->hdr, ctx->hdr_len);
	return ctx->hdr_len;
}

void rkvenc_hevc_build_pic(struct rkvenc_ctx *ctx,
			   struct rkvenc_hevc_pic *pic, bool idr)
{
	pic->seq = &ctx->hevc_seq;
	pic->nal_unit_type = idr ? HEVC_NAL_IDR_W_RADL : HEVC_NAL_TRAIL_R;
	pic->poc_lsb = ctx->gop.frames_since_idr & 0xff;
}
