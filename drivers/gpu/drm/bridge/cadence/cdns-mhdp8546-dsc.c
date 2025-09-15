// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Cadence Design Systems, Inc.
 *
 * Author: Swapnil Jakhade <sjakhade@cadence.com>
 */

#include "cdns-mhdp8546-core.h"
#include "cdns-mhdp8546-dsc.h"

void cdns_mhdp_dsc_write_config(struct cdns_mhdp_device *mhdp)
{
	u32 main_conf = 0;

	main_conf = CDNS_DP_COM_MAIN_CONF_INPUT_MODE |
				CDNS_DP_COM_MAIN_CONF_AUTO_DB_UPDATE;

	if (mhdp->dsc_params.dsc_split) {
		main_conf |= CDNS_DP_COM_MAIN_CONF_MUX_MODE |
					 CDNS_DP_COM_MAIN_CONF_SPLIT_PANEL;
	}

	writel(main_conf, mhdp->dsc_regs + CDNS_DP_COM_MAIN_CONF);
}

static u8 cdns_mhdp_dsc_compute_initial_lines(struct cdns_mhdp_device *mhdp)
{
	struct drm_dsc_config *dsc_cfg = &mhdp->dsc_config;
	unsigned long k1, real_bpp;
	u8 initial_lines;

	real_bpp = (unsigned long)dsc_cfg->bits_per_pixel / 16;

	if (dsc_cfg->bits_per_component == 8)
		k1 = 296;
	else
		k1 = 320;

	if (mhdp->dsc_params.dsc_split) {
		initial_lines = (u8)DIV_ROUND_UP
				((k1 + (unsigned long)dsc_cfg->initial_xmit_delay +
				((((unsigned long)dsc_cfg->slice_chunk_size * 8) + 144) /
				real_bpp)), (unsigned long)dsc_cfg->slice_width);
	} else {
		initial_lines = (u8)DIV_ROUND_UP
				((k1 + (unsigned long)dsc_cfg->initial_xmit_delay +
				((DIV_ROUND_UP(((1 - (real_bpp / 48)) *
				((unsigned long)dsc_cfg->slice_chunk_size * 8)), 1) + 144) /
				real_bpp)), (unsigned long)dsc_cfg->slice_width);
	}

	return initial_lines;
}

static void write_enc_main_conf(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val = 0;

	if (mhdp->dsc_config.convert_rgb)
		reg_val |= CDNS_DP_ENC_MAIN_CONF_CONVERT_RGB;

	reg_val |= (mhdp->dsc_config.line_buf_depth <<
		   CDNS_DP_ENC_MAIN_CONF_LINEBUF_DEPTH_SHIFT) &
		   CDNS_DP_ENC_MAIN_CONF_LINEBUF_DEPTH;

	reg_val |= (mhdp->dsc_config.bits_per_pixel <<
		   CDNS_DP_ENC_MAIN_CONF_BITS_PER_PIXEL_SHIFT) &
		   CDNS_DP_ENC_MAIN_CONF_BITS_PER_PIXEL;

	if (mhdp->dsc_config.block_pred_enable)
		reg_val |= CDNS_DP_ENC_MAIN_CONF_BLOCK_PRED_EN;

	reg_val |= CDNS_DP_ENC_MAIN_CONF_VIDEO_MODE;

	reg_val |= (cdns_mhdp_dsc_compute_initial_lines(mhdp) <<
		   CDNS_DP_ENC_MAIN_CONF_INITIAL_LINES_SHIFT) &
		   CDNS_DP_ENC_MAIN_CONF_INITIAL_LINES;

	if (mhdp->dsc_config.bits_per_component == 10)
		reg_val |= 0x1;
	else if (mhdp->dsc_config.bits_per_component != 8)
		dev_err(mhdp->dev, "Unsupported bits_per_component = %d\n",
			mhdp->dsc_config.bits_per_component);

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_MAIN_CONF(stream_id));
}

