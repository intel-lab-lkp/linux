/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2022 TOSHIBA CORPORATION
 * (C) Copyright 2022 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef __UAPI_VISCONTI_VIIF_H_
#define __UAPI_VISCONTI_VIIF_H_

#include <linux/types.h>
#include <linux/v4l2-controls.h>

/* Visconti specific compound controls */
#define V4L2_CID_VISCONTI_VIIF_MAIN_SET_RAWPACK_MODE	       (V4L2_CID_USER_VISCONTI_BASE + 1)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE	       (V4L2_CID_USER_VISCONTI_BASE + 2)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RGB_TO_Y_COEF	       (V4L2_CID_USER_VISCONTI_BASE + 3)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG_MODE	       (V4L2_CID_USER_VISCONTI_BASE + 4)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG		       (V4L2_CID_USER_VISCONTI_BASE + 5)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRE		       (V4L2_CID_USER_VISCONTI_BASE + 6)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_EXTRACTION       (V4L2_CID_USER_VISCONTI_BASE + 7)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_DPC		       (V4L2_CID_USER_VISCONTI_BASE + 8)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_PRESET_WHITE_BALANCE (V4L2_CID_USER_VISCONTI_BASE + 9)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RAW_COLOR_NOISE_REDUCTION \
	(V4L2_CID_USER_VISCONTI_BASE + 10)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRS			 (V4L2_CID_USER_VISCONTI_BASE + 11)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_BLACK_LEVEL_CORRECTION (V4L2_CID_USER_VISCONTI_BASE + 12)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_LSC			 (V4L2_CID_USER_VISCONTI_BASE + 13)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_MAIN_PROCESS		 (V4L2_CID_USER_VISCONTI_BASE + 14)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AWB			 (V4L2_CID_USER_VISCONTI_BASE + 15)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_LOCK_AWB_GAIN		 (V4L2_CID_USER_VISCONTI_BASE + 16)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC			 (V4L2_CID_USER_VISCONTI_BASE + 17)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC_LTM		 (V4L2_CID_USER_VISCONTI_BASE + 18)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_GAMMA			 (V4L2_CID_USER_VISCONTI_BASE + 19)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_QUALITY_ADJUSTMENT (V4L2_CID_USER_VISCONTI_BASE + 20)
#define V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AVG_LUM_GENERATION	 (V4L2_CID_USER_VISCONTI_BASE + 21)
#define V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_UNDIST		 (V4L2_CID_USER_VISCONTI_BASE + 22)
#define V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_ROI			 (V4L2_CID_USER_VISCONTI_BASE + 23)
#define V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_GAMMA			 (V4L2_CID_USER_VISCONTI_BASE + 24)
#define V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_CALIBRATION_STATUS	 (V4L2_CID_USER_VISCONTI_BASE + 25)
#define V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_ERR_STATUS		 (V4L2_CID_USER_VISCONTI_BASE + 26)
#define V4L2_CID_VISCONTI_VIIF_GET_LAST_CAPTURE_STATUS		 (V4L2_CID_USER_VISCONTI_BASE + 27)
#define V4L2_CID_VISCONTI_VIIF_GET_REPORTED_ERRORS		 (V4L2_CID_USER_VISCONTI_BASE + 28)

/* Enable/Disable flag */
#define VIIF_DISABLE (0U)
#define VIIF_ENABLE  (1U)

/**
 * enum viif_rawpack_mode - RAW pack mode for :ref:`V4L2_CID_VISCONTI_VIIF_MAIN_SET_RAWPACK_MODE`
 *
 * @VIIF_RAWPACK_DISABLE: RAW pack disable
 * @VIIF_RAWPACK_MSBFIRST: RAW pack enable (MSB First)
 * @VIIF_RAWPACK_LSBFIRST: RAW pack enable (LSB First)
 */
enum viif_rawpack_mode {
	VIIF_RAWPACK_DISABLE = 0,
	VIIF_RAWPACK_MSBFIRST = 2,
	VIIF_RAWPACK_LSBFIRST = 3,
};

/**
 * enum viif_l1_input - L1ISP preprocessing mode,
 * referred by :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE`
 *
 * @VIIF_L1_INPUT_HDR: bypass(HDR input)
 * @VIIF_L1_INPUT_PWL: HDRE(PWL input)
 * @VIIF_L1_INPUT_HDR_IMG_CORRECT: SLIC-ABPC-PWHB-RCNR-HDRS
 * @VIIF_L1_INPUT_PWL_IMG_CORRECT: HDRE-SLIC-ABPC-PWHB-RCNR-HDRS
 */
enum viif_l1_input {
	VIIF_L1_INPUT_HDR = 0,
	VIIF_L1_INPUT_PWL = 1,
	VIIF_L1_INPUT_HDR_IMG_CORRECT = 3,
	VIIF_L1_INPUT_PWL_IMG_CORRECT = 4,
};

/**
 * enum viif_l1_raw - L1ISP RAW color filter mode,
 * referred by :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE`
 *
 * @VIIF_L1_RAW_GR_R_B_GB: Gr-R-B-Gb
 * @VIIF_L1_RAW_R_GR_GB_B: R-Gr-Gb-B
 * @VIIF_L1_RAW_B_GB_GR_R: B-Gb-Gr-R
 * @VIIF_L1_RAW_GB_B_R_GR: Gb-B-R-Gr
 */
enum viif_l1_raw {
	VIIF_L1_RAW_GR_R_B_GB = 0,
	VIIF_L1_RAW_R_GR_GB_B = 1,
	VIIF_L1_RAW_B_GB_GR_R = 2,
	VIIF_L1_RAW_GB_B_R_GR = 3,
};

/**
 * struct viif_l1_input_mode_config - L1ISP INPUT MODE parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE`
 *
 * @mode: &enum viif_l1_input value.
 * @depth: Color depth (even only). Range for each L1ISP pre-processing mode is:
 *
 *  - VIIF_L1_INPUT_HDR, VIIF_L1_INPUT_HDR_IMG_CORRECT: Range: [8..24].
 *  - VIIF_L1_INPUT_PWL, VIIF_L1_INPUT_PWL_IMG_CORRECT: Range: [8..14].
 * @raw_color_filter: &enum viif_l1_raw value.
 */
struct viif_l1_input_mode_config {
	__u32 mode;
	__u32 depth;
	__u32 raw_color_filter;
};

/**
 * struct viif_l1_rgb_to_y_coef_config - L1ISP coefficient for calculating
 * Y from RGB parameters for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RGB_TO_Y_COEF`
 * @coef_r: R co-efficient. Range: [256..65024], Accuracy: 1/65536.
 * @coef_g: G co-efficient. Range: [256..65024], Accuracy: 1/65536.
 * @coef_b: B co-efficient. Range: [256..65024], Accuracy: 1/65536.
 */
struct viif_l1_rgb_to_y_coef_config {
	__u16 coef_r;
	__u16 coef_g;
	__u16 coef_b;
};

/**
 * enum viif_l1_img_sensitivity_mode - L1ISP image sensitivity
 *
 * @VIIF_L1_IMG_SENSITIVITY_HIGH: high sensitivity
 * @VIIF_L1_IMG_SENSITIVITY_MIDDLE_LED: middle sensitivity or led
 * @VIIF_L1_IMG_SENSITIVITY_LOW: low sensitivity
 */
enum viif_l1_img_sensitivity_mode {
	VIIF_L1_IMG_SENSITIVITY_HIGH = 0,
	VIIF_L1_IMG_SENSITIVITY_MIDDLE_LED = 1,
	VIIF_L1_IMG_SENSITIVITY_LOW = 2,
};

/**
 * struct viif_l1_ag_mode_config - L1ISP AG mode parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG_MODE`
 * @sysm_ag_grad: Analog gain slope. Range: [0..255], Index corresponds to psel id.
 * @sysm_ag_ofst: Analog gain offset. Range: [0..65535], Index corresponds to psel id.
 * @sysm_ag_cont_hobc_en_high: set 1 to enable  control analog gain
 *                             for high sensitivity image of OBCC
 * @sysm_ag_psel_hobc_high: Analog gain id for high sensitivity image of OBCC. Range: [0..3]
 * @sysm_ag_cont_hobc_en_middle_led: set 1 to enable control analog gain
 *                                   for middle sensitivity or LED image of OBCC
 * @sysm_ag_psel_hobc_middle_led: Analog gain id for middle sensitivity
 *                                or LED image of OBCC. Range: [0..3]
 * @sysm_ag_cont_hobc_en_low: set 1 to enable control analog gain
 *                            for low sensitivity image of OBCC
 * @sysm_ag_psel_hobc_low: Analog gain id for low sensitivity image of OBCC. Range:[0..3]
 * @sysm_ag_cont_abpc_en_high: set 1 to enable control analog gain
 *                             for high sensitivity image of ABPC
 * @sysm_ag_psel_abpc_high: Analog gain id for high sensitivity image of ABPC. Range: [0..3]
 * @sysm_ag_cont_abpc_en_middle_led: set 1 to enable control analog gain
 *                                   for middle sensitivity or LED image of ABPC
 * @sysm_ag_psel_abpc_middle_led: Analog gain id for middle sensitivity
 *                                or LED image of ABPC. Range: [0..3]
 * @sysm_ag_cont_abpc_en_low: set 1 to enable control analog gain
 *                            for low sensitivity image of ABPC
 * @sysm_ag_psel_abpc_low: Analog gain id for low sensitivity image of ABPC. Range: [0..3]
 * @sysm_ag_cont_rcnr_en_high: set 1 to enable control analog gain
 *                             for high sensitivity image of RCNR
 * @sysm_ag_psel_rcnr_high: Analog gain id for high sensitivity image of RCNR. Range: [0..3]
 * @sysm_ag_cont_rcnr_en_middle_led: set 1 to enable control analog gain
 *                                   for middle sensitivity or LED image of RCNR
 * @sysm_ag_psel_rcnr_middle_led: Analog gain id for middle sensitivity
 *                                or LED image of RCNR. Range: [0..3]
 * @sysm_ag_cont_rcnr_en_low: set 1 to enable control analog gain
 *                            for low sensitivity image of RCNR
 * @sysm_ag_psel_rcnr_low: Analog gain id for low sensitivity image of RCNR. Range: [0..3]
 * @sysm_ag_cont_lssc_en: set 1 to enable control analog gain for LSC
 * @sysm_ag_ssel_lssc: &enum viif_l1_img_sensitivity_mode value. Sensitive image used for LSC.
 * @sysm_ag_psel_lssc: Analog gain id for LSC. Range: [0..3]
 * @sysm_ag_cont_mpro_en: set 1 to enable control analog gain for color matrix
 * @sysm_ag_ssel_mpro: &enum viif_l1_img_sensitivity_mode value.
 *                     Sensitive image used for color matrix.
 * @sysm_ag_psel_mpro: Analog gain id for color matrix. Range: [0..3]
 * @sysm_ag_cont_vpro_en: set 1 to enable control analog gain for image adjustment
 * @sysm_ag_ssel_vpro: &enum viif_l1_img_sensitivity_mode value.
 *                     Sensitive image used for image adjustment.
 * @sysm_ag_psel_vpro: Analog gain id for image adjustment. Range: [0..3]
 * @sysm_ag_cont_hobc_test_high: Manual analog gain for high sensitivity image
 *                               of OBCC. Range: [0..255]
 * @sysm_ag_cont_hobc_test_middle_led: Manual analog gain for middle sensitivity
 *                                     or led image of OBCC. Range: [0..255]
 * @sysm_ag_cont_hobc_test_low: Manual analog gain for low sensitivity image
 *                              of OBCC. Range: [0..255]
 * @sysm_ag_cont_abpc_test_high: Manual analog gain for high sensitivity image
 *                               of ABPC. Range: [0..255]
 * @sysm_ag_cont_abpc_test_middle_led: Manual analog gain for middle sensitivity
 *                                     or led image of ABPC. Range: [0..255]
 * @sysm_ag_cont_abpc_test_low: Manual analog gain for low sensitivity image
 *                              of ABPC. Range: [0..255]
 * @sysm_ag_cont_rcnr_test_high: Manual analog gain for high sensitivity image
 *                               of RCNR. Range:  [0..255]
 * @sysm_ag_cont_rcnr_test_middle_led: Manual analog gain for middle sensitivity
 *                                     or led image of RCNR. Range: [0..255]
 * @sysm_ag_cont_rcnr_test_low: Manual analog gain for low sensitivity image
 *                              of RCNR. Range:  [0..255]
 * @sysm_ag_cont_lssc_test: Manual analog gain for LSSC. Range: [0..255]
 * @sysm_ag_cont_mpro_test: Manual analog gain for color matrix. Range: [0..255]
 * @sysm_ag_cont_vpro_test: Manual analog gain for image adjustment. Range: [0..255]
 *
 * Configuration of analog gain generation for each L1ISP module.
 *
 * If sysm_ag_cont_xxxx_en = 1, analog_gain for each module is generated from
 * sysm_ag_grad, sysm_ag_ofst and the value specified at
 * :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG`.
 * If sysm_ag_cont_xxxx_en = 0,
 * the value of sysm_ag_cont_xxxx_test is used for analog_gain for each module.
 *
 * Up to 4 sets of parameters (sysm_ag_grad[4] and sysm_ag_ofst[4]) can be used
 * to generate analog gain.
 * The parameter sysm_ag_psel_xxxx specifies the set to be used for module xxxx.
 * For example, if sysm_ag_psel_hobc_high is set to 2,
 * values in sysm_ag_grad[2] and sysm_ag_ofst[2] are used
 * to generate analog gain for high sensitivity images in OBCC processing.
 *
 * Analog gain generation for each L1ISP module is disabled when
 * "sysm_ag_cont_xxxx_en=0" and "sysm_ag_cont_xxxx_test=0".
 * Be sure to disable the analog gain generation
 * if VIIF_L1_INPUT_HDR or VIIF_L1_INPUT_PWL is set to
 * :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_INPUT_MODE`.
 *
 */
