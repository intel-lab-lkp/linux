// SPDX-License-Identifier: GPL-2.0-only
/*
 * RKVENC v1 register image builder, H.264 part.
 */

#include "rkvenc-vepu541.h"

#define H264_PROFILE_MAIN	77
#define H264_LEVEL_1		10
#define H264_LEVEL_3		30
#define H264_NAL_SLICE		1
#define H264_NAL_IDR		5
#define H264_SLICE_P		0
#define H264_SLICE_I		2

/* Coarse-ME RAM height by MB-aligned width */
static const u16 h264_rama_h_width[] = {
	3584, 3136, 2816, 2560, 2368, 2176, 2048, 1856, 1792,
};

static void h264_me_build(const struct rkvenc_frame *f, struct rkvenc_img *img)
{
	const struct rkvenc_h264_seq *seq = f->h264.seq;
	u32 w16 = ALIGN(f->src.width, 16), h16 = ALIGN(f->src.height, 16);
	u32 mbw = w16 / 16, w64 = DIV_ROUND_UP(w16, 64);
	u32 srch_h, srch_v, rama_h, v, hc, i;
	u32 ah64 = ALIGN(h16, 64);

	srch_h = mbw < 22 ? mbw / 2 : 11;
	srch_v = seq->level_idc <= H264_LEVEL_1 ? 3 : 7;
	if (ah64 <= 64)
		srch_v = 0;
	else if ((ah64 == 128 || ah64 == 192) && seq->level_idc > H264_LEVEL_1)
		srch_v = 4;

	rkvenc_img_l1(img, RKVENC_ME_RNGE,
		      FIELD_PREP(RKVENC_ME_RNGE_CME_SRCH_H, srch_h) |
		      FIELD_PREP(RKVENC_ME_RNGE_CME_SRCH_V, srch_v) |
		      FIELD_PREP(RKVENC_ME_RNGE_RME_SRCH_H, 7) |
		      FIELD_PREP(RKVENC_ME_RNGE_RME_SRCH_V, 5));

	rkvenc_img_l1(img, RKVENC_ME_CNST,
		      FIELD_PREP(RKVENC_ME_CNST_PMV_MDST_H, f->intra ? 0 : 5) |
		      FIELD_PREP(RKVENC_ME_CNST_PMV_MDST_V, f->intra ? 0 : 5) |
		      FIELD_PREP(RKVENC_ME_CNST_MV_LIMIT, 2) |
		      FIELD_PREP(RKVENC_ME_CNST_PMV_NUM, 2));

	rama_h = 17;
	for (i = 0; i < ARRAY_SIZE(h264_rama_h_width); i++) {
		if (w16 > h264_rama_h_width[i]) {
			rama_h = 8 + i;
			break;
		}
	}
	v = 2 * srch_v + 1;
	hc = (srch_h * 4 + 15) / 16 * 2 + 1;
	rkvenc_img_l1(img, RKVENC_ME_RAM,
		      FIELD_PREP(RKVENC_ME_RAM_CME_RAMA_MAX,
				 (min(v, rama_h) - 1) * w64 + hc) |
		      FIELD_PREP(RKVENC_ME_RAM_CME_RAMA_H, rama_h));
}

static void h264_syntax_build(const struct rkvenc_frame *f,
			      struct rkvenc_img *img)
{
	const struct rkvenc_h264_seq *seq = f->h264.seq;
	const struct rkvenc_h264_pic *pic = &f->h264;
	u32 rdo, sli2;

	rdo = RKVENC_H264_RDO_INTER_4X4 | RKVENC_H264_RDO_CHRM_SPCL |
	      RKVENC_H264_RDO_CCWA_E | RKVENC_H264_RDO_ATR_E;
	if (seq->profile_idc < H264_PROFILE_MAIN && seq->level_idc <= H264_LEVEL_3)
		rdo |= RKVENC_H264_RDO_RECT_SIZE;
	if (seq->profile_idc < H264_PROFILE_MAIN && !seq->cabac)
		rdo |= RKVENC_H264_RDO_VLC_LMT;
	rkvenc_img_l1(img, RKVENC_RDO_CFG, rdo);
	rkvenc_img_l1(img, RKVENC_IPRD_CSTS,
		      FIELD_PREP(RKVENC_IPRD_CSTS_VTHD_Y, 2501) |
		      FIELD_PREP(RKVENC_IPRD_CSTS_VTHD_C, 2501));

