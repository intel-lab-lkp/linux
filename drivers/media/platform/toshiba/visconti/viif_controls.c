// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-common.h>
#include <media/v4l2-subdev.h>

#include "viif.h"
#include "viif_controls.h"
#include "viif_csi2rx.h"
#include "viif_isp.h"
#include "viif_common.h"
#include "viif_regs.h"

/*=============================================*/
/* parameters */
/*=============================================*/
/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE */
#define VIIF_L1_INPUT_DEPTH_MIN	    8U
#define VIIF_L1_INPUT_DEPTH_MAX	    24U
#define VIIF_L1_INPUT_DEPTH_PWL_MAX 14U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RGB_TO_Y_COEF */
#define VIIF_L1_COEF_MIN 256U
#define VIIF_L1_COEF_MAX 65024U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG_MODE */
#define VIIF_L1_AG_ID_NUM	      4U
#define VIIF_L1_SENSITIVITY_IMAGE_NUM 3U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRE */
#define VIIF_L1_HDRE_MAX_KNEEPOINT_VAL	 0x3fffU
#define VIIF_L1_HDRE_MAX_HDRE_SIG_VAL	 0xffffffU
#define VIIF_L1_HDRE_MAX_OUT_PIXEL_RATIO 0x400000U
#define VIIF_L1_HDRE_MAX_OUT_PIXEL_VAL	 0xffffffU

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_EXTRACTION */
#define VIIF_L1_IMG_EXTRACT_MAX_BLACK_LEVEL_VAL 0xffffffU

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_DPC */
#define VIIF_L1_DPC_MAX_RATIO_LIMIT_VAL 1023U
#define VIIF_L1_DPC_MIN_LUMA_ADJ_VAL	1U
#define VIIF_L1_DPC_MAX_LUMA_ADJ_VAL	31U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_PRESET_WHITE_BALANCE */
#define VIIF_L1_PWHB_MAX_OUT_PIXEL_VAL 4095U
#define VIIF_L1_PWHB_MAX_GAIN_VAL      0x80000U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RAW_COLOR_NOISE_REDUCTION */
#define VIIF_L1_RCNR_MAX_DARK_ADJUSTMENT_VAL	     63U
#define VIIF_L1_RCNR_MAX_LUMA_LINKAGE_ADJUSTMENT_VAL 31U
#define VIIF_L1_RCNR_MAX_ADJUSTMENT_GAIN_VAL	     3U
#define VIIF_L1_RCNR_MAX_ZERO_CLIP_VAL		     256U
#define VIIF_L1_RCNR_MAX_BLEND_VAL		     16U
#define VIIF_L1_RCNR_MAX_BLACK_LEVEL_VAL	     64U
#define VIIF_L1_RCNR_MIN_0DIV_GUARD_VAL		     4U
#define VIIF_L1_RCNR_MAX_0DIV_GUARD_VAL		     16U
#define VIIF_L1_RCNR_MAX_CALC_MSF_NOISE_MULTI_VAL    32U
#define VIIF_L1_RCNR_MAX_GEN_LUMA_SIG_BLEND_VAL	     2U
#define VIIF_L1_RCNR_MAX_UP_LIMIT_GRGB_SENS_RATIO    15U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRS */
#define VIIF_L1_HDRS_MIN_BLEND_RATIO	  0x400U
#define VIIF_L1_HDRS_MAX_BLEND_RATIO	  0x400000U
#define VIIF_L1_HDRS_MAX_DIGITAL_GAIN_VAL 0x400000U
#define VIIF_L1_HDRS_MAX_DST_MAX_VAL	  0xffffffU
#define VIIF_L1_HDRS_MAX_BLEND_PIX_VAL	  4095U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_BLACK_LEVEL_CORRECTION */
#define VIIF_L1_BLACK_LEVEL_MAX_VAL	 0xffffffU
#define VIIF_L1_BLACK_LEVEL_MAX_GAIN_VAL 0x100000U
#define VIIF_L1_BLACK_LEVEL_MAX_DST_VAL	 0xffffffU

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_LSC */
#define VIIF_L1_LSC_MIN_GAIN		-4096
#define VIIF_L1_LSC_MAX_GAIN		4096
#define VIIF_L1_LSC_GRID_MIN_COORDINATE 1U
#define VIIF_L1_LSC_PWB_MAX_COEF_VAL	0x800U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_MAIN_PROCESS */
#define VIIF_DAMP_MAX_LSBSEL		    15U
#define VIIF_MAIN_PROCESS_MAX_OUT_PIXEL_VAL 0xffffffU

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AWB */
#define VIIF_L1_AWB_MIN_GAIN		     64U
#define VIIF_L1_AWB_MAX_GAIN		     1024U
#define VIIF_L1_AWB_GATE_LOWER		     -127
#define VIIF_L1_AWB_GATE_UPPER		     127
#define VIIF_L1_AWB_UNSIGNED_GATE_UPPER	     127U
#define VIIF_L1_AWB_MAX_UV_CONVERGENCE_SPEED 15U
#define VIIF_L1_AWB_MAX_UV_CONVERGENCE_LEVEL 31U
#define VIIF_L1_AWB_INTEGRATION_STOP_TH	     1023U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC */
#define VIIF_L1_HDRC_MAX_THROUGH_SHIFT_VAL 8U
#define VIIF_L1_HDRC_MIN_INPUT_DATA_WIDTH  10U
#define VIIF_L1_HDRC_MAX_INPUT_DATA_WIDTH  24U
#define VIIF_L1_HDRC_MAX_PT_SLOPE	   13U
#define VIIF_L1_HDRC_MAX_BLEND_RATIO	   256U
#define VIIF_L1_HDRC_MAX_FLARE_VAL	   0xffffffU
#define VIIF_L1_HDRC_MAX_BLEND_LUMA	   16U
#define VIIF_L1_HDRC_RATIO_OFFSET	   10U
#define VIIF_REGBUF_ACCESS_TIME		   15360UL
#define VIIF_L1_DELAY_W_HDRC		   31U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC_LTM */
#define VIIF_L1_HDRC_MAX_LTM_TONE_BLEND_RATIO 0x400000U
#define VIIF_L1_HDRC_MAX_LTM_MAGNIFICATION    0x4000U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_GAMMA */
#define VIIF_L1_GAMMA_MAX_VAL 8191U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_QUALITY_ADJUSTMENT */
#define VIIF_L1_SUPPRESSION_MAX_VAL	   0x4000U
#define VIIF_L1_EDGE_SUPPRESSION_MAX_LIMIT 15U
#define VIIF_L1_COLOR_LEVEL_MAX_GAIN	   0x1000U

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AVG_LUM_GENERATION */
#define VIIF_L1_AEXP_MAX_WEIGHT		     3U
#define VIIF_L1_AEXP_MAX_BLOCK_TH	     256U
#define VIIF_L1_AEXP_MAX_SATURATION_PIXEL_TH 0xffffffU
#define VIIF_L1_AEXP_MIN_BLOCK_WIDTH	     64U
#define VIIF_L1_AEXP_MIN_BLOCK_HEIGHT	     64U

/* V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_UNDIST */
#define VIIF_L2_UNDIST_POLY_NUM			11U
#define VIIF_L2_UNDIST_MIN_SENSOR_CROP_OFS_H	-4296
#define VIIF_L2_UNDIST_MAX_SENSOR_CROP_OFS_H	4296
#define VIIF_L2_UNDIST_MIN_SENSOR_CROP_OFS_V	-2360
#define VIIF_L2_UNDIST_MAX_SENSOR_CROP_OFS_V	2360
#define VIIF_L2_UNDIST_MAX_NORM_SCALE		1677721U
#define VIIF_L2_UNDIST_MAX_VALID_R_NORM2	0x4000000U
#define VIIF_L2_UNDIST_MAX_ROI_WRITE_AREA_DELTA 0x800U
#define VIIF_L2_UNDIST_MIN_POLY_COEF		-2147352576
#define VIIF_L2_UNDIST_MAX_POLY_COEF		2147352576
#define VIIF_L2_UNDIST_MIN_GRID_NUM		16U
#define VIIF_L2_UNDIST_MAX_GRID_NUM		64U
#define VIIF_L2_UNDIST_MAX_GRID_TOTAL_NUM	2048U
#define VIIF_L2_UNDIST_MAX_GRID_PATCH_SIZE_INV	0x800000U
#define VIIF_L2_UNDIST_MIN_TABLE_SIZE		0x400U
#define VIIF_L2_UNDIST_MAX_TABLE_SIZE		0x2000U
#define VIIF_L2_UNDIST_MAX_GRID_TOTAL_NUM	2048U
#define VIIF_L2_UNDIST_MAX_GRID_PATCH_SIZE_INV	0x800000U

/* V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_ROI */
#define VIIF_L2_ROI_MIN_SCALE			 32768U
#define VIIF_L2_ROI_MAX_SCALE			 131072U
#define VIIF_L2_ROI_MIN_SCALE_INV		 32768U
#define VIIF_L2_ROI_MAX_SCALE_INV		 131072U
#define VIIF_L2_ROI_MIN_CORRECTED_WO_SCALE_HSIZE 128U
#define VIIF_L2_ROI_MAX_CORRECTED_WO_SCALE_HSIZE 8190U
#define VIIF_L2_ROI_MIN_CORRECTED_WO_SCALE_VSIZE 128U
#define VIIF_L2_ROI_MAX_CORRECTED_WO_SCALE_VSIZE 4094U
#define VIIF_L2_ROI_MIN_CORRECTED_HSIZE		 128U
#define VIIF_L2_ROI_MAX_CORRECTED_HSIZE		 8190U
#define VIIF_L2_ROI_MIN_CORRECTED_VSIZE		 128U
#define VIIF_L2_ROI_MAX_CORRECTED_VSIZE		 4094U

/* V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_GAMMA */
#define VIIF_GAMMA_MAX_VSPLIT 4094U

/*=============================================*/
/* Table Transmission (Memory -> ISP) */
/*=============================================*/
static void viif_config_vdm_tgroup(struct viif_device *viif_dev, int idx)
{
	const struct {
		u32 cfg;
		u32 sram_base;
		u32 sram_size;
	} conf[] = {
		/* T01: L1_SET_DPC, L1_SET_LSC */
		{ VAL_TGROUP_CFG_64BIT_RD, 0x600, 0x20 },
		/* T02: L2_UNDIST grid table */
		{ VAL_TGROUP_CFG_32BIT_RD, 0x620, 0x20 },
		/* T02: L2_GAMMA (path0) */
		{ VAL_TGROUP_CFG_32BIT_RD, 0x640, 0x20 },
		/* T03: L2 GAMMA (path1) */
		{ VAL_TGROUP_CFG_32BIT_RD, 0x660, 0x20 },
	};

	viif_capture_write(viif_dev, REG_VDM_TGROUP_X_CFG(idx), conf[idx].cfg);
	viif_capture_write(viif_dev, REG_VDM_TGROUP_X_SRAM_BASE(idx), conf[idx].sram_base);
	viif_capture_write(viif_dev, REG_VDM_TGROUP_X_SRAM_SIZE(idx), conf[idx].sram_size);
}

/*=============================================*/
/* vendor specific ISP request handlers */
/*=============================================*/
/* V4L2_CID_VISCONTI_VIIF_MAIN_SET_RAWPACK_MODE */
static int viif_main_set_rawpack_mode_try(struct viif_device *viif_dev, u32 *rawpack)
{
	if (!rawpack)
		return -EINVAL;
	if ((*rawpack != VIIF_RAWPACK_DISABLE) && (*rawpack != VIIF_RAWPACK_MSBFIRST) &&
	    (*rawpack != VIIF_RAWPACK_LSBFIRST))
		return -EINVAL;
	return 0;
}

static int viif_main_set_rawpack_mode(struct viif_device *viif_dev, u32 *rawpack)
{
	if (vb2_is_streaming(&viif_dev->cap_dev0.vb2_vq))
		return -EBUSY;

	/* value is already tested in _try*/
	viif_dev->rawpack_mode = *rawpack;
	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE */
static int viif_l1_set_input_mode_try(struct viif_device *viif_dev,
				      struct viif_l1_input_mode_config *arg)
{
	u32 filter = arg->raw_color_filter;
	u32 depth = arg->depth;
	u32 mode = arg->mode;
	u32 depth_max;

	/* SDR input is not supported by this driver*/
	if (mode != VIIF_L1_INPUT_HDR && mode != VIIF_L1_INPUT_PWL &&
	    mode != VIIF_L1_INPUT_HDR_IMG_CORRECT && mode != VIIF_L1_INPUT_PWL_IMG_CORRECT)
		return -EINVAL;

	if (filter != VIIF_L1_RAW_GR_R_B_GB && filter != VIIF_L1_RAW_R_GR_GB_B &&
	    filter != VIIF_L1_RAW_B_GB_GR_R && filter != VIIF_L1_RAW_GB_B_R_GR)
		return -EINVAL;

	if (mode == VIIF_L1_INPUT_PWL || mode == VIIF_L1_INPUT_PWL_IMG_CORRECT)
		depth_max = VIIF_L1_INPUT_DEPTH_PWL_MAX;
	else
		depth_max = VIIF_L1_INPUT_DEPTH_MAX;

	if (depth < VIIF_L1_INPUT_DEPTH_MIN || depth > depth_max || depth % 2U)
		return -EINVAL;

	return 0;
}

static int viif_l1_set_input_mode(struct viif_device *viif_dev,
				  struct viif_l1_input_mode_config *arg)
{
	unsigned long irqflags;

	/* values are already tested in _try*/
	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_SYSM_INPUT_MODE, (u32)arg->mode);
	viif_capture_write(viif_dev, REG_L1_IBUF_DEPTH, (u32)arg->depth);
	viif_capture_write(viif_dev, REG_L1_SYSM_START_COLOR, (u32)arg->raw_color_filter);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RGB_TO_Y_COEF */
static int viif_l1_set_rgb_to_y_coef_try(struct viif_device *viif_dev,
					 struct viif_l1_rgb_to_y_coef_config *arg)
{
	u16 coef_r = arg->coef_r;
	u16 coef_g = arg->coef_g;
	u16 coef_b = arg->coef_b;

	if (coef_r < VIIF_L1_COEF_MIN || coef_r > VIIF_L1_COEF_MAX || coef_g < VIIF_L1_COEF_MIN ||
	    coef_g > VIIF_L1_COEF_MAX || coef_b < VIIF_L1_COEF_MIN || coef_b > VIIF_L1_COEF_MAX) {
		return -EINVAL;
	}
	return 0;
}

static int viif_l1_set_rgb_to_y_coef(struct viif_device *viif_dev,
				     struct viif_l1_rgb_to_y_coef_config *arg)
{
	unsigned long irqflags;

	/* value is already tested in _try*/
	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_SYSM_YCOEF_R, (u32)arg->coef_r);
	viif_capture_write(viif_dev, REG_L1_SYSM_YCOEF_G, (u32)arg->coef_g);
	viif_capture_write(viif_dev, REG_L1_SYSM_YCOEF_B, (u32)arg->coef_b);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG_MODE */
static int viif_l1_set_ag_mode_try(struct viif_device *viif_dev, struct viif_l1_ag_mode_config *arg)
{
	if (arg->sysm_ag_psel_hobc_high >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_psel_hobc_middle_led >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_psel_hobc_low >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_psel_abpc_high >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_psel_abpc_middle_led >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_psel_abpc_low >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_psel_rcnr_high >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_psel_rcnr_middle_led >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_psel_rcnr_low >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_ssel_lssc >= VIIF_L1_SENSITIVITY_IMAGE_NUM ||
	    arg->sysm_ag_psel_lssc >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_ssel_mpro >= VIIF_L1_SENSITIVITY_IMAGE_NUM ||
	    arg->sysm_ag_psel_mpro >= VIIF_L1_AG_ID_NUM ||
	    arg->sysm_ag_ssel_vpro >= VIIF_L1_SENSITIVITY_IMAGE_NUM ||
	    arg->sysm_ag_psel_vpro >= VIIF_L1_AG_ID_NUM) {
		return -EINVAL;
	}
	return 0;
}

static int viif_l1_set_ag_mode(struct viif_device *viif_dev, struct viif_l1_ag_mode_config *arg)
{
	unsigned long irqflags;
	u32 val;

	/* value is already tested in _try*/
	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	/* SYSM_AG_PARAM */
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_PARAM_A,
			   PACK_L1_SYSM_AG_PARAM(arg->sysm_ag_grad[0], arg->sysm_ag_ofst[0]));
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_PARAM_B,
			   PACK_L1_SYSM_AG_PARAM(arg->sysm_ag_grad[1], arg->sysm_ag_ofst[1]));
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_PARAM_C,
			   PACK_L1_SYSM_AG_PARAM(arg->sysm_ag_grad[2], arg->sysm_ag_ofst[2]));
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_PARAM_D,
			   PACK_L1_SYSM_AG_PARAM(arg->sysm_ag_grad[3], arg->sysm_ag_ofst[3]));

	/* SYSM_AG_SEL */
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_SEL_HOBC,
			   PACK_L1_SYSM_AG_SEL_HML(arg->sysm_ag_psel_hobc_high,
						   arg->sysm_ag_psel_hobc_middle_led,
						   arg->sysm_ag_psel_hobc_low));
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_SEL_ABPC,
			   PACK_L1_SYSM_AG_SEL_HML(arg->sysm_ag_psel_abpc_high,
						   arg->sysm_ag_psel_abpc_middle_led,
						   arg->sysm_ag_psel_abpc_low));
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_SEL_RCNR,
			   PACK_L1_SYSM_AG_SEL_HML(arg->sysm_ag_psel_rcnr_high,
						   arg->sysm_ag_psel_rcnr_middle_led,
						   arg->sysm_ag_psel_rcnr_low));

	viif_capture_write(viif_dev, REG_L1_SYSM_AG_SEL_LSSC,
			   PACK_L1_SYSM_AG_SEL_SP(arg->sysm_ag_ssel_lssc, arg->sysm_ag_psel_lssc));
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_SEL_MPRO,
			   PACK_L1_SYSM_AG_SEL_SP(arg->sysm_ag_ssel_mpro, arg->sysm_ag_psel_mpro));
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_SEL_VPRO,
			   PACK_L1_SYSM_AG_SEL_SP(arg->sysm_ag_ssel_vpro, arg->sysm_ag_psel_vpro));

	/* SYSM_AG_CONT */
	val = arg->sysm_ag_cont_hobc_en_middle_led ? MASK_L1_SYSM_AG_CONT_M_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_M_VAL, arg->sysm_ag_cont_hobc_test_middle_led);
	val |= arg->sysm_ag_cont_hobc_en_high ? MASK_L1_SYSM_AG_CONT_H_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_H_VAL, arg->sysm_ag_cont_hobc_test_high);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_CONT_HOBC01_EN, val);

	val = arg->sysm_ag_cont_hobc_en_low ? MASK_L1_SYSM_AG_CONT_L_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_L_VAL, arg->sysm_ag_cont_hobc_test_low);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_CONT_HOBC2_EN, val);

	val = arg->sysm_ag_cont_abpc_en_middle_led ? MASK_L1_SYSM_AG_CONT_M_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_M_VAL, arg->sysm_ag_cont_abpc_test_middle_led);
	val |= arg->sysm_ag_cont_abpc_en_high ? MASK_L1_SYSM_AG_CONT_H_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_H_VAL, arg->sysm_ag_cont_abpc_test_high);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_CONT_ABPC01_EN, val);

	val = arg->sysm_ag_cont_abpc_en_low ? MASK_L1_SYSM_AG_CONT_L_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_L_VAL, arg->sysm_ag_cont_abpc_test_low);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_CONT_ABPC2_EN, val);

	val = arg->sysm_ag_cont_rcnr_en_middle_led ? MASK_L1_SYSM_AG_CONT_M_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_M_VAL, arg->sysm_ag_cont_rcnr_test_middle_led);
	val |= arg->sysm_ag_cont_rcnr_en_high ? MASK_L1_SYSM_AG_CONT_H_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_H_VAL, arg->sysm_ag_cont_rcnr_test_high);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_CONT_RCNR01_EN, val);

	val = arg->sysm_ag_cont_rcnr_en_low ? MASK_L1_SYSM_AG_CONT_L_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_L_VAL, arg->sysm_ag_cont_rcnr_test_low);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_CONT_RCNR2_EN, val);

	val = arg->sysm_ag_cont_lssc_en ? MASK_L1_SYSM_AG_CONT_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_VAL, arg->sysm_ag_cont_lssc_test);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_CONT_LSSC_EN, val);

	val = arg->sysm_ag_cont_mpro_en ? MASK_L1_SYSM_AG_CONT_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_VAL, arg->sysm_ag_cont_mpro_test);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_CONT_MPRO_EN, val);

	val = arg->sysm_ag_cont_vpro_en ? MASK_L1_SYSM_AG_CONT_EN : 0;
	val |= FIELD_PREP(MASK_L1_SYSM_AG_CONT_VAL, arg->sysm_ag_cont_vpro_test);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_CONT_VPRO_EN, val);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG */
