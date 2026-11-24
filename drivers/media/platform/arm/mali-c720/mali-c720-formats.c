// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#include <linux/array_size.h>
#include <linux/media-bus-format.h>

#include "mali-c720-common.h"

static const struct mali_c720_buffer_cfg mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_MAX] = {
	[MALI_C720_BUFFER_CFG_BAYER24] = {
	  .data_format = 23,
	  .data_layout = MALI_C720_BUFFER_LAYOUT_NORMAL,
	  .data_align = MALI_C720_BUFFER_ALIGN_DENSE,
	  .num_fifos = 1,
	},

	[MALI_C720_BUFFER_CFG_BAYER20_DENSE] = {
	  .data_format = 19,
	  .data_layout = MALI_C720_BUFFER_LAYOUT_NORMAL,
	  .data_align = MALI_C720_BUFFER_ALIGN_DENSE,
	  .num_fifos = 1,
	},

	[MALI_C720_BUFFER_CFG_BAYER16] = {
	  .data_format = 15,
	  .data_layout = MALI_C720_BUFFER_LAYOUT_NORMAL,
	  .data_align = MALI_C720_BUFFER_ALIGN_DENSE,
	  .num_fifos = 1,
	},

	[MALI_C720_BUFFER_CFG_BAYER14_DENSE] = {
	  .data_format = 13,
	  .data_layout = MALI_C720_BUFFER_LAYOUT_NORMAL,
	  .data_align = MALI_C720_BUFFER_ALIGN_DENSE,
	  .num_fifos = 1,
	},

	[MALI_C720_BUFFER_CFG_BAYER12_DENSE] = {
	  .data_format = 11,
	  .data_layout = MALI_C720_BUFFER_LAYOUT_NORMAL,
	  .data_align = MALI_C720_BUFFER_ALIGN_DENSE,
	  .num_fifos = 1,
	},

	[MALI_C720_BUFFER_CFG_BAYER10_DENSE] = {
	  .data_format = 9,
	  .data_layout = MALI_C720_BUFFER_LAYOUT_NORMAL,
	  .data_align = MALI_C720_BUFFER_ALIGN_DENSE,
	  .num_fifos = 1,
	},

	[MALI_C720_BUFFER_CFG_BAYER8] = {
	  .data_format = 7,
	  .data_layout = MALI_C720_BUFFER_LAYOUT_NORMAL,
	  .data_align = MALI_C720_BUFFER_ALIGN_DENSE,
	  .num_fifos = 1,
	},

	[MALI_C720_BUFFER_CFG_3SEMIPLANE_24BIT] = {
	  .data_format = 48,
	  .data_layout = MALI_C720_BUFFER_LAYOUT_NORMAL,
	  .data_align = MALI_C720_BUFFER_ALIGN_DENSE,
	  .num_fifos = 1,
	},

	[MALI_C720_BUFFER_CFG_NV12] = {
	  .data_format = 144,
	  .data_layout = MALI_C720_BUFFER_LAYOUT_NORMAL,
	  .data_align = MALI_C720_BUFFER_ALIGN_DENSE,
	  .num_fifos = 2,
	},
};