struct viif_l1_ag_mode_config {
	__u8 sysm_ag_grad[4];
	__u16 sysm_ag_ofst[4];
	__u32 sysm_ag_cont_hobc_en_high;
	__u32 sysm_ag_psel_hobc_high;
	__u32 sysm_ag_cont_hobc_en_middle_led;
	__u32 sysm_ag_psel_hobc_middle_led;
	__u32 sysm_ag_cont_hobc_en_low;
	__u32 sysm_ag_psel_hobc_low;
	__u32 sysm_ag_cont_abpc_en_high;
	__u32 sysm_ag_psel_abpc_high;
	__u32 sysm_ag_cont_abpc_en_middle_led;
	__u32 sysm_ag_psel_abpc_middle_led;
	__u32 sysm_ag_cont_abpc_en_low;
	__u32 sysm_ag_psel_abpc_low;
	__u32 sysm_ag_cont_rcnr_en_high;
	__u32 sysm_ag_psel_rcnr_high;
	__u32 sysm_ag_cont_rcnr_en_middle_led;
	__u32 sysm_ag_psel_rcnr_middle_led;
	__u32 sysm_ag_cont_rcnr_en_low;
	__u32 sysm_ag_psel_rcnr_low;
	__u32 sysm_ag_cont_lssc_en;
	__u32 sysm_ag_ssel_lssc;
	__u32 sysm_ag_psel_lssc;
	__u32 sysm_ag_cont_mpro_en;
	__u32 sysm_ag_ssel_mpro;
	__u32 sysm_ag_psel_mpro;
	__u32 sysm_ag_cont_vpro_en;
	__u32 sysm_ag_ssel_vpro;
	__u32 sysm_ag_psel_vpro;
	__u8 sysm_ag_cont_hobc_test_high;
	__u8 sysm_ag_cont_hobc_test_middle_led;
	__u8 sysm_ag_cont_hobc_test_low;
	__u8 sysm_ag_cont_abpc_test_high;
	__u8 sysm_ag_cont_abpc_test_middle_led;
	__u8 sysm_ag_cont_abpc_test_low;
	__u8 sysm_ag_cont_rcnr_test_high;
	__u8 sysm_ag_cont_rcnr_test_middle_led;
	__u8 sysm_ag_cont_rcnr_test_low;
	__u8 sysm_ag_cont_lssc_test;
	__u8 sysm_ag_cont_mpro_test;
	__u8 sysm_ag_cont_vpro_test;
};

/**
 * struct viif_l1_ag_config - L1ISP AG parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AG`
 * @gain_h: Analog gain for high sensitive image. Range: [0..65535].
 * @gain_m: Analog gain for middle sensitive image or LED image. Range: [0..65535].
 * @gain_l: Analog gain for low sensitive image. Range:  [0..65535].
 */
struct viif_l1_ag_config {
	__u16 gain_h;
	__u16 gain_m;
	__u16 gain_l;
};

/**
 * struct viif_l1_hdre_config - L1ISP HDRE parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRE`
 * @hdre_src_point: Knee point N value of PWL compressed signal. Range: [0..0x3FFF].
 * @hdre_dst_base: Offset value of HDR signal in Knee area M. Range: [0..0xFFFFFF].
 * @hdre_ratio: Slope of output pixel value in Knee area M.
 *              Range: [0..0x3FFFFF], Accuracy: 1/64.
 * @hdre_dst_max_val: Maximum value of output pixel. Range: [0..0xFFFFFF].
 */
struct viif_l1_hdre_config {
	__u32 hdre_src_point[16];
	__u32 hdre_dst_base[17];
	__u32 hdre_ratio[17];
	__u32 hdre_dst_max_val;
};

/**
 * struct viif_l1_img_extraction_config -  L1ISP image extraction parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_EXTRACTION`
 * @input_black_gr: Black level of input pixel (Gr). Range: [0..0xFFFFFF].
 * @input_black_r: Black level of input pixel (R). Range: [0..0xFFFFFF].
 * @input_black_b: Black level of input pixel (B). Range: [0..0xFFFFFF].
 * @input_black_gb: Black level of input pixel (Gb). Range: [0..0xFFFFFF].
 */
struct viif_l1_img_extraction_config {
	__u32 input_black_gr;
	__u32 input_black_r;
	__u32 input_black_b;
	__u32 input_black_gb;
};

/**
 * enum viif_l1_dpc_mode - L1ISP defect pixel correction mode
 * @VIIF_L1_DPC_1PIXEL: 1 pixel correction mode
 * @VIIF_L1_DPC_2PIXEL: 2 pixel correction mode
 */
enum viif_l1_dpc_mode {
	VIIF_L1_DPC_1PIXEL = 0,
	VIIF_L1_DPC_2PIXEL = 1,
};

/**
 * struct viif_l1_dpc - L1ISP defect pixel correction parameters
 * for &struct viif_l1_dpc_config
 * @abpc_sta_en: 1:enable/0:disable setting of Static DPC
 * @abpc_dyn_en: 1:enable/0:disable setting of Dynamic DPC
 * @abpc_dyn_mode: &enume viif_l1_dpc_mode value. Sets dynamic DPC mode.
 * @abpc_ratio_limit: Variation adjustment of dynamic DPC. Range: [0..1023].
 * @abpc_dark_limit: White defect judgment limit of dark area. Range: [0..1023].
 * @abpc_sn_coef_w_ag_min: Luminance difference adjustment of white DPC
 *                         (undere lower threshold).
 * @abpc_sn_coef_w_ag_mid: Luminance difference adjustment of white DPC
 *                         (between lower and upper threshold).
 * @abpc_sn_coef_w_ag_max: Luminance difference adjustment of white DPC
 *                         (over upper threshold).
 * @abpc_sn_coef_b_ag_min: Luminance difference adjustment of black DPC
 *                         (undere lower threshold).
 * @abpc_sn_coef_b_ag_mid: Luminance difference adjustment of black DPC
 *                         (between lower and upper threshold).
 * @abpc_sn_coef_b_ag_max: Luminance difference adjustment of black DPC
 *                         (over upper threshold).
 * @abpc_sn_coef_w_th_min: Luminance difference adjustment of white DPC
 *                         analog gain lower threshold.
 * @abpc_sn_coef_w_th_max: Luminance difference adjustment of white DPC
 *                         analog gain upper threshold.
 * @abpc_sn_coef_b_th_min: Luminance difference adjustment of black DPC
 *                         analog gain lower threshold.
 * @abpc_sn_coef_b_th_max: Luminance difference adjustment of black DPC
 *                         analog gain upper threshold.
 *
 * Range of abpc_sn_coef_{w,b}_ag_{min,mid,max} is:
 *
 * - Range: [1..31]
 *
 * Range and constraints of sn_coef_{w,b}_th_{min,max} are:
 *
 * - Range: [0..255]
 * - Constraint: abpc_sn_coef_w_th_min < abpc_sn_coef_w_th_max
 * - Constraint: abpc_sn_coef_b_th_min < abpc_sn_coef_b_th_max
 */
struct viif_l1_dpc {
	__u32 abpc_sta_en;
	__u32 abpc_dyn_en;
	__u32 abpc_dyn_mode;
	__u32 abpc_ratio_limit;
	__u32 abpc_dark_limit;
	__u32 abpc_sn_coef_w_ag_min;
	__u32 abpc_sn_coef_w_ag_mid;
	__u32 abpc_sn_coef_w_ag_max;
	__u32 abpc_sn_coef_b_ag_min;
	__u32 abpc_sn_coef_b_ag_mid;
	__u32 abpc_sn_coef_b_ag_max;
	__u8 abpc_sn_coef_w_th_min;
	__u8 abpc_sn_coef_w_th_max;
	__u8 abpc_sn_coef_b_th_min;
	__u8 abpc_sn_coef_b_th_max;
};

/**
 * struct viif_l1_dpc_config - L1ISP defect pixel correction parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_DPC`
 * @param_h: DPC parameter for high sensitive image. Refer to &struct viif_l1_dpc
 * @param_m: DPC parameter for middle sensitive image. Refer to &struct viif_l1_dpc
 * @param_l: DPC parameter for low sensitive image. Refer to &struct viif_l1_dpc
 * @table_h: DPC table for high sensitive image.
 *           The table is referred only when param_h.abpc_sta_en =1
 * @table_m: DPC table for middle sensitive image or LED image.
 *           The table is referred only when param_m.abpc_sta_en =1
 * @table_l: DPC table for low sensitive image.
 *           The table is referred only when param_l.abpc_sta_en =1
 *
 * The size of each table is 8192 Bytes (u32 * 2048)
 * Application should make sure that the table data is based on HW specification
 * since this driver does not check the DPC table.
 */
struct viif_l1_dpc_config {
	struct viif_l1_dpc param_h;
	struct viif_l1_dpc param_m;
	struct viif_l1_dpc param_l;
	__u32 table_h[2048];
	__u32 table_m[2048];
	__u32 table_l[2048];
};

/**
 * struct viif_l1_preset_wb - L1ISP  preset white balance parameters
 * for &struct viif_l1_preset_white_balance_config
 * @gain_gr: Gr gain. Range: [0..524287], Accuracy 1/16384
 * @gain_r: R gain. Range: [0..524287], Accuracy 1/16384
 * @gain_b: B gain. Range: [0..524287], Accuracy 1/16384
 * @gain_gb: Gb gain. Range: [0..524287], Accuracy 1/16384
 */
struct viif_l1_preset_wb {
	__u32 gain_gr;
	__u32 gain_r;
	__u32 gain_b;
	__u32 gain_gb;
};