static int viif_l1_set_ag(struct viif_device *viif_dev, struct viif_l1_ag_config *arg)
{
	unsigned long irqflags;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_SYSM_AG_H, (u32)arg->gain_h);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_M, (u32)arg->gain_m);
	viif_capture_write(viif_dev, REG_L1_SYSM_AG_L, (u32)arg->gain_l);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRE */
static int viif_l1_set_hdre_try(struct viif_device *viif_dev, struct viif_l1_hdre_config *arg)
{
	u32 idx;

	for (idx = 0; idx < 16U; idx++) {
		if (arg->hdre_src_point[idx] > VIIF_L1_HDRE_MAX_KNEEPOINT_VAL)
			return -EINVAL;
	}

	for (idx = 0; idx < 17U; idx++) {
		if (arg->hdre_dst_base[idx] > VIIF_L1_HDRE_MAX_HDRE_SIG_VAL ||
		    arg->hdre_ratio[idx] >= VIIF_L1_HDRE_MAX_OUT_PIXEL_RATIO) {
			return -EINVAL;
		}
	}

	if (arg->hdre_dst_max_val > VIIF_L1_HDRE_MAX_OUT_PIXEL_VAL)
		return -EINVAL;

	return 0;
}

static int viif_l1_set_hdre(struct viif_device *viif_dev, struct viif_l1_hdre_config *arg)
{
	unsigned long irqflags;
	int i;

	/* value is already tested in _try*/
	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	for (i = 0; i < LEN_L1_HDRE_SRCPOINT; i++)
		viif_capture_write(viif_dev, REG_L1_HDRE_SRCPOINT(i), arg->hdre_src_point[i]);

	viif_capture_write(viif_dev, REG_L1_HDRE_SRCBASE(0), 0);
	for (i = 1; i < LEN_L1_HDRE_SRCBASE; i++)
		viif_capture_write(viif_dev, REG_L1_HDRE_SRCBASE(i), arg->hdre_src_point[i - 1]);

	for (i = 0; i < LEN_L1_HDRE_DSTBASE; i++)
		viif_capture_write(viif_dev, REG_L1_HDRE_DSTBASE(i), arg->hdre_dst_base[i]);

	for (i = 0; i < LEN_L1_HDRE_RATIO; i++)
		viif_capture_write(viif_dev, REG_L1_HDRE_RATIO(i), arg->hdre_ratio[i]);

	viif_capture_write(viif_dev, REG_L1_HDRE_DSTMAXVAL, arg->hdre_dst_max_val);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_EXTRACTION */
static int viif_l1_set_img_extraction_try(struct viif_device *viif_dev,
					  struct viif_l1_img_extraction_config *arg)
{
	if (arg->input_black_gr > VIIF_L1_IMG_EXTRACT_MAX_BLACK_LEVEL_VAL ||
	    arg->input_black_r > VIIF_L1_IMG_EXTRACT_MAX_BLACK_LEVEL_VAL ||
	    arg->input_black_b > VIIF_L1_IMG_EXTRACT_MAX_BLACK_LEVEL_VAL ||
	    arg->input_black_gb > VIIF_L1_IMG_EXTRACT_MAX_BLACK_LEVEL_VAL) {
		return -EINVAL;
	}
	return 0;
}

static int viif_l1_set_img_extraction(struct viif_device *viif_dev,
				      struct viif_l1_img_extraction_config *arg)
{
	unsigned long irqflags;

	/* value is already tested in _try*/
	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_SLIC_SRCBLACKLEVEL_GR, arg->input_black_gr);
	viif_capture_write(viif_dev, REG_L1_SLIC_SRCBLACKLEVEL_R, arg->input_black_r);
	viif_capture_write(viif_dev, REG_L1_SLIC_SRCBLACKLEVEL_B, arg->input_black_b);
	viif_capture_write(viif_dev, REG_L1_SLIC_SRCBLACKLEVEL_GB, arg->input_black_gb);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_DPC */
static int viif_l1_set_dpc_try(struct viif_device *viif_dev, struct viif_l1_dpc_config *arg)
{
	const struct viif_l1_dpc *params[] = {
		&arg->param_h,
		&arg->param_m,
		&arg->param_l,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(params); i++) {
		const struct viif_l1_dpc *param = params[i];

		if (!param->abpc_dyn_en)
			continue;

		if (param->abpc_ratio_limit > VIIF_L1_DPC_MAX_RATIO_LIMIT_VAL ||
		    param->abpc_dark_limit > VIIF_L1_DPC_MAX_RATIO_LIMIT_VAL ||
		    param->abpc_sn_coef_w_ag_min < VIIF_L1_DPC_MIN_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_w_ag_min > VIIF_L1_DPC_MAX_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_w_ag_mid < VIIF_L1_DPC_MIN_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_w_ag_mid > VIIF_L1_DPC_MAX_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_w_ag_max < VIIF_L1_DPC_MIN_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_w_ag_max > VIIF_L1_DPC_MAX_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_b_ag_min < VIIF_L1_DPC_MIN_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_b_ag_min > VIIF_L1_DPC_MAX_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_b_ag_mid < VIIF_L1_DPC_MIN_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_b_ag_mid > VIIF_L1_DPC_MAX_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_b_ag_max < VIIF_L1_DPC_MIN_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_b_ag_max > VIIF_L1_DPC_MAX_LUMA_ADJ_VAL ||
		    param->abpc_sn_coef_w_th_min >= param->abpc_sn_coef_w_th_max ||
		    param->abpc_sn_coef_b_th_min >= param->abpc_sn_coef_b_th_max) {
			return -EINVAL;
		}
	}
	return 0;
}

static void dpc_table_transmission(struct viif_device *viif_dev, uintptr_t table_h,
				   uintptr_t table_m, uintptr_t table_l)
{
	u32 val = 0x0U;

	viif_config_vdm_tgroup(viif_dev, IDX_TGROUP_L1_ISP);

	if (table_h) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L1_DPC_H),
				   (u32)table_h);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L1_DPC_H),
				   VIIF_DPC_TABLE_BYTES);
		val |= MASK_VDM_T_ENABLE_L1_DPC_H;
	}

	if (table_m) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L1_DPC_M),
				   (u32)table_m);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L1_DPC_M),
				   VIIF_DPC_TABLE_BYTES);
		val |= MASK_VDM_T_ENABLE_L1_DPC_M;
	}

	if (table_l) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L1_DPC_L),
				   (u32)table_l);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L1_DPC_L),
				   VIIF_DPC_TABLE_BYTES);
		val |= MASK_VDM_T_ENABLE_L1_DPC_L;
	}

	val |= (viif_capture_read(viif_dev, REG_VDM_T_ENABLE) & ~MASK_VDM_T_ENABLE_L1_DPC);
	viif_capture_write(viif_dev, REG_VDM_T_ENABLE, val);
}

static int viif_l1_set_dpc(struct viif_device *viif_dev, struct viif_l1_dpc_config *arg)
{
	const struct viif_l1_dpc *param_h, *param_m, *param_l;
	dma_addr_t table_h = 0;
	dma_addr_t table_m = 0;
	dma_addr_t table_l = 0;
	unsigned long irqflags;
	u32 val;

	if (arg->param_h.abpc_sta_en) {
		memcpy(viif_dev->tables->dpc_table_h, arg->table_h, VIIF_DPC_TABLE_BYTES);
		table_h = (dma_addr_t)viif_dev->tables_dma->dpc_table_h;
	}
	if (arg->param_m.abpc_sta_en) {
		memcpy(viif_dev->tables->dpc_table_m, arg->table_m, VIIF_DPC_TABLE_BYTES);
		table_m = (dma_addr_t)viif_dev->tables_dma->dpc_table_m;
	}
	if (arg->param_l.abpc_sta_en) {
		memcpy(viif_dev->tables->dpc_table_l, arg->table_l, VIIF_DPC_TABLE_BYTES);
		table_l = (dma_addr_t)viif_dev->tables_dma->dpc_table_l;
	}

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	dpc_table_transmission(viif_dev, table_h, table_m, table_l);

	param_h = &arg->param_h;
	param_m = &arg->param_m;
	param_l = &arg->param_l;

	val = (param_h->abpc_sta_en) ? MASK_L1_ABPC_ENABLE_H : 0;
	val |= (param_m->abpc_sta_en) ? MASK_L1_ABPC_ENABLE_M : 0;
	val |= (param_l->abpc_sta_en) ? MASK_L1_ABPC_ENABLE_L : 0;
	viif_capture_write(viif_dev, REG_L1_ABPC012_STA_EN, val);

	val = (param_h->abpc_dyn_en) ? MASK_L1_ABPC_ENABLE_H : 0;
	val |= (param_m->abpc_dyn_en) ? MASK_L1_ABPC_ENABLE_M : 0;
	val |= (param_l->abpc_dyn_en) ? MASK_L1_ABPC_ENABLE_L : 0;
	viif_capture_write(viif_dev, REG_L1_ABPC012_DYN_EN, val);

	val = (param_h->abpc_dyn_mode == VIIF_L1_DPC_2PIXEL) ? MASK_L1_ABPC_DYN_MODE_2PIXEL_H : 0;
	val |= (param_m->abpc_dyn_mode == VIIF_L1_DPC_2PIXEL) ? MASK_L1_ABPC_DYN_MODE_2PIXEL_M : 0;
	val |= (param_l->abpc_dyn_mode == VIIF_L1_DPC_2PIXEL) ? MASK_L1_ABPC_DYN_MODE_2PIXEL_L : 0;
	viif_capture_write(viif_dev, REG_L1_ABPC012_DYN_MODE, val);

	/* setup param_h */
	viif_capture_write(viif_dev, REG_L1_ABPC0_RATIO_LIMIT, param_h->abpc_ratio_limit);
	viif_capture_write(viif_dev, REG_L1_ABPC0_DARK_LIMIT, param_h->abpc_dark_limit);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_W_AG_MIN, param_h->abpc_sn_coef_w_ag_min);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_W_AG_MID, param_h->abpc_sn_coef_w_ag_mid);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_W_AG_MAX, param_h->abpc_sn_coef_w_ag_max);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_B_AG_MIN, param_h->abpc_sn_coef_b_ag_min);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_B_AG_MID, param_h->abpc_sn_coef_b_ag_mid);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_B_AG_MAX, param_h->abpc_sn_coef_b_ag_max);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_W_TH_MIN, param_h->abpc_sn_coef_w_th_min);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_W_TH_MAX, param_h->abpc_sn_coef_w_th_max);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_B_TH_MIN, param_h->abpc_sn_coef_b_th_min);
	viif_capture_write(viif_dev, REG_L1_ABPC0_SN_COEF_B_TH_MAX, param_h->abpc_sn_coef_b_th_max);

	/* setup param_m */
	viif_capture_write(viif_dev, REG_L1_ABPC1_RATIO_LIMIT, param_m->abpc_ratio_limit);
	viif_capture_write(viif_dev, REG_L1_ABPC1_DARK_LIMIT, param_m->abpc_dark_limit);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_W_AG_MIN, param_m->abpc_sn_coef_w_ag_min);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_W_AG_MID, param_m->abpc_sn_coef_w_ag_mid);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_W_AG_MAX, param_m->abpc_sn_coef_w_ag_max);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_B_AG_MIN, param_m->abpc_sn_coef_b_ag_min);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_B_AG_MID, param_m->abpc_sn_coef_b_ag_mid);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_B_AG_MAX, param_m->abpc_sn_coef_b_ag_max);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_W_TH_MIN, param_m->abpc_sn_coef_w_th_min);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_W_TH_MAX, param_m->abpc_sn_coef_w_th_max);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_B_TH_MIN, param_m->abpc_sn_coef_b_th_min);
	viif_capture_write(viif_dev, REG_L1_ABPC1_SN_COEF_B_TH_MAX, param_m->abpc_sn_coef_b_th_max);

	/* setup param_l */
	viif_capture_write(viif_dev, REG_L1_ABPC2_RATIO_LIMIT, param_l->abpc_ratio_limit);
	viif_capture_write(viif_dev, REG_L1_ABPC2_DARK_LIMIT, param_l->abpc_dark_limit);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_W_AG_MIN, param_l->abpc_sn_coef_w_ag_min);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_W_AG_MID, param_l->abpc_sn_coef_w_ag_mid);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_W_AG_MAX, param_l->abpc_sn_coef_w_ag_max);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_B_AG_MIN, param_l->abpc_sn_coef_b_ag_min);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_B_AG_MID, param_l->abpc_sn_coef_b_ag_mid);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_B_AG_MAX, param_l->abpc_sn_coef_b_ag_max);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_W_TH_MIN, param_l->abpc_sn_coef_w_th_min);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_W_TH_MAX, param_l->abpc_sn_coef_w_th_max);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_B_TH_MIN, param_l->abpc_sn_coef_b_th_min);
	viif_capture_write(viif_dev, REG_L1_ABPC2_SN_COEF_B_TH_MAX, param_l->abpc_sn_coef_b_th_max);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_PRESET_WHITE_BALANCE */
static int viif_l1_set_preset_white_balance_try(struct viif_device *viif_dev,
						struct viif_l1_preset_white_balance_config *arg)
{
	const struct viif_l1_preset_wb *param_h, *param_m, *param_l;

	param_h = &arg->param_h;
	param_m = &arg->param_m;
	param_l = &arg->param_l;

	if (arg->dstmaxval > VIIF_L1_PWHB_MAX_OUT_PIXEL_VAL)
		return -EINVAL;

	if (param_h->gain_gr >= VIIF_L1_PWHB_MAX_GAIN_VAL ||
	    param_h->gain_r >= VIIF_L1_PWHB_MAX_GAIN_VAL ||
	    param_h->gain_b >= VIIF_L1_PWHB_MAX_GAIN_VAL ||
	    param_h->gain_gb >= VIIF_L1_PWHB_MAX_GAIN_VAL) {
		return -EINVAL;
	}

	if (param_m->gain_gr >= VIIF_L1_PWHB_MAX_GAIN_VAL ||
	    param_m->gain_r >= VIIF_L1_PWHB_MAX_GAIN_VAL ||
	    param_m->gain_b >= VIIF_L1_PWHB_MAX_GAIN_VAL ||
	    param_m->gain_gb >= VIIF_L1_PWHB_MAX_GAIN_VAL) {
		return -EINVAL;
	}

	if (param_l->gain_gr >= VIIF_L1_PWHB_MAX_GAIN_VAL ||
	    param_l->gain_r >= VIIF_L1_PWHB_MAX_GAIN_VAL ||
	    param_l->gain_b >= VIIF_L1_PWHB_MAX_GAIN_VAL ||
	    param_l->gain_gb >= VIIF_L1_PWHB_MAX_GAIN_VAL) {
		return -EINVAL;
	}

	return 0;
}

static int viif_l1_set_preset_white_balance(struct viif_device *viif_dev,
					    struct viif_l1_preset_white_balance_config *arg)
{
	const struct viif_l1_preset_wb *param_h, *param_m, *param_l;
	unsigned long irqflags;

	param_h = &arg->param_h;
	param_m = &arg->param_m;
	param_l = &arg->param_l;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_PWHB_DSTMAXVAL, arg->dstmaxval);

	viif_capture_write(viif_dev, REG_L1_PWHB_H_GR, param_h->gain_gr);
	viif_capture_write(viif_dev, REG_L1_PWHB_HR, param_h->gain_r);
	viif_capture_write(viif_dev, REG_L1_PWHB_HB, param_h->gain_b);
	viif_capture_write(viif_dev, REG_L1_PWHB_H_GB, param_h->gain_gb);

	viif_capture_write(viif_dev, REG_L1_PWHB_M_GR, param_m->gain_gr);
	viif_capture_write(viif_dev, REG_L1_PWHB_MR, param_m->gain_r);
	viif_capture_write(viif_dev, REG_L1_PWHB_MB, param_m->gain_b);
	viif_capture_write(viif_dev, REG_L1_PWHB_M_GB, param_m->gain_gb);

	viif_capture_write(viif_dev, REG_L1_PWHB_L_GR, param_l->gain_gr);
	viif_capture_write(viif_dev, REG_L1_PWHB_LR, param_l->gain_r);
	viif_capture_write(viif_dev, REG_L1_PWHB_LB, param_l->gain_b);
	viif_capture_write(viif_dev, REG_L1_PWHB_L_GB, param_l->gain_gb);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RAW_COLOR_NOISE_REDUCTION */
