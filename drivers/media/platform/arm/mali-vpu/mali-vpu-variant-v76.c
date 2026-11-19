// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/array_size.h>
#include <linux/videodev2.h>

#include "mali-vpu-formats.h"
#include "mali-vpu-protocol-def-v3.h"
#include "mali-vpu-variant.h"

#define MALI_VPU_HW_PID_V76 0x56627

#define MALI_V76_MAX_FRAME_WIDTH 8192
#define MALI_V76_MAX_FRAME_HEIGHT 8192
#define MALI_V76_MIN_FRAME_WIDTH 2
#define MALI_V76_MIN_FRAME_HEIGHT 2
#define MALI_V76_DEFAULT_FRAME_WIDTH 640
#define MALI_V76_DEFAULT_FRAME_HEIGHT 480

static const char * const mali_v76_rst_names[] = {
	"warm",
};

static const struct mali_vpu_format_desc mali_v76_dec_src_fmts[] = {
	{
		.fourcc		= V4L2_PIX_FMT_H264,
		.mve_frame_fmt	= 0,
		.codec_fmt	= MALI_VPU_FORMAT_CODEC_H264,
		.flags		= V4L2_FMT_FLAG_COMPRESSED | V4L2_FMT_FLAG_DYN_RESOLUTION,
	},
};

static const struct mali_vpu_format_desc mali_v76_dec_dst_fmts[] = {
	{
		.fourcc		= V4L2_PIX_FMT_YUV420M,
		.mve_frame_fmt	= MVE_FORMAT_YUV420_I420,
		.codec_fmt	= 0,
		.flags		= 0,
	},
	{
		.fourcc		= V4L2_PIX_FMT_NV12M,
		.mve_frame_fmt	= MVE_FORMAT_YUV420_NV12,
		.codec_fmt	= 0,
		.flags		= 0,
	},
	{
		.fourcc		= V4L2_PIX_FMT_NV21M,
		.mve_frame_fmt	= MVE_FORMAT_YUV420_NV21,
		.codec_fmt	= 0,
		.flags		= 0,
	},
};

const struct mali_vpu_variant mali_vpu_v76_variant = {
	.expected_hw_pid = MALI_VPU_HW_PID_V76,

	.rst_names = mali_v76_rst_names,
	.num_rsts = ARRAY_SIZE(mali_v76_rst_names),

	.fmt_caps = {
		.dec_src = {
			.fmts = mali_v76_dec_src_fmts,
			.num_fmts = ARRAY_SIZE(mali_v76_dec_src_fmts),
		},
		.dec_dst = {
			.fmts = mali_v76_dec_dst_fmts,
			.num_fmts = ARRAY_SIZE(mali_v76_dec_dst_fmts),
		},
	},
	.frmsz_caps = {
		.stepwise = {
			.min_width = MALI_V76_MIN_FRAME_WIDTH,
			.max_width = MALI_V76_MAX_FRAME_WIDTH,
			.step_width = 2,
			.min_height = MALI_V76_MIN_FRAME_HEIGHT,
			.max_height = MALI_V76_MAX_FRAME_HEIGHT,
			.step_height = 2,
		},
		.width_default = MALI_V76_DEFAULT_FRAME_WIDTH,
		.height_default = MALI_V76_DEFAULT_FRAME_HEIGHT,
	},
};
