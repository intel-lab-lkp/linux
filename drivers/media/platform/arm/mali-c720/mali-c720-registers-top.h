/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_REGISTERS_TOP_H
#define MALI_C720_REGISTERS_TOP_H

#include "mali-c720-registers.h"

struct mali_c720_irq_regs {
	struct mali_c720_reg_field cm_input_0_stream_3_cmsi_active_video;
	struct mali_c720_reg_field cm_input_0_stream_2_cmsi_active_video;
	struct mali_c720_reg_field cm_input_0_stream_1_cmsi_active_video;
	struct mali_c720_reg_field cm_input_0_stream_0_cmsi_active_video;
	struct mali_c720_reg_field cm_input_1_stream_3_cmsi_active_video;
	struct mali_c720_reg_field cm_input_1_stream_2_cmsi_active_video;
	struct mali_c720_reg_field cm_input_1_stream_1_cmsi_active_video;
	struct mali_c720_reg_field cm_input_1_stream_0_cmsi_active_video;
	struct mali_c720_reg_field cm_input_2_stream_3_cmsi_active_video;
	struct mali_c720_reg_field cm_input_2_stream_2_cmsi_active_video;
	struct mali_c720_reg_field cm_input_2_stream_1_cmsi_active_video;
	struct mali_c720_reg_field cm_input_2_stream_0_cmsi_active_video;
	struct mali_c720_reg_field cm_input_3_stream_3_cmsi_active_video;
	struct mali_c720_reg_field cm_input_3_stream_2_cmsi_active_video;
	struct mali_c720_reg_field cm_input_3_stream_1_cmsi_active_video;
	struct mali_c720_reg_field cm_input_3_stream_0_cmsi_active_video;
	struct mali_c720_reg_field cm_status_slot_15;
	struct mali_c720_reg_field cm_status_slot_14;
	struct mali_c720_reg_field cm_status_slot_13;
	struct mali_c720_reg_field cm_status_slot_12;
	struct mali_c720_reg_field cm_status_slot_11;
	struct mali_c720_reg_field cm_status_slot_10;
	struct mali_c720_reg_field cm_status_slot_9;
	struct mali_c720_reg_field cm_status_slot_8;
	struct mali_c720_reg_field cm_status_slot_7;
	struct mali_c720_reg_field cm_status_slot_6;
	struct mali_c720_reg_field cm_status_slot_5;
	struct mali_c720_reg_field cm_status_slot_4;
	struct mali_c720_reg_field cm_status_slot_3;
	struct mali_c720_reg_field cm_status_slot_2;
	struct mali_c720_reg_field cm_status_slot_1;
	struct mali_c720_reg_field cm_status_slot_0;
};

struct mali_c720_pre_cm_regs {
	struct mali_c720_reg_field tpg_0_splitter_sel;
	struct mali_c720_reg_field tpg_1_splitter_sel;
	struct mali_c720_reg_field input_0_mux_sel;
	struct mali_c720_reg_field input_1_mux_sel;
	struct mali_c720_reg_field input_2_mux_sel;
	struct mali_c720_reg_field input_3_mux_sel;
};

struct mali_c720_precm_tpg_regs {
	struct mali_c720_group_regs group;
	struct mali_c720_reg_field glbl_active_width;
	struct mali_c720_reg_field glbl_active_height;
	struct mali_c720_reg_field glbl_rggb_start;
	struct mali_c720_reg_field test_pattern_off_on;
	struct mali_c720_reg_field generate_mode;
	struct mali_c720_reg_field frame_request;
	struct mali_c720_reg_field pattern_type;
	struct mali_c720_reg_field horizontal_blanking;
	struct mali_c720_reg_field vertical_blanking;
};