static int
viif_l1_set_raw_color_noise_reduction_try(struct viif_device *viif_dev,
					  struct viif_l1_raw_color_noise_reduction_config *arg)
{
	const struct viif_l1_raw_color_noise_reduction *params[] = {
		&arg->param_h,
		&arg->param_m,
		&arg->param_l,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(params); i++) {
		const struct viif_l1_raw_color_noise_reduction *param = params[i];

		if (param->rcnr_cnf_dark_ag0 > VIIF_L1_RCNR_MAX_DARK_ADJUSTMENT_VAL ||
		    param->rcnr_cnf_dark_ag1 > VIIF_L1_RCNR_MAX_DARK_ADJUSTMENT_VAL ||
		    param->rcnr_cnf_dark_ag2 > VIIF_L1_RCNR_MAX_DARK_ADJUSTMENT_VAL ||
		    param->rcnr_cnf_ratio_ag0 > VIIF_L1_RCNR_MAX_LUMA_LINKAGE_ADJUSTMENT_VAL ||
		    param->rcnr_cnf_ratio_ag1 > VIIF_L1_RCNR_MAX_LUMA_LINKAGE_ADJUSTMENT_VAL ||
		    param->rcnr_cnf_ratio_ag2 > VIIF_L1_RCNR_MAX_LUMA_LINKAGE_ADJUSTMENT_VAL ||
		    param->rcnr_cnf_clip_gain_r > VIIF_L1_RCNR_MAX_ADJUSTMENT_GAIN_VAL ||
		    param->rcnr_cnf_clip_gain_g > VIIF_L1_RCNR_MAX_ADJUSTMENT_GAIN_VAL ||
		    param->rcnr_cnf_clip_gain_b > VIIF_L1_RCNR_MAX_ADJUSTMENT_GAIN_VAL ||
		    param->rcnr_a1l_dark_ag0 > VIIF_L1_RCNR_MAX_DARK_ADJUSTMENT_VAL ||
		    param->rcnr_a1l_dark_ag1 > VIIF_L1_RCNR_MAX_DARK_ADJUSTMENT_VAL ||
		    param->rcnr_a1l_dark_ag2 > VIIF_L1_RCNR_MAX_DARK_ADJUSTMENT_VAL ||
		    param->rcnr_a1l_ratio_ag0 > VIIF_L1_RCNR_MAX_LUMA_LINKAGE_ADJUSTMENT_VAL ||
		    param->rcnr_a1l_ratio_ag1 > VIIF_L1_RCNR_MAX_LUMA_LINKAGE_ADJUSTMENT_VAL ||
		    param->rcnr_a1l_ratio_ag2 > VIIF_L1_RCNR_MAX_LUMA_LINKAGE_ADJUSTMENT_VAL ||
		    param->rcnr_inf_zero_clip > VIIF_L1_RCNR_MAX_ZERO_CLIP_VAL ||
		    param->rcnr_merge_d2blend_ag0 > VIIF_L1_RCNR_MAX_BLEND_VAL ||
		    param->rcnr_merge_d2blend_ag1 > VIIF_L1_RCNR_MAX_BLEND_VAL ||
		    param->rcnr_merge_d2blend_ag2 > VIIF_L1_RCNR_MAX_BLEND_VAL ||
		    param->rcnr_merge_black > VIIF_L1_RCNR_MAX_BLACK_LEVEL_VAL ||
		    param->rcnr_merge_mindiv < VIIF_L1_RCNR_MIN_0DIV_GUARD_VAL ||
		    param->rcnr_merge_mindiv > VIIF_L1_RCNR_MAX_0DIV_GUARD_VAL) {
			return -EINVAL;
		}

		switch (param->rcnr_hry_type) {
		case VIIF_L1_RCNR_LOW_RESOLUTION:
		case VIIF_L1_RCNR_MIDDLE_RESOLUTION:
		case VIIF_L1_RCNR_HIGH_RESOLUTION:
		case VIIF_L1_RCNR_ULTRA_HIGH_RESOLUTION:
			break;
		default:
			return -EINVAL;
		}

		if (param->rcnr_anf_blend_ag0 != VIIF_L1_MSF_BLEND_RATIO_0_DIV_64 &&
		    param->rcnr_anf_blend_ag0 != VIIF_L1_MSF_BLEND_RATIO_1_DIV_64 &&
		    param->rcnr_anf_blend_ag0 != VIIF_L1_MSF_BLEND_RATIO_2_DIV_64) {
			return -EINVAL;
		}
		if (param->rcnr_anf_blend_ag1 != VIIF_L1_MSF_BLEND_RATIO_0_DIV_64 &&
		    param->rcnr_anf_blend_ag1 != VIIF_L1_MSF_BLEND_RATIO_1_DIV_64 &&
		    param->rcnr_anf_blend_ag1 != VIIF_L1_MSF_BLEND_RATIO_2_DIV_64) {
			return -EINVAL;
		}
		if (param->rcnr_anf_blend_ag2 != VIIF_L1_MSF_BLEND_RATIO_0_DIV_64 &&
		    param->rcnr_anf_blend_ag2 != VIIF_L1_MSF_BLEND_RATIO_1_DIV_64 &&
		    param->rcnr_anf_blend_ag2 != VIIF_L1_MSF_BLEND_RATIO_2_DIV_64) {
			return -EINVAL;
		}

		if (param->rcnr_lpf_threshold >= VIIF_L1_RCNR_MAX_CALC_MSF_NOISE_MULTI_VAL ||
		    param->rcnr_merge_hlblend_ag0 > VIIF_L1_RCNR_MAX_GEN_LUMA_SIG_BLEND_VAL ||
		    param->rcnr_merge_hlblend_ag1 > VIIF_L1_RCNR_MAX_GEN_LUMA_SIG_BLEND_VAL ||
		    param->rcnr_merge_hlblend_ag2 > VIIF_L1_RCNR_MAX_GEN_LUMA_SIG_BLEND_VAL) {
			return -EINVAL;
		}

		if (param->rcnr_gnr_sw) {
			if (param->rcnr_gnr_ratio > VIIF_L1_RCNR_MAX_UP_LIMIT_GRGB_SENS_RATIO)
				return -EINVAL;
		}
	}

	return 0;
}

static int
viif_l1_set_raw_color_noise_reduction(struct viif_device *viif_dev,
				      struct viif_l1_raw_color_noise_reduction_config *arg)
{
	const struct viif_l1_raw_color_noise_reduction *params[] = {
		&arg->param_h,
		&arg->param_m,
		&arg->param_l,
	};
	unsigned long irqflags;
	int i;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	for (i = 0; i < 3; i++) {
		const struct viif_l1_raw_color_noise_reduction *param = params[i];
		/* param_h */
		viif_capture_write(viif_dev, REG_L1_RCNR_X_SW(i), param->rcnr_sw ? 1 : 0);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_CNF_DARK_AG0(i),
				   param->rcnr_cnf_dark_ag0);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_CNF_DARK_AG1(i),
				   param->rcnr_cnf_dark_ag1);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_CNF_DARK_AG2(i),
				   param->rcnr_cnf_dark_ag2);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_CNF_RATIO_AG0(i),
				   param->rcnr_cnf_ratio_ag0);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_CNF_RATIO_AG1(i),
				   param->rcnr_cnf_ratio_ag1);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_CNF_RATIO_AG2(i),
				   param->rcnr_cnf_ratio_ag2);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_CNF_CLIP_GAIN_R(i),
				   param->rcnr_cnf_clip_gain_r);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_CNF_CLIP_GAIN_G(i),
				   param->rcnr_cnf_clip_gain_g);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_CNF_CLIP_GAIN_B(i),
				   param->rcnr_cnf_clip_gain_b);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_A1L_DARK_AG0(i),
				   param->rcnr_a1l_dark_ag0);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_A1L_DARK_AG1(i),
				   param->rcnr_a1l_dark_ag1);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_A1L_DARK_AG2(i),
				   param->rcnr_a1l_dark_ag2);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_A1L_RATIO_AG0(i),
				   param->rcnr_a1l_ratio_ag0);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_A1L_RATIO_AG1(i),
				   param->rcnr_a1l_ratio_ag1);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_A1L_RATIO_AG2(i),
				   param->rcnr_a1l_ratio_ag2);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_INF_ZERO_CLIP(i),
				   param->rcnr_inf_zero_clip);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_MERGE_D2BLEND_AG0(i),
				   param->rcnr_merge_d2blend_ag0);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_MERGE_D2BLEND_AG1(i),
				   param->rcnr_merge_d2blend_ag1);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_MERGE_D2BLEND_AG2(i),
				   param->rcnr_merge_d2blend_ag2);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_MERGE_BLACK(i), param->rcnr_merge_black);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_MERGE_MINDIV(i),
				   param->rcnr_merge_mindiv);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_HRY_TYPE(i), param->rcnr_hry_type);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_ANF_BLEND_AG0(i),
				   param->rcnr_anf_blend_ag0);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_ANF_BLEND_AG1(i),
				   param->rcnr_anf_blend_ag1);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_ANF_BLEND_AG2(i),
				   param->rcnr_anf_blend_ag2);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_LPF_THRESHOLD(i),
				   param->rcnr_lpf_threshold);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_MERGE_HLBLEND_AG0(i),
				   param->rcnr_merge_hlblend_ag0);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_MERGE_HLBLEND_AG1(i),
				   param->rcnr_merge_hlblend_ag1);
		viif_capture_write(viif_dev, REG_L1_RCNR_X_MERGE_HLBLEND_AG2(i),
				   param->rcnr_merge_hlblend_ag2);

		viif_capture_write(viif_dev, REG_L1_RCNR_X_GNR_SW(i), param->rcnr_gnr_sw ? 1 : 0);

		if (param->rcnr_gnr_sw) {
			viif_capture_write(viif_dev, REG_L1_RCNR_X_GNR_RATIO(i),
					   param->rcnr_gnr_ratio);
			viif_capture_write(viif_dev, REG_L1_RCNR_X_GNR_WIDE_EN(i),
					   param->rcnr_gnr_wide_en ? 1 : 0);
		}
	}

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRS */
static int viif_l1_set_hdrs_try(struct viif_device *viif_dev, struct viif_l1_hdrs_config *arg)
{
	if ((arg->hdrs_hdr_mode != VIIF_L1_HDRS_NOT_USE_MIDDLE_SENS_IMAGE &&
	     arg->hdrs_hdr_mode != VIIF_L1_HDRS_USE_MIDDLE_SENS_IMAGE) ||
	    arg->hdrs_hdr_ratio_m < VIIF_L1_HDRS_MIN_BLEND_RATIO ||
	    arg->hdrs_hdr_ratio_m > VIIF_L1_HDRS_MAX_BLEND_RATIO ||
	    arg->hdrs_hdr_ratio_l < VIIF_L1_HDRS_MIN_BLEND_RATIO ||
	    arg->hdrs_hdr_ratio_l > VIIF_L1_HDRS_MAX_BLEND_RATIO ||
	    arg->hdrs_hdr_ratio_e < VIIF_L1_HDRS_MIN_BLEND_RATIO ||
	    arg->hdrs_hdr_ratio_e > VIIF_L1_HDRS_MAX_BLEND_RATIO ||
	    arg->hdrs_dg_h >= VIIF_L1_HDRS_MAX_DIGITAL_GAIN_VAL ||
	    arg->hdrs_dg_m >= VIIF_L1_HDRS_MAX_DIGITAL_GAIN_VAL ||
	    arg->hdrs_dg_l >= VIIF_L1_HDRS_MAX_DIGITAL_GAIN_VAL ||
	    arg->hdrs_dg_e >= VIIF_L1_HDRS_MAX_DIGITAL_GAIN_VAL ||
	    arg->hdrs_blendend_h > VIIF_L1_HDRS_MAX_BLEND_PIX_VAL ||
	    arg->hdrs_blendend_m > VIIF_L1_HDRS_MAX_BLEND_PIX_VAL ||
	    arg->hdrs_blendend_e > VIIF_L1_HDRS_MAX_BLEND_PIX_VAL ||
	    arg->hdrs_blendbeg_h > VIIF_L1_HDRS_MAX_BLEND_PIX_VAL ||
	    arg->hdrs_blendbeg_m > VIIF_L1_HDRS_MAX_BLEND_PIX_VAL ||
	    arg->hdrs_blendbeg_e > VIIF_L1_HDRS_MAX_BLEND_PIX_VAL ||
	    arg->hdrs_dst_max_val > VIIF_L1_HDRS_MAX_DST_MAX_VAL) {
		return -EINVAL;
	}
	return 0;
}

static int viif_l1_set_hdrs(struct viif_device *viif_dev, struct viif_l1_hdrs_config *arg)
{
	unsigned long irqflags;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_HDRS_HDRMODE, arg->hdrs_hdr_mode);

	viif_capture_write(viif_dev, REG_L1_HDRS_HDRRATIO_M, arg->hdrs_hdr_ratio_m);
	viif_capture_write(viif_dev, REG_L1_HDRS_HDRRATIO_L, arg->hdrs_hdr_ratio_l);
	viif_capture_write(viif_dev, REG_L1_HDRS_HDRRATIO_E, arg->hdrs_hdr_ratio_e);

	viif_capture_write(viif_dev, REG_L1_HDRS_DG_H, arg->hdrs_dg_h);
	viif_capture_write(viif_dev, REG_L1_HDRS_DG_M, arg->hdrs_dg_m);
	viif_capture_write(viif_dev, REG_L1_HDRS_DG_L, arg->hdrs_dg_l);
	viif_capture_write(viif_dev, REG_L1_HDRS_DG_E, arg->hdrs_dg_e);

	viif_capture_write(viif_dev, REG_L1_HDRS_BLENDEND_H, arg->hdrs_blendend_h);
	viif_capture_write(viif_dev, REG_L1_HDRS_BLENDEND_M, arg->hdrs_blendend_m);
	viif_capture_write(viif_dev, REG_L1_HDRS_BLENDEND_E, arg->hdrs_blendend_e);

	viif_capture_write(viif_dev, REG_L1_HDRS_BLENDBEG_H, arg->hdrs_blendbeg_h);
	viif_capture_write(viif_dev, REG_L1_HDRS_BLENDBEG_M, arg->hdrs_blendbeg_m);
	viif_capture_write(viif_dev, REG_L1_HDRS_BLENDBEG_E, arg->hdrs_blendbeg_e);

	viif_capture_write(viif_dev, REG_L1_HDRS_LEDMODE_ON, arg->hdrs_led_mode_on ? 1 : 0);
	viif_capture_write(viif_dev, REG_L1_HDRS_DSTMAXVAL, arg->hdrs_dst_max_val);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_BLACK_LEVEL_CORRECTION */
static int viif_l1_set_black_level_correction_try(struct viif_device *viif_dev,
						  struct viif_l1_black_level_correction_config *arg)
{
	if (arg->srcblacklevel_gr > VIIF_L1_BLACK_LEVEL_MAX_VAL ||
	    arg->srcblacklevel_r > VIIF_L1_BLACK_LEVEL_MAX_VAL ||
	    arg->srcblacklevel_b > VIIF_L1_BLACK_LEVEL_MAX_VAL ||
	    arg->srcblacklevel_gb > VIIF_L1_BLACK_LEVEL_MAX_VAL ||
	    arg->mulval_gr >= VIIF_L1_BLACK_LEVEL_MAX_GAIN_VAL ||
	    arg->mulval_r >= VIIF_L1_BLACK_LEVEL_MAX_GAIN_VAL ||
	    arg->mulval_b >= VIIF_L1_BLACK_LEVEL_MAX_GAIN_VAL ||
	    arg->mulval_gb >= VIIF_L1_BLACK_LEVEL_MAX_GAIN_VAL ||
	    arg->dstmaxval > VIIF_L1_BLACK_LEVEL_MAX_DST_VAL) {
		return -EINVAL;
	}
	return 0;
}

static int viif_l1_set_black_level_correction(struct viif_device *viif_dev,
					      struct viif_l1_black_level_correction_config *arg)
{
	unsigned long irqflags;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_BLVC_SRCBLACKLEVEL_GR, arg->srcblacklevel_gr);
	viif_capture_write(viif_dev, REG_L1_BLVC_SRCBLACKLEVEL_R, arg->srcblacklevel_r);
	viif_capture_write(viif_dev, REG_L1_BLVC_SRCBLACKLEVEL_B, arg->srcblacklevel_b);
	viif_capture_write(viif_dev, REG_L1_BLVC_SRCBLACKLEVELGB, arg->srcblacklevel_gb);

	viif_capture_write(viif_dev, REG_L1_BLVC_MULTVAL_GR, arg->mulval_gr);
	viif_capture_write(viif_dev, REG_L1_BLVC_MULTVAL_R, arg->mulval_r);
	viif_capture_write(viif_dev, REG_L1_BLVC_MULTVAL_B, arg->mulval_b);
	viif_capture_write(viif_dev, REG_L1_BLVC_MULTVAL_GB, arg->mulval_gb);

	viif_capture_write(viif_dev, REG_L1_BLVC_DSTMAXVAL, arg->dstmaxval);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_LSC */
static inline u32 gen_grid_size(u32 param)
{
	switch (param) {
	case 32U:
		return 5U;
	case 64U:
		return 6U;
	case 128U:
		return 7U;
	case 256U:
		return 8U;
	case 512U:
		return 9U;
	default:
		return 0;
	}
}

static int viif_l1_set_lsc_try(struct viif_device *viif_dev, struct viif_l1_lsc_config *arg)
{
	struct viif_l1_lsc *param;
	u32 sysm_width, sysm_height;

	/* disable LSC */
	if (!arg->enable)
		return 0;

	param = &arg->param;

	sysm_width = viif_capture_read(viif_dev, REG_L1_SYSM_WIDTH);
	sysm_height = viif_capture_read(viif_dev, REG_L1_SYSM_HEIGHT);

	/* enabling LSC Parabola */
	if (arg->enable & VIIF_L1_LSC_PARABOLA_EN_MASK) {
		struct viif_l1_lsc_parabola_param *parabola_param = &param->lssc_parabola_param;
		unsigned int idx;

		if (parabola_param->lssc_para_h_center >= sysm_width ||
		    parabola_param->lssc_para_v_center >= sysm_height ||
		    parabola_param->lssc_para_h_gain >= VIIF_L1_LSC_MAX_GAIN ||
		    parabola_param->lssc_para_v_gain >= VIIF_L1_LSC_MAX_GAIN) {
			return -EINVAL;
		}

		switch (parabola_param->lssc_para_mgsel2) {
		case VIIF_L1_PARA_COEF_GAIN_ONE_EIGHTH:
		case VIIF_L1_PARA_COEF_GAIN_ONE_FOURTH:
		case VIIF_L1_PARA_COEF_GAIN_ONE_SECOND:
		case VIIF_L1_PARA_COEF_GAIN_ONE_FIRST:
			break;
		default:
			return -EINVAL;
		}

		switch (parabola_param->lssc_para_mgsel4) {
		case VIIF_L1_PARA_COEF_GAIN_ONE_EIGHTH:
		case VIIF_L1_PARA_COEF_GAIN_ONE_FOURTH:
		case VIIF_L1_PARA_COEF_GAIN_ONE_SECOND:
		case VIIF_L1_PARA_COEF_GAIN_ONE_FIRST:
			break;
		default:
			return -EINVAL;
		}

		for (idx = 0U; idx < 8U; idx++) {
			const struct viif_l1_lsc_parabola_ag_param *ag_param;

			switch (idx) {
			case 0U:
				ag_param = &parabola_param->r_2d;
				break;
			case 1U:
				ag_param = &parabola_param->r_4d;
				break;
			case 2U:
				ag_param = &parabola_param->gr_2d;
				break;
			case 3U:
				ag_param = &parabola_param->gr_4d;
				break;
			case 4U:
				ag_param = &parabola_param->gb_2d;
				break;
			case 5U:
				ag_param = &parabola_param->gb_4d;
				break;
			case 6U:
				ag_param = &parabola_param->b_2d;
				break;
			default:
				ag_param = &parabola_param->b_4d;
				break;
			}

			if (!ag_param || ag_param->lssc_paracoef_h_l_max < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_h_l_max >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_h_l_min < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_h_l_min >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_h_l_min > ag_param->lssc_paracoef_h_l_max ||
			    ag_param->lssc_paracoef_h_r_max < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_h_r_max >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_h_r_min < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_h_r_min >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_h_r_min > ag_param->lssc_paracoef_h_r_max ||
			    ag_param->lssc_paracoef_v_u_max < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_v_u_max >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_v_u_min < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_v_u_min >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_v_u_min > ag_param->lssc_paracoef_v_u_max ||
			    ag_param->lssc_paracoef_v_d_max < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_v_d_max >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_v_d_min < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_v_d_min >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_v_d_min > ag_param->lssc_paracoef_v_d_max ||
			    ag_param->lssc_paracoef_hv_lu_max < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_hv_lu_max >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_hv_lu_min < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_hv_lu_min >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_hv_lu_min > ag_param->lssc_paracoef_hv_lu_max ||
			    ag_param->lssc_paracoef_hv_ru_max < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_hv_ru_max >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_hv_ru_min < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_hv_ru_min >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_hv_ru_min > ag_param->lssc_paracoef_hv_ru_max ||
			    ag_param->lssc_paracoef_hv_ld_max < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_hv_ld_max >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_hv_ld_min < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_hv_ld_min >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_hv_ld_min > ag_param->lssc_paracoef_hv_ld_max ||
			    ag_param->lssc_paracoef_hv_rd_max < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_hv_rd_max >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_hv_rd_min < VIIF_L1_LSC_MIN_GAIN ||
			    ag_param->lssc_paracoef_hv_rd_min >= VIIF_L1_LSC_MAX_GAIN ||
			    ag_param->lssc_paracoef_hv_rd_min > ag_param->lssc_paracoef_hv_rd_max) {
				return -EINVAL;
			}
		}
	}

	/*  enabling LSC Grid */
	if (arg->enable & VIIF_L1_LSC_GRID_EN_MASK) {
		struct viif_l1_lsc_grid_param *grid_param = &param->lssc_grid_param;
		u32 grid_h_size = gen_grid_size(grid_param->lssc_grid_h_size);
		u32 grid_v_size = gen_grid_size(grid_param->lssc_grid_v_size);

		if (!grid_h_size || !grid_v_size)
			return -EINVAL;

		if (grid_param->lssc_grid_h_center < VIIF_L1_LSC_GRID_MIN_COORDINATE ||
		    grid_param->lssc_grid_h_center > grid_param->lssc_grid_h_size) {
			return -EINVAL;
		}

		if (sysm_width >
		    (grid_param->lssc_grid_h_center + (grid_param->lssc_grid_h_size * 31U))) {
			return -EINVAL;
		}

		if (grid_param->lssc_grid_v_center < VIIF_L1_LSC_GRID_MIN_COORDINATE ||
		    grid_param->lssc_grid_v_center > grid_param->lssc_grid_v_size) {
			return -EINVAL;
		}

		if (sysm_height >
		    (grid_param->lssc_grid_v_center + (grid_param->lssc_grid_v_size * 23U))) {
			return -EINVAL;
		}

		if (grid_param->lssc_grid_mgsel != VIIF_L1_GRID_COEF_GAIN_X1 &&
		    grid_param->lssc_grid_mgsel != VIIF_L1_GRID_COEF_GAIN_X2) {
			return -EINVAL;
		}
	}

	if (param->lssc_pwhb_r_gain_max >= VIIF_L1_LSC_PWB_MAX_COEF_VAL ||
	    param->lssc_pwhb_r_gain_min >= VIIF_L1_LSC_PWB_MAX_COEF_VAL ||
	    param->lssc_pwhb_r_gain_min > param->lssc_pwhb_r_gain_max ||
	    param->lssc_pwhb_gr_gain_max >= VIIF_L1_LSC_PWB_MAX_COEF_VAL ||
	    param->lssc_pwhb_gr_gain_min >= VIIF_L1_LSC_PWB_MAX_COEF_VAL ||
	    param->lssc_pwhb_gr_gain_min > param->lssc_pwhb_gr_gain_max ||
	    param->lssc_pwhb_gb_gain_max >= VIIF_L1_LSC_PWB_MAX_COEF_VAL ||
	    param->lssc_pwhb_gb_gain_min >= VIIF_L1_LSC_PWB_MAX_COEF_VAL ||
	    param->lssc_pwhb_gb_gain_min > param->lssc_pwhb_gb_gain_max ||
	    param->lssc_pwhb_b_gain_max >= VIIF_L1_LSC_PWB_MAX_COEF_VAL ||
	    param->lssc_pwhb_b_gain_min >= VIIF_L1_LSC_PWB_MAX_COEF_VAL ||
	    param->lssc_pwhb_b_gain_min > param->lssc_pwhb_b_gain_max) {
		return -EINVAL;
	}

	return 0;
}