static void write_enc_picture_size(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val = 0;

	reg_val = (mhdp->dsc_config.pic_width &
		   CDNS_DP_ENC_PICTURE_SIZE_WIDTH);

	reg_val |= (mhdp->dsc_config.pic_height <<
		   CDNS_DP_ENC_PICTURE_SIZE_HEIGHT_SHIFT) &
		   CDNS_DP_ENC_PICTURE_SIZE_HEIGHT;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_PICTURE_SIZE(stream_id));
}

static void write_enc_slice_size(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val = 0;

	reg_val = (mhdp->dsc_config.slice_width &
		   CDNS_DP_ENC_SLICE_SIZE_WIDTH);

	reg_val |= (mhdp->dsc_config.slice_height <<
		   CDNS_DP_ENC_SLICE_SIZE_HEIGHT_SHIFT) &
		   CDNS_DP_ENC_SLICE_SIZE_HEIGHT;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_SLICE_SIZE(stream_id));
}

static void write_enc_misc_size(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val = 0;

	reg_val = ((mhdp->dsc_config.slice_width + 2) % 3) &
		   CDNS_DP_ENC_MISC_SIZE_LAST_GRP_SIZE;

	reg_val |= (DSC_OUTPUT_BUFFER_MAX_ADDRESS <<
		   CDNS_DP_ENC_MISC_SIZE_OB_MAX_ADDR_SHIFT) &
		   CDNS_DP_ENC_MISC_SIZE_OB_MAX_ADDR;

	reg_val |= (mhdp->dsc_config.slice_chunk_size <<
		   CDNS_DP_ENC_MISC_SIZE_CHUNK_SIZE_SHIFT) &
		   CDNS_DP_ENC_MISC_SIZE_CHUNK_SIZE;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_MISC_SIZE(stream_id));
}

static void write_enc_hrd_delays(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val = 0;

	reg_val = (mhdp->dsc_config.initial_xmit_delay &
		   CDNS_DP_ENC_HRD_DELAYS_INIT_XMIT_DELAY);

	reg_val |= (mhdp->dsc_config.initial_dec_delay <<
		   CDNS_DP_ENC_HRD_DELAYS_INIT_DEC_DELAY_SHIFT) &
		   CDNS_DP_ENC_HRD_DELAYS_INIT_DEC_DELAY;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_HRD_DELAYS(stream_id));
}

static void write_enc_rc_scale(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val = 0;

	reg_val = (mhdp->dsc_config.initial_scale_value &
		   CDNS_DP_ENC_RC_SCALE_INIT_SCALE_VALUE);

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_SCALE(stream_id));
}

static void write_enc_rc_scale_inc_dec(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val = 0;

	reg_val = (mhdp->dsc_config.scale_increment_interval &
		   CDNS_DP_ENC_RC_SCALE_INC_INTERVAL);

	reg_val |= (mhdp->dsc_config.scale_decrement_interval <<
		   CDNS_DP_ENC_RC_SCALE_DEC_INTERVAL_SHIFT) &
		   CDNS_DP_ENC_RC_SCALE_DEC_INTERVAL;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_SCALE_INC_DEC(stream_id));
}

static void write_enc_rc_offsets(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val;

	reg_val = (mhdp->dsc_config.first_line_bpg_offset &
		     CDNS_DP_ENC_RC_OFFSETS_1_FL_BPG_OFFSET);

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_OFFSETS_1(stream_id));

	reg_val = (mhdp->dsc_config.nfl_bpg_offset &
		   CDNS_DP_ENC_RC_OFFSETS_2_NFL_BPG_OFFSET);

	reg_val |= (mhdp->dsc_config.slice_bpg_offset <<
		   CDNS_DP_ENC_RC_OFFSETS_2_SL_BPG_OFFSET_SHIFT) &
		   CDNS_DP_ENC_RC_OFFSETS_2_SL_BPG_OFFSET;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_OFFSETS_2(stream_id));

	reg_val = (mhdp->dsc_config.initial_offset &
		   CDNS_DP_ENC_RC_OFFSETS_3_INIT_OFFSET);

	reg_val |= (mhdp->dsc_config.final_offset <<
		   CDNS_DP_ENC_RC_OFFSETS_3_FINAL_OFFSET_SHIFT) &
		   CDNS_DP_ENC_RC_OFFSETS_3_FINAL_OFFSET;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_OFFSETS_3(stream_id));
}

