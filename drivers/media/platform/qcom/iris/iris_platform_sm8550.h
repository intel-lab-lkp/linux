/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_PLATFORM_SM8550_H__
#define __IRIS_PLATFORM_SM8550_H__

static const char * const sm8550_clk_reset_table[] = { "bus" };

static const struct platform_clk_data sm8550_clk_table[] = {
	{IRIS_AXI_CLK,  "iface"        },
	{IRIS_CTRL_CLK, "core"         },
	{IRIS_HW_CLK,   "vcodec0_core" },
};

static struct platform_inst_caps platform_inst_cap_sm8550 = {
	.min_frame_width = 96,
	.max_frame_width = 8192,
	.min_frame_height = 96,
	.max_frame_height = 8192,
	.max_mbpf = (8192 * 4352) / 256,
	.mb_cycles_vpp = 200,
	.mb_cycles_fw = 489583,
	.mb_cycles_fw_vpp = 66234,
	.max_frame_rate = MAXIMUM_FPS,
	.max_operating_rate = MAXIMUM_FPS,
	.max_slices_per_frame = 128,
	.max_slice_frame_rate = 60,
	.max_mb_slice_width = 4096,
	.max_mb_slice_height = 2160,
	.max_bytes_slice_width = 1920,
	.max_bytes_slice_height = 1088,
	.min_hevc_slice_width = 384,
	.min_avc_slice_width = 192,
	.min_slice_height = 128,
};

#endif