static void lsc_table_transmission(struct viif_device *viif_dev, dma_addr_t table_gr,
				   dma_addr_t table_r, dma_addr_t table_b, dma_addr_t table_gb)
{
	u32 val = 0x0U;

	viif_config_vdm_tgroup(viif_dev, IDX_TGROUP_L1_ISP);

	if (table_gr) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L1_LSSC_GR),
				   (u32)table_gr);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L1_LSSC_GR),
				   VIIF_LSC_TABLE_BYTES);
		val |= MASK_VDM_T_ENABLE_L1_LSSC_GR;
	}

	if (table_r) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L1_LSSC_R),
				   (u32)table_r);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L1_LSSC_R),
				   VIIF_LSC_TABLE_BYTES);
		val |= MASK_VDM_T_ENABLE_L1_LSSC_R;
	}

	if (table_b) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L1_LSSC_B),
				   (u32)table_b);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L1_LSSC_B),
				   VIIF_LSC_TABLE_BYTES);
		val |= MASK_VDM_T_ENABLE_L1_LSSC_B;
	}

	if (table_gb) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L1_LSSC_GB),
				   (u32)table_gb);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L1_LSSC_GB),
				   VIIF_LSC_TABLE_BYTES);
		val |= MASK_VDM_T_ENABLE_L1_LSSC_GB;
	}

	val |= (viif_capture_read(viif_dev, REG_VDM_T_ENABLE) & ~MASK_VDM_T_ENABLE_L1_LSSC);
	viif_capture_write(viif_dev, REG_VDM_T_ENABLE, val);
}

#define PACK_PARA_COEF(max, min) (FIELD_PREP(0x1FFF0000, (max)) | FIELD_PREP(0x1FFF, (min)))

static int viif_l1_set_lsc(struct viif_device *viif_dev, struct viif_l1_lsc_config *arg)
{
	dma_addr_t table_gr = 0;
	dma_addr_t table_gb = 0;
	dma_addr_t table_r = 0;
	dma_addr_t table_b = 0;
	unsigned long irqflags;
	u32 val;

	if (!arg->enable) {
		spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
		hwd_viif_isp_guard_start(viif_dev);

		viif_capture_write(viif_dev, REG_L1_LSSC_EN, 0);

		hwd_viif_isp_guard_end(viif_dev);
		spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);
		return 0;
	}

	if (arg->enable & VIIF_L1_LSC_GRID_EN_MASK) {
		memcpy(viif_dev->tables->lsc_table_gr, arg->table_gr, VIIF_LSC_TABLE_BYTES);
		memcpy(viif_dev->tables->lsc_table_r, arg->table_r, VIIF_LSC_TABLE_BYTES);
		memcpy(viif_dev->tables->lsc_table_b, arg->table_b, VIIF_LSC_TABLE_BYTES);
		memcpy(viif_dev->tables->lsc_table_gb, arg->table_gb, VIIF_LSC_TABLE_BYTES);
		table_gr = (dma_addr_t)viif_dev->tables_dma->lsc_table_gr;
		table_r = (dma_addr_t)viif_dev->tables_dma->lsc_table_r;
		table_b = (dma_addr_t)viif_dev->tables_dma->lsc_table_b;
		table_gb = (dma_addr_t)viif_dev->tables_dma->lsc_table_gb;
	}

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);
	lsc_table_transmission(viif_dev, table_gr, table_r, table_b, table_gb);

	/* parabola shading */
	if (arg->enable & VIIF_L1_LSC_PARABOLA_EN_MASK) {
		struct viif_l1_lsc_parabola_param *parabola_param = &arg->param.lssc_parabola_param;
		const struct viif_l1_lsc_parabola_ag_param *params[] = {
			&parabola_param->r_2d,	&parabola_param->r_4d,	&parabola_param->gr_2d,
			&parabola_param->gr_4d, &parabola_param->gb_2d, &parabola_param->gb_4d,
			&parabola_param->b_2d,	&parabola_param->b_4d,
		};
		int i;

		viif_capture_write(viif_dev, REG_L1_LSSC_PARA_EN, 1);

		viif_capture_write(viif_dev, REG_L1_LSSC_PARA_H_CENTER,
				   parabola_param->lssc_para_h_center);
		viif_capture_write(viif_dev, REG_L1_LSSC_PARA_V_CENTER,
				   parabola_param->lssc_para_v_center);

		viif_capture_write(viif_dev, REG_L1_LSSC_PARA_H_GAIN,
				   parabola_param->lssc_para_h_gain);
		viif_capture_write(viif_dev, REG_L1_LSSC_PARA_V_GAIN,
				   parabola_param->lssc_para_v_gain);

		viif_capture_write(viif_dev, REG_L1_LSSC_PARA_MGSEL2,
				   parabola_param->lssc_para_mgsel2);
		viif_capture_write(viif_dev, REG_L1_LSSC_PARA_MGSEL4,
				   parabola_param->lssc_para_mgsel4);

		for (i = 0; i < 8; i++) {
			const struct viif_l1_lsc_parabola_ag_param *p = params[i];

			viif_capture_write(viif_dev, REG_L1_LSSC_PARA_COEF_X_H_L(i),
					   PACK_PARA_COEF(p->lssc_paracoef_h_l_max,
							  p->lssc_paracoef_h_l_min));
			viif_capture_write(viif_dev, REG_L1_LSSC_PARA_COEF_X_H_R(i),
					   PACK_PARA_COEF(p->lssc_paracoef_h_r_max,
							  p->lssc_paracoef_h_r_min));
			viif_capture_write(viif_dev, REG_L1_LSSC_PARA_COEF_X_V_U(i),
					   PACK_PARA_COEF(p->lssc_paracoef_v_u_max,
							  p->lssc_paracoef_v_u_min));
			viif_capture_write(viif_dev, REG_L1_LSSC_PARA_COEF_X_V_D(i),
					   PACK_PARA_COEF(p->lssc_paracoef_v_d_max,
							  p->lssc_paracoef_v_d_min));
			viif_capture_write(viif_dev, REG_L1_LSSC_PARA_COEF_X_HV_LU(i),
					   PACK_PARA_COEF(p->lssc_paracoef_hv_lu_max,
							  p->lssc_paracoef_hv_lu_min));
			viif_capture_write(viif_dev, REG_L1_LSSC_PARA_COEF_X_HV_RU(i),
					   PACK_PARA_COEF(p->lssc_paracoef_hv_ru_max,
							  p->lssc_paracoef_hv_ru_min));
			viif_capture_write(viif_dev, REG_L1_LSSC_PARA_COEF_X_HV_LD(i),
					   PACK_PARA_COEF(p->lssc_paracoef_hv_ld_max,
							  p->lssc_paracoef_hv_ld_min));
			viif_capture_write(viif_dev, REG_L1_LSSC_PARA_COEF_X_HV_RD(i),
					   PACK_PARA_COEF(p->lssc_paracoef_hv_rd_max,
							  p->lssc_paracoef_hv_rd_min));
		}
	} else {
		viif_capture_write(viif_dev, REG_L1_LSSC_PARA_EN, 0);
	}

	/* grid shading */
	if (arg->enable & VIIF_L1_LSC_GRID_EN_MASK) {
		struct viif_l1_lsc_grid_param *grid_param = &arg->param.lssc_grid_param;
		u32 grid_h_size = gen_grid_size(grid_param->lssc_grid_h_size);
		u32 grid_v_size = gen_grid_size(grid_param->lssc_grid_v_size);

		viif_capture_write(viif_dev, REG_L1_LSSC_GRID_EN, 1);
		viif_capture_write(viif_dev, REG_L1_LSSC_GRID_H_SIZE, grid_h_size);
		viif_capture_write(viif_dev, REG_L1_LSSC_GRID_V_SIZE, grid_v_size);
		viif_capture_write(viif_dev, REG_L1_LSSC_GRID_H_CENTER,
				   grid_param->lssc_grid_h_center);
		viif_capture_write(viif_dev, REG_L1_LSSC_GRID_V_CENTER,
				   grid_param->lssc_grid_v_center);
		viif_capture_write(viif_dev, REG_L1_LSSC_GRID_MGSEL, grid_param->lssc_grid_mgsel);

	} else {
		viif_capture_write(viif_dev, REG_L1_LSSC_GRID_EN, 0);
	}

	/* preset white balance */
	val = (arg->param.lssc_pwhb_r_gain_max << 16U) | (arg->param.lssc_pwhb_r_gain_min);
	viif_capture_write(viif_dev, REG_L1_LSSC_PWHB_R_GAIN, val);

	val = (arg->param.lssc_pwhb_gr_gain_max << 16U) | (arg->param.lssc_pwhb_gr_gain_min);
	viif_capture_write(viif_dev, REG_L1_LSSC_PWHB_GR_GAIN, val);

	val = (arg->param.lssc_pwhb_gb_gain_max << 16U) | (arg->param.lssc_pwhb_gb_gain_min);
	viif_capture_write(viif_dev, REG_L1_LSSC_PWHB_GB_GAIN, val);

	val = (arg->param.lssc_pwhb_b_gain_max << 16U) | (arg->param.lssc_pwhb_b_gain_min);
	viif_capture_write(viif_dev, REG_L1_LSSC_PWHB_B_GAIN, val);

	viif_capture_write(viif_dev, REG_L1_LSSC_EN, 1);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_MAIN_PROCESS */
static int viif_l1_set_main_process_try(struct viif_device *viif_dev,
					struct viif_l1_main_process_config *arg)
{
	if (arg->demosaic_mode != VIIF_L1_DEMOSAIC_ACPI &&
	    arg->demosaic_mode != VIIF_L1_DEMOSAIC_DMG) {
		return -EINVAL;
	}

	if (arg->damp_lsbsel > VIIF_DAMP_MAX_LSBSEL)
		return -EINVAL;

	if (arg->colormat_enable) {
		struct viif_l1_color_matrix_correction *color_matrix = &arg->colormat_param;

		if (color_matrix->coef_rmg_min > color_matrix->coef_rmg_max ||
		    color_matrix->coef_rmb_min > color_matrix->coef_rmb_max ||
		    color_matrix->coef_gmr_min > color_matrix->coef_gmr_max ||
		    color_matrix->coef_gmb_min > color_matrix->coef_gmb_max ||
		    color_matrix->coef_bmr_min > color_matrix->coef_bmr_max ||
		    color_matrix->coef_bmg_min > color_matrix->coef_bmg_max ||
		    (u32)color_matrix->dst_minval > arg->dst_maxval)
			return -EINVAL;
	}

	if (arg->dst_maxval > VIIF_MAIN_PROCESS_MAX_OUT_PIXEL_VAL)
		return -EINVAL;

	return 0;
}

static int viif_l1_set_main_process(struct viif_device *viif_dev,
				    struct viif_l1_main_process_config *arg)
{
	unsigned long irqflags;
	u32 val;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_MPRO_CONF, arg->damp_lsbsel << 4);
	viif_capture_write(viif_dev, REG_L1_MPRO_LCS_MODE, arg->demosaic_mode);

	if (arg->colormat_enable) {
		const struct viif_l1_color_matrix_correction *color_matrix = &arg->colormat_param;

		viif_capture_write(viif_dev, REG_L1_MPRO_SW, 1);

		val = (u32)color_matrix->coef_rmg_min & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_RMG_MIN, val);

		val = (u32)color_matrix->coef_rmg_max & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_RMG_MAX, val);

		val = (u32)color_matrix->coef_rmb_min & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_RMB_MIN, val);

		val = (u32)color_matrix->coef_rmb_max & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_RMB_MAX, val);

		val = (u32)color_matrix->coef_gmr_min & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_GMR_MIN, val);

		val = (u32)color_matrix->coef_gmr_max & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_GMR_MAX, val);

		val = (u32)color_matrix->coef_gmb_min & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_GMB_MIN, val);

		val = (u32)color_matrix->coef_gmb_max & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_GMB_MAX, val);

		val = (u32)color_matrix->coef_bmr_min & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_BMR_MIN, val);

		val = (u32)color_matrix->coef_bmr_max & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_BMR_MAX, val);

		val = (u32)color_matrix->coef_bmg_min & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_BMG_MIN, val);

		val = (u32)color_matrix->coef_bmg_max & 0xffffU;
		viif_capture_write(viif_dev, REG_L1_MPRO_LM0_BMG_MAX, val);

		viif_capture_write(viif_dev, REG_L1_MPRO_DST_MINVAL, (u32)color_matrix->dst_minval);
	} else {
		viif_capture_write(viif_dev, REG_L1_MPRO_SW, 0);
	}

	viif_capture_write(viif_dev, REG_L1_MPRO_DST_MAXVAL, arg->dst_maxval);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AWB */