static void write_enc_flatness_detection(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val;

	reg_val = (mhdp->dsc_config.flatness_min_qp &
		   CDNS_DP_ENC_FLATNESS_DETECTION_MIN_QP);

	reg_val |= (mhdp->dsc_config.flatness_max_qp <<
		   CDNS_DP_ENC_FLATNESS_DETECTION_MAX_QP_SHIFT) &
		   CDNS_DP_ENC_FLATNESS_DETECTION_MAX_QP;

	reg_val |= (drm_dsc_flatness_det_thresh(&mhdp->dsc_config) <<
		   CDNS_DP_ENC_FLATNESS_DETECTION_THRESH_SHIFT) &
		   CDNS_DP_ENC_FLATNESS_DETECTION_THRESH;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_FLATNESS_DETECTION(stream_id));
}

static void write_enc_rc_model_size(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val;

	reg_val = (mhdp->dsc_config.rc_model_size &
		   CDNS_DP_ENC_RC_MODEL_SIZE_RC_MODEL_SIZE);

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_MODEL_SIZE(stream_id));
}

static void write_enc_rc_config(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val;

	reg_val = (mhdp->dsc_config.rc_edge_factor &
		   CDNS_DP_ENC_RC_CONFIG_EDGE_FACTOR);

	reg_val |= (mhdp->dsc_config.rc_quant_incr_limit0 <<
		   CDNS_DP_ENC_RC_CONFIG_QUANT_INC_LIM_0_SHIFT) &
		   CDNS_DP_ENC_RC_CONFIG_QUANT_INC_LIM_0;

	reg_val |= (mhdp->dsc_config.rc_quant_incr_limit1 <<
		   CDNS_DP_ENC_RC_CONFIG_QUANT_INC_LIM_1_SHIFT) &
		   CDNS_DP_ENC_RC_CONFIG_QUANT_INC_LIM_1;

	reg_val |= (mhdp->dsc_config.rc_tgt_offset_high <<
		   CDNS_DP_ENC_RC_CONFIG_TGT_OFFSET_HI_SHIFT) &
		   CDNS_DP_ENC_RC_CONFIG_TGT_OFFSET_HI;

	reg_val |= (mhdp->dsc_config.rc_tgt_offset_low <<
		   CDNS_DP_ENC_RC_CONFIG_TGT_OFFSET_LO_SHIFT) &
		   CDNS_DP_ENC_RC_CONFIG_TGT_OFFSET_LO;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_CONFIG(stream_id));
}

static void write_enc_rc_buf_thresh(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val;
	int index = 0;

	reg_val = (mhdp->dsc_config.rc_buf_thresh[index++] &
		   CDNS_DP_ENC_RC_BUF_THRESH_0_THRESH_0);

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_1_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_0_THRESH_1;

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_2_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_0_THRESH_2;

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_3_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_0_THRESH_3;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_BUF_THRESH_0(stream_id));

	reg_val = (mhdp->dsc_config.rc_buf_thresh[index++] &
		   CDNS_DP_ENC_RC_BUF_THRESH_1_THRESH_4);

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_1_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_1_THRESH_5;

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_2_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_1_THRESH_6;

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_3_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_1_THRESH_7;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_BUF_THRESH_1(stream_id));

	reg_val = (mhdp->dsc_config.rc_buf_thresh[index++] &
		   CDNS_DP_ENC_RC_BUF_THRESH_2_THRESH_8);

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_1_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_2_THRESH_9;

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_2_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_2_THRESH_10;

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_3_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_2_THRESH_11;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_BUF_THRESH_2(stream_id));

	reg_val = (mhdp->dsc_config.rc_buf_thresh[index++] &
		   CDNS_DP_ENC_RC_BUF_THRESH_3_THRESH_12);

	reg_val |= (mhdp->dsc_config.rc_buf_thresh[index++] <<
		   CDNS_DP_ENC_RC_BUF_THRESH_THRESH_1_SHIFT) &
		   CDNS_DP_ENC_RC_BUF_THRESH_3_THRESH_13;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_BUF_THRESH_3(stream_id));
}