struct mali_c720_pipeline_regs {
	struct mali_c720_reg_field frame_count;
	struct mali_c720_reg_field active_width;
	struct mali_c720_reg_field active_height;
	struct mali_c720_reg_field rggb_start;
	struct mali_c720_reg_field inp_remosaic_cv_mux_sel;
	struct mali_c720_reg_field inp_remosaic_ir0_mux_sel;
	struct mali_c720_reg_field inp_cv_mux_sel;
	struct mali_c720_reg_field fr0_hv_cv_mux_sel;
	struct mali_c720_reg_field fr1_hv_cv_mux_sel;
	struct mali_c720_reg_field fr2_hv_cv_mux_sel;
	struct mali_c720_reg_field fr3_hv_cv_mux_sel;
	struct mali_c720_reg_field pyr_hv_cv_ir0_mux_sel;
	struct mali_c720_reg_field fov_fr_mux_sel;
	struct mali_c720_reg_field fov_raw_fr_mux_sel;
	struct mali_c720_reg_field raw_input_mux_sel;
	struct mali_c720_reg_field af_stats_mux_sel;
	struct mali_c720_reg_field color_stats_0_mux_sel;
	struct mali_c720_reg_field color_stats_1_mux_sel;
	struct mali_c720_reg_field histogram_0_mux_sel;
	struct mali_c720_reg_field histogram_1_stream_mux_sel;
	struct mali_c720_reg_field histogram_1_mux_sel;
	struct mali_c720_reg_field histogram_2_mux_sel;
	struct mali_c720_reg_field histogram_3_stream_mux_sel;
	struct mali_c720_reg_field histogram_3_mux_sel;
	struct mali_c720_reg_field histogram_4_mux_sel;
	struct mali_c720_reg_field intensity_zone_1_mux_sel;
	struct mali_c720_reg_field intensity_zone_3_mux_sel;
	struct mali_c720_reg_field bypass_inp_decompander;
	struct mali_c720_reg_field bypass_inp_frame_stitch;
	struct mali_c720_reg_field bypass_inp_dgain;
	struct mali_c720_reg_field bypass_inp_square_root_a;
	struct mali_c720_reg_field bypass_inp_dpc;
	struct mali_c720_reg_field bypass_inp_remosaic;
	struct mali_c720_reg_field bypass_inp_cac;
	struct mali_c720_reg_field bypass_inp_temper;
	struct mali_c720_reg_field bypass_inp_sinter;
	struct mali_c720_reg_field bypass_hv_black_level;
	struct mali_c720_reg_field bypass_hv_wb_gain;
	struct mali_c720_reg_field bypass_hv_smart_shading;
	struct mali_c720_reg_field bypass_hv_raw_to_intensity;
	struct mali_c720_reg_field bypass_hv_dehaze;
	struct mali_c720_reg_field bypass_hv_dgain_iridix;
	struct mali_c720_reg_field bypass_hv_iridix;
	struct mali_c720_reg_field bypass_hv_square_a;
	struct mali_c720_reg_field bypass_hv_demosaic;
	struct mali_c720_reg_field bypass_hv_pf_correction;
	struct mali_c720_reg_field bypass_hv_square_b;
	struct mali_c720_reg_field bypass_hv_ccm;
	struct mali_c720_reg_field bypass_hv_square_root_b;
	struct mali_c720_reg_field bypass_hv_cnr;
	struct mali_c720_reg_field bypass_cv_black_level;
	struct mali_c720_reg_field bypass_cv_wb_gain;
	struct mali_c720_reg_field bypass_cv_smart_shading;
	struct mali_c720_reg_field bypass_cv_raw_to_intensity;
	struct mali_c720_reg_field bypass_cv_dehaze;
	struct mali_c720_reg_field bypass_cv_dgain_iridix;
	struct mali_c720_reg_field bypass_cv_iridix;
	struct mali_c720_reg_field bypass_cv_square_a;
	struct mali_c720_reg_field bypass_cv_demosaic_dfd;
	struct mali_c720_reg_field bypass_cv_square_b;
	struct mali_c720_reg_field bypass_cv_ccm;
	struct mali_c720_reg_field bypass_cv_square_root_b;
	struct mali_c720_reg_field bypass_cv_cnr;
	struct mali_c720_reg_field bypass_fr0_rgb_scaler;
	struct mali_c720_reg_field bypass_fr1_rgb_scaler;
	struct mali_c720_reg_field bypass_fr2_rgb_scaler;
	struct mali_c720_reg_field bypass_fr3_rgb_scaler;
	struct mali_c720_reg_field bypass_fr0_crop;
	struct mali_c720_reg_field bypass_fr0_rgb_sharpening;
	struct mali_c720_reg_field bypass_fr0_gain_and_offset;
	struct mali_c720_reg_field bypass_fr0_gamma;
	struct mali_c720_reg_field bypass_fr1_crop;
	struct mali_c720_reg_field bypass_fr1_rgb_sharpening;
	struct mali_c720_reg_field bypass_fr1_gain_and_offset;
	struct mali_c720_reg_field bypass_fr1_gamma;
	struct mali_c720_reg_field bypass_fr2_crop;
	struct mali_c720_reg_field bypass_fr2_rgb_sharpening;
	struct mali_c720_reg_field bypass_fr2_gain_and_offset;
	struct mali_c720_reg_field bypass_fr2_gamma;
	struct mali_c720_reg_field bypass_fr3_crop;
	struct mali_c720_reg_field bypass_fr3_rgb_sharpening;
	struct mali_c720_reg_field bypass_fr3_gain_and_offset;
	struct mali_c720_reg_field bypass_fr3_gamma;
	struct mali_c720_reg_field bypass_pyr_tc_lut;
	struct mali_c720_reg_field bypass_fov_crop_0;
	struct mali_c720_reg_field bypass_fov_crop_1;
	struct mali_c720_reg_field bypass_fov_crop_2;
	struct mali_c720_reg_field bypass_raw_offset;
	struct mali_c720_reg_field bypass_raw_dgain;
	struct mali_c720_reg_field bypass_raw_tc_lut;
	struct mali_c720_reg_field bypass_raw_crop;
};