/**
 * struct viif_l1_preset_white_balance_config - L1ISP  preset white balance
 * parameters for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_PRESET_WHITE_BALANCE`
 * @dstmaxval: Maximum value of output pixel. Range: [0..4095]
 * @param_h: Preset white balance parameter for high sensitive image.
 *           Refer to &struct viif_l1_preset_wb
 * @param_m: Preset white balance parameters for middle sensitive image or LED image.
 *           Refer to &struct viif_l1_preset_wb
 * @param_l: Preset white balance parameters for low sensitive image.
 *           Refer to &struct viif_l1_preset_wb
 */
struct viif_l1_preset_white_balance_config {
	__u32 dstmaxval;
	struct viif_l1_preset_wb param_h;
	struct viif_l1_preset_wb param_m;
	struct viif_l1_preset_wb param_l;
};

/**
 * enum viif_l1_rcnr_type - L1ISP high resolution luminance filter type
 *
 * @VIIF_L1_RCNR_LOW_RESOLUTION: low resolution
 * @VIIF_L1_RCNR_MIDDLE_RESOLUTION: middle resolution
 * @VIIF_L1_RCNR_HIGH_RESOLUTION: high resolution
 * @VIIF_L1_RCNR_ULTRA_HIGH_RESOLUTION: ultra high resolution
 */
enum viif_l1_rcnr_type {
	VIIF_L1_RCNR_LOW_RESOLUTION = 0,
	VIIF_L1_RCNR_MIDDLE_RESOLUTION = 1,
	VIIF_L1_RCNR_HIGH_RESOLUTION = 2,
	VIIF_L1_RCNR_ULTRA_HIGH_RESOLUTION = 3,
};

/**
 * enum viif_l1_msf_blend_ratio - L1ISP MSF blend ratio
 *
 * @VIIF_L1_MSF_BLEND_RATIO_0_DIV_64: 0/64
 * @VIIF_L1_MSF_BLEND_RATIO_1_DIV_64: 1/64
 * @VIIF_L1_MSF_BLEND_RATIO_2_DIV_64: 2/64
 */
enum viif_l1_msf_blend_ratio {
	VIIF_L1_MSF_BLEND_RATIO_0_DIV_64 = 0,
	VIIF_L1_MSF_BLEND_RATIO_1_DIV_64 = 1,
	VIIF_L1_MSF_BLEND_RATIO_2_DIV_64 = 2,
};

/**
 * struct viif_l1_raw_color_noise_reduction - L1ISP RCNR parameters
 * for &struct viif_l1_raw_color_noise_reduction_config
 * @rcnr_sw: set 1 to enable RAW color noise reduction, 0 to disable.
 * @rcnr_cnf_dark_ag0: Maximum value of LSF dark noise adjustment. Range: [0..63].
 * @rcnr_cnf_dark_ag1: Middle value of LSF dark noise adjustment. Range: [0..63].
 * @rcnr_cnf_dark_ag2: Minimum value of LSF dark noise adjustment. Range: [0..63].
 * @rcnr_cnf_ratio_ag0: Maximum value of LSF luminance interlocking noise adjustment.
 *                      Range: [0..31].
 * @rcnr_cnf_ratio_ag1: Middle value of LSF luminance interlocking noise adjustment:
 *                      Range: [0..31].
 * @rcnr_cnf_ratio_ag2: Minimum value of LSF luminance interlocking noise adjustment:
 *                      Range: [0..31].
 * @rcnr_cnf_clip_gain_r: LSF color correction limit adjustment gain R. Range: [0..3].
 * @rcnr_cnf_clip_gain_g: LSF color correction limit adjustment gain G. Range: [0..3].
 * @rcnr_cnf_clip_gain_b: LSF color correction limit adjustment gain B. Range: [0..3].
 * @rcnr_a1l_dark_ag0: Maximum value of MSF dark noise adjustment. Range: [0..63].
 * @rcnr_a1l_dark_ag1: Middle value of MSF dark noise adjustment. Range: [0..63].
 * @rcnr_a1l_dark_ag2: Minimum value of MSF dark noise adjustment. Range: [0..63].
 * @rcnr_a1l_ratio_ag0: Maximum value of MSF luminance interlocking noise adjustment.
 *                      Range: [0..31].
 * @rcnr_a1l_ratio_ag1: Middle value of MSF luminance interlocking noise adjustment.
 *                      Range: [0..31].
 * @rcnr_a1l_ratio_ag2: Minimum value of MSF luminance interlocking noise adjustment.
 *                      Range: [0..31].
 * @rcnr_inf_zero_clip: Input stage zero clip setting. Range: [0..256].
 * @rcnr_merge_d2blend_ag0: Maximum value of filter results and input blend ratio. Range: [0..16].
 * @rcnr_merge_d2blend_ag1: Middle value of filter results and input blend ratio. Range: [0..16].
 * @rcnr_merge_d2blend_ag2: Minimum value of filter results and input blend ratio. Range: [0..16].
 * @rcnr_merge_black: Black level minimum value. Range: [0..64].
 * @rcnr_merge_mindiv: 0 div guard value of inverse arithmetic unit. Range: [4..16].
 * @rcnr_hry_type: &enum viif_l1_rcnr_type value. Filter type for HSF filter process.
 * @rcnr_anf_blend_ag0: &enum viif_l1_msf_blend_ratio value.
 *                      Maximum value of MSF result blend ratio in write back data to line memory.
 * @rcnr_anf_blend_ag1: &enum viif_l1_msf_blend_ratio value.
 *                      Middle value of MSF result blend ratio in write back data to line memory.
 * @rcnr_anf_blend_ag2: &enum viif_l1_msf_blend_ratio value.
 *                      Minimum value of MSF result blend ratio in write back data to line memory.
 * @rcnr_lpf_threshold: Multiplier value for calculating dark noise / luminance
 *                      interlock noise of MSF. Range: [0..31], Accuracy: 1/8.
 * @rcnr_merge_hlblend_ag0: Maximum value of luminance signal generation blend. Range: [0..2].
 * @rcnr_merge_hlblend_ag1: Middle value of luminance signal generation blend. Range: [0..2].
 * @rcnr_merge_hlblend_ag2: Minimum value of luminance signal generation blend. Range: [0..2].
 * @rcnr_gnr_sw: set 1 to enable Gr/Gb sensitivity ratio correction function switching,
 *                0 to disable.
 * @rcnr_gnr_ratio: Upper limit of Gr/Gb sensitivity ratio correction factor. Range: [0..15].
 * @rcnr_gnr_wide_en: set 1 to double correction upper limit ratio of rcnr_gnr_ratio,
 *                    0 to just use the specified ratio.
 */
struct viif_l1_raw_color_noise_reduction {
	__u32 rcnr_sw;
	__u32 rcnr_cnf_dark_ag0;
	__u32 rcnr_cnf_dark_ag1;
	__u32 rcnr_cnf_dark_ag2;
	__u32 rcnr_cnf_ratio_ag0;
	__u32 rcnr_cnf_ratio_ag1;
	__u32 rcnr_cnf_ratio_ag2;
	__u32 rcnr_cnf_clip_gain_r;
	__u32 rcnr_cnf_clip_gain_g;
	__u32 rcnr_cnf_clip_gain_b;
	__u32 rcnr_a1l_dark_ag0;
	__u32 rcnr_a1l_dark_ag1;
	__u32 rcnr_a1l_dark_ag2;
	__u32 rcnr_a1l_ratio_ag0;
	__u32 rcnr_a1l_ratio_ag1;
	__u32 rcnr_a1l_ratio_ag2;
	__u32 rcnr_inf_zero_clip;
	__u32 rcnr_merge_d2blend_ag0;
	__u32 rcnr_merge_d2blend_ag1;
	__u32 rcnr_merge_d2blend_ag2;
	__u32 rcnr_merge_black;
	__u32 rcnr_merge_mindiv;
	__u32 rcnr_hry_type;
	__u32 rcnr_anf_blend_ag0;
	__u32 rcnr_anf_blend_ag1;
	__u32 rcnr_anf_blend_ag2;
	__u32 rcnr_lpf_threshold;
	__u32 rcnr_merge_hlblend_ag0;
	__u32 rcnr_merge_hlblend_ag1;
	__u32 rcnr_merge_hlblend_ag2;
	__u32 rcnr_gnr_sw;
	__u32 rcnr_gnr_ratio;
	__u32 rcnr_gnr_wide_en;
};

/**
 * struct viif_l1_raw_color_noise_reduction_config - L1ISP RCNR parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_RAW_COLOR_NOISE_REDUCTION`
 * @param_h: RAW color noise reduction parameter for high sensitive image.
 *           Refer to &struct viif_l1_raw_color_noise_reduction
 * @param_m: RAW color noise reduction parameter for middle sensitive image or LED image.
 *           Refer to &struct viif_l1_raw_color_noise_reduction
 * @param_l: RAW color noise reduction parameter for low sensitive image.
 *           Refer to &struct viif_l1_raw_color_noise_reduction
 */
struct viif_l1_raw_color_noise_reduction_config {
	struct viif_l1_raw_color_noise_reduction param_h;
	struct viif_l1_raw_color_noise_reduction param_m;
	struct viif_l1_raw_color_noise_reduction param_l;
};

/**
 * enum viif_l1_hdrs_middle_img_mode - L1ISP HDR setting
 *
 * @VIIF_L1_HDRS_NOT_USE_MIDDLE_SENS_IMAGE: not use middle image
 * @VIIF_L1_HDRS_USE_MIDDLE_SENS_IMAGE: use middle image
 */
enum viif_l1_hdrs_middle_img_mode {
	VIIF_L1_HDRS_NOT_USE_MIDDLE_SENS_IMAGE = 0,
	VIIF_L1_HDRS_USE_MIDDLE_SENS_IMAGE = 1,
};

/**
 * struct viif_l1_hdrs_config - L1ISP HDRS parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRS`
 * @hdrs_hdr_mode: &enum viif_l1_hdrs_middle_img_mode value.
 *                 Switch for use of middle sensitivity image in HDRS.
 * @hdrs_hdr_ratio_m: Magnification ratio of middle sensitivity image for high
 *                    sensitivity image.
 * @hdrs_hdr_ratio_l: Magnification ratio of low sensitivity image for high
 *                    sensitivity image.
 * @hdrs_hdr_ratio_e: Magnification ratio of LED image for high sensitivity image.
 * @hdrs_dg_h: High sensitivity image digital gain.
 * @hdrs_dg_m: Middle sensitivity image digital gain.
 * @hdrs_dg_l: Low sensitivity image digital gain.
 * @hdrs_dg_e: LED image digital gain.
 * @hdrs_blendend_h: Maximum luminance used for blend high sensitivity image.
 * @hdrs_blendend_m: Maximum luminance used for blend middle sensitivity image.
 * @hdrs_blendend_e: Maximum luminance used for blend LED image.
 * @hdrs_blendbeg_h: Minimum luminance used for blend high sensitivity image.
 * @hdrs_blendbeg_m: Minimum luminance used for blend middle sensitivity image.
 * @hdrs_blendbeg_e: Minimum luminance used for blend LED image.
 * @hdrs_led_mode_on: set 1 to enable LED mode, 0 to disable
 * @hdrs_dst_max_val: Maximum value of output pixel. Range: [0..0xFFFFFF].
 *
 * Range and Accuracy of parameters are:
 *
 * - hdrs_hdr_ratio_{m,l,e}
 *
 *   - Range: [0x400..0x400000]
 *   - Accuracy: 1/1024
 *
 * - hdrs_dg_{h,m,l,e}
 *
 *   - Range: [0..0x3FFFFF]
 *   - Accuracy: 1/1024
 *
 * - hdrs_blend{end,beg}_{h,m,e}
 *
 *   - Range [0..4095]
 *
 * Parameter error will be returned when:
 * (hdrs_hdr_mode == VIIF_L1_HDRS_USE_MIDDLE_SENS_IMAGE) && (hdrs_led_mode_on == 1)
 */
