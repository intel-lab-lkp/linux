// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/bits.h>

#include "mali-c720-registers-modules.h"

static const struct mali_c720_decompander_regs decompander = {
	.enable = { .offset = 0x0, .mask = BIT(0), .shift = 0 },
	.lut_x = { .offset = 0x4, .mask = GENMASK(23, 0), .shift = 0 },
	.lut_y = { .offset = 0x88, .mask = GENMASK(24, 0), .shift = 0 },
};

static const struct mali_c720_digital_gain_regs inp_dgain = {
	.gain = { .offset = 0x0, .mask = GENMASK(19, 0), .shift = 0 },
	.offset = { .offset = 0x4, .mask = GENMASK(23, 0), .shift = 0 },
};

static const struct mali_c720_square_root_regs square_root_a = {
	.black_level_out = { .offset = 0x0, .mask = GENMASK(11, 0), .shift = 0 },
	.black_level_in = { .offset = 0x4, .mask = GENMASK(23, 0), .shift = 0 },
	.normalization = { .offset = 0x8, .mask = GENMASK(16, 0), .shift = 0 },
};

static const struct mali_c720_black_level_regs black_level = {
	.black_00 = { .offset = 0x0, .mask = GENMASK(15, 0), .shift = 0 },
	.black_01 = { .offset = 0x0, .mask = GENMASK(31, 16), .shift = 16 },
	.black_10 = { .offset = 0x4, .mask = GENMASK(15, 0), .shift = 0 },
	.black_11 = { .offset = 0x4, .mask = GENMASK(31, 16), .shift = 16 },
};

static const struct mali_c720_wb_gain_regs wb_gain = {
	.gain_00 = { .offset = 0x0, .mask = GENMASK(11, 0), .shift = 0 },
	.gain_01 = { .offset = 0x4, .mask = GENMASK(11, 0), .shift = 0 },
	.gain_10 = { .offset = 0x8, .mask = GENMASK(11, 0), .shift = 0 },
	.gain_11 = { .offset = 0xc, .mask = GENMASK(11, 0), .shift = 0 },
};

static const struct mali_c720_square_regs square_a = {
	.black_level_in = { .offset = 0x0, .mask = GENMASK(23, 0), .shift = 0 },
	.black_level_out = { .offset = 0x4, .mask = GENMASK(11, 0), .shift = 0 },
	.normalization = { .offset = 0x8, .mask = GENMASK(16, 0), .shift = 0 },
};

static const struct mali_c720_square_regs square_b = {
	.black_level_in = { .offset = 0x0, .mask = GENMASK(11, 0), .shift = 0 },
	.black_level_out = { .offset = 0x4, .mask = GENMASK(23, 0), .shift = 0 },
	.normalization = { .offset = 0x8, .mask = GENMASK(16, 0), .shift = 0 },
};

static const struct mali_c720_square_root_regs square_root_b = {
	.black_level_out = { .offset = 0x0, .mask = GENMASK(11, 0), .shift = 0 },
	.black_level_in = { .offset = 0x4, .mask = GENMASK(23, 0), .shift = 0 },
	.normalization = { .offset = 0x8, .mask = GENMASK(16, 0), .shift = 0 },
};

static const struct mali_c720_rgb_scaler_regs rgb_scaler = {
	.hfilt_enable = { .offset = 0x0, .mask = BIT(0), .shift = 0 },
	.vfilt_enable = { .offset = 0x0, .mask = BIT(1), .shift = 1 },
	.hfilt_tincr = { .offset = 0x4, .mask = GENMASK(23, 0), .shift = 0 },
	.hfilt_start_phase = { .offset = 0x8, .mask = GENMASK(23, 0), .shift = 0 },
	.vfilt_tincr = { .offset = 0xc, .mask = GENMASK(23, 0), .shift = 0 },
	.vfilt_start_phase = { .offset = 0x10, .mask = GENMASK(23, 0), .shift = 0 },
	.hmem = { .offset = 0x0, .mask = GENMASK(31, 0), .shift = 0 },
	.vmem = { .offset = 0x0, .mask = GENMASK(31, 0), .shift = 0 },
};

static const struct mali_c720_crop_regs crop = {
	.enable_crop = { .offset = 0x0, .mask = BIT(0), .shift = 0 },
	.start_x = { .offset = 0x4, .mask = GENMASK(15, 0), .shift = 0 },
	.start_y = { .offset = 0x4, .mask = GENMASK(31, 16), .shift = 16 },
	.size_x = { .offset = 0x8, .mask = GENMASK(15, 0), .shift = 0 },
	.size_y = { .offset = 0x8, .mask = GENMASK(31, 16), .shift = 16 },
};