	rkvenc_img_l1(img, RKVENC_SYNT_NAL,
		      FIELD_PREP(RKVENC_H264_NAL_REF_IDC, pic->nal_ref_idc) |
		      FIELD_PREP(RKVENC_H264_NAL_UNIT_TYPE,
				 f->intra ? H264_NAL_IDR : H264_NAL_SLICE));
	rkvenc_img_l1(img, RKVENC_SYNT_SPS,
		      FIELD_PREP(RKVENC_H264_SPS_MAX_FNUM,
				 seq->log2_max_frame_num_m4) |
		      RKVENC_H264_SPS_DRCT_8X8 |
		      FIELD_PREP(RKVENC_H264_SPS_MPOC_LM4,
				 seq->log2_max_poc_lsb_m4));
	rkvenc_img_l1(
		img, RKVENC_SYNT_PPS,
		(seq->cabac ? RKVENC_H264_PPS_ETPY_MODE : 0) |
			(seq->transform_8x8 ? RKVENC_H264_PPS_TRNS_8X8 : 0) |
			FIELD_PREP(RKVENC_PPS_PIC_INIT_QP, seq->pic_init_qp) |
			FIELD_PREP(RKVENC_H264_PPS_CB_OFST,
				   rkvenc_s2u(seq->chroma_qp_offset, 5)) |
			FIELD_PREP(RKVENC_H264_PPS_CR_OFST,
				   rkvenc_s2u(seq->chroma_qp_offset, 5)) |
			RKVENC_H264_PPS_DBF_CP_FLG);
	rkvenc_img_l1(img, RKVENC_SYNT_SLI0,
		      FIELD_PREP(RKVENC_H264_SLI0_SLI_TYPE,
				 f->intra ? H264_SLICE_I : H264_SLICE_P) |
		      FIELD_PREP(RKVENC_H264_SLI0_FRM_NUM, pic->frame_num));
	rkvenc_img_l1(img, RKVENC_SYNT_SLI1,
		      FIELD_PREP(RKVENC_H264_SLI1_IDR_PIC_ID,
				 f->intra ? pic->idr_pic_id : 0) |
		      FIELD_PREP(RKVENC_H264_SLI1_POC_LSB, pic->poc_lsb));

	sli2 = FIELD_PREP(RKVENC_H264_SLI2_DIS_DBLK_IDC, seq->dblk_idc);
	sli2 |= FIELD_PREP(RKVENC_H264_SLI2_ALPH_OFST,
			   rkvenc_s2u(seq->alpha_offset, 4));
	sli2 |= FIELD_PREP(RKVENC_H264_SLI2_BETA_OFST,
			   rkvenc_s2u(seq->beta_offset, 4));
	rkvenc_img_l1(img, RKVENC_SYNT_SLI2, sli2);
}