struct viif_l1_hdrs_config {
	__u32 hdrs_hdr_mode;
	__u32 hdrs_hdr_ratio_m;
	__u32 hdrs_hdr_ratio_l;
	__u32 hdrs_hdr_ratio_e;
	__u32 hdrs_dg_h;
	__u32 hdrs_dg_m;
	__u32 hdrs_dg_l;
	__u32 hdrs_dg_e;
	__u32 hdrs_blendend_h;
	__u32 hdrs_blendend_m;
	__u32 hdrs_blendend_e;
	__u32 hdrs_blendbeg_h;
	__u32 hdrs_blendbeg_m;
	__u32 hdrs_blendbeg_e;
	__u32 hdrs_led_mode_on;
	__u32 hdrs_dst_max_val;
};

/**
 * struct viif_l1_black_level_correction_config -  L1ISP image level conversion
 * parameters for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_BLACK_LEVEL_CORRECTION`
 * @srcblacklevel_gr: Black level of Gr input pixel. Range: [0..0xFFFFFF].
 * @srcblacklevel_r: Black level of R input pixel. Range: [0..0xFFFFFF].
 * @srcblacklevel_b: Black level of B input pixel. Range: [0..0xFFFFFF].
 * @srcblacklevel_gb: Black level of Gb input pixel. Range: [0..0xFFFFFF].
 * @mulval_gr: Gr gain. Range: [0..0xFFFFF], Accuracy: 1/256.
 * @mulval_r: R gain. Range: [0..0xFFFFF], Accuracy: 1/256.
 * @mulval_b: B gain. Range: [0..0xFFFFF], Accuracy: 1/256.
 * @mulval_gb: Gb gain. Range: [0..0xFFFFF], Accuracy: 1/256.
 * @dstmaxval: Maximum value of output pixel. Range: [0..0xFFFFFF].
 */
struct viif_l1_black_level_correction_config {
	__u32 srcblacklevel_gr;
	__u32 srcblacklevel_r;
	__u32 srcblacklevel_b;
	__u32 srcblacklevel_gb;
	__u32 mulval_gr;
	__u32 mulval_r;
	__u32 mulval_b;
	__u32 mulval_gb;
	__u32 dstmaxval;
};

/**
 * enum viif_l1_para_coef_gain - L1ISP parabola shading correction coefficient ratio
 *
 * @VIIF_L1_PARA_COEF_GAIN_ONE_EIGHTH: 1/8
 * @VIIF_L1_PARA_COEF_GAIN_ONE_FOURTH: 1/4
 * @VIIF_L1_PARA_COEF_GAIN_ONE_SECOND: 1/2
 * @VIIF_L1_PARA_COEF_GAIN_ONE_FIRST: 1/1
 */
enum viif_l1_para_coef_gain {
	VIIF_L1_PARA_COEF_GAIN_ONE_EIGHTH = 0, /* 1/8 */
	VIIF_L1_PARA_COEF_GAIN_ONE_FOURTH = 1, /* 1/4 */
	VIIF_L1_PARA_COEF_GAIN_ONE_SECOND = 2, /* 1/2 */
	VIIF_L1_PARA_COEF_GAIN_ONE_FIRST = 3, /* 1/1 */
};

/**
 * enum viif_l1_grid_coef_gain - L1ISP grid shading correction coefficient ratio
 *
 * @VIIF_L1_GRID_COEF_GAIN_X1: x1
 * @VIIF_L1_GRID_COEF_GAIN_X2: x2
 */
enum viif_l1_grid_coef_gain {
	VIIF_L1_GRID_COEF_GAIN_X1 = 0,
	VIIF_L1_GRID_COEF_GAIN_X2 = 1,
};

/**
 * struct viif_l1_lsc_parabola_ag_param - L2ISP parabola shading parameters
 * for &struct viif_l1_lsc_parabola_param
 * @lssc_paracoef_h_l_max: Parabola coefficient left maximum gain value
 * @lssc_paracoef_h_l_min: Parabola coefficient left minimum gain value
 * @lssc_paracoef_h_r_max: Parabola coefficient right maximum gain value
 * @lssc_paracoef_h_r_min: Parabola coefficient right minimum gain value
 * @lssc_paracoef_v_u_max: Parabola coefficient upper maximum gain value
 * @lssc_paracoef_v_u_min: Parabola coefficient upper minimum gain value
 * @lssc_paracoef_v_d_max: Parabola coefficient lower maximum gain value
 * @lssc_paracoef_v_d_min: Parabola coefficient lower minimum gain value
 * @lssc_paracoef_hv_lu_max: Parabola coefficient upper left gain maximum value
 * @lssc_paracoef_hv_lu_min: Parabola coefficient upper left gain minimum value
 * @lssc_paracoef_hv_ru_max: Parabola coefficient upper right gain maximum value
 * @lssc_paracoef_hv_ru_min: Parabola coefficient upper right minimum gain value
 * @lssc_paracoef_hv_ld_max: Parabola coefficient lower left gain maximum value
 * @lssc_paracoef_hv_ld_min: Parabola coefficient lower left gain minimum value
 * @lssc_paracoef_hv_rd_max: Parabola coefficient lower right gain maximum value
 * @lssc_paracoef_hv_rd_min: Parabola coefficient lower right minimum gain value
 *
 * The Range, Accuracy and Constraint of each coefficient are:
 *
 * - Range: [-4096..4095]
 * - Accuracy: accuracy: 1/256
 * - Constraint: lssc_paracoef_xx_xx_min <= lssc_paracoef_xx_xx_max
 */
struct viif_l1_lsc_parabola_ag_param {
	__s16 lssc_paracoef_h_l_max;
	__s16 lssc_paracoef_h_l_min;
	__s16 lssc_paracoef_h_r_max;
	__s16 lssc_paracoef_h_r_min;
	__s16 lssc_paracoef_v_u_max;
	__s16 lssc_paracoef_v_u_min;
	__s16 lssc_paracoef_v_d_max;
	__s16 lssc_paracoef_v_d_min;
	__s16 lssc_paracoef_hv_lu_max;
	__s16 lssc_paracoef_hv_lu_min;
	__s16 lssc_paracoef_hv_ru_max;
	__s16 lssc_paracoef_hv_ru_min;
	__s16 lssc_paracoef_hv_ld_max;
	__s16 lssc_paracoef_hv_ld_min;
	__s16 lssc_paracoef_hv_rd_max;
	__s16 lssc_paracoef_hv_rd_min;
};

/**
 * struct viif_l1_lsc_parabola_param - L2ISP parabola shading parameters
 * for &struct viif_l1_lsc
 * @lssc_para_h_center: Horizontal coordinate of central optical axis.
 *                      Range: [0..(Input image width - 1)].
 * @lssc_para_v_center: Vertical coordinate of central optical axis.
 *                      Range: [0..(Input image height - 1)].
 * @lssc_para_h_gain: Horizontal distance gain with the optical axis.
 *                    Range: [0..4095], Accuracy: 1/256.
 * @lssc_para_v_gain: Vertical distance gain with the optical axis.
 *                    Range: [0..4095], Accuracy: 1/256.
 * @lssc_para_mgsel2: &enum viif_l1_para_coef_gain value.
 *                    Parabola 2D correction coefficient gain magnification ratio.
 * @lssc_para_mgsel4: &enum viif_l1_para_coef_gain value.
 *                    Parabola 4D correction coefficient gain magnification ratio.
 * @r_2d: 2D parabola coefficient for R.
 *        Refer to &struct viif_l1_lsc_parabola_ag_param
 * @r_4d: 4D parabola coefficient for R.
 *        Refer to &struct viif_l1_lsc_parabola_ag_param
 * @gr_2d: 2D parabola coefficient for Gr
 *        Refer to &struct viif_l1_lsc_parabola_ag_param
 * @gr_4d: 4D parabola coefficient for Gr
 *        Refer to &struct viif_l1_lsc_parabola_ag_param
 * @gb_2d: 2D parabola coefficient for Gb
 *        Refer to &struct viif_l1_lsc_parabola_ag_param
 * @gb_4d: 4D parabola coefficient for Gb
 *        Refer to &struct viif_l1_lsc_parabola_ag_param
 * @b_2d: 2D parabola coefficient for B
 *        Refer to &struct viif_l1_lsc_parabola_ag_param
 * @b_4d: 4D parabola coefficient for B
 *        Refer to &struct viif_l1_lsc_parabola_ag_param
 */
struct viif_l1_lsc_parabola_param {
	__u32 lssc_para_h_center;
	__u32 lssc_para_v_center;
	__u32 lssc_para_h_gain;
	__u32 lssc_para_v_gain;
	__u32 lssc_para_mgsel2;
	__u32 lssc_para_mgsel4;
	struct viif_l1_lsc_parabola_ag_param r_2d;
	struct viif_l1_lsc_parabola_ag_param r_4d;
	struct viif_l1_lsc_parabola_ag_param gr_2d;
	struct viif_l1_lsc_parabola_ag_param gr_4d;
	struct viif_l1_lsc_parabola_ag_param gb_2d;
	struct viif_l1_lsc_parabola_ag_param gb_4d;
	struct viif_l1_lsc_parabola_ag_param b_2d;
	struct viif_l1_lsc_parabola_ag_param b_4d;
};

/**
 * struct viif_l1_lsc_grid_param - L2ISP grid shading parameters
 * for &struct viif_l1_lsc
 * @lssc_grid_h_size:  Grid horizontal direction pixel count.
 *                     Range: [32, 64, 128, 256, 512]
 * @lssc_grid_v_size:  Grid vertical direction pixel count.
 *                     Range: [32, 64, 128, 256, 512]
 * @lssc_grid_h_center: Horizontal coordinates of grid (1, 1)
 * @lssc_grid_v_center: Vertical coordinates of grid (1, 1)
 * @lssc_grid_mgsel: &enum viif_l1_grid_coef_gain value.
 *                   Grid correction coefficient gain value magnification ratio.
 *
 * Range and constraint of parameters are:
 *
 * - lssc_grid_h_center:
 *
 *    - Range: [1..lssc_grid_h_size]
 *    - Constraint: "Input image width <= lssc_grid_h_center + lssc_grid_h_size * 31"
 *
 * - lssc_grid_v_center:
 *
 *    - Range: [1..lssc_grid_v_size]
 *    - Constraint: "Input image height <= lssc_grid_v_center + lssc_grid_v_size * 23"
 */
struct viif_l1_lsc_grid_param {
	__u32 lssc_grid_h_size;
	__u32 lssc_grid_v_size;
	__u32 lssc_grid_h_center;
	__u32 lssc_grid_v_center;
	__u32 lssc_grid_mgsel;
};

/**
 * struct viif_l1_lsc - L2ISP LSC parameters for &struct viif_l1_lsc_config
 * @lssc_parabola_param: see &struct viif_l1_lsc_parabola_param
 * @lssc_grid_param: see &struct viif_l1_lsc_grid_param
 * @lssc_pwhb_r_gain_max: PWB R correction processing coefficient maximum value
 * @lssc_pwhb_r_gain_min: PWB R correction processing coefficient minimum value
 * @lssc_pwhb_gr_gain_max: PWB Gr correction processing coefficient maximum value
 * @lssc_pwhb_gr_gain_min: PWB Gr correction processing coefficient minimum value
 * @lssc_pwhb_gb_gain_max: PWB Gb correction processing coefficient maximum value
 * @lssc_pwhb_gb_gain_min: PWB Gb correction processing coefficient minimum value
 * @lssc_pwhb_b_gain_max: PWB B correction processing coefficient maximum value
 * @lssc_pwhb_b_gain_min: PWB B correction processing coefficient minimum value
 *
 * The range, accuracy and restriction of lssc_pwhb_{r,gr,gb,b}_gain_{max,min} are:
 *
 * - Range: [0..2047]
 * - Accuracy: 1/256
 * - Restriction: xxxx_gain_min <= xxxx_gain_max
 */