static const struct mali_c720_out_format_regs out_format = {
	.coefft_11 = { .offset = 0x0, .mask = GENMASK(15, 0), .shift = 0 },
	.coefft_12 = { .offset = 0x0, .mask = GENMASK(31, 16), .shift = 16 },
	.coefft_13 = { .offset = 0x4, .mask = GENMASK(15, 0), .shift = 0 },
	.coefft_21 = { .offset = 0x4, .mask = GENMASK(31, 16), .shift = 16 },
	.coefft_22 = { .offset = 0x8, .mask = GENMASK(15, 0), .shift = 0 },
	.coefft_23 = { .offset = 0x8, .mask = GENMASK(31, 16), .shift = 16 },
	.coefft_31 = { .offset = 0xc, .mask = GENMASK(15, 0), .shift = 0 },
	.coefft_32 = { .offset = 0xc, .mask = GENMASK(31, 16), .shift = 16 },
	.coefft_33 = { .offset = 0x10, .mask = GENMASK(15, 0), .shift = 0 },
	.coefft_o1 = { .offset = 0x14, .mask = GENMASK(16, 0), .shift = 0 },
	.coefft_o2 = { .offset = 0x18, .mask = GENMASK(16, 0), .shift = 0 },
	.coefft_o3 = { .offset = 0x1c, .mask = GENMASK(16, 0), .shift = 0 },
	.clip_min_y = { .offset = 0x20, .mask = GENMASK(15, 0), .shift = 0 },
	.clip_max_y = { .offset = 0x20, .mask = GENMASK(31, 16), .shift = 16 },
	.clip_min_uv = { .offset = 0x24, .mask = GENMASK(15, 0), .shift = 0 },
	.clip_max_uv = { .offset = 0x24, .mask = GENMASK(31, 16), .shift = 16 },
};

static const struct mali_c720_stat_color_stats_regs stat_color_stats = {
	.awb_stats_mode = { .offset = 0xc, .mask = BIT(0), .shift = 0 },
	.white_level_awb = { .offset = 0x10, .mask = GENMASK(13, 0), .shift = 0 },
	.black_level_awb = { .offset = 0x14, .mask = GENMASK(13, 0), .shift = 0 },
	.cr_ref_max_awb = { .offset = 0x18, .mask = GENMASK(11, 0), .shift = 0 },
	.cr_ref_min_awb = { .offset = 0x1c, .mask = GENMASK(11, 0), .shift = 0 },
	.cb_ref_max_awb = { .offset = 0x20, .mask = GENMASK(11, 0), .shift = 0 },
	.cb_ref_min_awb = { .offset = 0x24, .mask = GENMASK(11, 0), .shift = 0 },
	.awb_nodes_used_horiz = { .offset = 0x28, .mask = GENMASK(7, 0), .shift = 0 },
	.awb_nodes_used_vert = { .offset = 0x28, .mask = GENMASK(15, 8), .shift = 8 },
	.cr_ref_high_awb = { .offset = 0x2c, .mask = GENMASK(11, 0), .shift = 0 },
	.cr_ref_low_awb = { .offset = 0x30, .mask = GENMASK(11, 0), .shift = 0 },
	.cb_ref_high_awb = { .offset = 0x34, .mask = GENMASK(11, 0), .shift = 0 },
	.cb_ref_low_awb = { .offset = 0x38, .mask = GENMASK(11, 0), .shift = 0 },
	.awb_weight = { .offset = 0x40, .mask = GENMASK(3, 0), .shift = 0 },
};

static const struct mali_c720_stat_histogram_regs stat_histogram = {
	.skip_x = { .offset = 0x28, .mask = GENMASK(2, 0), .shift = 0 },
	.skip_y = { .offset = 0x28, .mask = GENMASK(6, 4), .shift = 4 },
	.offset_x = { .offset = 0x28, .mask = BIT(3), .shift = 3 },
	.offset_y = { .offset = 0x28, .mask = BIT(7), .shift = 7 },
	.scale_bottom = { .offset = 0x2c, .mask = GENMASK(3, 0), .shift = 0 },
	.scale_top = { .offset = 0x2c, .mask = GENMASK(7, 4), .shift = 4 },
	.plane_mode = { .offset = 0x30, .mask = GENMASK(2, 0), .shift = 0 },
	.nodes_used_horiz = { .offset = 0x34, .mask = GENMASK(7, 0), .shift = 0 },
	.nodes_used_vert = { .offset = 0x34, .mask = GENMASK(15, 8), .shift = 8 },
	.weight = { .offset = 0x38, .mask = GENMASK(3, 0), .shift = 0 },
};

static const struct mali_c720_modules_regs mali_c720_modules_regs = {
	.decompander = &decompander,
	.inp_dgain = &inp_dgain,
	.square_root_a = &square_root_a,
	.black_level = &black_level,
	.wb_gain = &wb_gain,
	.square_a = &square_a,
	.square_b = &square_b,
	.square_root_b = &square_root_b,
	.rgb_scaler = &rgb_scaler,
	.crop = &crop,
	.out_format = &out_format,
	.stat_color_stats = &stat_color_stats,
	.stat_histogram = &stat_histogram,
};

const struct mali_c720_modules_regs *mali_c720_get_modules_regs(void)
{
	return &mali_c720_modules_regs;
}