static int viif_l1_set_awb_try(struct viif_device *viif_dev, struct viif_l1_awb_config *arg)
{
	struct viif_l1_awb *param;
	u32 val;

	if (arg->awhb_wbmrg < VIIF_L1_AWB_MIN_GAIN || arg->awhb_wbmrg >= VIIF_L1_AWB_MAX_GAIN ||
	    arg->awhb_wbmgg < VIIF_L1_AWB_MIN_GAIN || arg->awhb_wbmgg >= VIIF_L1_AWB_MAX_GAIN ||
	    arg->awhb_wbmbg < VIIF_L1_AWB_MIN_GAIN || arg->awhb_wbmbg >= VIIF_L1_AWB_MAX_GAIN) {
		return -EINVAL;
	}

	/* disabling AWB */
	if (!arg->enable)
		return 0;

	param = &arg->param;

	if (param->awhb_ygate_data != 64U && param->awhb_ygate_data != 128U &&
	    param->awhb_ygate_data != 256U && param->awhb_ygate_data != 512U) {
		return -EINVAL;
	}

	if (param->awhb_cgrange != VIIF_L1_AWB_ONE_SECOND &&
	    param->awhb_cgrange != VIIF_L1_AWB_X1 && param->awhb_cgrange != VIIF_L1_AWB_X2 &&
	    param->awhb_cgrange != VIIF_L1_AWB_X4) {
		return -EINVAL;
	}

	if (param->awhb_areamode != VIIF_L1_AWB_AREA_MODE0 &&
	    param->awhb_areamode != VIIF_L1_AWB_AREA_MODE1 &&
	    param->awhb_areamode != VIIF_L1_AWB_AREA_MODE2 &&
	    param->awhb_areamode != VIIF_L1_AWB_AREA_MODE3) {
		return -EINVAL;
	}

	val = viif_capture_read(viif_dev, REG_L1_SYSM_WIDTH);
	if (param->awhb_area_hsize < 1U || (param->awhb_area_hsize > ((val - 8U) / 8U)) ||
	    param->awhb_area_hofs > (val - 9U)) {
		return -EINVAL;
	}

	val = viif_capture_read(viif_dev, REG_L1_SYSM_HEIGHT);
	if (param->awhb_area_vsize < 1U || (param->awhb_area_vsize > ((val - 4U) / 8U)) ||
	    param->awhb_area_vofs > (val - 5U)) {
		return -EINVAL;
	}

	if (param->awhb_bycut0p > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_bycut0n > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_rycut0p > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_rycut0n > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_rbcut0h < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_rbcut0h > VIIF_L1_AWB_GATE_UPPER ||
	    param->awhb_rbcut0l < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_rbcut0l > VIIF_L1_AWB_GATE_UPPER ||
	    param->awhb_bycut_h[0] < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_bycut_h[0] > VIIF_L1_AWB_GATE_UPPER ||
	    param->awhb_bycut_h[1] < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_bycut_h[1] > VIIF_L1_AWB_GATE_UPPER ||
	    param->awhb_bycut_h[2] < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_bycut_h[2] > VIIF_L1_AWB_GATE_UPPER ||
	    param->awhb_bycut_l[0] > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_bycut_l[1] > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_bycut_l[2] > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_rycut_h[0] < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_rycut_h[0] > VIIF_L1_AWB_GATE_UPPER ||
	    param->awhb_rycut_h[1] < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_rycut_h[1] > VIIF_L1_AWB_GATE_UPPER ||
	    param->awhb_rycut_h[2] < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_rycut_h[2] > VIIF_L1_AWB_GATE_UPPER ||
	    param->awhb_rycut_l[0] > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_rycut_l[1] > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_rycut_l[2] > VIIF_L1_AWB_UNSIGNED_GATE_UPPER ||
	    param->awhb_awbsftu < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_awbsftu > VIIF_L1_AWB_GATE_UPPER ||
	    param->awhb_awbsftv < VIIF_L1_AWB_GATE_LOWER ||
	    param->awhb_awbsftv > VIIF_L1_AWB_GATE_UPPER) {
		return -EINVAL;
	}

	if (param->awhb_awbspd > VIIF_L1_AWB_MAX_UV_CONVERGENCE_SPEED ||
	    param->awhb_awbulv > VIIF_L1_AWB_MAX_UV_CONVERGENCE_LEVEL ||
	    param->awhb_awbvlv > VIIF_L1_AWB_MAX_UV_CONVERGENCE_LEVEL ||
	    param->awhb_awbondot > VIIF_L1_AWB_INTEGRATION_STOP_TH) {
		return -EINVAL;
	}

	switch (param->awhb_awbfztim) {
	case VIIF_L1_AWB_RESTART_NO:
	case VIIF_L1_AWB_RESTART_128FRAME:
	case VIIF_L1_AWB_RESTART_64FRAME:
	case VIIF_L1_AWB_RESTART_32FRAME:
	case VIIF_L1_AWB_RESTART_16FRAME:
	case VIIF_L1_AWB_RESTART_8FRAME:
	case VIIF_L1_AWB_RESTART_4FRAME:
	case VIIF_L1_AWB_RESTART_2FRAME:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int viif_l1_set_awb(struct viif_device *viif_dev, struct viif_l1_awb_config *arg)
{
	struct viif_l1_awb *param = &arg->param;
	unsigned long irqflags;
	u32 val, ygate_data;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_AWHB_WBMRG, arg->awhb_wbmrg);
	viif_capture_write(viif_dev, REG_L1_AWHB_WBMGG, arg->awhb_wbmgg);
	viif_capture_write(viif_dev, REG_L1_AWHB_WBMBG, arg->awhb_wbmbg);

	val = viif_capture_read(viif_dev, REG_L1_AWHB_SW) & ~MASK_L1_AWHB_SW_EN;

	/* disabling AWB */
	if (!arg->enable) {
		viif_capture_write(viif_dev, REG_L1_AWHB_SW, val);
		goto op_done;
	}

	/* enabling AWB */
	viif_capture_write(viif_dev, REG_L1_AWHB_SW, val | MASK_L1_AWHB_SW_EN);

	if (param->awhb_ygate_data == 64U)
		ygate_data = 0U;
	else if (param->awhb_ygate_data == 128U)
		ygate_data = 1U;
	else if (param->awhb_ygate_data == 256U)
		ygate_data = 2U;
	else
		ygate_data = 3U;

	val = param->awhb_ygate_sel ? MASK_L1_AWHB_GATE_YGATE_SEL : 0;
	val |= FIELD_PREP(MASK_L1_AWHB_GATE_YGATE_DATA, ygate_data);
	val |= FIELD_PREP(MASK_L1_AWHB_GATE_CGRANGE, param->awhb_cgrange);
	viif_capture_write(viif_dev, REG_L1_AWHB_GATE_CONF0, val);

	val = param->awhb_ygatesw ? MASK_L1_AWHB_GATE_YGATESW : 0;
	val |= param->awhb_hexsw ? MASK_L1_AWHB_GATE_HEXSW : 0;
	val |= FIELD_PREP(MASK_L1_AWHB_GATE_AREAMODE, param->awhb_areamode);
	viif_capture_write(viif_dev, REG_L1_AWHB_GATE_CONF1, val);

	viif_capture_write(viif_dev, REG_L1_AWHB_AREA_HSIZE, param->awhb_area_hsize);
	viif_capture_write(viif_dev, REG_L1_AWHB_AREA_VSIZE, param->awhb_area_vsize);
	viif_capture_write(viif_dev, REG_L1_AWHB_AREA_HOFS, param->awhb_area_hofs);
	viif_capture_write(viif_dev, REG_L1_AWHB_AREA_VOFS, param->awhb_area_vofs);

	viif_capture_write(viif_dev, REG_L1_AWHB_AREA_MASKH, param->awhb_area_maskh);
	viif_capture_write(viif_dev, REG_L1_AWHB_AREA_MASKL, param->awhb_area_maskl);

	val = param->awhb_sq_sw[0] ? MASK_L1_AWHB_SQ_CONF_SQ1SW : 0;
	val |= param->awhb_sq_pol[0] ? MASK_L1_AWHB_SQ_CONF_SQ1POL : 0;
	val |= param->awhb_sq_sw[1] ? MASK_L1_AWHB_SQ_CONF_SQ2SW : 0;
	val |= param->awhb_sq_pol[1] ? MASK_L1_AWHB_SQ_CONF_SQ2POL : 0;
	val |= param->awhb_sq_sw[2] ? MASK_L1_AWHB_SQ_CONF_SQ3SW : 0;
	val |= param->awhb_sq_pol[2] ? MASK_L1_AWHB_SQ_CONF_SQ3POL : 0;
	viif_capture_write(viif_dev, REG_L1_AWHB_SQ_CONF, val);

	viif_capture_write(viif_dev, REG_L1_AWHB_YGATEH, (u32)param->awhb_ygateh);
	viif_capture_write(viif_dev, REG_L1_AWHB_YGATEL, (u32)param->awhb_ygatel);

	viif_capture_write(viif_dev, REG_L1_AWHB_BYCUT0P, param->awhb_bycut0p);
	viif_capture_write(viif_dev, REG_L1_AWHB_BYCUT0N, param->awhb_bycut0n);
	viif_capture_write(viif_dev, REG_L1_AWHB_RYCUT0P, param->awhb_rycut0p);
	viif_capture_write(viif_dev, REG_L1_AWHB_RYCUT0N, param->awhb_rycut0n);

	val = (u32)param->awhb_rbcut0h & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_RBCUT0H, val);
	val = (u32)param->awhb_rbcut0l & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_RBCUT0L, val);

	val = (u32)param->awhb_bycut_h[0] & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_BYCUT1H, val);
	viif_capture_write(viif_dev, REG_L1_AWHB_BYCUT1L, param->awhb_bycut_l[0]);
	val = (u32)param->awhb_bycut_h[1] & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_BYCUT2H, val);
	viif_capture_write(viif_dev, REG_L1_AWHB_BYCUT2L, param->awhb_bycut_l[1]);
	val = (u32)param->awhb_bycut_h[2] & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_BYCUT3H, val);
	viif_capture_write(viif_dev, REG_L1_AWHB_BYCUT3L, param->awhb_bycut_l[2]);

	val = (u32)param->awhb_rycut_h[0] & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_RYCUT1H, val);
	viif_capture_write(viif_dev, REG_L1_AWHB_RYCUT1L, param->awhb_rycut_l[0]);
	val = (u32)param->awhb_rycut_h[1] & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_RYCUT2H, val);
	viif_capture_write(viif_dev, REG_L1_AWHB_RYCUT2L, param->awhb_rycut_l[1]);
	val = (u32)param->awhb_rycut_h[2] & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_RYCUT3H, val);
	viif_capture_write(viif_dev, REG_L1_AWHB_RYCUT3L, param->awhb_rycut_l[2]);

	val = (u32)param->awhb_awbsftu & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_AWBSFTU, val);
	val = (u32)param->awhb_awbsftv & 0xffU;
	viif_capture_write(viif_dev, REG_L1_AWHB_AWBSFTV, val);

	val = (param->awhb_awbhuecor ? MASK_L1_AWHB_AWBSPD_HUECOR : 0);
	val |= FIELD_PREP(MASK_L1_AWHB_AWBSPD_SPD, param->awhb_awbspd);
	viif_capture_write(viif_dev, REG_L1_AWHB_AWBSPD, val);

	viif_capture_write(viif_dev, REG_L1_AWHB_AWBULV, param->awhb_awbulv);
	viif_capture_write(viif_dev, REG_L1_AWHB_AWBVLV, param->awhb_awbvlv);
	viif_capture_write(viif_dev, REG_L1_AWHB_AWBWAIT, (u32)param->awhb_awbwait);

	viif_capture_write(viif_dev, REG_L1_AWHB_AWBONDOT, param->awhb_awbondot);
	viif_capture_write(viif_dev, REG_L1_AWHB_AWBFZTIM, param->awhb_awbfztim);

	viif_capture_write(viif_dev, REG_L1_AWHB_WBGRMAX, (u32)param->awhb_wbgrmax);
	viif_capture_write(viif_dev, REG_L1_AWHB_WBGBMAX, (u32)param->awhb_wbgbmax);
	viif_capture_write(viif_dev, REG_L1_AWHB_WBGRMIN, (u32)param->awhb_wbgrmin);
	viif_capture_write(viif_dev, REG_L1_AWHB_WBGBMIN, (u32)param->awhb_wbgbmin);

op_done:
	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_LOCK_AWB_GAIN */
static int viif_l1_lock_awb_gain(struct viif_device *viif_dev, u32 *enable)
{
	unsigned long irqflags;
	u32 val;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	val = viif_capture_read(viif_dev, REG_L1_AWHB_SW) & ~MASK_L1_AWHB_SW_LOCK;
	val |= (*enable ? MASK_L1_AWHB_SW_LOCK : 0);
	viif_capture_write(viif_dev, REG_L1_AWHB_SW, val);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC */
static int viif_l1_set_hdrc_try(struct viif_device *viif_dev, struct viif_l1_hdrc_config *arg)
{
	struct viif_l1_hdrc *param;

	/* disabling HDRC */
	if (!arg->enable) {
		if (arg->hdrc_thr_sft_amt > VIIF_L1_HDRC_MAX_THROUGH_SHIFT_VAL)
			return -EINVAL;
		return 0;
	}

	/* enabling HDRC */
	param = &arg->param;

	if (arg->hdrc_thr_sft_amt || param->hdrc_ratio < VIIF_L1_HDRC_MIN_INPUT_DATA_WIDTH ||
	    param->hdrc_ratio > VIIF_L1_HDRC_MAX_INPUT_DATA_WIDTH ||
	    param->hdrc_pt_ratio > VIIF_L1_HDRC_MAX_PT_SLOPE ||
	    param->hdrc_pt_blend > VIIF_L1_HDRC_MAX_BLEND_RATIO ||
	    param->hdrc_pt_blend2 > VIIF_L1_HDRC_MAX_BLEND_RATIO ||
	    (param->hdrc_pt_blend + param->hdrc_pt_blend2) > VIIF_L1_HDRC_MAX_BLEND_RATIO ||
	    (param->hdrc_tn_type != VIIF_L1_HDRC_TONE_USER &&
	     param->hdrc_tn_type != VIIF_L1_HDRC_TONE_PRESET) ||
	    param->hdrc_flr_val > VIIF_L1_HDRC_MAX_FLARE_VAL ||
	    param->hdrc_orgy_blend > VIIF_L1_HDRC_MAX_BLEND_LUMA) {
		return -EINVAL;
	}

	return 0;
}

/* Convert the unit of time-period (from sysclk, to num lines in the image) */
static u32 sysclk_to_numlines(u32 time_in_sysclk, const struct viif_img_clk *img_clk)
{
	u64 v1 = (u64)time_in_sysclk * img_clk->pixel_clock;
	u64 v2 = (u64)img_clk->htotal_size * VIIF_SYS_CLK;

	return (u32)div64_u64(v1, v2);
}

static int viif_l1_set_hdrc(struct viif_device *viif_dev, struct viif_l1_hdrc_config *arg)
{
	const struct viif_l1_hdrc *param = &arg->param;
	unsigned long irqflags;
	u32 val, sw_delay1;
	int i;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	/* disabling HDRC */
	if (!arg->enable) {
		viif_capture_write(viif_dev, REG_L1_HDRC_THR_SFT_AMT, arg->hdrc_thr_sft_amt);
		viif_capture_write(viif_dev, REG_L1_HDRC_EN, 0);

		goto done_op;
	}

	/* enabling HDRC */
	viif_capture_write(viif_dev, REG_L1_HDRC_RATIO,
			   (param->hdrc_ratio - VIIF_L1_HDRC_RATIO_OFFSET));
	viif_capture_write(viif_dev, REG_L1_HDRC_PT_RATIO, param->hdrc_pt_ratio);

	viif_capture_write(viif_dev, REG_L1_HDRC_PT_BLEND, param->hdrc_pt_blend);
	viif_capture_write(viif_dev, REG_L1_HDRC_PT_BLEND2, param->hdrc_pt_blend2);

	viif_capture_write(viif_dev, REG_L1_HDRC_PT_SAT, param->hdrc_pt_sat);
	viif_capture_write(viif_dev, REG_L1_HDRC_TN_TYPE, param->hdrc_tn_type);

	for (i = 0; i < LEN_L1_HDRC_UTN_TBL; i++)
		viif_capture_write(viif_dev, REG_L1_HDRC_UTN_TBL(i), param->hdrc_utn_tbl[i]);

	viif_capture_write(viif_dev, REG_L1_HDRC_FLR_VAL, param->hdrc_flr_val);
	viif_capture_write(viif_dev, REG_L1_HDRC_FLR_ADP, param->hdrc_flr_adp ? 1 : 0);

	viif_capture_write(viif_dev, REG_L1_HDRC_YBR_OFF, param->hdrc_ybr_off ? 1 : 0);
	viif_capture_write(viif_dev, REG_L1_HDRC_ORGY_BLEND, param->hdrc_orgy_blend);

	val = ((viif_capture_read(viif_dev, REG_L1_SYSM_HEIGHT)) % 64U) / 2U;
	viif_capture_write(viif_dev, REG_L1_HDRC_MAR_TOP, val);
	val = ((viif_capture_read(viif_dev, REG_L1_SYSM_WIDTH)) % 64U) / 2U;
	viif_capture_write(viif_dev, REG_L1_HDRC_MAR_LEFT, val);

	viif_capture_write(viif_dev, REG_L1_HDRC_EN, 1);

	/* update of sw_delay1 must be done when MAIN unit is NOT running. */
	if (!viif_dev->run_flag_main) {
		sw_delay1 = sysclk_to_numlines(VIIF_REGBUF_ACCESS_TIME, &viif_dev->img_clk) +
			    VIIF_L1_DELAY_W_HDRC + 1U;
		val = viif_capture_read(viif_dev, REG_INT_M1_LINE) & 0xffffU;
		val |= (sw_delay1 << 16U);
		viif_capture_write(viif_dev, REG_INT_M1_LINE, val);
		/* M2_LINE is the same condition as M1_LINE */
		viif_capture_write(viif_dev, REG_INT_M2_LINE, val);
	}

done_op:
	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC_LTM */
static int viif_l1_set_hdrc_ltm_try(struct viif_device *viif_dev,
				    struct viif_l1_hdrc_ltm_config *arg)
{
	u32 val;
	int idx;

	if (arg->tnp_max >= VIIF_L1_HDRC_MAX_LTM_TONE_BLEND_RATIO ||
	    arg->tnp_mag >= VIIF_L1_HDRC_MAX_LTM_MAGNIFICATION) {
		return -EINVAL;
	}

	val = (u32)arg->tnp_fil[0];
	for (idx = 1; idx < 5; idx++)
		val += (u32)arg->tnp_fil[idx] * 2U;

	if (val != 1024U)
		return -EINVAL;

	return 0;
}

static int viif_l1_set_hdrc_ltm(struct viif_device *viif_dev, struct viif_l1_hdrc_ltm_config *arg)
{
	unsigned long irqflags;
	int i;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	viif_capture_write(viif_dev, REG_L1_HDRC_TNP_MAX, arg->tnp_max);
	viif_capture_write(viif_dev, REG_L1_HDRC_TNP_MAG, arg->tnp_mag);

	for (i = 0; i < LEN_L1_HDRC_TNP_FIL; i++)
		viif_capture_write(viif_dev, REG_L1_HDRC_TNP_FIL(i), (u32)arg->tnp_fil[i]);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_GAMMA */
static int viif_l1_set_gamma_try(struct viif_device *viif_dev, struct viif_l1_gamma_config *arg)
{
	int idx;

	/* disabling L1 gamma */
	if (!arg->enable)
		return 0;

	/* enabling L1 gamma */
	for (idx = 0; idx < 44U; idx++) {
		if (arg->param.gam_p[idx] > VIIF_L1_GAMMA_MAX_VAL)
			return -EINVAL;
	}
	return 0;
}

static int viif_l1_set_gamma(struct viif_device *viif_dev, struct viif_l1_gamma_config *arg)
{
	const struct viif_l1_gamma *param = &arg->param;
	unsigned long irqflags;
	int i;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	/* disabling L1 gamma */
	if (!arg->enable) {
		viif_capture_write(viif_dev, REG_L1_VPRO_PGC_SW, 0);
		goto done_op;
	}

	/* enabling L1 gamma */
	for (i = 0; i < 44; i++)
		viif_capture_write(viif_dev, REG_L1_VPRO_GAMxP(i), param->gam_p[i]);
	viif_capture_write(viif_dev, REG_L1_VPRO_BLKADJ, param->blkadj);
	viif_capture_write(viif_dev, REG_L1_VPRO_PGC_SW, 1);

done_op:
	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_QUALITY_ADJUSTMENT */
static int viif_l1_set_img_quality_adjustment_try(struct viif_device *viif_dev,
						  struct viif_l1_img_quality_adjustment_config *arg)
{
	if (arg->enable & VIIF_L1_IQA_LUM_NOISE_REDUCTION_EN_MASK) {
		struct viif_l1_lum_noise_reduction *param = &arg->lum_noise_reduction;

		if (param->gain_min > param->gain_max || param->lim_min > param->lim_max)
			return -EINVAL;
	}
	if (arg->enable & VIIF_L1_IQA_EDGE_ENHANCEMENT_EN_MASK) {
		struct viif_l1_edge_enhancement *param = &arg->edge_enhancement;

		if (param->gain_min > param->gain_max || param->lim_min > param->lim_max ||
		    param->coring_min > param->coring_max)
			return -EINVAL;
	}
	if (arg->enable & VIIF_L1_IQA_UV_SUPPRESSION_EN_MASK) {
		struct viif_l1_uv_suppression *param = &arg->uv_suppression;

		if (param->bk_mp >= VIIF_L1_SUPPRESSION_MAX_VAL ||
		    param->black >= VIIF_L1_SUPPRESSION_MAX_VAL ||
		    param->wh_mp >= VIIF_L1_SUPPRESSION_MAX_VAL ||
		    param->white >= VIIF_L1_SUPPRESSION_MAX_VAL || param->bk_slv >= param->wh_slv)
			return -EINVAL;
	}
	if (arg->enable & VIIF_L1_IQA_CORING_SUPPRESSION_EN_MASK) {
		struct viif_l1_coring_suppression *param = &arg->coring_suppression;

		if (param->gain_min > param->gain_max || param->lv_min > param->lv_max)
			return -EINVAL;
	}
	if (arg->enable & VIIF_L1_IQA_EDGE_SUPPRESSION_EN_MASK) {
		if (arg->edge_suppression.lim > VIIF_L1_EDGE_SUPPRESSION_MAX_LIMIT)
			return -EINVAL;
	}
	if (arg->enable & VIIF_L1_IQA_COLOR_LEVEL_EN_MASK) {
		struct viif_l1_color_level *param = &arg->color_level;

		if (param->cb_gain >= VIIF_L1_COLOR_LEVEL_MAX_GAIN ||
		    param->cr_gain >= VIIF_L1_COLOR_LEVEL_MAX_GAIN ||
		    param->cbr_mgain_min >= VIIF_L1_COLOR_LEVEL_MAX_GAIN ||
		    param->cbp_gain_max >= VIIF_L1_COLOR_LEVEL_MAX_GAIN ||
		    param->cbm_gain_max >= VIIF_L1_COLOR_LEVEL_MAX_GAIN ||
		    param->crp_gain_max >= VIIF_L1_COLOR_LEVEL_MAX_GAIN ||
		    param->crm_gain_max >= VIIF_L1_COLOR_LEVEL_MAX_GAIN) {
			return -EINVAL;
		}
	}

	return 0;
}

static int viif_l1_set_img_quality_adjustment(struct viif_device *viif_dev,
					      struct viif_l1_img_quality_adjustment_config *arg)
{
	unsigned long irqflags;
	u32 val;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	/* RGB to YUV (enabled by default, should be enabled) */
	viif_capture_write(viif_dev, REG_L1_VPRO_YUVC_SW, 1);
	viif_capture_write(viif_dev, REG_L1_VPRO_CB_MAT, (u32)arg->coef_cb);
	viif_capture_write(viif_dev, REG_L1_VPRO_CR_MAT, (u32)arg->coef_cr);

	/* brightness */
	val = (u32)arg->brightness & 0xffffU;
	if (val) {
		viif_capture_write(viif_dev, REG_L1_VPRO_BRIGHT_SW, 1);
		viif_capture_write(viif_dev, REG_L1_VPRO_BRIGHT, val);
	} else {
		viif_capture_write(viif_dev, REG_L1_VPRO_BRIGHT_SW, 0);
	}

	/* linear contrast */
	if ((u32)arg->linear_contrast != 128U) {
		viif_capture_write(viif_dev, REG_L1_VPRO_LCNT_SW, 1);
		viif_capture_write(viif_dev, REG_L1_VPRO_LCONT_LEV, arg->linear_contrast);
	} else {
		viif_capture_write(viif_dev, REG_L1_VPRO_LCNT_SW, 0);
	}

	/* nonlinear contrast */
	if (arg->enable & VIIF_L1_IQA_NONLINEAR_CONTRAST_EN_MASK) {
		struct viif_l1_nonlinear_contrast *nonlinear_contrast = &arg->nonlinear_contrast;

		viif_capture_write(viif_dev, REG_L1_VPRO_NLCNT_SW, 1);
		viif_capture_write(viif_dev, REG_L1_VPRO_BLK_KNEE, nonlinear_contrast->blk_knee);
		viif_capture_write(viif_dev, REG_L1_VPRO_WHT_KNEE, nonlinear_contrast->wht_knee);

		viif_capture_write(viif_dev, REG_L1_VPRO_BLK_CONT0,
				   nonlinear_contrast->blk_cont[0]);
		viif_capture_write(viif_dev, REG_L1_VPRO_BLK_CONT1,
				   nonlinear_contrast->blk_cont[1]);
		viif_capture_write(viif_dev, REG_L1_VPRO_BLK_CONT2,
				   nonlinear_contrast->blk_cont[2]);

		viif_capture_write(viif_dev, REG_L1_VPRO_WHT_CONT0,
				   nonlinear_contrast->wht_cont[0]);
		viif_capture_write(viif_dev, REG_L1_VPRO_WHT_CONT1,
				   nonlinear_contrast->wht_cont[1]);
		viif_capture_write(viif_dev, REG_L1_VPRO_WHT_CONT2,
				   nonlinear_contrast->wht_cont[2]);
	} else {
		viif_capture_write(viif_dev, REG_L1_VPRO_NLCNT_SW, 0);
	}

	/* luminance noise reduction */
	if (arg->enable & VIIF_L1_IQA_LUM_NOISE_REDUCTION_EN_MASK) {
		struct viif_l1_lum_noise_reduction *lum_noise_reduction = &arg->lum_noise_reduction;

		viif_capture_write(viif_dev, REG_L1_VPRO_YNR_SW, 1);
		viif_capture_write(viif_dev, REG_L1_VPRO_YNR_GAIN_MIN,
				   lum_noise_reduction->gain_min);
		viif_capture_write(viif_dev, REG_L1_VPRO_YNR_GAIN_MAX,
				   lum_noise_reduction->gain_max);
		viif_capture_write(viif_dev, REG_L1_VPRO_YNR_LIM_MIN, lum_noise_reduction->lim_min);
		viif_capture_write(viif_dev, REG_L1_VPRO_YNR_LIM_MAX, lum_noise_reduction->lim_max);
	} else {
		viif_capture_write(viif_dev, REG_L1_VPRO_YNR_SW, 0);
	}

	/* edge enhancement */
	if (arg->enable & VIIF_L1_IQA_EDGE_ENHANCEMENT_EN_MASK) {
		struct viif_l1_edge_enhancement *edge_enhancement = &arg->edge_enhancement;

		viif_capture_write(viif_dev, REG_L1_VPRO_ETE_SW, 1);
		viif_capture_write(viif_dev, REG_L1_VPRO_ETE_GAIN_MIN, edge_enhancement->gain_min);
		viif_capture_write(viif_dev, REG_L1_VPRO_ETE_GAIN_MAX, edge_enhancement->gain_max);
		viif_capture_write(viif_dev, REG_L1_VPRO_ETE_LIM_MIN, edge_enhancement->lim_min);
		viif_capture_write(viif_dev, REG_L1_VPRO_ETE_LIM_MAX, edge_enhancement->lim_max);
		viif_capture_write(viif_dev, REG_L1_VPRO_ETE_CORING_MIN,
				   edge_enhancement->coring_min);
		viif_capture_write(viif_dev, REG_L1_VPRO_ETE_CORING_MAX,
				   edge_enhancement->coring_max);
	} else {
		viif_capture_write(viif_dev, REG_L1_VPRO_ETE_SW, 0);
	}

	/* UV suppression */
	if (arg->enable & VIIF_L1_IQA_UV_SUPPRESSION_EN_MASK) {
		struct viif_l1_uv_suppression *uv_suppression = &arg->uv_suppression;

		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_UVSUP_SW, 1);
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_BK_SLV, uv_suppression->bk_slv);
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_BK_MP, uv_suppression->bk_mp);
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_BLACK, uv_suppression->black);

		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_WH_SLV, uv_suppression->wh_slv);
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_WH_MP, uv_suppression->wh_mp);
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_WHITE, uv_suppression->white);
	} else {
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_UVSUP_SW, 0);
	}

	/* coring suppression */
	if (arg->enable & VIIF_L1_IQA_CORING_SUPPRESSION_EN_MASK) {
		struct viif_l1_coring_suppression *coring_suppression = &arg->coring_suppression;

		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_CORING_SW, 1);
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_CORING_LV_MIN,
				   coring_suppression->lv_min);
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_CORING_LV_MAX,
				   coring_suppression->lv_max);
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_CORING_GAIN_MIN,
				   coring_suppression->gain_min);
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_CORING_GAIN_MAX,
				   coring_suppression->gain_max);
	} else {
		viif_capture_write(viif_dev, REG_L1_VPRO_CSUP_CORING_SW, 0);
	}

	/* edge suppression */
	if (arg->enable & VIIF_L1_IQA_EDGE_SUPPRESSION_EN_MASK) {
		struct viif_l1_edge_suppression *edge_suppression = &arg->edge_suppression;

		viif_capture_write(viif_dev, REG_L1_VPRO_EDGE_SUP_SW, 1);
		viif_capture_write(viif_dev, REG_L1_VPRO_EDGE_SUP_GAIN, edge_suppression->gain);
		viif_capture_write(viif_dev, REG_L1_VPRO_EDGE_SUP_LIM, edge_suppression->lim);
	} else {
		viif_capture_write(viif_dev, REG_L1_VPRO_EDGE_SUP_SW, 0);
	}

	/* color level */
	if (arg->enable & VIIF_L1_IQA_COLOR_LEVEL_EN_MASK) {
		struct viif_l1_color_level *color_level = &arg->color_level;

		viif_capture_write(viif_dev, REG_L1_VPRO_CB_GAIN, color_level->cb_gain);
		viif_capture_write(viif_dev, REG_L1_VPRO_CR_GAIN, color_level->cr_gain);
		viif_capture_write(viif_dev, REG_L1_VPRO_CBR_MGAIN_MIN, color_level->cbr_mgain_min);
		viif_capture_write(viif_dev, REG_L1_VPRO_CB_P_GAIN_MAX, color_level->cbp_gain_max);
		viif_capture_write(viif_dev, REG_L1_VPRO_CB_M_GAIN_MAX, color_level->cbm_gain_max);
		viif_capture_write(viif_dev, REG_L1_VPRO_CR_P_GAIN_MAX, color_level->crp_gain_max);
		viif_capture_write(viif_dev, REG_L1_VPRO_CR_M_GAIN_MAX, color_level->crm_gain_max);
	} else {
		/* disable */
		viif_capture_write(viif_dev, REG_L1_VPRO_CB_GAIN, 1024U);
		viif_capture_write(viif_dev, REG_L1_VPRO_CR_GAIN, 1024U);
		viif_capture_write(viif_dev, REG_L1_VPRO_CBR_MGAIN_MIN, 1024U);
		viif_capture_write(viif_dev, REG_L1_VPRO_CB_P_GAIN_MAX, 0U);
		viif_capture_write(viif_dev, REG_L1_VPRO_CB_M_GAIN_MAX, 0U);
		viif_capture_write(viif_dev, REG_L1_VPRO_CR_P_GAIN_MAX, 0U);
		viif_capture_write(viif_dev, REG_L1_VPRO_CR_M_GAIN_MAX, 0U);
	}

	/* color noise reduction */
	viif_capture_write(viif_dev, REG_L1_VPRO_CNR_SW,
			   arg->enable & VIIF_L1_IQA_COLOR_NOISE_REDUCTION_EN_MASK ? 1 : 0);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AVG_LUM_GENERATION */