struct viif_l1_lsc {
	struct viif_l1_lsc_parabola_param lssc_parabola_param;
	struct viif_l1_lsc_grid_param lssc_grid_param;
	__u32 lssc_pwhb_r_gain_max;
	__u32 lssc_pwhb_r_gain_min;
	__u32 lssc_pwhb_gr_gain_max;
	__u32 lssc_pwhb_gr_gain_min;
	__u32 lssc_pwhb_gb_gain_max;
	__u32 lssc_pwhb_gb_gain_min;
	__u32 lssc_pwhb_b_gain_max;
	__u32 lssc_pwhb_b_gain_min;
};

/* MASKs for viif_l1_lsc_config::enable */
#define VIIF_L1_LSC_PARABOLA_EN_MASK BIT(0)
#define VIIF_L1_LSC_GRID_EN_MASK     BIT(1)

/**
 * struct viif_l1_lsc_config - L2ISP LSC parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_LSC`
 * @enable: set 0 to disable LSC operation,
 *          1 to enable parabola shading,
 *          2 to enable grid shading,
 *          3 to enable both parabola and grid shadings.
 * @param: see &struct viif_l1_lsc
 * @table_gr: Grid table for LSC of Gr component.
 *            This table is referred only when grid shading is used
 * @table_r:  Grid table for LSC of R component.
 *            This table is referred only when grid shading is used
 * @table_b:  Grid table for LSC of B component.
 *            This table is referred only when grid shading is used
 * @table_gb: Grid table for LSC of Gb component.
 *            This table is referred only when grid shading is used
 *
 * The size of each table is 1536 Bytes (u16 * 768).
 * Application should make sure that the table data is based on HW specification
 * since this driver does not check the grid table.
 */
struct viif_l1_lsc_config {
	__u32 enable;
	struct viif_l1_lsc param;
	__u16 table_gr[768];
	__u16 table_r[768];
	__u16 table_b[768];
	__u16 table_gb[768];
};

/**
 * enum viif_l1_demosaic_mode - L1ISP demosaic modeenum viif_l1_demosaic_mode
 *
 * @VIIF_L1_DEMOSAIC_ACPI: Toshiba ACPI algorithm
 * @VIIF_L1_DEMOSAIC_DMG: DMG algorithm
 */
enum viif_l1_demosaic_mode {
	VIIF_L1_DEMOSAIC_ACPI = 0,
	VIIF_L1_DEMOSAIC_DMG = 1,
};

/**
 * struct viif_l1_color_matrix_correction - L1ISP color matrix correction
 * parameters for &struct viif_l1_main_process_config
 * @coef_rmg_min: (R-G) Minimum coefficient
 * @coef_rmg_max: (R-G) Maximum coefficient
 * @coef_rmb_min: (R-B) Minimum coefficient
 * @coef_rmb_max: (R-B) Maximum coefficient
 * @coef_gmr_min: (G-R) Minimum coefficient
 * @coef_gmr_max: (G-R) Maximum coefficient
 * @coef_gmb_min: (G-B) Minimum coefficient
 * @coef_gmb_max: (G-B) Maximum coefficient
 * @coef_bmr_min: (B-R) Minimum coefficient
 * @coef_bmr_max: (B-R) Maximum coefficient
 * @coef_bmg_min: (B-G) Minimum coefficient
 * @coef_bmg_max: (B-G) Maximum coefficient
 * @dst_minval: Minimum value of output pixel. Range: [0..0xFFFF].
 *
 * The range, accuracy and restriction of each coefficient are:
 *
 * - Range: [-32768..32767]
 * - Accuracy: 1/4096
 * - Restriction: coef_xxx_min <= coef_xxx_max
 */
struct viif_l1_color_matrix_correction {
	__s16 coef_rmg_min;
	__s16 coef_rmg_max;
	__s16 coef_rmb_min;
	__s16 coef_rmb_max;
	__s16 coef_gmr_min;
	__s16 coef_gmr_max;
	__s16 coef_gmb_min;
	__s16 coef_gmb_max;
	__s16 coef_bmr_min;
	__s16 coef_bmr_max;
	__s16 coef_bmg_min;
	__s16 coef_bmg_max;
	__u16 dst_minval;
};

/**
 * struct viif_l1_main_process_config - L1ISP Main process operating parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_MAIN_PROCESS`
 * @demosaic_mode: &enum viif_l1_demosaic_mode value. Sets demosaic mode.
 * @damp_lsbsel: Clipping range of output pixel value to AWB adjustment function. Range: [0..15].
 * @colormat_enable: set 1 to enable color matrix correction, 0 to disable.
 * @dst_maxval: Maximum value of output pixel. Range: [0..0xFFFFFF].
 *              Applicable to output of each process (digital amplifier,
 *              demosaicing and color matrix correction) in L1ISP Main process.
 * @colormat_param: see &struct viif_l1_color_matrix_correction
 */
struct viif_l1_main_process_config {
	__u32 demosaic_mode;
	__u32 damp_lsbsel;
	__u32 colormat_enable;
	__u32 dst_maxval;
	struct viif_l1_color_matrix_correction colormat_param;
};

/**
 * enum viif_l1_awb_mag - L1ISP signal magnification before AWB adjustment
 *
 * @VIIF_L1_AWB_ONE_SECOND: x 1/2
 * @VIIF_L1_AWB_X1: 1 times
 * @VIIF_L1_AWB_X2: 2 times
 * @VIIF_L1_AWB_X4: 4 times
 */
enum viif_l1_awb_mag {
	VIIF_L1_AWB_ONE_SECOND = 0,
	VIIF_L1_AWB_X1 = 1,
	VIIF_L1_AWB_X2 = 2,
	VIIF_L1_AWB_X4 = 3,
};

/**
 * enum viif_l1_awb_area_mode - L1ISP AWB detection target area
 *
 * @VIIF_L1_AWB_AREA_MODE0: only center area
 * @VIIF_L1_AWB_AREA_MODE1: center area when uv is in square gate
 * @VIIF_L1_AWB_AREA_MODE2: all area except center area
 * @VIIF_L1_AWB_AREA_MODE3: all area
 */
enum viif_l1_awb_area_mode {
	VIIF_L1_AWB_AREA_MODE0 = 0,
	VIIF_L1_AWB_AREA_MODE1 = 1,
	VIIF_L1_AWB_AREA_MODE2 = 2,
	VIIF_L1_AWB_AREA_MODE3 = 3,
};

/**
 * enum viif_l1_awb_restart_cond - L1ISP AWB adjustment restart conditions
 *
 * @VIIF_L1_AWB_RESTART_NO: no restart
 * @VIIF_L1_AWB_RESTART_128FRAME: restart after 128 frame
 * @VIIF_L1_AWB_RESTART_64FRAME: restart after 64 frame
 * @VIIF_L1_AWB_RESTART_32FRAME: restart after 32 frame
 * @VIIF_L1_AWB_RESTART_16FRAME: restart after 16 frame
 * @VIIF_L1_AWB_RESTART_8FRAME: restart after 8 frame
 * @VIIF_L1_AWB_RESTART_4FRAME: restart after 4 frame
 * @VIIF_L1_AWB_RESTART_2FRAME: restart after 2 frame
 */
enum viif_l1_awb_restart_cond {
	VIIF_L1_AWB_RESTART_NO = 0,
	VIIF_L1_AWB_RESTART_128FRAME = 1,
	VIIF_L1_AWB_RESTART_64FRAME = 2,
	VIIF_L1_AWB_RESTART_32FRAME = 3,
	VIIF_L1_AWB_RESTART_16FRAME = 4,
	VIIF_L1_AWB_RESTART_8FRAME = 5,
	VIIF_L1_AWB_RESTART_4FRAME = 6,
	VIIF_L1_AWB_RESTART_2FRAME = 7,
};

/**
 * struct viif_l1_awb - L1ISP AWB adjustment parameters
 * for &struct viif_l1_awb_config
 * @awhb_ygate_sel: 1:Enable/0:Disable to fix Y value at YUV conversion
 * @awhb_ygate_data: Y value when Y value is fixed. Range: [64, 128, 256, 512].
 * @awhb_cgrange: &enum viif_l1_awb_mag value.
 *                Signal output magnification ratio before AWB adjustment.
 * @awhb_ygatesw: 1:Enable/0:Disable settings of luminance gate
 * @awhb_hexsw: 1:Enable/0:Disable settings of hexa-gate
 * @awhb_areamode: &enum viif_l1_awb_area_mode value.
 *                 Final selection of accumulation area for detection target area.
 * @awhb_area_hsize: Horizontal size per block in central area.
 *                   Range: [1..(Input image width -8)/8].
 * @awhb_area_vsize: Vertical size per block in central area.
 *                   Range: [1..(Input image height -4)/8].
 * @awhb_area_hofs: Horizontal offset of block [0] in central area.
 *                  Range: [0..(Input image width -9)].
 * @awhb_area_vofs: Vertical offset of block [0] in central area.
 *                  Range: [0..(Input image height -5)].
 * @awhb_area_maskh: Setting 1:Enable/0:Disable of accumulated selection.
 *                   Each bit corresponds to the following:
 *                   [31:0] = {
 *                   (7, 3),(6, 3),(5, 3),(4, 3),(3, 3),(2, 3),(1, 3),(0, 3),
 *                   (7, 2),(6, 2),(5, 2),(4, 2),(3, 2),(2, 2),(1, 2),(0, 2),
 *                   (7, 1),(6, 1),(5, 1),(4, 1),(3, 1),(2, 1),(1, 1),(0, 1),
 *                   (7, 0),(6, 0),(5, 0),(4, 0),(3, 0),(2, 0),(1, 0),(0, 0)}
 * @awhb_area_maskl: Setting 1:Enable/0:Disable of accumulated selection.
 *                   Each bit corresponds to the following:
 *                   [31:0] = {
 *                   (7, 7),(6, 7),(5, 7),(4, 7),(3, 7),(2, 7),(1, 7),(0, 7),
 *                   (7, 6),(6, 6),(5, 6),(4, 6),(3, 6),(2, 6),(1, 6),(0, 6),
 *                   (7, 5),(6, 5),(5, 5),(4, 5),(3, 5),(2, 5),(1, 5),(0, 5),
 *                   (7, 4),(6, 4),(5, 4),(4, 4),(3, 4),(2, 4),(1, 4),(0, 4)}
 * @awhb_sq_sw: 1:Enable/0:Disable each square gate
 * @awhb_sq_pol: 1:Enable/0:Disable to add accumulated gate for each square gate
 * @awhb_bycut0p: U upper end value. Range: [0..127].
 * @awhb_bycut0n: U lower end value. Range: [0..127].
 * @awhb_rycut0p: V upper end value. Range: [0..127].
 * @awhb_rycut0n: V lower end value. Range: [0..127].
 * @awhb_rbcut0h: V-axis intercept upper end. Range: [-127..127].
 * @awhb_rbcut0l: V-axis intercept lower end. Range: [-127..127].
 * @awhb_bycut_h: U direction center value of each square gate. Range:  [-127..127].
 * @awhb_bycut_l: U direction width of each square gate. Range: [0..127].
 * @awhb_rycut_h: V direction center value of each square gate. Range: [-127..127].
 * @awhb_rycut_l: V direction width of each square gate. Range: [0..127].
 * @awhb_awbsftu: U gain offset. Range: [-127..127].
 * @awhb_awbsftv: V gain offset. Range: [-127..127].
 * @awhb_awbhuecor: 1:Enable/0:Disable setting of color correlation retention function
 * @awhb_awbspd: UV convergence speed multiplier. Range: [0..15] (0 means "stop").
 * @awhb_awbulv: U convergence point level. Range: [0..31].
 * @awhb_awbvlv: V convergence point level. Range: [0..31].
 * @awhb_awbondot: Accumulation operation stop pixel count threshold. Range: [0..1023].
 * @awhb_awbfztim: &enum viif_l1_awb_restart_cond value. Condition to restart AWB process.
 * @awhb_wbgrmax: B gain adjustment range (Width from center to upper limit).
 *                Range: [0..255], Accuracy: 1/64.
 * @awhb_wbgbmax: R gain adjustment range (Width from center to upper limit).
 *                Range: [0..255], Accuracy: 1/64.
 * @awhb_wbgrmin: B gain adjustment range (Width from center to lower limit).
 *                Range: [0..255], Accuracy: 1/64.
 * @awhb_wbgbmin: R gain adjustment range (Width from center to lower limit).
 *                Range: [0..255], Accuracy: 1/64.
 * @awhb_ygateh: Luminance gate maximum value. Range: [0..255].
 * @awhb_ygatel: Luminance gate minimum value. Range: [0..255].
 * @awhb_awbwait: Number of restart frames after UV convergence freeze. Range: [0..255].
 */