static const struct mali_c720_mbus_format_info mali_c720_mbus_fmts[] = {
	{
	  .type = MALI_C720_FORMAT_BAYER_RGGB,
	  .depth = MALI_C720_FORMAT_24BIT,
	  .bit_depth = 24,
	  .code = MEDIA_BUS_FMT_SRGGB24_1X24,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER24],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_RGGB,
	  .depth = MALI_C720_FORMAT_20BIT,
	  .bit_depth = 20,
	  .code = MEDIA_BUS_FMT_SRGGB20_1X20,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER20_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_RGGB,
	  .depth = MALI_C720_FORMAT_16BIT,
	  .bit_depth = 16,
	  .code = MEDIA_BUS_FMT_SRGGB16_1X16,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER16],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_RGGB,
	  .depth = MALI_C720_FORMAT_14BIT,
	  .bit_depth = 14,
	  .code = MEDIA_BUS_FMT_SRGGB14_1X14,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER14_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_RGGB,
	  .depth = MALI_C720_FORMAT_12BIT,
	  .bit_depth = 12,
	  .code = MEDIA_BUS_FMT_SRGGB12_1X12,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER12_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_RGGB,
	  .depth = MALI_C720_FORMAT_10BIT,
	  .bit_depth = 10,
	  .code = MEDIA_BUS_FMT_SRGGB10_1X10,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER10_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_RGGB,
	  .depth = MALI_C720_FORMAT_8BIT,
	  .bit_depth = 8,
	  .code = MEDIA_BUS_FMT_SRGGB8_1X8,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER8],
	},

	{
	  .type = MALI_C720_FORMAT_BAYER_GRBG,
	  .depth = MALI_C720_FORMAT_24BIT,
	  .bit_depth = 24,
	  .code = MEDIA_BUS_FMT_SGRBG24_1X24,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER24],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GRBG,
	  .depth = MALI_C720_FORMAT_20BIT,
	  .bit_depth = 20,
	  .code = MEDIA_BUS_FMT_SGRBG20_1X20,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER20_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GRBG,
	  .depth = MALI_C720_FORMAT_16BIT,
	  .bit_depth = 16,
	  .code = MEDIA_BUS_FMT_SGRBG16_1X16,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER16],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GRBG,
	  .depth = MALI_C720_FORMAT_14BIT,
	  .bit_depth = 14,
	  .code = MEDIA_BUS_FMT_SGRBG14_1X14,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER14_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GRBG,
	  .depth = MALI_C720_FORMAT_12BIT,
	  .bit_depth = 12,
	  .code = MEDIA_BUS_FMT_SGRBG12_1X12,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER12_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GRBG,
	  .depth = MALI_C720_FORMAT_10BIT,
	  .bit_depth = 10,
	  .code = MEDIA_BUS_FMT_SGRBG10_1X10,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER10_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GRBG,
	  .depth = MALI_C720_FORMAT_8BIT,
	  .bit_depth = 8,
	  .code = MEDIA_BUS_FMT_SGRBG8_1X8,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER8],
	},

	{
	  .type = MALI_C720_FORMAT_BAYER_GBRG,
	  .depth = MALI_C720_FORMAT_24BIT,
	  .bit_depth = 24,
	  .code = MEDIA_BUS_FMT_SGBRG24_1X24,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER24],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GBRG,
	  .depth = MALI_C720_FORMAT_20BIT,
	  .bit_depth = 20,
	  .code = MEDIA_BUS_FMT_SGBRG20_1X20,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER20_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GBRG,
	  .depth = MALI_C720_FORMAT_16BIT,
	  .bit_depth = 16,
	  .code = MEDIA_BUS_FMT_SGBRG16_1X16,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER16],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GBRG,
	  .depth = MALI_C720_FORMAT_14BIT,
	  .bit_depth = 14,
	  .code = MEDIA_BUS_FMT_SGBRG14_1X14,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER14_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GBRG,
	  .depth = MALI_C720_FORMAT_12BIT,
	  .bit_depth = 12,
	  .code = MEDIA_BUS_FMT_SGBRG12_1X12,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER12_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GBRG,
	  .depth = MALI_C720_FORMAT_10BIT,
	  .bit_depth = 10,
	  .code = MEDIA_BUS_FMT_SGBRG10_1X10,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER10_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_GBRG,
	  .depth = MALI_C720_FORMAT_8BIT,
	  .bit_depth = 8,
	  .code = MEDIA_BUS_FMT_SGBRG8_1X8,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER8],
	},

	{
	  .type = MALI_C720_FORMAT_BAYER_BGGR,
	  .depth = MALI_C720_FORMAT_24BIT,
	  .bit_depth = 24,
	  .code = MEDIA_BUS_FMT_SBGGR24_1X24,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER24],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_BGGR,
	  .depth = MALI_C720_FORMAT_20BIT,
	  .bit_depth = 20,
	  .code = MEDIA_BUS_FMT_SBGGR20_1X20,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER20_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_BGGR,
	  .depth = MALI_C720_FORMAT_16BIT,
	  .bit_depth = 16,
	  .code = MEDIA_BUS_FMT_SBGGR16_1X16,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER16],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_BGGR,
	  .depth = MALI_C720_FORMAT_14BIT,
	  .bit_depth = 14,
	  .code = MEDIA_BUS_FMT_SBGGR14_1X14,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER14_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_BGGR,
	  .depth = MALI_C720_FORMAT_12BIT,
	  .bit_depth = 12,
	  .code = MEDIA_BUS_FMT_SBGGR12_1X12,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER12_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_BGGR,
	  .depth = MALI_C720_FORMAT_10BIT,
	  .bit_depth = 10,
	  .code = MEDIA_BUS_FMT_SBGGR10_1X10,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER10_DENSE],
	},
	{
	  .type = MALI_C720_FORMAT_BAYER_BGGR,
	  .depth = MALI_C720_FORMAT_8BIT,
	  .bit_depth = 8,
	  .code = MEDIA_BUS_FMT_SBGGR8_1X8,
	  .buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_BAYER8],
	},

	{
	  .type = MALI_C720_FORMAT_RGB,
	  .depth = MALI_C720_FORMAT_16BIT,
	  .bit_depth = 16,
	  .code = MEDIA_BUS_FMT_RGB161616_1X48
	},
	{
	  .type = MALI_C720_FORMAT_RGB,
	  .depth = MALI_C720_FORMAT_12BIT,
	  .bit_depth = 12,
	  .code = MEDIA_BUS_FMT_RGB121212_1X36
	},

	{
	  .type = MALI_C720_FORMAT_YUV,
	  .depth = MALI_C720_FORMAT_16BIT,
	  .bit_depth = 16,
	  .code = MEDIA_BUS_FMT_YUV16_1X48
	},
};

