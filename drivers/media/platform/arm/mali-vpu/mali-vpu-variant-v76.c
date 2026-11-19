// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/videodev2.h>

#include <linux/media/arm/mali-vpu-config.h>

#include "mali-vpu-controls.h"
#include "mali-vpu-dec.h"
#include "mali-vpu-formats.h"
#include "mali-vpu-protocol-def-v3.h"
#include "mali-vpu-variant.h"

#define MALI_VPU_HW_PID_V76 0x56627
#define MALI_VPU_FW_VER_MAJOR_V76 1

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

static const char *const mali_v76_nalu_format_qmenu[] = {
	"Start Codes",
	"One NALU Per Buffer",
	"One Byte Length Field",
	"Two Byte Length Field",
	"Four Byte Length Field",
	NULL,
};

static const struct mali_vpu_ctrl_desc mali_v76_dec_ctrls[] = {
	{
		.cid = V4L2_CID_MALI_VPU_NALU_FORMAT,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "NALU format",
		.qmenu = mali_v76_nalu_format_qmenu,
		.min_val = MALI_VPU_NALU_FORMAT_START_CODES,
		.max_val = MALI_VPU_NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD,
		.default_val = MALI_VPU_NALU_FORMAT_START_CODES,
		.step_or_mask = BIT(MALI_VPU_NALU_FORMAT_START_CODES) |
				BIT(MALI_VPU_NALU_FORMAT_ONE_NALU_PER_BUFFER) |
				BIT(MALI_VPU_NALU_FORMAT_ONE_BYTE_LENGTH_FIELD) |
				BIT(MALI_VPU_NALU_FORMAT_TWO_BYTE_LENGTH_FIELD) |
				BIT(MALI_VPU_NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD),
		.is_custom = true,
		.flags = 0,
		.set = mali_vpu_ctrl_set_nalu_format,
	},
	{
		.cid = V4L2_CID_MALI_VPU_INTBUF_SIZE,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Internal buffer size",
		.min_val = 0,
		.max_val = INT_MAX,
		.default_val = 0,
		.step_or_mask = 1,
		.is_custom = true,
		.flags = 0,
		.set = mali_vpu_ctrl_set_intbuf_size,
	},
	{
		.cid = V4L2_CID_MALI_VPU_STREAM_ESCAPING,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Stream Escaping",
		.min_val = -1,
		.max_val = 1,
		.default_val = -1,
		.step_or_mask = 1,
		.is_custom = true,
		.flags = 0,
		.set = mali_vpu_ctrl_set_stream_escaping,
	},
	{
		.cid = V4L2_CID_MALI_VPU_HOST_PRINT_ENABLE,
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.name = "Firmware debug prints",
		.min_val = false,
		.max_val = true,
		.default_val = false,
		.step_or_mask = 1,
		.is_custom = true,
		.flags = MALI_VPU_CTRL_FLAG_DYNAMIC,
		.set = mali_vpu_ctrl_set_host_print_enable,
	},
	{
		.cid = V4L2_CID_MALI_VPU_IGNORE_STREAM_HEADERS,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Ignore Stream Headers",
		.min_val = -1,
		.max_val = 1,
		.default_val = -1,
		.step_or_mask = 1,
		.is_custom = true,
		.flags = 0,
		.set = mali_vpu_ctrl_set_ignore_stream_headers,
	},
	{
		.cid = V4L2_CID_MALI_VPU_FRAME_REORDERING,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "Frame Reordering",
		.min_val = -1,
		.max_val = 1,
		.default_val = -1,
		.step_or_mask = 1,
		.is_custom = true,
		.flags = 0,
		.set = mali_vpu_ctrl_set_frame_reordering,
	},
};

const struct mali_vpu_variant mali_vpu_v76_variant = {
	.dma_mask = DMA_BIT_MASK(40),
	.expected_hw_pid = MALI_VPU_HW_PID_V76,
	.fw_ver_major = MALI_VPU_FW_VER_MAJOR_V76,

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
	.ctrl_caps = {
		.dec_ctrls = mali_v76_dec_ctrls,
		.num_dec_ctrls = ARRAY_SIZE(mali_v76_dec_ctrls),
	},
};