static int viif_l1_set_avg_lum_generation_try(struct viif_device *viif_dev,
					      struct viif_l1_avg_lum_generation_config *arg)
{
	u32 width, height;
	int i, j;

	if (!arg->enable)
		return 0;

	width = viif_capture_read(viif_dev, REG_L1_SYSM_WIDTH);
	height = viif_capture_read(viif_dev, REG_L1_SYSM_HEIGHT);

	if (arg->aexp_start_x > (width - 1U))
		return -EINVAL;

	if (arg->aexp_block_width < VIIF_L1_AEXP_MIN_BLOCK_WIDTH || arg->aexp_block_width > width)
		return -EINVAL;

	if (arg->aexp_block_width % 64U)
		return -EINVAL;

	if (arg->aexp_start_y > (height - 1U))
		return -EINVAL;

	if (arg->aexp_block_height < VIIF_L1_AEXP_MIN_BLOCK_HEIGHT ||
	    arg->aexp_block_height > height) {
		return -EINVAL;
	}
	if (arg->aexp_block_height % 64U)
		return -EINVAL;

	for (i = 0; i < 8; i++) {
		for (j = 0; j < 8; j++) {
			if (arg->aexp_weight[i][j] > VIIF_L1_AEXP_MAX_WEIGHT)
				return -EINVAL;
		}
	}

	if (arg->aexp_satur_ratio > VIIF_L1_AEXP_MAX_BLOCK_TH ||
	    arg->aexp_black_ratio > VIIF_L1_AEXP_MAX_BLOCK_TH ||
	    arg->aexp_satur_level > VIIF_L1_AEXP_MAX_SATURATION_PIXEL_TH) {
		return -EINVAL;
	}

	for (i = 0; i < 4; i++) {
		if (arg->aexp_ave4linesy[i] > (height - 4U))
			return -EINVAL;
	}

	return 0;
}

static inline u32 pack_weight(u32 *vec)
{
	return (vec[0] << 14) | (vec[1] << 12) | (vec[2] << 10) | (vec[3] << 8) | (vec[4] << 6) |
	       (vec[5] << 4) | (vec[6] << 2U) | (vec[7]);
}

static int viif_l1_set_avg_lum_generation(struct viif_device *viif_dev,
					  struct viif_l1_avg_lum_generation_config *arg)
{
	unsigned long irqflags;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	/* disabling aggregation */
	if (!arg->enable) {
		viif_capture_write(viif_dev, REG_L1_AEXP_ON, 0);
		goto op_done;
	}

	/* enabling aggregation */
	viif_capture_write(viif_dev, REG_L1_AEXP_ON, 1);
	viif_capture_write(viif_dev, REG_L1_AEXP_START_X, arg->aexp_start_x);
	viif_capture_write(viif_dev, REG_L1_AEXP_START_Y, arg->aexp_start_y);
	viif_capture_write(viif_dev, REG_L1_AEXP_BLOCK_WIDTH, arg->aexp_block_width);
	viif_capture_write(viif_dev, REG_L1_AEXP_BLOCK_HEIGHT, arg->aexp_block_height);

	viif_capture_write(viif_dev, REG_L1_AEXP_WEIGHT_0, pack_weight(arg->aexp_weight[0]));
	viif_capture_write(viif_dev, REG_L1_AEXP_WEIGHT_1, pack_weight(arg->aexp_weight[1]));
	viif_capture_write(viif_dev, REG_L1_AEXP_WEIGHT_2, pack_weight(arg->aexp_weight[2]));
	viif_capture_write(viif_dev, REG_L1_AEXP_WEIGHT_3, pack_weight(arg->aexp_weight[3]));
	viif_capture_write(viif_dev, REG_L1_AEXP_WEIGHT_3, pack_weight(arg->aexp_weight[4]));
	viif_capture_write(viif_dev, REG_L1_AEXP_WEIGHT_4, pack_weight(arg->aexp_weight[5]));
	viif_capture_write(viif_dev, REG_L1_AEXP_WEIGHT_5, pack_weight(arg->aexp_weight[6]));
	viif_capture_write(viif_dev, REG_L1_AEXP_WEIGHT_7, pack_weight(arg->aexp_weight[7]));

	viif_capture_write(viif_dev, REG_L1_AEXP_SATUR_RATIO, arg->aexp_satur_ratio);
	viif_capture_write(viif_dev, REG_L1_AEXP_BLACK_RATIO, arg->aexp_black_ratio);
	viif_capture_write(viif_dev, REG_L1_AEXP_SATUR_LEVEL, arg->aexp_satur_level);

	viif_capture_write(viif_dev, REG_L1_AEXP_AVE4LINESY0, arg->aexp_ave4linesy[0]);
	viif_capture_write(viif_dev, REG_L1_AEXP_AVE4LINESY1, arg->aexp_ave4linesy[1]);
	viif_capture_write(viif_dev, REG_L1_AEXP_AVE4LINESY2, arg->aexp_ave4linesy[2]);
	viif_capture_write(viif_dev, REG_L1_AEXP_AVE4LINESY3, arg->aexp_ave4linesy[3]);

op_done:
	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_UNDIST */
static void undist_table_transmission(struct viif_device *viif_dev, dma_addr_t write_g,
				      dma_addr_t read_b, dma_addr_t read_g, dma_addr_t read_r,
				      u32 size)
{
	u32 val = 0U;

	if (read_b) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L2_UNDIST_RD_B),
				   (u32)read_b);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L2_UNDIST_RD_B), size);
		val |= MASK_VDM_T_ENABLE_L2_UNDIST_RD_B;
	}
	if (read_g) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L2_UNDIST_RD_G),
				   (u32)read_g);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L2_UNDIST_RD_G), size);
		val |= MASK_VDM_T_ENABLE_L2_UNDIST_RD_G;
	}
	if (read_r) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L2_UNDIST_RD_R),
				   (u32)read_r);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L2_UNDIST_RD_R), size);
		val |= MASK_VDM_T_ENABLE_L2_UNDIST_RD_R;
	}
	if (write_g) {
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(IDX_TPORT_L2_UNDIST_WR_G),
				   (u32)write_g);
		viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(IDX_TPORT_L2_UNDIST_WR_G), size);
		val |= MASK_VDM_T_ENABLE_L2_UNDIST_WR_G;
	}

	if (val)
		viif_config_vdm_tgroup(viif_dev, IDX_TGROUP_L2_UNDIST);

	val |= viif_capture_read(viif_dev, REG_VDM_T_ENABLE) & ~MASK_VDM_T_ENABLE_L2_UNDIST;
	viif_capture_write(viif_dev, REG_VDM_T_ENABLE, val);
}

static int viif_l2_set_undist_try(struct viif_device *viif_dev, struct viif_l2_undist_config *arg)
{
	struct viif_l2_undist *param = &arg->param;
	u32 grid_num_h = param->grid_node_num_h;
	u32 grid_num_v = param->grid_node_num_v;
	u32 size = arg->size;
	bool transfer;
	u32 val;
	int i;

	if ((size && size < VIIF_L2_UNDIST_MIN_TABLE_SIZE) ||
	    size > VIIF_L2_UNDIST_MAX_TABLE_SIZE) {
		return -EINVAL;
	}

	if (size % 4U)
		return -EINVAL;

	transfer = ((param->roi_mode[0] != VIIF_L2_UNDIST_POLY) ||
		    (param->roi_mode[1] != VIIF_L2_UNDIST_POLY));
	if ((transfer && !size) || (!transfer && size))
		return -EINVAL;

	for (i = 0; i < 2; i++) {
		if (param->roi_mode[i] != VIIF_L2_UNDIST_POLY &&
		    param->roi_mode[i] != VIIF_L2_UNDIST_GRID &&
		    param->roi_mode[i] != VIIF_L2_UNDIST_POLY_TO_GRID &&
		    param->roi_mode[i] != VIIF_L2_UNDIST_GRID_TO_POLY) {
			return -EINVAL;
		}
	}
	if (param->roi_write_area_delta[0] >= VIIF_L2_UNDIST_MAX_ROI_WRITE_AREA_DELTA ||
	    param->roi_write_area_delta[1] >= VIIF_L2_UNDIST_MAX_ROI_WRITE_AREA_DELTA ||
	    param->sensor_crop_ofs_h < VIIF_L2_UNDIST_MIN_SENSOR_CROP_OFS_H ||
	    param->sensor_crop_ofs_h > VIIF_L2_UNDIST_MAX_SENSOR_CROP_OFS_H ||
	    param->sensor_crop_ofs_v < VIIF_L2_UNDIST_MIN_SENSOR_CROP_OFS_V ||
	    param->sensor_crop_ofs_v > VIIF_L2_UNDIST_MAX_SENSOR_CROP_OFS_V ||
	    param->norm_scale > VIIF_L2_UNDIST_MAX_NORM_SCALE ||
	    param->valid_r_norm2_poly >= VIIF_L2_UNDIST_MAX_VALID_R_NORM2 ||
	    param->valid_r_norm2_grid >= VIIF_L2_UNDIST_MAX_VALID_R_NORM2) {
		return -EINVAL;
	}

	for (i = 0; i < (int)VIIF_L2_UNDIST_POLY_NUM; i++) {
		if (param->poly_write_g_coef[i] < VIIF_L2_UNDIST_MIN_POLY_COEF ||
		    param->poly_write_g_coef[i] > VIIF_L2_UNDIST_MAX_POLY_COEF ||
		    param->poly_read_b_coef[i] < VIIF_L2_UNDIST_MIN_POLY_COEF ||
		    param->poly_read_b_coef[i] > VIIF_L2_UNDIST_MAX_POLY_COEF ||
		    param->poly_read_g_coef[i] < VIIF_L2_UNDIST_MIN_POLY_COEF ||
		    param->poly_read_g_coef[i] > VIIF_L2_UNDIST_MAX_POLY_COEF ||
		    param->poly_read_r_coef[i] < VIIF_L2_UNDIST_MIN_POLY_COEF ||
		    param->poly_read_r_coef[i] > VIIF_L2_UNDIST_MAX_POLY_COEF) {
			return -EINVAL;
		}
	}

	if (grid_num_h < VIIF_L2_UNDIST_MIN_GRID_NUM || grid_num_h > VIIF_L2_UNDIST_MAX_GRID_NUM ||
	    grid_num_v < VIIF_L2_UNDIST_MIN_GRID_NUM || grid_num_v > VIIF_L2_UNDIST_MAX_GRID_NUM) {
		return -EINVAL;
	}

	if (grid_num_h % 2U)
		grid_num_h += 1U;

	if (grid_num_v % 2U)
		grid_num_v += 1U;

	if ((grid_num_v * grid_num_h) > VIIF_L2_UNDIST_MAX_GRID_TOTAL_NUM ||
	    param->grid_patch_hsize_inv >= VIIF_L2_UNDIST_MAX_GRID_PATCH_SIZE_INV ||
	    param->grid_patch_vsize_inv >= VIIF_L2_UNDIST_MAX_GRID_PATCH_SIZE_INV) {
		return -EINVAL;
	}

	val = viif_capture_read(viif_dev, REG_L2_SENSOR_CROP_HSIZE) & GENMASK(12, 0);
	if (((param->sensor_crop_ofs_h / 2) + ((s16)val)) > 4095)
		return -EINVAL;

	val = viif_capture_read(viif_dev, REG_L2_SENSOR_CROP_VSIZE) & GENMASK(11, 0);
	if (((param->sensor_crop_ofs_v / 2) + ((s16)val)) > 2047)
		return -EINVAL;

	return 0;
}