struct viif_l1_awb {
	__u32 awhb_ygate_sel;
	__u32 awhb_ygate_data;
	__u32 awhb_cgrange;
	__u32 awhb_ygatesw;
	__u32 awhb_hexsw;
	__u32 awhb_areamode;
	__u32 awhb_area_hsize;
	__u32 awhb_area_vsize;
	__u32 awhb_area_hofs;
	__u32 awhb_area_vofs;
	__u32 awhb_area_maskh;
	__u32 awhb_area_maskl;
	__u32 awhb_sq_sw[3];
	__u32 awhb_sq_pol[3];
	__u32 awhb_bycut0p;
	__u32 awhb_bycut0n;
	__u32 awhb_rycut0p;
	__u32 awhb_rycut0n;
	__s32 awhb_rbcut0h;
	__s32 awhb_rbcut0l;
	__s32 awhb_bycut_h[3];
	__u32 awhb_bycut_l[3];
	__s32 awhb_rycut_h[3];
	__u32 awhb_rycut_l[3];
	__s32 awhb_awbsftu;
	__s32 awhb_awbsftv;
	__u32 awhb_awbhuecor;
	__u32 awhb_awbspd;
	__u32 awhb_awbulv;
	__u32 awhb_awbvlv;
	__u32 awhb_awbondot;
	__u32 awhb_awbfztim;
	__u8 awhb_wbgrmax;
	__u8 awhb_wbgbmax;
	__u8 awhb_wbgrmin;
	__u8 awhb_wbgbmin;
	__u8 awhb_ygateh;
	__u8 awhb_ygatel;
	__u8 awhb_awbwait;
};

/**
 * struct viif_l1_awb_config - L1ISP AWB parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AWB`
 * @enable: set 1 to enable AWB , 0 to disable
 * @awhb_wbmrg: White balance adjustment R gain. Range: [64..1023], Accuracy: 1/256.
 * @awhb_wbmgg: White balance adjustment G gain. Range: [64..1023], Accuracy: 1/256.
 * @awhb_wbmbg: White balance adjustment B gain. Range: [64..1023], Accuracy: 1/256.
 * @param: a &struct viif_l1_awb instance
 */
struct viif_l1_awb_config {
	__u32 enable;
	__u32 awhb_wbmrg;
	__u32 awhb_wbmgg;
	__u32 awhb_wbmbg;
	struct viif_l1_awb param;
};

/**
 * enum viif_l1_hdrc_tone_type - L1ISP HDRC tone type
 *
 * @VIIF_L1_HDRC_TONE_USER: User Tone
 * @VIIF_L1_HDRC_TONE_PRESET: Preset Tone
 */
enum viif_l1_hdrc_tone_type {
	VIIF_L1_HDRC_TONE_USER = 0,
	VIIF_L1_HDRC_TONE_PRESET = 1,
};

/**
 * struct viif_l1_hdrc - L1ISP HDRC parameters for &struct viif_l1_hdrc_config
 * @hdrc_ratio: Data width of input image. Range: [10..24] bits.
 * @hdrc_pt_ratio: Preset Tone curve slope. Range: [0..13].
 * @hdrc_pt_blend: Preset Tone0 curve blend ratio. Range: [0..256], Accuracy: 1/256.
 * @hdrc_pt_blend2: Preset Tone2 curve blend ratio. Range: [0..256], Accuracy: 1/256.
 * @hdrc_tn_type: &enum viif_l1_hdrc_tone_type value. L1ISP HDRC tone type.
 * @hdrc_utn_tbl: HDRC value of User Tone curve. Range: [0..0xFFFF].
 * @hdrc_flr_val: Constant flare value. Range: [0..0xFFFFFF].
 * @hdrc_flr_adp: set 1 to enable dynamic flare measurement, 0 to disable.
 * @hdrc_ybr_off: set 1 to turn OFF bilateral luminance filter, 0 to turn ON.
 * @hdrc_orgy_blend: Blend settings of luminance correction data after HDRC
 *                   and data before luminance correction. Range: [0..16]
 *                   (0:Luminance correction 100%, 8:Luminance correction 50%,
 *                   16:Luminance correction 0%).
 * @hdrc_pt_sat: Preset Tone saturation value. Range: [0..0xFFFF].
 *
 * Restrictions for parameters
 *
 * - hdrc_pt_blend + hdrc_pt_blend2 <= 256
 * - input_image_height % 64 != {18, 20, 22, 24, 26}
 *
 *   - only when dynamic flare control is enabled
 *   - note that the driver will not return error if this condition is not satisfied.
 *
 * - hdrc_utn_tbl[N] <= hdrc_utn_tbl[N+1]
 *
 *   - note that the driver will not return error if this condition is not satisfied.
 */
struct viif_l1_hdrc {
	__u32 hdrc_ratio;
	__u32 hdrc_pt_ratio;
	__u32 hdrc_pt_blend;
	__u32 hdrc_pt_blend2;
	__u32 hdrc_tn_type;
	__u16 hdrc_utn_tbl[20];
	__u32 hdrc_flr_val;
	__u32 hdrc_flr_adp;
	__u32 hdrc_ybr_off;
	__u32 hdrc_orgy_blend;
	__u16 hdrc_pt_sat;
};

/**
 * struct viif_l1_hdrc_config - L1ISP HDRC parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC`
 * @enable: set 1 to enable HDR compression, 0 to disable
 * @hdrc_thr_sft_amt: Amount of right shift in through mode (HDRC disabled). Range: [0..8].
 *                    Should be 0 if HDRC is enabled
 * @param: HDR compression parameter; see &struct viif_l1_hdrc
 */
struct viif_l1_hdrc_config {
	__u32 enable;
	__u32 hdrc_thr_sft_amt;
	struct viif_l1_hdrc param;
};

/**
 * struct viif_l1_hdrc_ltm_config - L1ISP HDRC LTM parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_HDRC_LTM`
 * @tnp_max: Tone blend rate maximum value of LTM function.
 *           Range: [0..4194303], Accuracy: 1/64. Set 0 to turn off LTM function.
 * @tnp_mag: Intensity adjustment of LTM function. Range: [0..16383], Accuracy: 1/64.
 * @tnp_fil: Smoothing filter coefficient. Range: [0..255].
 *
 * Restriction: (tmp_fil[1] + tnp_fil[2] + tnp_fil[3] + tnp_fil[4]) * 2 + tnp_fil[0] = 1024
 */
struct viif_l1_hdrc_ltm_config {
	__u32 tnp_max;
	__u32 tnp_mag;
	__u8 tnp_fil[5];
};

/**
 * struct viif_l1_gamma - L1ISP gamma correction parameters
 * for &struct viif_l1_gamma_config
 * @gam_p: Luminance value after gamma correction. Range: [0..8191].
 * @blkadj: Black level adjustment value after gamma correction. Range: [0..65535].
 */
struct viif_l1_gamma {
	__u16 gam_p[44];
	__u16 blkadj;
};

/**
 * struct viif_l1_gamma_config - L1ISP gamma correction parameters
 * @enable: set 1 to enable gamma correction at L1ISP, 0 to disable.
 * @param: see &struct viif_l1_gamma.
 */
struct viif_l1_gamma_config {
	__u32 enable;
	struct viif_l1_gamma param;
};

/**
 * struct viif_l1_nonlinear_contrast -  L1ISP non-linear contrast parameters
 * for &struct viif_l1_img_quality_adjustment_config
 * @blk_knee: Black side peak luminance value. Range: [0..0xFFFF].
 * @wht_knee: White side peak luminance value. Range: [0..0xFFFF].
 * @blk_cont: Black side slope
 * @wht_cont: White side slope
 *
 * Range, Accuracy and Index for {blk,wht}_cont is:
 *
 * - Range: [0..255]
 * - Accuracy: 1/256
 *
 * - Index
 *
 *   - 0: the value at AG minimum
 *   - 1: the value at AG less than 128
 *   - 2: the value at AG equal to or more than 128
 */
struct viif_l1_nonlinear_contrast {
	__u16 blk_knee;
	__u16 wht_knee;
	__u8 blk_cont[3];
	__u8 wht_cont[3];
};

/**
 * struct viif_l1_lum_noise_reduction -  L1ISP luminance noise reduction
 * parameters for &struct viif_l1_img_quality_adjustment_config
 * @gain_min: Minimum value of extracted noise gain. Range: [0..0xFFFF], Accuracy: 1/256
 * @gain_max: Maximum value of extracted noise gain. Range: [0..0xFFFF], Accuracy: 1/256
 * @lim_min: Minimum value of extracted noise limit. Range: [0..0xFFFF]
 * @lim_max: Maximum value of extracted noise limit. Range: [0..0xFFFF]
 *
 * Constraint: "gain_min <= gain_max" and "lim_min <= lim_max"
 *
 */
struct viif_l1_lum_noise_reduction {
	__u16 gain_min;
	__u16 gain_max;
	__u16 lim_min;
	__u16 lim_max;
};

/**
 * struct viif_l1_edge_enhancement -  L1ISP edge enhancement parameters
 * for &struct viif_l1_img_quality_adjustment_config
 * @gain_min: Extracted edge gain minimum value. Range: [0..0xFFFF], Accuracy: 1/256
 * @gain_max: Extracted edge gain maximum value. Range: [0..0xFFFF], Accuracy: 1/256
 * @lim_min: Extracted edge limit minimum value. Range: [0..0xFFFF]
 * @lim_max: Extracted edge limit maximum value. Range: [0..0xFFFF]
 * @coring_min: Extracted edge coring threshold minimum value. Range: [0..0xFFFF]
 * @coring_max: Extracted edge coring threshold maximum value. Range: [0..0xFFFF]
 *
 * Constraint: "gain_min <= gain_max" and "lim_min <= lim_max" and "coring_min <= coring_max"
 */
struct viif_l1_edge_enhancement {
	__u16 gain_min;
	__u16 gain_max;
	__u16 lim_min;
	__u16 lim_max;
	__u16 coring_min;
	__u16 coring_max;
};

/**
 * struct viif_l1_uv_suppression -  L1ISP UV suppression parameters
 * for &struct viif_l1_img_quality_adjustment_config
 * @bk_mp: Black side slope. Range: [0..0x3FFF], Accuracy: 1/16384
 * @black: Minimum black side gain. Range: [0..0x3FFF], Accuracy: 1/16384
 * @wh_mp: White side slope. Range: [0..0x3FFF], Accuracy: 1/16384
 * @white: Minimum white side gain. Range: [0..0x3FFF], Accuracy: 1/16384
 * @bk_slv: Black side intercept. Range: [0..0xFFFF]
 * @wh_slv: White side intercept. Range: [0..0xFFFF]
 *
 * Constraint: bk_slb < wh_slv
 */
struct viif_l1_uv_suppression {
	__u32 bk_mp;
	__u32 black;
	__u32 wh_mp;
	__u32 white;
	__u16 bk_slv;
	__u16 wh_slv;
};