static const struct mali_c720_v4l2_format_info mali_c720_v4l2_fmts[] = {
	/* RGB formats */
	{
		.fourcc = V4L2_PIX_FMT_RGB24,
		.mbus_code = MEDIA_BUS_FMT_RGB161616_1X48,

		.comp_planes = 1,
		.bpp = { 24, 0, 0, 0 },
		.hdiv = 1,
		.vdiv = 1,

		.supported_mask = BIT(MALI_C720_CAP_DEV_FR0_RGB),

		.buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_3SEMIPLANE_24BIT],
	},

	/* YUV formats */
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.mbus_code = MEDIA_BUS_FMT_YUV16_1X48,

		.comp_planes = 2,
		.bpp = { 8, 16, 0, 0 },
		.hdiv = 2,
		.vdiv = 2,

		.supported_mask = BIT(MALI_C720_CAP_DEV_FR0_YUV),

		.buffer_cfg = &mali_c720_buffer_configs[MALI_C720_BUFFER_CFG_NV12],
	},
};

int mali_c720_validate_mbus_format_match(const struct mali_c720_mbus_format_info *format,
					 const struct mali_c720_mbus_format_match *match)
{
	return (match->type_mask & format->type) &&
	       (match->depth_mask & format->depth);
}

const struct mali_c720_mbus_format_info *
mali_c720_get_mbus_config_by_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mali_c720_mbus_fmts); i++) {
		if (mali_c720_mbus_fmts[i].code == code)
			return &mali_c720_mbus_fmts[i];
	}

	return NULL;
}

const struct mali_c720_mbus_format_info *
mali_c720_get_mbus_config_by_matched_index(unsigned int index,
					   const struct mali_c720_mbus_format_match *match)
{
	unsigned int i;
	unsigned int count = 0;

	for (i = 0; i < ARRAY_SIZE(mali_c720_mbus_fmts); i++) {
		if (!mali_c720_validate_mbus_format_match(&mali_c720_mbus_fmts[i],
							  match))
			continue;

		if (count++ == index)
			return &mali_c720_mbus_fmts[i];
	}

	return NULL;
}

const struct mali_c720_mbus_format_info *
mali_c720_shift_mbus_format(const struct mali_c720_mbus_format_info *format,
			    enum mali_c720_format_depth out_depth)
{
	unsigned int i;

	if (format->depth == out_depth)
		return format;

	for (i = 0; i < ARRAY_SIZE(mali_c720_mbus_fmts); i++) {
		if (mali_c720_mbus_fmts[i].type == format->type &&
		    mali_c720_mbus_fmts[i].depth == out_depth)
			return &mali_c720_mbus_fmts[i];
	}

	return NULL;
}

bool
mali_c720_mbus_code_supports_fmt(const struct mali_c720_v4l2_format_info *fmt,
				 u32 code)
{
	/* Display all formats if no code is provided */
	if (!code)
		return true;

	return fmt->mbus_code == code;
}

bool
mali_c720_format_supported_by_vdev(const struct mali_c720_v4l2_format_info *fmt,
				   unsigned int dev_id)
{
	if (fmt->supported_mask & BIT(dev_id))
		return true;

	return false;
}

static int
mali_c720_validate_v4l2_format_match(const struct mali_c720_v4l2_format_info *format,
				     const struct mali_c720_v4l2_format_match *match)
{
	if (!mali_c720_mbus_code_supports_fmt(format, match->mbus_code))
		return 0;

	if (!mali_c720_format_supported_by_vdev(format, match->dev_id))
		return 0;

	return 1;
}

const struct mali_c720_v4l2_format_info *
mali_c720_get_v4l2_config_by_fourcc(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mali_c720_v4l2_fmts); i++) {
		if (mali_c720_v4l2_fmts[i].fourcc == fourcc)
			return &mali_c720_v4l2_fmts[i];
	}

	return NULL;
}

const struct mali_c720_v4l2_format_info *
mali_c720_get_v4l2_config_by_matched_index(unsigned int index,
					   const struct mali_c720_v4l2_format_match *match)
{
	unsigned int i;
	unsigned int count = 0;

	for (i = 0; i < ARRAY_SIZE(mali_c720_v4l2_fmts); i++) {
		if (!mali_c720_validate_v4l2_format_match(&mali_c720_v4l2_fmts[i], match))
			continue;

		if (count++ == index)
			return &mali_c720_v4l2_fmts[i];
	}

	return NULL;
}
