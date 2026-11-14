// SPDX-License-Identifier: GPL-2.0-only
/*
 * RKVENC v1 register image builder, HEVC part.
 */

#include "rkvenc-vepu541.h"

#define HEVC_SLICE_P		1
#define HEVC_SLICE_I		2

static void hevc_me_build(const struct rkvenc_frame *f, struct rkvenc_img *img)
{
	u32 w = ALIGN(f->src.width, 8), h = ALIGN(f->src.height, 8);
	u32 wc = DIV_ROUND_UP(w, 64);
	u32 sx, sy, srch_h, srch_v, rama_h, s, m, l2map;

	sx = w >= 384 + 60 ? 384 : 2 * (min(384U, w) / 4);
	sy = h >= 256 + 60 ? 256 : 2 * (min(256U, h) / 4);
	srch_h = (sx / 2 - 1) / 16;
	srch_v = (sy / 2 - 1) / 16;

	rkvenc_img_l1(img, RKVENC_ME_RNGE,
		      FIELD_PREP(RKVENC_ME_RNGE_CME_SRCH_H, srch_h) |
		      FIELD_PREP(RKVENC_ME_RNGE_CME_SRCH_V, srch_v) |
		      FIELD_PREP(RKVENC_ME_RNGE_RME_SRCH_H, 7) |
		      FIELD_PREP(RKVENC_ME_RNGE_RME_SRCH_V, 5) |
		      FIELD_PREP(RKVENC_ME_RNGE_DLT_FRM_NUM, 1));
	rkvenc_img_l1(img, RKVENC_ME_CNST,
		      FIELD_PREP(RKVENC_ME_CNST_PMV_MDST_H, 5) |
		      FIELD_PREP(RKVENC_ME_CNST_PMV_MDST_V, 5) |
		      FIELD_PREP(RKVENC_ME_CNST_PMV_NUM, 2));

	rama_h = w > 2688 ? 12 : w > 2048 ? 16 : 20;
	s = 2 * ((srch_h + 4) / 4) + 1;
	m = min(2 * ((srch_v + 3) / 4) + 1, rama_h / 4);
	if (wc * 64 <= 512)
		l2map = 0;
	else if (wc * 64 <= 1024)
		l2map = 1;
	else if (wc * 64 <= 2048)
		l2map = 2;
	else
		l2map = 3;
	rkvenc_img_l1(img, RKVENC_ME_RAM,
		      FIELD_PREP(RKVENC_ME_RAM_CME_RAMA_MAX,
				 wc * (m - 1) + (wc >= s ? s : 2 * wc)) |
		      FIELD_PREP(RKVENC_ME_RAM_CME_RAMA_H, rama_h) |
		      FIELD_PREP(RKVENC_ME_RAM_CACH_L2_MAP, l2map));
}

static void hevc_syntax_build(const struct rkvenc_frame *f,
			      struct rkvenc_img *img)
{
	const struct rkvenc_hevc_seq *seq = f->hevc.seq;
	const struct rkvenc_hevc_pic *pic = &f->hevc;
	u32 sli0, sli1;

	rkvenc_img_l1(img, RKVENC_RDO_CFG,
		      RKVENC_HEVC_RDO_CHRM_SPECIAL |
		      FIELD_PREP(RKVENC_HEVC_RDO_CU_INTER_EN, 0xf) |
		      FIELD_PREP(RKVENC_HEVC_RDO_CU_INTRA_EN, 0xf) |
		      RKVENC_HEVC_RDO_CHRM_KLUT_EN);