/* Register field encodings used by the R0 programming model. */
enum mali_c720_top_fov_fr_mux_sel {
	ISP_TOP_FOV_FR_MUX_SEL_FR0_GAMMA,
	ISP_TOP_FOV_FR_MUX_SEL_FR0_OUT_FORMAT_444_FOV,
	ISP_TOP_FOV_FR_MUX_SEL_FR1_GAMMA,
	ISP_TOP_FOV_FR_MUX_SEL_FR1_OUT_FORMAT_444_FOV,
	ISP_TOP_FOV_FR_MUX_SEL_FR2_GAMMA,
	ISP_TOP_FOV_FR_MUX_SEL_FR2_OUT_FORMAT_444_FOV,
	ISP_TOP_FOV_FR_MUX_SEL_FR3_GAMMA,
	ISP_TOP_FOV_FR_MUX_SEL_FR3_OUT_FORMAT_444_FOV,
};

enum mali_c720_top_fov_raw_fr_mux_sel {
	ISP_TOP_FOV_RAW_FR_MUX_SEL_RAW_TC_LUT,
	ISP_TOP_FOV_RAW_FR_MUX_SEL_FOV_FR_MUX,
};

enum mali_c720_top_fr0_hv_cv_mux_sel {
	ISP_TOP_FR0_HV_CV_MUX_SEL_HV,
	ISP_TOP_FR0_HV_CV_MUX_SEL_CV,
};

enum mali_c720_top_inp_cv_mux_sel {
	ISP_TOP_INP_CV_MUX_SEL_SQRT_A,
	ISP_TOP_INP_CV_MUX_SEL_REMOSAIC_CV,
	ISP_TOP_INP_CV_MUX_SEL_CAC,
	ISP_TOP_INP_CV_MUX_SEL_TEMPER,
	ISP_TOP_INP_CV_MUX_SEL_SINTER,
};

enum mali_c720_top_inp_remosaic_cv_mux_sel {
	ISP_TOP_INP_REMOSAIC_CV_MUX_SEL_REMOSAIC_IR_FULL,
	ISP_TOP_INP_REMOSAIC_CV_MUX_SEL_REMOSAIC_IR_QUARTER,
};

enum mali_c720_top_inp_remosaic_ir0_mux_sel {
	ISP_TOP_INP_REMOSAIC_IR0_MUX_SEL_REMOSAIC_IR_FULL,
	ISP_TOP_INP_REMOSAIC_IR0_MUX_SEL_REMOSAIC_IR_QUARTER,
};

enum mali_c720_top_pyr_hv_cv_ir0_mux_sel {
	ISP_TOP_PYR_HV_CV_IR0_MUX_SEL_HV,
	ISP_TOP_PYR_HV_CV_IR0_MUX_SEL_CV,
	ISP_TOP_PYR_HV_CV_IR0_MUX_SEL_REMOSAIC_IR0,
};

enum mali_c720_top_raw_input_mux_sel {
	ISP_TOP_RAW_INPUT_MUX_SEL_IN0,
	ISP_TOP_RAW_INPUT_MUX_SEL_DECOMPANDER,
	ISP_TOP_RAW_INPUT_MUX_SEL_SQRT_A,
	ISP_TOP_RAW_INPUT_MUX_SEL_CV_SQUARE_A,
	ISP_TOP_RAW_INPUT_MUX_SEL_HV_SQUARE_A,
	ISP_TOP_RAW_INPUT_MUX_SEL_REMOSAIC_IR0,
};

struct mali_c720_top_regs {
	const struct mali_c720_irq_regs *irq;
	const struct mali_c720_pre_cm_regs *pre_cm;
	const struct mali_c720_precm_tpg_regs *precm_tpgs;
	const struct mali_c720_pipeline_regs *pipeline;
};

const struct mali_c720_top_regs *mali_c720_get_top_regs(void);

#endif /* MALI_C720_REGISTERS_TOP_H */
