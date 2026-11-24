/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_REGISTERS_MODULES_H
#define MALI_C720_REGISTERS_MODULES_H

#include "mali-c720-registers.h"

struct mali_c720_decompander_regs {
	struct mali_c720_reg_field enable;
	struct mali_c720_reg_field lut_x;
	struct mali_c720_reg_field lut_y;
};

struct mali_c720_digital_gain_regs {
	struct mali_c720_reg_field gain;
	struct mali_c720_reg_field offset;
};

struct mali_c720_square_root_regs {
	struct mali_c720_reg_field black_level_out;
	struct mali_c720_reg_field black_level_in;
	struct mali_c720_reg_field normalization;
};

struct mali_c720_black_level_regs {
	struct mali_c720_reg_field black_00;
	struct mali_c720_reg_field black_01;
	struct mali_c720_reg_field black_10;
	struct mali_c720_reg_field black_11;
};

struct mali_c720_wb_gain_regs {
	struct mali_c720_reg_field gain_00;
	struct mali_c720_reg_field gain_01;
	struct mali_c720_reg_field gain_10;
	struct mali_c720_reg_field gain_11;
};

struct mali_c720_square_regs {
	struct mali_c720_reg_field black_level_in;
	struct mali_c720_reg_field black_level_out;
	struct mali_c720_reg_field normalization;
};

struct mali_c720_rgb_scaler_regs {
	struct mali_c720_reg_field hfilt_enable;
	struct mali_c720_reg_field vfilt_enable;
	struct mali_c720_reg_field hfilt_tincr;
	struct mali_c720_reg_field hfilt_start_phase;
	struct mali_c720_reg_field vfilt_tincr;
	struct mali_c720_reg_field vfilt_start_phase;
	struct mali_c720_reg_field hmem;
	struct mali_c720_reg_field vmem;
};

struct mali_c720_crop_regs {
	struct mali_c720_reg_field enable_crop;
	struct mali_c720_reg_field start_x;
	struct mali_c720_reg_field start_y;
	struct mali_c720_reg_field size_x;
	struct mali_c720_reg_field size_y;
};

struct mali_c720_out_format_regs {
	struct mali_c720_reg_field coefft_11;
	struct mali_c720_reg_field coefft_12;
	struct mali_c720_reg_field coefft_13;
	struct mali_c720_reg_field coefft_21;
	struct mali_c720_reg_field coefft_22;
	struct mali_c720_reg_field coefft_23;
	struct mali_c720_reg_field coefft_31;
	struct mali_c720_reg_field coefft_32;
	struct mali_c720_reg_field coefft_33;
	struct mali_c720_reg_field coefft_o1;
	struct mali_c720_reg_field coefft_o2;
	struct mali_c720_reg_field coefft_o3;
	struct mali_c720_reg_field clip_min_y;
	struct mali_c720_reg_field clip_max_y;
	struct mali_c720_reg_field clip_min_uv;
	struct mali_c720_reg_field clip_max_uv;
};

struct mali_c720_stat_color_stats_regs {
	struct mali_c720_reg_field awb_stats_mode;
	struct mali_c720_reg_field white_level_awb;
	struct mali_c720_reg_field black_level_awb;
	struct mali_c720_reg_field cr_ref_max_awb;
	struct mali_c720_reg_field cr_ref_min_awb;
	struct mali_c720_reg_field cb_ref_max_awb;
	struct mali_c720_reg_field cb_ref_min_awb;
	struct mali_c720_reg_field awb_nodes_used_horiz;
	struct mali_c720_reg_field awb_nodes_used_vert;
	struct mali_c720_reg_field cr_ref_high_awb;
	struct mali_c720_reg_field cr_ref_low_awb;
	struct mali_c720_reg_field cb_ref_high_awb;
	struct mali_c720_reg_field cb_ref_low_awb;
	struct mali_c720_reg_field awb_weight;
};

struct mali_c720_stat_histogram_regs {
	struct mali_c720_reg_field skip_x;
	struct mali_c720_reg_field skip_y;
	struct mali_c720_reg_field offset_x;
	struct mali_c720_reg_field offset_y;
	struct mali_c720_reg_field scale_bottom;
	struct mali_c720_reg_field scale_top;
	struct mali_c720_reg_field plane_mode;
	struct mali_c720_reg_field nodes_used_horiz;
	struct mali_c720_reg_field nodes_used_vert;
	struct mali_c720_reg_field weight;
};

struct mali_c720_modules_regs {
	const struct mali_c720_decompander_regs *decompander;
	const struct mali_c720_digital_gain_regs *inp_dgain;
	const struct mali_c720_square_root_regs *square_root_a;
	const struct mali_c720_black_level_regs *black_level;
	const struct mali_c720_wb_gain_regs *wb_gain;
	const struct mali_c720_square_regs *square_a;
	const struct mali_c720_square_regs *square_b;
	const struct mali_c720_square_root_regs *square_root_b;
	const struct mali_c720_rgb_scaler_regs *rgb_scaler;
	const struct mali_c720_crop_regs *crop;
	const struct mali_c720_out_format_regs *out_format;
	const struct mali_c720_stat_color_stats_regs *stat_color_stats;
	const struct mali_c720_stat_histogram_regs *stat_histogram;
};

const struct mali_c720_modules_regs *mali_c720_get_modules_regs(void);

#endif /* MALI_C720_REGISTERS_MODULES_H */
