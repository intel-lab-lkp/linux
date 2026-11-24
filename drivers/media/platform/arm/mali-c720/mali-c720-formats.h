/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ARM Mali-C720 ISP Driver - Image signal processor
 *
 * Copyright (C) 2026 Arm Ltd
 */

#ifndef MALI_C720_FORMATS_H
#define MALI_C720_FORMATS_H

#include <linux/bits.h>
#include <linux/types.h>

enum mali_c720_format_type {
	MALI_C720_FORMAT_BAYER_RGGB = BIT(0),
	MALI_C720_FORMAT_BAYER_GRBG = BIT(1),
	MALI_C720_FORMAT_BAYER_GBRG = BIT(2),
	MALI_C720_FORMAT_BAYER_BGGR = BIT(3),
	MALI_C720_FORMAT_RGB = BIT(4),
	MALI_C720_FORMAT_YUV = BIT(5),
};

#define MALI_C720_FORMAT_BAYER		(MALI_C720_FORMAT_BAYER_RGGB | \
					 MALI_C720_FORMAT_BAYER_GRBG | \
					 MALI_C720_FORMAT_BAYER_GBRG | \
					 MALI_C720_FORMAT_BAYER_BGGR)

enum mali_c720_format_depth {
	MALI_C720_FORMAT_8BIT = BIT(0),
	MALI_C720_FORMAT_10BIT = BIT(1),
	MALI_C720_FORMAT_12BIT = BIT(2),
	MALI_C720_FORMAT_14BIT = BIT(3),
	MALI_C720_FORMAT_16BIT = BIT(4),
	MALI_C720_FORMAT_20BIT = BIT(5),
	MALI_C720_FORMAT_24BIT = BIT(6),
};

enum mali_c720_buffer_layout_ids {
	MALI_C720_BUFFER_LAYOUT_NORMAL,
	MALI_C720_BUFFER_LAYOUT_ALTERNATE,
};

enum mali_c720_buffer_align_ids {
	MALI_C720_BUFFER_ALIGN_DENSE = 0,
	MALI_C720_BUFFER_ALIGN_LSB = 2,
	MALI_C720_BUFFER_ALIGN_MSB = 3,
};

enum mali_c720_buffer_cfg_ids {
	/* Bayer */
	MALI_C720_BUFFER_CFG_BAYER24,
	MALI_C720_BUFFER_CFG_BAYER20_DENSE,
	MALI_C720_BUFFER_CFG_BAYER16,
	MALI_C720_BUFFER_CFG_BAYER14_DENSE,
	MALI_C720_BUFFER_CFG_BAYER12_DENSE,
	MALI_C720_BUFFER_CFG_BAYER10_DENSE,
	MALI_C720_BUFFER_CFG_BAYER8,

	/* Packed/Planar for RGB and YUV */
	MALI_C720_BUFFER_CFG_3SEMIPLANE_24BIT,

	/* YUV */
	MALI_C720_BUFFER_CFG_NV12,

	MALI_C720_BUFFER_CFG_MAX,
};

struct mali_c720_buffer_cfg {
	unsigned int data_format;
	unsigned int data_layout;
	unsigned int data_align;
	unsigned int num_fifos;
};

struct mali_c720_v4l2_format_match {
	u32 mbus_code;
	unsigned int dev_id;
};

struct mali_c720_v4l2_format_info {
	u32 fourcc;
	unsigned int mbus_code;

	unsigned int comp_planes;
	unsigned int bpp[4];
	unsigned int hdiv;
	unsigned int vdiv;

	/* Mask designating which of the available video devs support this format. */
	unsigned int supported_mask;

	const struct mali_c720_buffer_cfg *buffer_cfg;
};

struct mali_c720_mbus_format_match {
	unsigned int type_mask;
	unsigned int depth_mask;
};

struct mali_c720_mbus_format_info {
	enum mali_c720_format_type type;
	enum mali_c720_format_depth depth;
	unsigned int bit_depth;
	u32 code;

	const struct mali_c720_buffer_cfg *buffer_cfg;
};

int mali_c720_validate_mbus_format_match(const struct mali_c720_mbus_format_info *format,
					 const struct mali_c720_mbus_format_match *match);
const struct mali_c720_mbus_format_info *
mali_c720_get_mbus_config_by_code(u32 code);
const struct mali_c720_mbus_format_info *
mali_c720_get_mbus_config_by_matched_index(unsigned int index,
					   const struct mali_c720_mbus_format_match *match);
const struct mali_c720_mbus_format_info *
mali_c720_shift_mbus_format(const struct mali_c720_mbus_format_info *format,
			    enum mali_c720_format_depth out_depth);

bool
mali_c720_mbus_code_supports_fmt(const struct mali_c720_v4l2_format_info *fmt,
				 u32 code);
bool
mali_c720_format_supported_by_vdev(const struct mali_c720_v4l2_format_info *fmt,
				   unsigned int dev_id);
const struct mali_c720_v4l2_format_info *
mali_c720_get_v4l2_config_by_fourcc(u32 fourcc);
const struct mali_c720_v4l2_format_info *
mali_c720_get_v4l2_config_by_matched_index(unsigned int index,
					   const struct mali_c720_v4l2_format_match *match);

#endif /* MALI_C720_FORMATS_H */
