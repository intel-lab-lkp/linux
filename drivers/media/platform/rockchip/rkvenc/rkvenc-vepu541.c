// SPDX-License-Identifier: GPL-2.0-only
/*
 * RKVENC v1 register image builder, codec independent part.
 */

#include <linux/math64.h>

#include "rkvenc-vepu541.h"

#define RKVENC_MIN_SIZE		8
#define RKVENC_MAX_SIZE		4096

static const u16 rkvenc_l1_gaps[][2] = {
	{ RKVENC_INT_CLR, 0x02c },
	{ 0x184, 0x190 },
	{ 0x1bc, 0x1bc },
};

bool rkvenc_vepu541_l1_written(u32 reg)
{
	u32 i;

	if (reg < RKVENC_ENC_CLR || reg >= RKVENC_L1_CFG_END)
		return false;

	for (i = 0; i < ARRAY_SIZE(rkvenc_l1_gaps); i++)
		if (reg >= rkvenc_l1_gaps[i][0] && reg <= rkvenc_l1_gaps[i][1])
			return false;

	return true;
}

void rkvenc_vepu541_slot_sizes(enum rkvenc_codec codec, u32 width, u32 height,
			       struct rkvenc_slot_sizes *sz)
{
	u32 aw, ah, wc, hx;

	if (codec == RKVENC_CODEC_H264) {
		aw = ALIGN(width, 64);
		ah = ALIGN(height, 64) + 16;
		sz->rfp_hdr = ALIGN(aw * ah / 64, 8192);
		sz->rfp_body = aw * ah * 3 / 2;
		sz->dsp = ALIGN(aw / 64 * ah / 64 * 256, 8192);
		sz->cmv = 0;
		return;
	}

	wc = DIV_ROUND_UP(width, 64);
	hx = DIV_ROUND_UP(height, 64) + 1;
	sz->rfp_hdr = ALIGN(wc * hx * 64, 8192);
	sz->rfp_body = wc * hx * 4096 * 3 / 2;
	sz->dsp = wc * hx * 256;
	sz->cmv = ALIGN(wc * hx * 64, 256);
}

static void rkvenc_rc_build(const struct rkvenc_frame *f, u32 blocks,
			    u32 row_blocks, struct rkvenc_img *img)
{
	const struct rkvenc_rc *rc = f->rc;
	u32 cfg, ebit, d, adj0, adj1, i;
	s32 thd[RKVENC_RC_STEPS];
	u64 ebit64;

	cfg = 0;
	if (f->aq)
		cfg |= RKVENC_RC_CFG_AQ_EN;
	if (f->aq && f->codec == RKVENC_CODEC_HEVC)
		cfg |= RKVENC_RC_CFG_AQ_MODE;

	rkvenc_img_l1(img, RKVENC_RC_QP,
		      FIELD_PREP(RKVENC_RC_QP_RANGE, rc ? rc->qp_range : 0) |
		      FIELD_PREP(RKVENC_RC_QP_MAX, f->qp_max) |
		      FIELD_PREP(RKVENC_RC_QP_MIN, f->qp_min));

	if (!rc) {
		rkvenc_img_l1(img, RKVENC_RC_CFG, cfg);
		return;
	}

	cfg |= RKVENC_RC_CFG_RC_EN |
	       FIELD_PREP(RKVENC_RC_CFG_CTU_NUM, row_blocks);
	rkvenc_img_l1(img, RKVENC_RC_CFG, cfg);

	ebit64 = div_u64(16ULL * rc->target_bits, blocks);
	ebit = min_t(u64, ebit64, RKVENC_RC_TGT_CTU_EBIT);
	rkvenc_img_l1(img, RKVENC_RC_TGT, ebit);

	/* The row rate control measures its deviation per row of blocks */
	d = ebit * row_blocks / 16 * 5 / 16;
	thd[0] = -2 * (s32)d;
	thd[1] = -(s32)d;
	thd[2] = d;
	thd[3] = 2 * d;
	for (i = 4; i < RKVENC_RC_STEPS; i++)
		thd[i] = 0x7fffffff;

	adj0 = 0;
	adj1 = 0;
	for (i = 0; i < RKVENC_RC_STEPS; i++) {
		rkvenc_img_l1(img, RKVENC_RC_DTHD(i), (u32)thd[i]);
		if (i < 5)
			adj0 |= rkvenc_s2u(rc->step[i], 5) << (5 * i);
		else
			adj1 |= rkvenc_s2u(rc->step[i], 5) << (5 * (i - 5));
	}
	rkvenc_img_l1(img, RKVENC_RC_ADJ0, adj0);
	rkvenc_img_l1(img, RKVENC_RC_ADJ1, adj1);
}

static void rkvenc_buffers_build(const struct rkvenc_frame *f,
				 struct rkvenc_img *img)
{
	const struct rkvenc_src *src = &f->src;
	const struct rkvenc_bs *bs = &f->bs;