static void write_enc_rc_min_qp(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val;
	int index = 0;

	reg_val = (mhdp->dsc_config.rc_range_params[index++].range_min_qp &
		   CDNS_DP_ENC_RC_MIN_QP_0_RANGE_0);

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_1_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_0_RANGE_1;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_2_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_0_RANGE_2;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_3_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_0_RANGE_3;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_4_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_0_RANGE_4;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_MIN_QP_0(stream_id));

	reg_val = (mhdp->dsc_config.rc_range_params[index++].range_min_qp &
		   CDNS_DP_ENC_RC_MIN_QP_1_RANGE_5);

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_1_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_1_RANGE_6;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_2_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_1_RANGE_7;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_3_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_1_RANGE_8;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_4_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_1_RANGE_9;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_MIN_QP_1(stream_id));

	reg_val = (mhdp->dsc_config.rc_range_params[index++].range_min_qp &
		   CDNS_DP_ENC_RC_MIN_QP_2_RANGE_10);

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_1_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_2_RANGE_11;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_2_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_2_RANGE_12;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_3_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_2_RANGE_13;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_min_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_4_SHIFT) &
		   CDNS_DP_ENC_RC_MIN_QP_2_RANGE_14;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_MIN_QP_2(stream_id));
}

static void write_enc_rc_max_qp(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val;
	int index = 0;

	reg_val = (mhdp->dsc_config.rc_range_params[index++].range_max_qp &
		   CDNS_DP_ENC_RC_MAX_QP_0_RANGE_0);

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_1_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_0_RANGE_1;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_2_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_0_RANGE_2;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_3_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_0_RANGE_3;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_4_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_0_RANGE_4;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_MAX_QP_0(stream_id));

	reg_val = (mhdp->dsc_config.rc_range_params[index++].range_max_qp &
		   CDNS_DP_ENC_RC_MAX_QP_1_RANGE_5);

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_1_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_1_RANGE_6;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_2_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_1_RANGE_7;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_3_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_1_RANGE_8;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_4_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_1_RANGE_9;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_MAX_QP_1(stream_id));

	reg_val = (mhdp->dsc_config.rc_range_params[index++].range_max_qp &
		   CDNS_DP_ENC_RC_MAX_QP_2_RANGE_10);

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_1_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_2_RANGE_11;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_2_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_2_RANGE_12;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_3_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_2_RANGE_13;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_max_qp <<
		   CDNS_DP_ENC_RC_QP_RANGE_4_SHIFT) &
		   CDNS_DP_ENC_RC_MAX_QP_2_RANGE_14;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_MAX_QP_2(stream_id));
}

static void write_enc_rc_range_bpg(struct cdns_mhdp_device *mhdp, int stream_id)
{
	u32 reg_val;
	int index = 0;

	reg_val = (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_0);

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_1_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_1;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_2_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_2;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_3_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_3;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_4_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_4;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0(stream_id));

	reg_val = (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_5);

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_1_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_6;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_2_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_7;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_3_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_8;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_4_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_9;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_1(stream_id));

	reg_val = (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_10);

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_1_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_11;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_2_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_12;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_3_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_13;

	reg_val |= (mhdp->dsc_config.rc_range_params[index++].range_bpg_offset <<
		   CDNS_DP_ENC_RC_QP_RANGE_4_SHIFT) &
		   CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_0_14;

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_RC_RANGE_BPG_OFFSETS_2(stream_id));
}

static void write_enc_dpi_ctrl(struct cdns_mhdp_device *mhdp, int stream_id,
			       const struct drm_display_mode *mode)
{
	u32 reg_val = 0;

	reg_val = ((mode->crtc_htotal * cdns_mhdp_dsc_compute_initial_lines(mhdp)) &
		  CDNS_DP_ENC_DPI_CTRL_OUT_DELAY_CYCLES);

	writel(reg_val, mhdp->dsc_regs + CDNS_DP_ENC_DPI_CTRL_OUT_DELAY(stream_id));
}

