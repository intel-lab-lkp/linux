/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_PLATFORM_QCS8300_H__
#define __IRIS_PLATFORM_QCS8300_H__

static struct platform_inst_caps platform_inst_cap_qcs8300 = {
	.min_frame_width = 96,
	.max_frame_width = 4096,
	.min_frame_height = 96,
	.max_frame_height = 4096,
	.max_mbpf = (4096 * 2176) / 256,
	.mb_cycles_vpp = 200,
	.mb_cycles_fw = 326389,
	.mb_cycles_fw_vpp = 44156,
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