/**
 * struct viif_l1_coring_suppression -  L1ISP coring suppression parameters
 * for &struct viif_l1_img_quality_adjustment_config
 * @lv_min: Minimum coring threshold. Range: [0..0xFFFF]
 * @lv_max: Maximum coring threshold. Range: [0..0xFFFF]
 * @gain_min: Minimum gain. Range: [0..0xFFFF], Accuracy: 1/65536
 * @gain_max: Maximum gain. Range: [0..0xFFFF], Accuracy: 1/65536
 *
 * Constraint: "lv_min <= lv_max" and "gain_min <= gain_max"
 */
struct viif_l1_coring_suppression {
	__u16 lv_min;
	__u16 lv_max;
	__u16 gain_min;
	__u16 gain_max;
};

/**
 * struct viif_l1_edge_suppression -  L1ISP edge suppression parameters
 * for &struct viif_l1_img_quality_adjustment_config
 * @gain: Gain of edge color suppression. Range: [0..0xFFFF], Accuracy: 1/256
 * @lim: Limiter threshold of edge color suppression. Range: [0..15]
 */
struct viif_l1_edge_suppression {
	__u16 gain;
	__u32 lim;
};

/**
 * struct viif_l1_color_level -  L1ISP color level parameters
 * for &struct viif_l1_img_quality_adjustment_config
 * @cb_gain: U component gain
 * @cr_gain: V component gain
 * @cbr_mgain_min: UV component gain
 * @cbp_gain_max: Positive U component gain
 * @cbm_gain_max: Negative V component gain
 * @crp_gain_max: Positive U component gain
 * @crm_gain_max: Negative V component gain
 *
 * Range and Accuracy of parameters are:
 *
 * - Range: [0..0xFFF]
 * - Accuracy: 1/2048
 */
struct viif_l1_color_level {
	__u32 cb_gain;
	__u32 cr_gain;
	__u32 cbr_mgain_min;
	__u32 cbp_gain_max;
	__u32 cbm_gain_max;
	__u32 crp_gain_max;
	__u32 crm_gain_max;
};

/* MASKs for viif_l1_img_quality_adjustment_config::enable */
#define VIIF_L1_IQA_NONLINEAR_CONTRAST_EN_MASK	  BIT(0)
#define VIIF_L1_IQA_LUM_NOISE_REDUCTION_EN_MASK	  BIT(1)
#define VIIF_L1_IQA_EDGE_ENHANCEMENT_EN_MASK	  BIT(2)
#define VIIF_L1_IQA_UV_SUPPRESSION_EN_MASK	  BIT(3)
#define VIIF_L1_IQA_CORING_SUPPRESSION_EN_MASK	  BIT(4)
#define VIIF_L1_IQA_EDGE_SUPPRESSION_EN_MASK	  BIT(5)
#define VIIF_L1_IQA_COLOR_LEVEL_EN_MASK		  BIT(6)
#define VIIF_L1_IQA_COLOR_NOISE_REDUCTION_EN_MASK BIT(7)

/**
 * struct viif_l1_img_quality_adjustment_config -  L1ISP image quality
 * adjustment parameters for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_IMG_QUALITY_ADJUSTMENT`
 * @enable: | bit vector; set 1 to enable each function, 0 to disable.
 *          | bit0: nonlinear_contrast
 *          | bit1: lum_noise_reduction
 *          | bit2: edge_enhancement
 *          | bit3: uv_suppression
 *          | bit4: coring_suppression
 *          | bit5: edge_suppression
 *          | bit6: color_level
 *          | bit7: color_noise_reduction
 * @coef_cb: Cb coefficient used in RGB to YUV conversion.
 *           Range: [0..0xFFFF], Accuracy: 1/65536
 * @coef_cr: Cr coefficient used in RGB to YUV conversion.
 *           Range: [0..0xFFFF], Accuracy: 1/65536
 * @brightness: Brightness adjustment value. Range: [-32768..32767] (0 to turn off)
 * @linear_contrast: Linear contrast adjustment value.
 *                   Range: [0..0xFF], Accuracy: 1/128 (128 to turn off)
 * @nonlinear_contrast: see &struct viif_l1_nonlinear_contrast; controlled by bit0 of enable.
 * @lum_noise_reduction: see &struct viif_l1_lum_noise_reduction; controlled by bit1 of enable.
 * @edge_enhancement: see &struct viif_l1_edge_enhancement; controlled by bit2 of enable.
 * @uv_suppression: see &struct viif_l1_uv_suppression: controlled by bit3 of enable.
 * @coring_suppression: see &struct viif_l1_coring_suppression; controlled by bit4 of enable.
 * @edge_suppression: see &struct viif_l1_edge_suppression; controlled by bit5 of enable.
 * @color_level: see &struct viif_l1_color_level; controlled by bit6 of enable.
 */
struct viif_l1_img_quality_adjustment_config {
	__u32 enable;
	__u16 coef_cb;
	__u16 coef_cr;
	__s16 brightness;
	__u8 linear_contrast;
	struct viif_l1_nonlinear_contrast nonlinear_contrast;
	struct viif_l1_lum_noise_reduction lum_noise_reduction;
	struct viif_l1_edge_enhancement edge_enhancement;
	struct viif_l1_uv_suppression uv_suppression;
	struct viif_l1_coring_suppression coring_suppression;
	struct viif_l1_edge_suppression edge_suppression;
	struct viif_l1_color_level color_level;
};

/**
 * struct viif_l1_avg_lum_generation_config - L1ISP average luminance generation configuration
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L1_SET_AVG_LUM_GENERATION`
 * @enable: set 1 to enable aggregation of AVG LUM, 0 to disable
 * @aexp_start_x: horizontal position of block 0. Range: [0.."width of input image - 1"]
 * @aexp_start_y: vertical position of block 0. Range: [0.."height of input image - 1"]
 * @aexp_block_width: width of one block.
 *                    Range: [64.."width of input image"] (Should be multiple of 64)
 * @aexp_block_height: height of one block.
 *                     Range: [64.."height of input image"] (Should be multiple of 64)
 * @aexp_weight: weight of each block. Range: [0..3].
 *               Nested indices are: [y position][x position].
 * @aexp_satur_ratio: threshold to judge whether saturated block or not. Range: [0..256]
 * @aexp_black_ratio: threshold to judge whether black block or not. Range: [0..256]
 * @aexp_satur_level: threshold to judge whether saturated pixel or not. Range: [0x0..0xffffff]
 * @aexp_ave4linesy: vertical position of the initial line
 *                   for 4-lines average luminance.
 *                   Range: [0.."height of input image - 4"]
 */
struct viif_l1_avg_lum_generation_config {
	__u32 enable;
	__u32 aexp_start_x;
	__u32 aexp_start_y;
	__u32 aexp_block_width;
	__u32 aexp_block_height;
	__u32 aexp_weight[8][8];
	__u32 aexp_satur_ratio;
	__u32 aexp_black_ratio;
	__u32 aexp_satur_level;
	__u32 aexp_ave4linesy[4];
};

/**
 * enum viif_l2_undist_mode - L2ISP undistortion mode
 * @VIIF_L2_UNDIST_POLY: polynomial mode
 * @VIIF_L2_UNDIST_GRID: grid table mode
 * @VIIF_L2_UNDIST_POLY_TO_GRID: polynomial, then grid table mode
 * @VIIF_L2_UNDIST_GRID_TO_POLY: grid table, then polynomial mode
 */
enum viif_l2_undist_mode {
	VIIF_L2_UNDIST_POLY = 0,
	VIIF_L2_UNDIST_GRID = 1,
	VIIF_L2_UNDIST_POLY_TO_GRID = 2,
	VIIF_L2_UNDIST_GRID_TO_POLY = 3,
};

/**
 * struct viif_l2_undist - L2ISP UNDIST parameters
 * for &struct viif_l2_undist_config
 * @through_mode: 1:enable or 0:disable through mode of undistortion
 * @roi_mode: &enum viif_l2_undist_mode value. Sets L2ISP undistortion mode.
 * @sensor_crop_ofs_h: Horizontal start position of sensor crop area.
 * @sensor_crop_ofs_v: Vertical start position of sensor crop area.
 * @norm_scale: Normalization coefficient for distance from center
 * @valid_r_norm2_poly: Setting target area for polynomial correction
 * @valid_r_norm2_grid: Setting target area for grid table correction
 * @roi_write_area_delta: Error adjustment value of forward function and
 *                        inverse function for pixel position calculation
 * @poly_write_g_coef: 10th-order polynomial coefficient for G write pixel position calculation
 * @poly_read_b_coef: 10th-order polynomial coefficient for B read pixel position calculation
 * @poly_read_g_coef: 10th-order polynomial coefficient for G read pixel position calculation
 * @poly_read_r_coef: 10th-order polynomial coefficient for R read pixel position calculation
 * @grid_node_num_h: Number of horizontal grids
 * @grid_node_num_v: Number of vertical grids
 * @grid_patch_hsize_inv: Inverse pixel size between horizontal grids
 * @grid_patch_vsize_inv: Inverse pixel size between vertical grids
 *
 * Range and Accuracy of parameters are:
 *
 * - sensor_crop_ofs_{h,v}
 *
 *   - Range: [-4296..4296]
 *   - Accuracy: 1/2
 *
 * - norm_scale
 *
 *   - Range: [0..1677721]
 *   - Accuracy: 1/33554432
 *
 * - valid_r_norm2_{poly,grid}
 *
 *   - Range: [0..0x3FFFFFF]
 *   - Accuracy: 1/33554432
 *
 * - roi_write_area_delta
 *
 *   - Range: [0..0x7FF]
 *   - Accuracy: 1/1024
 *
 * - poly_write_g_coef, poly_read_{b,g,r}_coef
 *
 *   - Range: [-2147352576..2147352576]
 *   - Accuracy: 1/131072
 *
 * - grid_node_num_{v.h}
 *
 *   - Range: [16..64]
 *
 * - grid_patch_{hsize,vsize}_inv
 *
 *   - Range: [0..0x7FFFFF]
 *   - Accuracy: 1/8388608
 */
struct viif_l2_undist {
	__u32 through_mode;
	__u32 roi_mode[2];
	__s32 sensor_crop_ofs_h;
	__s32 sensor_crop_ofs_v;
	__u32 norm_scale;
	__u32 valid_r_norm2_poly;
	__u32 valid_r_norm2_grid;
	__u32 roi_write_area_delta[2];
	__s32 poly_write_g_coef[11];
	__s32 poly_read_b_coef[11];
	__s32 poly_read_g_coef[11];
	__s32 poly_read_r_coef[11];
	__u32 grid_node_num_h;
	__u32 grid_node_num_v;
	__u32 grid_patch_hsize_inv;
	__u32 grid_patch_vsize_inv;
};

/**
 * struct viif_l2_undist_config - L2ISP UNDIST parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_UNDIST`
 * @param: &struct viif_l2_undist
 * @write_g: write-G grid table.
 * @read_b: read-B grid table.
 * @read_g: read-G grid table.
 * @read_r: read-R grid table.
 * @size: Table size in bytes. Range: [1024..8192] or 0.
 *        The value should be "grid_node_num_h * grid_node_num_v * 4".
 *        See also &struct viif_l2_undist.
 *
 * The tables are referred when param.roi_mode[] is
 * either of VIIF_L2_UNDIST_GRID, VIIF_L2_UNDIST_POLY_TO_GRID, VIIF_L2_UNDIST_GRID_TO_POLY
 * Application should make sure that the table data is based on HW specification
 * since this driver does not check the contents of specified grid table.
 */
struct viif_l2_undist_config {
	struct viif_l2_undist param;
	__u8 write_g[8192];
	__u8 read_b[8192];
	__u8 read_g[8192];
	__u8 read_r[8192];
	__u32 size;
};