static void undist_setup(struct viif_device *viif_dev, struct viif_l2_undist *param)
{
	u32 val;
	unsigned int i;

	/* Undist through mode */
	if (param->through_mode) {
		/* Enable through mode */
		viif_capture_write(viif_dev, REG_L2_MODE, 1);
		return;
	}

	/* Undist operation */
	val = (param->roi_mode[0] << 1U) | (param->roi_mode[1] << 3U);
	viif_capture_write(viif_dev, REG_L2_MODE, val);
	val = (u32)param->sensor_crop_ofs_h & GENMASK(13, 0);
	viif_capture_write(viif_dev, REG_L2_SENSOR_CROP_OFS_H, val);
	val = (u32)param->sensor_crop_ofs_v & GENMASK(12, 0);
	viif_capture_write(viif_dev, REG_L2_SENSOR_CROP_OFS_V, val);
	viif_capture_write(viif_dev, REG_L2_NORM_SCALE, param->norm_scale);
	viif_capture_write(viif_dev, REG_L2_VALID_R_NORM2_POLY, param->valid_r_norm2_poly);
	viif_capture_write(viif_dev, REG_L2_VALID_R_NORM2_GRID, param->valid_r_norm2_grid);
	viif_capture_write(viif_dev, REG_L2_ROI_WRITE_AREA_DELTA(0),
			   param->roi_write_area_delta[0]);
	viif_capture_write(viif_dev, REG_L2_ROI_WRITE_AREA_DELTA(1),
			   param->roi_write_area_delta[1]);

	for (i = 0; i < VIIF_L2_UNDIST_POLY_NUM; i++) {
		val = (u32)param->poly_write_g_coef[i];
		viif_capture_write(viif_dev, REG_L2_POLY10_WRITE_G_COEF(i), val);
		val = (u32)param->poly_read_b_coef[i];
		viif_capture_write(viif_dev, REG_L2_POLY10_READ_B_COEF(i), val);
		val = (u32)param->poly_read_g_coef[i];
		viif_capture_write(viif_dev, REG_L2_POLY10_READ_G_COEF(i), val);
		val = (u32)param->poly_read_r_coef[i];
		viif_capture_write(viif_dev, REG_L2_POLY10_READ_R_COEF(i), val);
	}
	viif_capture_write(viif_dev, REG_L2_GRID_NODE_NUM_H, param->grid_node_num_h);
	viif_capture_write(viif_dev, REG_L2_GRID_NODE_NUM_V, param->grid_node_num_v);
	viif_capture_write(viif_dev, REG_L2_GRID_PATCH_HSIZE_INV, param->grid_patch_hsize_inv);
	viif_capture_write(viif_dev, REG_L2_GRID_PATCH_VSIZE_INV, param->grid_patch_vsize_inv);
}

static int viif_l2_set_undist(struct viif_device *viif_dev, struct viif_l2_undist_config *arg)
{
	dma_addr_t table_write_g = 0;
	dma_addr_t table_read_b = 0;
	dma_addr_t table_read_g = 0;
	dma_addr_t table_read_r = 0;
	unsigned long irqflags;

	if (arg->param.roi_mode[0] != VIIF_L2_UNDIST_POLY ||
	    arg->param.roi_mode[1] != VIIF_L2_UNDIST_POLY) {
		memcpy(viif_dev->tables->undist_write_g, arg->write_g, arg->size);
		memcpy(viif_dev->tables->undist_read_b, arg->read_b, arg->size);
		memcpy(viif_dev->tables->undist_read_g, arg->read_g, arg->size);
		memcpy(viif_dev->tables->undist_read_r, arg->read_r, arg->size);

		table_write_g = (uintptr_t)viif_dev->tables_dma->undist_write_g;
		table_read_b = (uintptr_t)viif_dev->tables_dma->undist_read_b;
		table_read_g = (uintptr_t)viif_dev->tables_dma->undist_read_g;
		table_read_r = (uintptr_t)viif_dev->tables_dma->undist_read_r;
	}

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	undist_table_transmission(viif_dev, table_write_g, table_read_b, table_read_g, table_read_r,
				  arg->size);

	undist_setup(viif_dev, &arg->param);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);
	return 0;
}