	rkvenc_img_l1(img, RKVENC_SYNT_NAL,
		      FIELD_PREP(RKVENC_HEVC_NAL_UNIT_TYPE, pic->nal_unit_type));
	rkvenc_img_l1(img, RKVENC_SYNT_SPS,
		      (seq->sao ? RKVENC_HEVC_SPS_SAO_EN : 0) |
		      FIELD_PREP(RKVENC_HEVC_SPS_LOG2_MAX_POC_LSB,
				 seq->log2_max_poc_lsb_m4) |
		      (seq->strong_intra_smoothing ?
		       RKVENC_HEVC_SPS_STRG_INTRA_SMTH : 0));
	rkvenc_img_l1(img, RKVENC_SYNT_PPS,
		      (seq->cabac_init_present ?
		       RKVENC_HEVC_PPS_CBC_INIT_PRSNT : 0) |
		      FIELD_PREP(RKVENC_PPS_PIC_INIT_QP, seq->pic_init_qp) |
		      RKVENC_HEVC_PPS_CU_QP_DLT_EN);

	sli0 = RKVENC_HEVC_SLI0_MERGE_UP | RKVENC_HEVC_SLI0_MERGE_LEFT |
	       RKVENC_HEVC_SLI0_PIC_OUT_FLG |
	       FIELD_PREP(RKVENC_HEVC_SLI0_SLI_TYPE,
			  f->intra ? HEVC_SLICE_I : HEVC_SLICE_P);
	if (seq->cabac_init_present)
		sli0 |= RKVENC_HEVC_SLI0_CBC_INIT_FLG;
	if (seq->sao)
		sli0 |= RKVENC_HEVC_SLI0_SAO_LUMA | RKVENC_HEVC_SLI0_SAO_CHRM;
	rkvenc_img_l1(img, RKVENC_SYNT_SLI0, sli0);

	/* The header QP must match the QP the picture is quantised with */
	sli1 = FIELD_PREP(RKVENC_HEVC_SLI1_SLI_QP, f->qp) |
	       FIELD_PREP(RKVENC_HEVC_SLI1_MAX_MRG_CND, seq->max_merge_cand) |
	       RKVENC_HEVC_SLI1_COL_FRM_L0;
	sli1 |= FIELD_PREP(RKVENC_HEVC_SLI1_TC_OFST,
			   rkvenc_s2u(seq->tc_offset, 4));
	sli1 |= FIELD_PREP(RKVENC_HEVC_SLI1_BETA_OFST,
			   rkvenc_s2u(seq->beta_offset, 4));
	if (seq->deblock_disabled)
		sli1 |= RKVENC_HEVC_SLI1_DBLK_DIS;
	rkvenc_img_l1(img, RKVENC_SYNT_SLI1, sli1);
	rkvenc_img_l1(img, RKVENC_SYNT_SLI2,
		      FIELD_PREP(RKVENC_HEVC_SLI2_POC_LSB, pic->poc_lsb));

	if (!f->intra)
		rkvenc_img_l1(img, RKVENC_SYNT_REFM1,
			      FIELD_PREP(RKVENC_HEVC_REFM1_USED_BY_S0, 1) |
			      FIELD_PREP(RKVENC_HEVC_REFM1_NUM_NEG_PIC, 1));
}