void cdns_mhdp_dsc_write_enc_config(struct cdns_mhdp_device *mhdp, int stream_id,
				    const struct drm_display_mode *mode)
{
	write_enc_main_conf(mhdp, stream_id);
	write_enc_picture_size(mhdp, stream_id);
	write_enc_slice_size(mhdp, stream_id);
	write_enc_misc_size(mhdp, stream_id);
	write_enc_hrd_delays(mhdp, stream_id);
	write_enc_rc_scale(mhdp, stream_id);
	write_enc_rc_scale_inc_dec(mhdp, stream_id);
	write_enc_rc_offsets(mhdp, stream_id);
	write_enc_flatness_detection(mhdp, stream_id);
	write_enc_rc_model_size(mhdp, stream_id);
	write_enc_rc_config(mhdp, stream_id);
	write_enc_rc_buf_thresh(mhdp, stream_id);
	write_enc_rc_min_qp(mhdp, stream_id);
	write_enc_rc_max_qp(mhdp, stream_id);
	write_enc_rc_range_bpg(mhdp, stream_id);
	write_enc_dpi_ctrl(mhdp, stream_id, mode);
}

int cdns_mhdp_dsc_sink_support(struct cdns_mhdp_device *mhdp)
{
	int ret;

	ret = drm_dp_dpcd_read(&mhdp->aux, DP_DSC_SUPPORT, &mhdp->dsc_params.dsc_cap,
			       DP_DSC_RECEIVER_CAP_SIZE);
	if (ret != DP_DSC_RECEIVER_CAP_SIZE) {
		DRM_DEV_ERROR(mhdp->dev, "cannot read sink DSC DPCD: %d\n", ret);
		goto err;
	}
	if (!(mhdp->dsc_params.dsc_cap[0] & DP_DSC_DECOMPRESSION_IS_SUPPORTED)) {
		ret = -EOPNOTSUPP;
		DRM_DEV_ERROR(mhdp->dev, "sink does not support DSC: %d\n", ret);
		goto err;
	}

	ret = 0;
err:
	return ret;
}

int cdns_mhdp_compute_dsc_params(struct cdns_mhdp_device *mhdp)
{
	struct drm_bridge *bridge = &mhdp->bridge;
	struct drm_dsc_config *dsc_cfg = &mhdp->dsc_config;
	struct drm_crtc_state *crtc_state = bridge->encoder->crtc->state;
	u8 *dsc_dpcd = (u8 *)&mhdp->dsc_params.dsc_cap;
	int ret;

	dsc_cfg->pic_width = crtc_state->adjusted_mode.crtc_hdisplay;
	dsc_cfg->pic_height = crtc_state->adjusted_mode.crtc_vdisplay;

	dsc_cfg->slice_width = dsc_cfg->pic_width;
	dsc_cfg->slice_height = DIV_ROUND_UP(dsc_cfg->pic_height,
					     mhdp->dsc_params.slice_count);

	dsc_cfg->dsc_version_major = (dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT] &
				      DP_DSC_MAJOR_MASK) >> DP_DSC_MAJOR_SHIFT;
	dsc_cfg->dsc_version_minor =
		min(DSC_SUPPORTED_VERSION_MIN,
		    (dsc_dpcd[DP_DSC_REV - DP_DSC_SUPPORT] &
		     DP_DSC_MINOR_MASK) >> DP_DSC_MINOR_SHIFT);

	dsc_cfg->convert_rgb =
		dsc_dpcd[DP_DSC_DEC_COLOR_FORMAT_CAP - DP_DSC_SUPPORT] & DP_DSC_RGB;

	dsc_cfg->vbr_enable = false;

	dsc_cfg->block_pred_enable =
		dsc_dpcd[DP_DSC_BLK_PREDICTION_SUPPORT - DP_DSC_SUPPORT] &
		DP_DSC_BLK_PREDICTION_IS_SUPPORTED;

	dsc_cfg->bits_per_pixel = mhdp->dsc_params.compressed_bpp << 4;

	dsc_cfg->bits_per_component = mhdp->display_fmt.bpc;

	if (mhdp->dsc_config.bits_per_component == 8)
		dsc_cfg->line_buf_depth = min(9, drm_dp_dsc_sink_line_buf_depth(dsc_dpcd));
	else if (mhdp->dsc_config.bits_per_component == 10)
		dsc_cfg->line_buf_depth = min(11, drm_dp_dsc_sink_line_buf_depth(dsc_dpcd));

	drm_dsc_set_const_params(dsc_cfg);
	drm_dsc_set_rc_buf_thresh(dsc_cfg);

	ret = drm_dsc_setup_rc_params(dsc_cfg, DRM_DSC_1_1_PRE_SCR);
	if (ret) {
		dev_err(mhdp->dev, "could not find DSC RC parameters");
		return ret;
	}

	dsc_cfg->initial_scale_value = drm_dsc_initial_scale_value(dsc_cfg);
	dsc_cfg->slice_count = mhdp->dsc_params.slice_count;

	return drm_dsc_compute_rc_parameters(dsc_cfg);
}