	rkvenc_img_l1(img, RKVENC_SRC_FMT,
		      FIELD_PREP(RKVENC_SRC_FMT_CFMT,
				 RKVENC_SRC_FMT_CFMT_NV12));
	rkvenc_img_l1(img, RKVENC_SRC_PROC, RKVENC_SRC_PROC_TXA_EN);
	rkvenc_img_l1(img, RKVENC_SRC_STRID,
		      FIELD_PREP(RKVENC_SRC_STRID_STRD0, src->stride[0]) |
		      FIELD_PREP(RKVENC_SRC_STRID_STRD1, src->stride[1]));
	rkvenc_img_l1(img, RKVENC_ADR_SRC0, src->plane[0]);
	rkvenc_img_l1(img, RKVENC_ADR_SRC1, src->plane[1]);
	rkvenc_img_l1(img, RKVENC_ADR_SRC2, src->plane[2]);

	if (f->referenced) {
		rkvenc_img_l1(img, RKVENC_ADR_RFPW_H, f->recon.rfp_hdr);
		rkvenc_img_l1(img, RKVENC_ADR_RFPW_B, f->recon.rfp_body);
	}
	rkvenc_img_l1(img, RKVENC_ADR_DSPW, f->recon.dsp);
	rkvenc_img_l1(img, RKVENC_ADR_RFPR_H, f->ref.rfp_hdr);
	rkvenc_img_l1(img, RKVENC_ADR_RFPR_B, f->ref.rfp_body);
	rkvenc_img_l1(img, RKVENC_ADR_DSPR, f->ref.dsp);
	if (f->codec == RKVENC_CODEC_HEVC) {
		rkvenc_img_l1(img, RKVENC_ADR_CMVW, f->recon.cmv);
		rkvenc_img_l1(img, RKVENC_ADR_CMVR, f->ref.cmv);
	}

	rkvenc_img_l1(img, RKVENC_ADR_BSBB, bs->base);
	rkvenc_img_l1(img, RKVENC_ADR_BSBR, bs->base);
	rkvenc_img_l1(img, RKVENC_ADR_BSBT, bs->base + bs->size - 1);
	rkvenc_img_l1(img, RKVENC_ADR_BSBS, bs->base + bs->offset);
	rkvenc_img_l1(img, RKVENC_DTRNS_MAP,
		      FIELD_PREP(RKVENC_DTRNS_MAP_BSW_BUS_EDIN, 7));
}

static int rkvenc_frame_check(const struct rkvenc_frame *f)
{
	u32 w = f->src.width, h = f->src.height;

	if (w < RKVENC_MIN_SIZE || h < RKVENC_MIN_SIZE ||
	    w > RKVENC_MAX_SIZE || h > RKVENC_MAX_SIZE)
		return -EINVAL;

	if (f->qp > RKVENC_QP_MAX || f->qp_min > f->qp_max ||
	    f->qp_max > RKVENC_QP_MAX)
		return -EINVAL;

	/* Hardware bug: H.264 at most 2 MBs wide and over 1 MB high hangs */
	if (f->codec == RKVENC_CODEC_H264 && ALIGN(w, 16) <= 32 &&
	    ALIGN(h, 16) > 16)
		return -EINVAL;

	/* Hardware bug: HEVC P pictures one CTU row high decode wrongly */
	if (f->codec == RKVENC_CODEC_HEVC && !f->intra && h <= 64)
		return -EINVAL;

	if (f->bs.size < 256 || f->bs.offset >= f->bs.size)
		return -EINVAL;

	return 0;
}

int rkvenc_vepu541_img_build(const struct rkvenc_frame *f,
			     struct rkvenc_img *img)
{
	u32 blocks, row_blocks, int_en;
	int ret;

	ret = rkvenc_frame_check(f);
	if (ret)
		return ret;

	memset(img, 0, sizeof(*img));

	int_en = RKVENC_INT_ENC_DONE | RKVENC_INT_SCLR_DONE | RKVENC_INT_OFLW |
		 RKVENC_INT_BRSP | RKVENC_INT_BERR | RKVENC_INT_RERR;
	if (f->wdg_vs_load)
		int_en |= RKVENC_INT_WDG;
	rkvenc_img_l1(img, RKVENC_INT_EN, int_en);
	rkvenc_img_l1(img, RKVENC_ENC_WDG,
		      FIELD_PREP(RKVENC_ENC_WDG_VS_LOAD_THD, f->wdg_vs_load));

	rkvenc_buffers_build(f, img);

	if (f->codec == RKVENC_CODEC_H264) {
		row_blocks = DIV_ROUND_UP(f->src.width, 16);
		blocks = row_blocks * DIV_ROUND_UP(f->src.height, 16);
		rkvenc_vepu541_h264_build(f, img);
	} else {
		row_blocks = DIV_ROUND_UP(f->src.width, 64);
		blocks = row_blocks * DIV_ROUND_UP(f->src.height, 64);
		rkvenc_vepu541_hevc_build(f, img);
	}

	rkvenc_rc_build(f, blocks, row_blocks, img);

	img->enc_strt = FIELD_PREP(RKVENC_ENC_STRT_CMD,
				   RKVENC_ENC_STRT_CMD_FRAME) |
			RKVENC_ENC_STRT_CLK_GATE_EN |
			RKVENC_ENC_STRT_DONE_TMVP_EN;

	return 0;
}