int visconti_viif_l2_undist_through(struct viif_device *viif_dev)
{
	struct viif_l2_undist undist = { 0 };

	undist.through_mode = VIIF_ENABLE;
	undist.sensor_crop_ofs_h =
		1 - FIELD_GET(0x1FFF, viif_capture_read(viif_dev, REG_L2_SENSOR_CROP_HSIZE));
	undist.sensor_crop_ofs_v =
		1 - FIELD_GET(0x0FFF, viif_capture_read(viif_dev, REG_L2_SENSOR_CROP_VSIZE));
	undist.grid_node_num_h = 16;
	undist.grid_node_num_v = 16;

	undist_setup(viif_dev, &undist);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_ROI */
static int viif_l2_set_roi_try(struct viif_device *viif_dev, struct viif_l2_roi_config *param)
{
	int i;

	if (!param)
		return -EINVAL;

	if (param->roi_num != 1 && param->roi_num != 2)
		return -EINVAL;

	for (i = 0; i < 2; i++) {
		if (param->roi_scale[i] < VIIF_L2_ROI_MIN_SCALE ||
		    param->roi_scale[i] > VIIF_L2_ROI_MAX_SCALE ||
		    param->roi_scale_inv[i] < VIIF_L2_ROI_MIN_SCALE_INV ||
		    param->roi_scale_inv[i] > VIIF_L2_ROI_MAX_SCALE_INV ||
		    param->corrected_wo_scale_hsize[i] < VIIF_L2_ROI_MIN_CORRECTED_WO_SCALE_HSIZE ||
		    param->corrected_wo_scale_hsize[i] > VIIF_L2_ROI_MAX_CORRECTED_WO_SCALE_HSIZE ||
		    param->corrected_wo_scale_vsize[i] < VIIF_L2_ROI_MIN_CORRECTED_WO_SCALE_VSIZE ||
		    param->corrected_wo_scale_vsize[i] > VIIF_L2_ROI_MAX_CORRECTED_WO_SCALE_VSIZE ||
		    param->corrected_hsize[i] < VIIF_L2_ROI_MIN_CORRECTED_HSIZE ||
		    param->corrected_hsize[i] > VIIF_L2_ROI_MAX_CORRECTED_HSIZE ||
		    param->corrected_vsize[i] < VIIF_L2_ROI_MIN_CORRECTED_VSIZE ||
		    param->corrected_vsize[i] > VIIF_L2_ROI_MAX_CORRECTED_VSIZE) {
			return -EINVAL;
		}
	}
	return 0;
}

static int viif_l2_set_roi_wrap(struct viif_device *viif_dev, struct viif_l2_roi_config *roi)
{
	unsigned long irqflags;

	/* update ROI parameter */
	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);
	visconti_viif_l2_set_roi(viif_dev, roi);
	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	/* reflect change to pad::compose_rect */
	visconti_viif_isp_set_compose_rect(viif_dev, roi);
	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_GAMMA */
struct viif_l2_gamma_table {
	dma_addr_t table[VIIF_L2_GAMMA_TABLE_CH_NUM];
};

static void l2_gamma_table_transmission(struct viif_device *viif_dev, u32 post_id,
					const struct viif_l2_gamma_table *gamma_table)
{
	u32 vdm_enable = 0U;
	u32 i;

	/* 0: LUT0-G/Y, 1: LUT1-G/Y, 2: LUT0-B/U, 3: LUT1-B/U, 4: LUT0-R/V, 5: LUT1-R/V */
	for (i = 0; i < VIIF_L2_GAMMA_TABLE_CH_NUM; i++) {
		if (gamma_table->table[i]) {
			int idx = IDX_TPORT_L2_GAMMA_LUT(post_id, i);

			viif_capture_write(viif_dev, REG_VDM_TPORT_X_STADR(idx),
					   (u32)gamma_table->table[i]);
			viif_capture_write(viif_dev, REG_VDM_TPORT_X_SIZE(idx),
					   VIIF_L2_GAMMA_TABLE_BYTES);
			vdm_enable |= MASK_VDM_T_ENABLE_L2_GAMMA(post_id, i);
		}
	}
	if (vdm_enable)
		viif_config_vdm_tgroup(viif_dev, IDX_TGROUP_L2_GAMMA_LUT(post_id));

	vdm_enable |= viif_capture_read(viif_dev, REG_VDM_T_ENABLE) &
		      ~MASK_VDM_T_ENABLE_L2_GAMMA_ALL(post_id);

	viif_capture_write(viif_dev, REG_VDM_T_ENABLE, vdm_enable);
}

static int viif_l2_set_gamma_try(struct viif_device *viif_dev,
				 struct viif_l2_gamma_config *l2_gamma)
{
	u32 vsplit, mode;
	bool enable;

	if (!l2_gamma)
		return -EINVAL;

	enable = (l2_gamma->table_en != 0);
	vsplit = l2_gamma->vsplit;
	mode = l2_gamma->mode;

	if (l2_gamma->pathid != CAPTURE_PATH_MAIN_POST0 &&
	    l2_gamma->pathid != CAPTURE_PATH_MAIN_POST1)
		return -EINVAL;

	if (vsplit > VIIF_GAMMA_MAX_VSPLIT ||
	    (mode != VIIF_GAMMA_COMPRESSED && mode != VIIF_GAMMA_LINEAR) || (!enable && vsplit) ||
	    (!enable && mode != VIIF_GAMMA_COMPRESSED)) {
		return -EINVAL;
	}

	return 0;
}

static int viif_l2_set_gamma(struct viif_device *viif_dev, struct viif_l2_gamma_config *l2_gamma)
{
	struct viif_l2_gamma_table dma_table = { 0 };
	int pathid = l2_gamma->pathid;
	unsigned long irqflags;
	int table_en;
	int postid;
	u32 val;
	int i;

	postid = (pathid == CAPTURE_PATH_MAIN_POST0) ? VIIF_L2ISP_POST_0 : VIIF_L2ISP_POST_1;

	table_en = l2_gamma->table_en;
	for (i = 0; i < 6; i++) {
		if (table_en & BIT(i)) {
			memcpy(viif_dev->tables->l2_gamma_table[pathid][i], l2_gamma->table[i],
			       VIIF_L2_GAMMA_TABLE_BYTES);
			dma_table.table[i] =
				(dma_addr_t)viif_dev->tables_dma->l2_gamma_table[pathid][i];
		}
	}

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	l2_gamma_table_transmission(viif_dev, postid, &dma_table);

	val = (l2_gamma->vsplit << 16U) | (l2_gamma->mode << 4U) | (table_en != 0 ? 1 : 0);
	viif_capture_write(viif_dev, REG_L2_POST_X_GAMMA_M(postid), val);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_CALIBRATION_STATUS */
static const struct viif_csi2rx_dphy_calibration_status calib_status_not_streaming = {
	.term_cal_with_rext = -EAGAIN,
	.clock_lane_offset_cal = -EAGAIN,
	.data_lane0_offset_cal = -EAGAIN,
	.data_lane1_offset_cal = -EAGAIN,
	.data_lane2_offset_cal = -EAGAIN,
	.data_lane3_offset_cal = -EAGAIN,
	.data_lane0_ddl_tuning_cal = -EAGAIN,
	.data_lane1_ddl_tuning_cal = -EAGAIN,
	.data_lane2_ddl_tuning_cal = -EAGAIN,
	.data_lane3_ddl_tuning_cal = -EAGAIN,
};

static int
viif_csi2rx_get_calibration_status(struct viif_device *viif_dev,
				   struct viif_csi2rx_dphy_calibration_status *calibration_status)
{
	if (!vb2_start_streaming_called(&viif_dev->cap_dev0.vb2_vq)) {
		*calibration_status = calib_status_not_streaming;
		return 0;
	}
	visconti_viif_csi2rx_get_calibration_status(viif_dev, calibration_status);
	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_ERR_STATUS */
static const struct viif_csi2rx_err_status csi_err_not_streaming;

static int viif_csi2rx_get_err_status(struct viif_device *viif_dev,
				      struct viif_csi2rx_err_status *csi_err)
{
	if (!vb2_is_streaming(&viif_dev->cap_dev0.vb2_vq)) {
		*csi_err = csi_err_not_streaming;
		return 0;
	}
	visconti_viif_csi2rx_get_err_status(viif_dev, csi_err);
	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_GET_LAST_CAPTURE_STATUS */
static int viif_isp_get_last_capture_status(struct viif_device *viif_dev,
					    struct viif_isp_capture_status *status)
{
	return 0;
}

/* V4L2_CID_VISCONTI_VIIF_GET_REPORTED_ERRORS */
static int viif_isp_get_reported_errors(struct viif_device *viif_dev,
					struct viif_reported_errors *status)
{
	status->main = viif_dev->reported_err_main;
	status->sub = viif_dev->reported_err_sub;
	status->csi2rx = viif_dev->reported_err_csi2rx;
	viif_dev->reported_err_main = 0;
	viif_dev->reported_err_sub = 0;
	viif_dev->reported_err_csi2rx = 0;

	return 0;
}

void visconti_viif_save_l1_info(struct viif_device *viif_dev)
{
	struct viif_isp_capture_status status;
	struct viif_l1_info *l1_info = &status.l1_info;
	unsigned long irqflags;
	int i, j;
	u32 val;

	if (!viif_dev->isp_subdev.ctrl_last_capture_status)
		return;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	/* change register buffer to regbuf0 where driver gets information */
	viif_capture_write(viif_dev, REG_L1_CRGBF_ACC_CONF, VAL_L1_CRGBF_ACC_CONF_MODE_BUFFER0);

	/* get AWB info */
	l1_info->awb_ave_u = viif_capture_read(viif_dev, REG_L1_AWHB_AVE_USIG);
	l1_info->awb_ave_v = viif_capture_read(viif_dev, REG_L1_AWHB_AVE_VSIG);
	l1_info->awb_accumulated_pixel = viif_capture_read(viif_dev, REG_L1_AWHB_NUM_UVON);
	l1_info->awb_gain_r = viif_capture_read(viif_dev, REG_L1_AWHB_AWBGAINR);
	l1_info->awb_gain_g = viif_capture_read(viif_dev, REG_L1_AWHB_AWBGAING);
	l1_info->awb_gain_b = viif_capture_read(viif_dev, REG_L1_AWHB_AWBGAINB);
	val = viif_capture_read(viif_dev, REG_L1_AWHB_R_CTR_STOP);
	l1_info->awb_status_u = (FIELD_GET(BIT(1), val) != 0);
	l1_info->awb_status_v = (FIELD_GET(BIT(0), val) != 0);

	/* get average luminance info */
	l1_info->avg_lum_weight = viif_capture_read(viif_dev, REG_L1_AEXP_RESULT_AVE);
	val = viif_capture_read(viif_dev, REG_L1_AEXP_SATUR_BLACK_PIXNUM);
	l1_info->avg_satur_pixnum = FIELD_GET(GENMASK(31, 16), val);
	l1_info->avg_black_pixnum = FIELD_GET(GENMASK(15, 0), val);
	for (i = 0; i < 8; i++) {
		for (j = 0; j < 8; j++) {
			l1_info->avg_lum_block[i][j] =
				viif_capture_read(viif_dev, REG_L1_AEXP_AVE(i, j));
		}
	}
	l1_info->avg_lum_four_line_lum[0] = viif_capture_read(viif_dev, REG_L1_AEXP_AVE4LINES0);
	l1_info->avg_lum_four_line_lum[1] = viif_capture_read(viif_dev, REG_L1_AEXP_AVE4LINES1);
	l1_info->avg_lum_four_line_lum[2] = viif_capture_read(viif_dev, REG_L1_AEXP_AVE4LINES2);
	l1_info->avg_lum_four_line_lum[3] = viif_capture_read(viif_dev, REG_L1_AEXP_AVE4LINES3);

	/* revert to register access from register buffer access */
	viif_capture_write(viif_dev, REG_L1_CRGBF_ACC_CONF, VAL_L1_CRGBF_ACC_CONF_MODE_BYPASS);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);

	v4l2_ctrl_s_ctrl_compound(viif_dev->isp_subdev.ctrl_last_capture_status,
				  viif_dev->isp_subdev.ctrl_last_capture_status->type, &status);
}

/* ===== v4l2 subdevice control handlers ===== */
static int isp_set_ctrl(struct viif_device *viif_dev, struct v4l2_ctrl *ctrl)
{
	switch (ctrl->id) {
	case V4L2_CID_VISCONTI_VIIF_MAIN_SET_RAWPACK_MODE:
		return viif_main_set_rawpack_mode(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE:
		return viif_l1_set_input_mode(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RGB_TO_Y_COEF:
		return viif_l1_set_rgb_to_y_coef(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG_MODE:
		return viif_l1_set_ag_mode(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG:
		return viif_l1_set_ag(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRE:
		return viif_l1_set_hdre(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_EXTRACTION:
		return viif_l1_set_img_extraction(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_DPC:
		return viif_l1_set_dpc(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_PRESET_WHITE_BALANCE:
		return viif_l1_set_preset_white_balance(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RAW_COLOR_NOISE_REDUCTION:
		return viif_l1_set_raw_color_noise_reduction(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRS:
		return viif_l1_set_hdrs(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_BLACK_LEVEL_CORRECTION:
		return viif_l1_set_black_level_correction(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_LSC:
		return viif_l1_set_lsc(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_MAIN_PROCESS:
		return viif_l1_set_main_process(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AWB:
		return viif_l1_set_awb(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_LOCK_AWB_GAIN:
		return viif_l1_lock_awb_gain(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC:
		return viif_l1_set_hdrc(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC_LTM:
		return viif_l1_set_hdrc_ltm(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_GAMMA:
		return viif_l1_set_gamma(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_QUALITY_ADJUSTMENT:
		return viif_l1_set_img_quality_adjustment(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AVG_LUM_GENERATION:
		return viif_l1_set_avg_lum_generation(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_UNDIST:
		return viif_l2_set_undist(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_ROI:
		return viif_l2_set_roi_wrap(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_GAMMA:
		return viif_l2_set_gamma(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_GET_LAST_CAPTURE_STATUS:
		return 0;
	}
	return -EINVAL;
}

static int isp_get_ctrl(struct viif_device *viif_dev, struct v4l2_ctrl *ctrl)
{
	switch (ctrl->id) {
	case V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_CALIBRATION_STATUS:
		return viif_csi2rx_get_calibration_status(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_ERR_STATUS:
		return viif_csi2rx_get_err_status(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_GET_LAST_CAPTURE_STATUS:
		return viif_isp_get_last_capture_status(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_GET_REPORTED_ERRORS:
		return viif_isp_get_reported_errors(viif_dev, ctrl->p_new.p);
	}
	return -EINVAL;
}

static int visconti_viif_isp_try_ctrl(struct v4l2_ctrl *ctrl)
{
	struct viif_device *viif_dev = ctrl->priv;

	switch (ctrl->id) {
	case V4L2_CID_VISCONTI_VIIF_MAIN_SET_RAWPACK_MODE:
		return viif_main_set_rawpack_mode_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE:
		return viif_l1_set_input_mode_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RGB_TO_Y_COEF:
		return viif_l1_set_rgb_to_y_coef_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG_MODE:
		return viif_l1_set_ag_mode_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG:
		return 0; //no need to check
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRE:
		return viif_l1_set_hdre_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_EXTRACTION:
		return viif_l1_set_img_extraction_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_DPC:
		return viif_l1_set_dpc_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_PRESET_WHITE_BALANCE:
		return viif_l1_set_preset_white_balance_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RAW_COLOR_NOISE_REDUCTION:
		return viif_l1_set_raw_color_noise_reduction_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRS:
		return viif_l1_set_hdrs_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_BLACK_LEVEL_CORRECTION:
		return viif_l1_set_black_level_correction_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_LSC:
		return viif_l1_set_lsc_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_MAIN_PROCESS:
		return viif_l1_set_main_process_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AWB:
		return viif_l1_set_awb_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_LOCK_AWB_GAIN:
		return 0;
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC:
		return viif_l1_set_hdrc_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC_LTM:
		return viif_l1_set_hdrc_ltm_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_GAMMA:
		return viif_l1_set_gamma_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_QUALITY_ADJUSTMENT:
		return viif_l1_set_img_quality_adjustment_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AVG_LUM_GENERATION:
		return viif_l1_set_avg_lum_generation_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_UNDIST:
		return viif_l2_set_undist_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_ROI:
		return viif_l2_set_roi_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_GAMMA:
		return viif_l2_set_gamma_try(viif_dev, ctrl->p_new.p);
	case V4L2_CID_VISCONTI_VIIF_GET_LAST_CAPTURE_STATUS:
		return 0;
	}
	return -EINVAL;
}

static int visconti_viif_isp_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct viif_device *viif_dev = ctrl->priv;
	int ret;

	if (!pm_runtime_get_if_in_use(viif_dev->dev))
		return -EIO;

	mutex_lock(&viif_dev->isp_subdev.ops_lock);
	ret = isp_set_ctrl(viif_dev, ctrl);
	mutex_unlock(&viif_dev->isp_subdev.ops_lock);

	pm_runtime_put(viif_dev->dev);

	return ret;
}

static int visconti_viif_isp_get_ctrl(struct v4l2_ctrl *ctrl)
{
	struct viif_device *viif_dev = ctrl->priv;
	int ret;

	if (!pm_runtime_get_if_in_use(viif_dev->dev))
		return -EIO;

	mutex_lock(&viif_dev->isp_subdev.ops_lock);
	ret = isp_get_ctrl(viif_dev, ctrl);
	mutex_unlock(&viif_dev->isp_subdev.ops_lock);

	pm_runtime_put(viif_dev->dev);

	return ret;
}

/* ===== register v4l2 subdevice controls ===== */
static bool visconti_viif_isp_custom_ctrl_equal(const struct v4l2_ctrl *ctrl,
						union v4l2_ctrl_ptr ptr1, union v4l2_ctrl_ptr ptr2)
{
	return !memcmp(ptr1.p_const, ptr2.p_const, ctrl->elem_size);
}

static void visconti_viif_isp_custom_ctrl_init(const struct v4l2_ctrl *ctrl, u32 idx,
					       union v4l2_ctrl_ptr ptr)
{
	if (ctrl->p_def.p_const)
		memcpy(ptr.p, ctrl->p_def.p_const, ctrl->elem_size);
	else
		memset(ptr.p, 0, ctrl->elem_size);
}

static void visconti_viif_isp_custom_ctrl_log(const struct v4l2_ctrl *ctrl)
{
}

static int visconti_viif_isp_custom_ctrl_validate(const struct v4l2_ctrl *ctrl,
						  union v4l2_ctrl_ptr ptr)
{
	return 0;
}

static const struct v4l2_ctrl_type_ops custom_type_ops = {
	.equal = visconti_viif_isp_custom_ctrl_equal,
	.init = visconti_viif_isp_custom_ctrl_init,
	.log = visconti_viif_isp_custom_ctrl_log,
	.validate = visconti_viif_isp_custom_ctrl_validate,
};

static const struct v4l2_ctrl_ops visconti_viif_isp_ctrl_ops = {
	.g_volatile_ctrl = visconti_viif_isp_get_ctrl,
	.try_ctrl = visconti_viif_isp_try_ctrl,
	.s_ctrl = visconti_viif_isp_set_ctrl,
};

/* ----- control handler ----- */
#define CTRL_CONFIG_DEFAULT_ENTRY                                         \
	.ops = &visconti_viif_isp_ctrl_ops, .type_ops = &custom_type_ops, \
	.type = V4L2_CTRL_TYPE_VISCONTI_ISP

#define CTRL_CONFIG_VOLATILE_ENTRY                                        \
	.ops = &visconti_viif_isp_ctrl_ops, .type_ops = &custom_type_ops, \
	.type = V4L2_CTRL_TYPE_VISCONTI_ISP,                              \
	.flags = (V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY)

#define CTRL_CONFIG_RDONLY_ENTRY                                          \
	.ops = &visconti_viif_isp_ctrl_ops, .type_ops = &custom_type_ops, \
	.type = V4L2_CTRL_TYPE_VISCONTI_ISP, .flags = (V4L2_CTRL_FLAG_READ_ONLY)

static const u32 defval_main_set_rawpack_mode = VIIF_RAWPACK_DISABLE;

static const struct viif_l1_input_mode_config defval_l1_set_input_mode = {
	.mode = VIIF_L1_INPUT_HDR,
	.depth = 10,
	.raw_color_filter = VIIF_L1_RAW_B_GB_GR_R,
};

static const struct viif_l1_rgb_to_y_coef_config defval_l1_set_rgb_to_y_coef = {
	/* ITU-R BT.601 */
	.coef_r = 0x4C8C,
	.coef_g = 0x9644,
	.coef_b = 0x1D30,
};

static const struct viif_l1_ag_mode_config defval_l1_set_ag_mode = { 0 };

static const struct viif_l1_ag_config defval_l1_set_ag = { 0 };

static const struct viif_l1_hdre_config defval_l1_set_hdre = {
	.hdre_src_point[0] = 0x3FFF,
	.hdre_dst_max_val = 0xFFFFFF,
};

static const struct viif_l1_img_extraction_config defval_l1_set_img_extraction = {
	.input_black_gr = 256,
	.input_black_r = 256,
	.input_black_b = 256,
	.input_black_gb = 256,
};

static const struct viif_l1_dpc_config defval_l1_set_dpc = { 0 };

static const struct viif_l1_preset_white_balance_config defval_l1_set_preset_white_balance = {
	.dstmaxval = 0x0FFF,
	.param_h = {
		.gain_gr = 0x4000,
		.gain_r  = 0x4000,
		.gain_b  = 0x4000,
		.gain_gb = 0x4000,
	},
	.param_m = {
		.gain_gr = 0x4000,
		.gain_r  = 0x4000,
		.gain_b  = 0x4000,
		.gain_gb = 0x4000,
	},
	.param_l = {
		.gain_gr = 0x4000,
		.gain_r  = 0x4000,
		.gain_b  = 0x4000,
		.gain_gb = 0x4000,
	},
};

static const
struct viif_l1_raw_color_noise_reduction_config defval_l1_set_raw_color_noise_reduction = {
	.param_h = {
		.rcnr_cnf_clip_gain_r = 3,
		.rcnr_cnf_clip_gain_g = 2,
		.rcnr_cnf_clip_gain_b = 3,
		.rcnr_merge_black = 0x20,
		.rcnr_merge_mindiv = 4,
		.rcnr_anf_blend_ag0 = 1,
		.rcnr_anf_blend_ag1 = 2,
		.rcnr_anf_blend_ag2 = 2,
		.rcnr_lpf_threshold = 8,
	},
	.param_m = {
		.rcnr_cnf_clip_gain_r = 3,
		.rcnr_cnf_clip_gain_g = 2,
		.rcnr_cnf_clip_gain_b = 3,
		.rcnr_merge_black = 0x20,
		.rcnr_merge_mindiv = 4,
		.rcnr_anf_blend_ag0 = 1,
		.rcnr_anf_blend_ag1 = 2,
		.rcnr_anf_blend_ag2 = 2,
		.rcnr_lpf_threshold = 8,
	},
	.param_l = {
		.rcnr_cnf_clip_gain_r = 3,
		.rcnr_cnf_clip_gain_g = 2,
		.rcnr_cnf_clip_gain_b = 3,
		.rcnr_merge_black = 0x20,
		.rcnr_merge_mindiv = 4,
		.rcnr_anf_blend_ag0 = 1,
		.rcnr_anf_blend_ag1 = 2,
		.rcnr_anf_blend_ag2 = 2,
		.rcnr_lpf_threshold = 8,
	},
};

static const struct viif_l1_hdrs_config defval_l1_set_hdrs = {
	.hdrs_hdr_mode = 1,
	.hdrs_hdr_ratio_m = 0x10000,
	.hdrs_hdr_ratio_l = 0x400000,
	.hdrs_hdr_ratio_e = 0x400,
	.hdrs_dg_h = 0x400,
	.hdrs_dg_m = 0x400,
	.hdrs_dg_l = 0x400,
	.hdrs_dg_e = 0x400,
	.hdrs_blendend_h = 0xFA0,
	.hdrs_blendend_m = 0xFA0,
	.hdrs_blendend_e = 0xFA0,
	.hdrs_blendbeg_h = 0x12C,
	.hdrs_blendbeg_m = 0x12C,
	.hdrs_blendbeg_e = 0x12C,
	.hdrs_dst_max_val = 0xFFFFFF,
};

static const struct viif_l1_black_level_correction_config defval_l1_set_black_level_correction = {
	.srcblacklevel_gr = 0x100,
	.srcblacklevel_r = 0x100,
	.srcblacklevel_b = 0x100,
	.srcblacklevel_gb = 0x100,
	.mulval_gr = 0x100,
	.mulval_r = 0x100,
	.mulval_b = 0x100,
	.mulval_gb = 0x100,
	.dstmaxval = 0xFFFFFF,
};

static const struct viif_l1_lsc_config defval_l1_set_lsc = { 0 };

static const struct viif_l1_main_process_config defval_l1_set_main_process = {
	.damp_lsbsel = 0x8,
	.demosaic_mode = 1,
	.colormat_enable = 0,
	.dst_maxval = 0xFFFFFF,
};

static const struct viif_l1_awb_config defval_l1_set_awb = {
	.enable = 0,
	.awhb_wbmrg = 256,
	.awhb_wbmgg = 256,
	.awhb_wbmbg = 256,
};

static const u32 defval_l1_lock_awb_gain;

static const struct viif_l1_hdrc_config
	defval_l1_set_hdrc = { .enable = 1,
			       .param = {
				       .hdrc_ratio = 0x0E + VIIF_L1_HDRC_RATIO_OFFSET,
				       .hdrc_pt_ratio = 7,
				       .hdrc_pt_sat = 0xFFC0,
				       .hdrc_tn_type = 1,
			       } };

static const struct viif_l1_hdrc_ltm_config defval_l1_set_hdrc_ltm = {
	.tnp_max = 0x3FFFFF,
	.tnp_mag = 0x40,
	.tnp_fil = { 0x88, 0x84, 0x7A, 0x6A, 0x54 },
};

static const struct viif_l1_gamma_config defval_l1_set_gamma = {
	.enable = 1,
	.param = {
		.gam_p = {
			0x02F, 0x01B, 0x02A, 0x023, 0x020, 0x037, 0x031, 0x057, 0x04D, 0x088,
			0x078, 0x0D6, 0x0BD, 0x14F, 0x12A, 0x20D, 0x1D3, 0x1AB, 0x18D, 0x2DC,
			0x29E, 0x271, 0x47C, 0x41B, 0x3D4, 0x70A, 0x672, 0x601, 0xB0C, 0xA1D,
			0x96C, 0x8E2, 0x874, 0xFDD, 0xEC9, 0xDF2, 0xD42, 0xCB1, 0xC35, 0xBC9,
			0xB6A, 0xB16, 0xACB, 0xA86},
		.blkadj = 0x1000,
	},
};

static const struct viif_l1_img_quality_adjustment_config defval_l1_set_img_quality_adjustment = {
	.enable = 0,
	.coef_cb = 0x9078,
	.coef_cr = 0xB699,
	.brightness = 0,
	.linear_contrast = 128,
};

static const struct viif_l1_avg_lum_generation_config defval_l1_set_avg_lum_generation = {
	.enable = 0
};

static const struct viif_l2_undist_config defval_l2_set_undist = { .param = {
									   .through_mode = 0,
									   .roi_mode = { 0, 0 },
									   .grid_node_num_h = 0x10,
									   .grid_node_num_v = 0x10,
								   } };

static const struct viif_l2_roi_config defval_l2_set_roi = {
	.roi_num = 1,
	.roi_scale = { 65536, 65536 },
	.roi_scale_inv = { 65536, 65536 },
	.corrected_wo_scale_hsize = { 0x80, 0x80 },
	.corrected_hsize = { 0x80, 0x80 },
	.corrected_wo_scale_vsize = { 0x80, 0x80 },
	.corrected_vsize = { 0x80, 0x80 },
};

static const struct viif_l2_gamma_config defval_l2_set_gamma = { 0 };

static const struct v4l2_ctrl_config visconti_viif_isp_ctrl_config[] = {
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_MAIN_SET_RAWPACK_MODE,
		.name = "Pack 2 Raw Datastreams",
		.p_def = { .p_const = &defval_main_set_rawpack_mode },
		.elem_size = sizeof(u32),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE,
		.name = "L1:Input Mode",
		.p_def = { .p_const = &defval_l1_set_input_mode },
		.elem_size = sizeof(struct viif_l1_input_mode_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RGB_TO_Y_COEF,
		.name = "Coef RGB to Y",
		.p_def = { .p_const = &defval_l1_set_rgb_to_y_coef },
		.elem_size = sizeof(struct viif_l1_rgb_to_y_coef_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG_MODE,
		.name = "L1:Analog Gain Mode",
		.p_def = { .p_const = &defval_l1_set_ag_mode },
		.elem_size = sizeof(struct viif_l1_ag_mode_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG,
		.name = "L1:Analog Gain Value",
		.p_def = { .p_const = &defval_l1_set_ag },
		.elem_size = sizeof(struct viif_l1_ag_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRE,
		.name = "L1:HDR Expansion",
		.p_def = { .p_const = &defval_l1_set_hdre },
		.elem_size = sizeof(struct viif_l1_hdre_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_EXTRACTION,
		.name = "L1:BlackLevel to extract SDR images from HDR input",
		.p_def = { .p_const = &defval_l1_set_img_extraction },
		.elem_size = sizeof(struct viif_l1_img_extraction_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_DPC,
		.name = "L1:Defect Pixel Correction",
		.p_def = { .p_const = &defval_l1_set_dpc },
		.elem_size = sizeof(struct viif_l1_dpc_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_PRESET_WHITE_BALANCE,
		.name = "L1:Preset White Balance",
		.p_def = { .p_const = &defval_l1_set_preset_white_balance },
		.elem_size = sizeof(struct viif_l1_preset_white_balance_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RAW_COLOR_NOISE_REDUCTION,
		.name = "L1:Raw Color Noise Reduction",
		.p_def = { .p_const = &defval_l1_set_raw_color_noise_reduction },
		.elem_size = sizeof(struct viif_l1_raw_color_noise_reduction_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRS,
		.name = "L1:HDR Synthesis",
		.p_def = { .p_const = &defval_l1_set_hdrs },
		.elem_size = sizeof(struct viif_l1_hdrs_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_BLACK_LEVEL_CORRECTION,
		.name = "L1:Black Level Correction",
		.p_def = { .p_const = &defval_l1_set_black_level_correction },
		.elem_size = sizeof(struct viif_l1_black_level_correction_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_LSC,
		.name = "L1:Lens Shading Correction",
		.p_def = { .p_const = &defval_l1_set_lsc },
		.elem_size = sizeof(struct viif_l1_lsc_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_MAIN_PROCESS,
		.name = "L1:Main Process",
		.p_def = { .p_const = &defval_l1_set_main_process },
		.elem_size = sizeof(struct viif_l1_main_process_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AWB,
		.name = "L1:Auto White Balance",
		.p_def = { .p_const = &defval_l1_set_awb },
		.elem_size = sizeof(struct viif_l1_awb_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_LOCK_AWB_GAIN,
		.name = "L1:Lock Auto White Balance Gain",
		.p_def = { .p_const = &defval_l1_lock_awb_gain },
		.elem_size = sizeof(u32),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC,
		.name = "L1:HDR Compression",
		.p_def = { .p_const = &defval_l1_set_hdrc },
		.elem_size = sizeof(struct viif_l1_hdrc_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC_LTM,
		.name = "L1:HDR Compression Local Tone",
		.p_def = { .p_const = &defval_l1_set_hdrc_ltm },
		.elem_size = sizeof(struct viif_l1_hdrc_ltm_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_GAMMA,
		.name = "L1:Gamma Correction",
		.p_def = { .p_const = &defval_l1_set_gamma },
		.elem_size = sizeof(struct viif_l1_gamma_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_QUALITY_ADJUSTMENT,
		.name = "L1:Image Quality Adjustment",
		.p_def = { .p_const = &defval_l1_set_img_quality_adjustment },
		.elem_size = sizeof(struct viif_l1_img_quality_adjustment_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AVG_LUM_GENERATION,
		.name = "L1:Average Luminance",
		.p_def = { .p_const = &defval_l1_set_avg_lum_generation },
		.elem_size = sizeof(struct viif_l1_avg_lum_generation_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_UNDIST,
		.name = "L2:Undistortion",
		.p_def = { .p_const = &defval_l2_set_undist },
		.elem_size = sizeof(struct viif_l2_undist_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_ROI,
		.name = "L2:ROI",
		.p_def = { .p_const = &defval_l2_set_roi },
		.elem_size = sizeof(struct viif_l2_roi_config),
	},
	{
		CTRL_CONFIG_DEFAULT_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_GAMMA,
		.name = "L2:Gamma Correction",
		.p_def = { .p_const = &defval_l2_set_gamma },
		.elem_size = sizeof(struct viif_l2_gamma_config),
	},
	{
		CTRL_CONFIG_VOLATILE_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_CALIBRATION_STATUS,
		.name = "CSI2RX: Calibration Status",
		.elem_size = sizeof(struct viif_csi2rx_dphy_calibration_status),
	},
	{
		CTRL_CONFIG_VOLATILE_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_ERR_STATUS,
		.name = "CSI2RX: Error Status",
		.elem_size = sizeof(struct viif_csi2rx_err_status),
	},
	{
		CTRL_CONFIG_RDONLY_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_GET_LAST_CAPTURE_STATUS,
		.name = "Last Frame Capture Status",
		.elem_size = sizeof(struct viif_isp_capture_status),
	},
	{
		CTRL_CONFIG_VOLATILE_ENTRY,
		.id = V4L2_CID_VISCONTI_VIIF_GET_REPORTED_ERRORS,
		.name = "Accumerated Reported Errors",
		.elem_size = sizeof(struct viif_reported_errors),
	},
};

int visconti_viif_isp_init_controls(struct viif_device *viif_dev)
{
	struct v4l2_ctrl_handler *ctrl_handler = &viif_dev->isp_subdev.ctrl_handler;
	int ret;
	int i;

	ret = v4l2_ctrl_handler_init(ctrl_handler, ARRAY_SIZE(visconti_viif_isp_ctrl_config));
	if (ret) {
		dev_err(viif_dev->dev, "failed on v4l2_ctrl_handler_init");
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(visconti_viif_isp_ctrl_config); i++) {
		struct v4l2_ctrl *ctrl;

		ctrl = v4l2_ctrl_new_custom(ctrl_handler, &visconti_viif_isp_ctrl_config[i],
					    viif_dev);
		if (!ctrl) {
			dev_err(viif_dev->dev, "failed to add ctrl: %d", ctrl_handler->error);
			return ctrl_handler->error;
		}
		/* controls to be evaluated before S_FMT */
		if (visconti_viif_isp_ctrl_config[i].id ==
		    V4L2_CID_VISCONTI_VIIF_MAIN_SET_RAWPACK_MODE) {
			viif_dev->isp_subdev.ctrl_rawpack_mode = ctrl;
		}
		if (visconti_viif_isp_ctrl_config[i].id ==
		    V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE) {
			viif_dev->isp_subdev.ctrl_input_mode = ctrl;
		}
		/* controls to be updated at HW interrupt */
		if (visconti_viif_isp_ctrl_config[i].id ==
		    V4L2_CID_VISCONTI_VIIF_GET_LAST_CAPTURE_STATUS) {
			viif_dev->isp_subdev.ctrl_last_capture_status = ctrl;
		}
	}

	viif_dev->isp_subdev.sd.ctrl_handler = &viif_dev->isp_subdev.ctrl_handler;
	return 0;
}