static void cdns_mhdp_write_data_packet(struct cdns_mhdp_device *mhdp, u32 *buf,
					int length, int stream_id)
{
	int i;
	u32 reg_val;

	for (i = 0; i < length; i++) {
		reg_val = buf[i];
		writel(reg_val, mhdp->regs + CDNS_SOURCE_PIF_DATA_WR(stream_id));
	}
}

static void cdns_mhdp_write_pps_header(struct cdns_mhdp_device *mhdp, u32 *buf,
				       int stream_id)
{
	writel(SOURCE_PIF_PPS_PPS, mhdp->regs + CDNS_SOURCE_PIF_PPS(stream_id));
	writel(*buf, mhdp->regs + CDNS_SOURCE_PIF_PPS_HEADER(stream_id));
}

static int cdns_mhdp_write_pps_infoframe(struct cdns_mhdp_device *mhdp, int stream_id,
					 struct drm_dsc_pps_infoframe *pps_infoframe)
{
	u32 reg_val;
	u32 entry_id = 0;

	writel(1, mhdp->regs + CDNS_SOURCE_PIF_FIFO1_FLUSH(stream_id));

	cdns_mhdp_write_pps_header(mhdp, (u32 *)&pps_infoframe->pps_header, 0);
	cdns_mhdp_write_data_packet(mhdp, (u32 *)&pps_infoframe->pps_payload,
				    DP_DSC_PPS_SIZE / 4, 0);

	/* Entry ID */
	writel(entry_id, mhdp->regs + CDNS_SOURCE_PIF_WR_ADDR(stream_id));

	writel(SOURCE_PIF_WR_REQ_HOST_WR, mhdp->regs + CDNS_SOURCE_PIF_WR_REQ(stream_id));

	reg_val = SOURCE_PIF_PKT_ALLOC_REG_ACTIVE_IDLE_TYPE |
		  SOURCE_PIF_PKT_ALLOC_REG_TYPE_VALID |
		  ((DP_SDP_PPS << SOURCE_PIF_PKT_ALLOC_REG_PACKET_TYPE_SHIFT) &
		  SOURCE_PIF_PKT_ALLOC_REG_PACKET_TYPE) |
		  (entry_id & SOURCE_PIF_PKT_ALLOC_REG_PKT_ALLOC_ADDR);

	writel(reg_val, mhdp->regs + CDNS_SOURCE_PIF_PKT_ALLOC_REG(stream_id));

	writel
	(SOURCE_PIF_PKT_ALLOC_WR_EN_EN, mhdp->regs + CDNS_SOURCE_PIF_PKT_ALLOC_WR_EN(stream_id));

	return 0;
}

int cdns_mhdp_dsc_send_pps_sdp(struct cdns_mhdp_device *mhdp, int stream_id)
{
	struct drm_dsc_config *dsc_cfg = &mhdp->dsc_config;
	struct drm_dsc_pps_infoframe dp_dsc_pps_sdp;

	drm_dsc_dp_pps_header_init(&dp_dsc_pps_sdp.pps_header);

	drm_dsc_pps_payload_pack(&dp_dsc_pps_sdp.pps_payload, dsc_cfg);

	return cdns_mhdp_write_pps_infoframe(mhdp, stream_id, &dp_dsc_pps_sdp);
}