/**
 * struct viif_l2_roi_config - L2ISP ROI parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_ROI`
 * @roi_num:
 *     1 when only capture path0 is activated,
 *     2 when both capture path 0 and path 1 are activated.
 * @roi_scale: Scale value for each ROI. Range: [32768..131072], Accuracy: 1/65536
 * @roi_scale_inv: Inverse scale value for each ROI. Range: [32768..131072], Accuracy: 1/65536
 * @corrected_wo_scale_hsize: Corrected image width for each ROI. Range: [128..8190]
 * @corrected_wo_scale_vsize: Corrected image height for each ROI. Range: [128..4094]
 * @corrected_hsize: Corrected and scaled image width for each ROI. Range: [128..8190]
 * @corrected_vsize: Corrected and scaled image height for each ROI. Range: [128..4094]
 */
struct viif_l2_roi_config {
	__u32 roi_num;
	__u32 roi_scale[2];
	__u32 roi_scale_inv[2];
	__u32 corrected_wo_scale_hsize[2];
	__u32 corrected_wo_scale_vsize[2];
	__u32 corrected_hsize[2];
	__u32 corrected_vsize[2];
};

/** enum viif_gamma_mode - Gamma correction mode
 *
 * @VIIF_GAMMA_COMPRESSED: compressed table mode
 * @VIIF_GAMMA_LINEAR: linear table mode
 */
enum viif_gamma_mode {
	VIIF_GAMMA_COMPRESSED = 0,
	VIIF_GAMMA_LINEAR = 1,
};

/**
 * struct viif_l2_gamma_config - L2ISP gamma correction parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_ISP_L2_SET_GAMMA`
 * @pathid: 0 for Capture Path 0, 1 for Capture Path 1.
 * @table_en: 6bit vector to enable gamma table; all 0 to disable gamma correction
 * @vsplit: Line switching position of first table and second table. Range: [0..4094].
 *          Should set 0 in case 0 is set to @enable
 * @mode: &enum viif_gamma_mode value.
 *        Should set VIIF_GAMMA_COMPRESSED when 0 is set to @enable
 * @table: gamma table for L2ISP gamma; 6 channels, each has __u16 typed 512 bytes.
 *         [0]: G/Y(1st table), [1]: G/Y(2nd table), [2]: B/U(1st table)
 *         [3]: B/U(2nd table), [4]: R/V(1st table), [5]: R/V(2nd table)
 */
struct viif_l2_gamma_config {
	__u32 pathid;
	__u32 table_en;
	__u32 vsplit;
	__u32 mode;
	__u16 table[6][256];
};

/**
 * struct viif_csi2rx_dphy_calibration_status - CSI2-RX D-PHY Calibration
 * information for :ref:`V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_CALIBRATION_STATUS`
 * @term_cal_with_rext: Result of termination calibration with rext
 * @clock_lane_offset_cal: Result of offset calibration of clock lane
 * @data_lane0_offset_cal: Result of offset calibration of data lane0
 * @data_lane1_offset_cal: Result of offset calibration of data lane1
 * @data_lane2_offset_cal: Result of offset calibration of data lane2
 * @data_lane3_offset_cal: Result of offset calibration of data lane3
 * @data_lane0_ddl_tuning_cal: Result of digital delay line tuning calibration of data lane0
 * @data_lane1_ddl_tuning_cal: Result of digital delay line tuning calibration of data lane1
 * @data_lane2_ddl_tuning_cal: Result of digital delay line tuning calibration of data lane2
 * @data_lane3_ddl_tuning_cal: Result of digital delay line tuning calibration of data lane3
 *
 * Possible returned values for each member are:
 *
 * - -EAGAIN: calibration is not done
 * - -EIO: calibration was failed
 * - 0; calibration is succeeded
 */
struct viif_csi2rx_dphy_calibration_status {
	__s32 term_cal_with_rext;
	__s32 clock_lane_offset_cal;
	__s32 data_lane0_offset_cal;
	__s32 data_lane1_offset_cal;
	__s32 data_lane2_offset_cal;
	__s32 data_lane3_offset_cal;
	__s32 data_lane0_ddl_tuning_cal;
	__s32 data_lane1_ddl_tuning_cal;
	__s32 data_lane2_ddl_tuning_cal;
	__s32 data_lane3_ddl_tuning_cal;
};

/**
 * struct viif_csi2rx_err_status - CSI2RX Error status parameters
 * for :ref:`V4L2_CID_VISCONTI_VIIF_CSI2RX_GET_ERR_STATUS`
 * @err_phy_fatal: D-PHY FATAL error.
 *
 *  - bit[3]: Start of transmission error on DATA Lane3.
 *  - bit[2]: Start of transmission error on DATA Lane2.
 *  - bit[1]: Start of transmission error on DATA Lane1.
 *  - bit[0]: Start of transmission error on DATA Lane0.
 * @err_pkt_fatal: Packet FATAL error.
 *
 *  - bit[16]: Header ECC contains 2 errors, unrecoverable.
 *  - bit[3]: Checksum error detected on virtual channel 3.
 *  - bit[2]: Checksum error detected on virtual channel 2.
 *  - bit[1]: Checksum error detected on virtual channel 1.
 *  - bit[0]: Checksum error detected on virtual channel 0.
 * @err_frame_fatal: Frame FATAL error.
 *
 *  - bit[19]: Last received Frame, in virtual channel 3, has at least one CRC error.
 *  - bit[18]: Last received Frame, in virtual channel 2, has at least one CRC error.
 *  - bit[17]: Last received Frame, in virtual channel 1, has at least one CRC error.
 *  - bit[16]: Last received Frame, in virtual channel 0, has at least one CRC error.
 *  - bit[11]: Incorrect Frame Sequence detected in virtual channel 3.
 *  - bit[10]: Incorrect Frame Sequence detected in virtual channel 2.
 *  - bit[9]: Incorrect Frame Sequence detected in virtual channel 1.
 *  - bit[8]: Incorrect Frame Sequence detected in virtual channel 0.
 *  - bit[3]: Error matching Frame Start with Frame End for virtual channel 3.
 *  - bit[2]: Error matching Frame Start with Frame End for virtual channel 2.
 *  - bit[1]: Error matching Frame Start with Frame End for virtual channel 1.
 *  - bit[0]: Error matching Frame Start with Frame End for virtual channel 0.
 * @err_phy: D-PHY error.
 *
 *  - bit[19]: Escape Entry Error on Data Lane 3.
 *  - bit[18]: Escape Entry Error on Data Lane 2.
 *  - bit[17]: Escape Entry Error on Data Lane 1.
 *  - bit[16]: Escape Entry Error on Data Lane 0.
 *  - bit[3]: Start of Transmission Error on Data Lane 3 (synchronization can still be achieved).
 *  - bit[2]: Start of Transmission Error on Data Lane 2 (synchronization can still be achieved).
 *  - bit[1]: Start of Transmission Error on Data Lane 1 (synchronization can still be achieved).
 *  - bit[0]: Start of Transmission Error on Data Lane 0 (synchronization can still be achieved).
 * @err_pkt: Packet error.
 *
 *  - bit[19]: Header Error detected and corrected on virtual channel 3.
 *  - bit[18]: Header Error detected and corrected on virtual channel 2.
 *  - bit[17]: Header Error detected and corrected on virtual channel 1.
 *  - bit[16]: Header Error detected and corrected on virtual channel 0.
 *  - bit[3]: Unrecognized or unimplemented data type detected in virtual channel 3.
 *  - bit[2]: Unrecognized or unimplemented data type detected in virtual channel 2.
 *  - bit[1]: Unrecognized or unimplemented data type detected in virtual channel 1.
 *  - bit[0]: Unrecognized or unimplemented data type detected in virtual channel 0.
 * @err_line: Line error.
 *
 *  - bit[23]: Error in the sequence of lines for vc7 and dt7.
 *  - bit[22]: Error in the sequence of lines for vc6 and dt6.
 *  - bit[21]: Error in the sequence of lines for vc5 and dt5.
 *  - bit[20]: Error in the sequence of lines for vc4 and dt4.
 *  - bit[19]: Error in the sequence of lines for vc3 and dt3.
 *  - bit[18]: Error in the sequence of lines for vc2 and dt2.
 *  - bit[17]: Error in the sequence of lines for vc1 and dt1.
 *  - bit[16]: Error in the sequence of lines for vc0 and dt0.
 *  - bit[7]: Error matching Line Start with Line End for vc7 and dt7.
 *  - bit[6]: Error matching Line Start with Line End for vc6 and dt6.
 *  - bit[5]: Error matching Line Start with Line End for vc5 and dt5.
 *  - bit[4]: Error matching Line Start with Line End for vc4 and dt4.
 *  - bit[3]: Error matching Line Start with Line End for vc3 and dt3.
 *  - bit[2]: Error matching Line Start with Line End for vc2 and dt2.
 *  - bit[1]: Error matching Line Start with Line End for vc1 and dt1.
 *  - bit[0]: Error matching Line Start with Line End for vc0 and dt0.
 */
struct viif_csi2rx_err_status {
	__u32 err_phy_fatal;
	__u32 err_pkt_fatal;
	__u32 err_frame_fatal;
	__u32 err_phy;
	__u32 err_pkt;
	__u32 err_line;
};

/**
 * struct viif_l1_info - L1ISP AWB information
 * for &struct viif_isp_capture_status
 * @avg_lum_weight: weighted average luminance value at average luminance generation
 * @avg_lum_block: average luminance of each block.
 *                 Nested indices are: [y position][x position].
 * @avg_lum_four_line_lum: 4-lines average luminance.
 *                         avg_lum_four_line_lum[n] corresponds to aexp_ave4linesy[n]
 * @avg_satur_pixnum: the number of saturated pixel at average luminance generation
 * @avg_black_pixnum: the number of black pixel at average luminance generation
 * @awb_ave_u: U average value of AWB adjustment
 * @awb_ave_v: V average value of AWB adjustment
 * @awb_accumulated_pixel: Accumulated pixel count of AWB adjustment
 * @awb_gain_r: R gain used in the next frame of AWB adjustment
 * @awb_gain_g: G gain used in the next frame of AWB adjustment
 * @awb_gain_b: B gain used in the next frame of AWB adjustment
 * @awb_status_u: boolean value of U convergence state of AWB adjustment
 *                (0: not-converged, 1: converged)
 * @awb_status_v: boolean value of V convergence state of AWB adjustment
 *                (0: not-converged, 1: converged)
 */
struct viif_l1_info {
	__u32 avg_lum_weight;
	__u32 avg_lum_block[8][8];
	__u32 avg_lum_four_line_lum[4];
	__u32 avg_satur_pixnum;
	__u32 avg_black_pixnum;
	__u32 awb_ave_u;
	__u32 awb_ave_v;
	__u32 awb_accumulated_pixel;
	__u32 awb_gain_r;
	__u32 awb_gain_g;
	__u32 awb_gain_b;
	__u8 awb_status_u;
	__u8 awb_status_v;
};

/**
 * struct viif_isp_capture_status - L1ISP capture information
 * for :ref:`V4L2_CID_VISCONTI_VIIF_GET_LAST_CAPTURE_STATUS`
 * @l1_info: L1ISP AWB information. Refer to &struct viif_l1_info
 */
struct viif_isp_capture_status {
	struct viif_l1_info l1_info;
};

/**
 * struct viif_reported_errors - Errors since last call
 * for :ref:`V4L2_CID_VISCONTI_VIIF_GET_REPORTED_ERRORS`
 * @main: error flag value for capture device 0 and 1
 * @sub: error flag value for capture device 2
 * @csi2rx: error flag value for CSI2 receiver
 */
struct viif_reported_errors {
	__u32 main;
	__u32 sub;
	__u32 csi2rx;
};

#endif /* __UAPI_VISCONTI_VIIF_H_ */