static void h264_l2_build(const struct rkvenc_frame *f, struct rkvenc_img *img)
{
	u32 i;

	for (i = 0; i < 2; i++) {
		rkvenc_img_l2(img, RKVENC_L2_IPRD_TTHDY4(i),
			      i ? rkvenc_pair(9, 36) : rkvenc_pair(1, 4));
		rkvenc_img_l2(img, RKVENC_L2_IPRD_TTHDC8(i),
			      i ? rkvenc_pair(9, 36) : rkvenc_pair(1, 4));
		rkvenc_img_l2(img, RKVENC_L2_IPRD_TTHDY8(i),
			      i ? rkvenc_pair(9, 36) : rkvenc_pair(1, 4));
	}
	rkvenc_img_l2(img, RKVENC_L2_IPRD_TTHD_UL, 2501);
	rkvenc_img_l2(img, RKVENC_L2_IPRD_WGTY8, rkvenc_bytes(48, 60, 40, 48));
	rkvenc_img_l2(img, RKVENC_L2_IPRD_WGTY4, rkvenc_bytes(48, 60, 40, 48));
	rkvenc_img_l2(img, RKVENC_L2_IPRD_WGTY16, rkvenc_bytes(48, 60, 40, 48));
	rkvenc_img_l2(img, RKVENC_L2_IPRD_WGTC8, rkvenc_bytes(36, 42, 28, 32));

	/* Rounding offsets in 1/2048 units */
	rkvenc_img_l2(img, RKVENC_L2_QNT_BIAS,
		      FIELD_PREP(RKVENC_L2_QNT_BIAS_I, f->intra ? 683 : 583) |
		      FIELD_PREP(RKVENC_L2_QNT_BIAS_P, 171));

	rkvenc_img_l2(img, RKVENC_L2_ATR_THD0, rkvenc_pair(1, 4));
	rkvenc_img_l2(img, RKVENC_L2_ATR_THD1,
		      rkvenc_pair(f->intra ? 36 : 81, 45));
	rkvenc_img_l2(img, RKVENC_L2_ATR_WGT16, f->intra ?
		      rkvenc_bytes(16, 16, 16, 0) : rkvenc_bytes(18, 17, 16, 0));
	rkvenc_img_l2(img, RKVENC_L2_ATR_WGT8, rkvenc_bytes(20, 20, 20, 0));
	rkvenc_img_l2(img, RKVENC_L2_ATR_WGT4, f->intra ?
		      rkvenc_bytes(20, 18, 16, 0) : rkvenc_bytes(18, 17, 16, 0));

	rkvenc_img_l2(img, RKVENC_L2_ATF_TTHD(0), rkvenc_pair(0, 64));
	rkvenc_img_l2(img, RKVENC_L2_ATF_TTHD(1), rkvenc_pair(144, 2500));
	rkvenc_img_l2(img, RKVENC_L2_ATF_STHD(0), rkvenc_pair(80, 280));
	rkvenc_img_l2(img, RKVENC_L2_ATF_STHD(1), rkvenc_pair(144, 192));
	rkvenc_img_l2(img, RKVENC_L2_ATF_WGT(0), rkvenc_pair(26, 24));
	rkvenc_img_l2(img, RKVENC_L2_ATF_WGT(1), rkvenc_pair(19, 22));
	rkvenc_img_l2(img, RKVENC_L2_ATF_WGT(2), rkvenc_pair(19, 19));
	rkvenc_img_l2(img, RKVENC_L2_ATF_OFST(0), rkvenc_pair(3500, 3500));
	rkvenc_img_l2(img, RKVENC_L2_ATF_OFST(1), rkvenc_pair(0, 3500));
	rkvenc_img_l2(img, RKVENC_L2_ATF_OFST(2), rkvenc_pair(1000, 0));

	for (i = 0; i <= RKVENC_QP_MAX; i++) {
		rkvenc_img_l2(img, RKVENC_L2_RDO_WGTA_QP(i),
			      rkvenc_h264_lambda[i + 1]);
		rkvenc_img_l2(img, RKVENC_L2_RDO_WGTB_QP(i),
			      rkvenc_h264_lambda[i]);
	}

	rkvenc_img_l2_u8(img, RKVENC_L2_AQ_TTHD(0), rkvenc_h264_aq_thd, 16);
	rkvenc_img_l2_s6(img, RKVENC_L2_AQ_STP(0), f->intra ?
			 rkvenc_h264_aq_step_i : rkvenc_h264_aq_step_p, 16);

	rkvenc_img_l2(img, RKVENC_L2_RME_MVD_PNSH, 16000 << 6);
	rkvenc_img_l2(img, RKVENC_L2_ATR1_THD0, rkvenc_pair(1, 4));
	rkvenc_img_l2(img, RKVENC_L2_ATR1_THD1, 49);
	img->l2_last = RKVENC_L2_H264_LAST;
}

void rkvenc_vepu541_h264_build(const struct rkvenc_frame *f,
			       struct rkvenc_img *img)
{
	u32 w16 = ALIGN(f->src.width, 16), h16 = ALIGN(f->src.height, 16);
	u32 pic, i;

	rkvenc_img_l1(img, RKVENC_ENC_RSL,
		      FIELD_PREP(RKVENC_ENC_RSL_WD8_M1, w16 / 8 - 1) |
		      FIELD_PREP(RKVENC_ENC_RSL_WFILL, w16 - f->src.width) |
		      FIELD_PREP(RKVENC_ENC_RSL_HD8_M1, h16 / 8 - 1) |
		      FIELD_PREP(RKVENC_ENC_RSL_HFILL, h16 - f->src.height));

	pic = RKVENC_ENC_PIC_BS_SCP | FIELD_PREP(RKVENC_ENC_PIC_QP, f->qp);
	if (f->referenced)
		pic |= RKVENC_ENC_PIC_CUR_FRM_REF;
	if (!f->intra)
		pic |= RKVENC_ENC_PIC_LAMB_MOD_SEL;
	rkvenc_img_l1(img, RKVENC_ENC_PIC, pic);

	rkvenc_img_l1(img, RKVENC_DTRNS_CFG, RKVENC_DTRNS_CFG_DSPR_OTSD);

	rkvenc_img_l1(img, RKVENC_KLUT_OFST, f->intra ? 0 : 3);
	for (i = 0; i < RKVENC_KLUT_WORDS; i++)
		rkvenc_img_l1(img, RKVENC_KLUT_WGT(i), rkvenc_h264_klut[i]);

	h264_me_build(f, img);
	h264_syntax_build(f, img);
	h264_l2_build(f, img);
}