static void hevc_l2_build(struct rkvenc_img *img)
{
	const u16 *thd = rkvenc_hevc_intra_thd;
	u32 i;

	for (i = 0; i < 2; i++) {
		rkvenc_img_l2(img, RKVENC_L2_HEVC_INTRA_THD32(i),
			      thd[2 * i] | thd[2 * i + 1] << 16);
		rkvenc_img_l2(img, RKVENC_L2_HEVC_INTRA_THD16(i),
			      thd[2 * i] | thd[2 * i + 1] << 16);
	}
	rkvenc_img_l2_u8(img, RKVENC_L2_HEVC_INTRA_WGT32(0),
			 rkvenc_hevc_intra_wgt32, 7);
	rkvenc_img_l2_u8(img, RKVENC_L2_HEVC_INTRA_WGT16(0),
			 rkvenc_hevc_intra_wgt16, 7);

	/* Rounding offsets in 1/512 units */
	rkvenc_img_l2(img, RKVENC_L2_QNT_BIAS,
		      FIELD_PREP(RKVENC_L2_QNT_BIAS_I, 171) |
		      FIELD_PREP(RKVENC_L2_QNT_BIAS_P, 85));

	for (i = 0; i < 5; i++)
		rkvenc_img_l2(img, RKVENC_L2_HEVC_ATF_THD(i), rkvenc_pair(0, 63));
	rkvenc_img_l2(img, RKVENC_L2_HEVC_ATF_WGT(0), rkvenc_pair(16, 16));
	rkvenc_img_l2(img, RKVENC_L2_HEVC_ATF_WGT(1), rkvenc_pair(16, 16));
	rkvenc_img_l2(img, RKVENC_L2_HEVC_ATF_WGT(2), 16);

	for (i = 0; i <= RKVENC_QP_MAX; i++) {
		rkvenc_img_l2(img, RKVENC_L2_IPRD_WGT_QP(i),
			      rkvenc_hevc_lambda_satd[i]);
		rkvenc_img_l2(img, RKVENC_L2_RDO_WGTA_QP(i),
			      rkvenc_hevc_lambda_a[i]);
		rkvenc_img_l2(img, RKVENC_L2_RDO_WGTB_QP(i),
			      rkvenc_hevc_lambda_b[i]);
	}

	rkvenc_img_l2_u8(img, RKVENC_L2_AQ_TTHD(0), rkvenc_hevc_aq_thd, 16);
	rkvenc_img_l2_s6(img, RKVENC_L2_AQ_STP(0), rkvenc_hevc_aq_step, 16);
	img->l2_last = RKVENC_L2_HEVC_LAST;
}

void rkvenc_vepu541_hevc_build(const struct rkvenc_frame *f,
			       struct rkvenc_img *img)
{
	u32 w8 = ALIGN(f->src.width, 8), h8 = ALIGN(f->src.height, 8);
	u32 ctus = DIV_ROUND_UP(w8, 64) * DIV_ROUND_UP(h8, 64);
	u32 pic, i;

	rkvenc_img_l1(img, RKVENC_ENC_RSL,
		      FIELD_PREP(RKVENC_ENC_RSL_WD8_M1, w8 / 8 - 1) |
		      FIELD_PREP(RKVENC_ENC_RSL_WFILL, w8 - f->src.width) |
		      FIELD_PREP(RKVENC_ENC_RSL_HD8_M1, h8 / 8 - 1) |
		      FIELD_PREP(RKVENC_ENC_RSL_HFILL, h8 - f->src.height));

	pic = RKVENC_ENC_PIC_STND_HEVC | RKVENC_ENC_PIC_BS_SCP |
	      FIELD_PREP(RKVENC_ENC_PIC_QP, f->qp) |
	      FIELD_PREP(RKVENC_ENC_PIC_TOT_POC_NUM, f->intra ? 0 : 1) |
	      FIELD_PREP(RKVENC_ENC_PIC_LOG2_CTU_NUM, order_base_2(ctus));
	if (f->referenced)
		pic |= RKVENC_ENC_PIC_CUR_FRM_REF;
	if (!f->intra)
		pic |= RKVENC_ENC_PIC_LAMB_MOD_SEL;
	rkvenc_img_l1(img, RKVENC_ENC_PIC, pic);

	rkvenc_img_l1(img, RKVENC_DTRNS_CFG,
		      f->intra ? 0 : RKVENC_DTRNS_CFG_DSPR_OTSD);

	rkvenc_img_l1(img, RKVENC_KLUT_OFST, f->intra ? 0 : 3);
	for (i = 0; i < RKVENC_KLUT_WORDS; i++)
		rkvenc_img_l1(img, RKVENC_KLUT_WGT(i), rkvenc_hevc_klut[i]);

	hevc_me_build(f, img);
	hevc_syntax_build(f, img);
	hevc_l2_build(img);
}
