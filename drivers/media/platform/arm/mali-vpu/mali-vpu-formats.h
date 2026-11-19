/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_FORMATS_H__
#define __MALI_VPU_FORMATS_H__

#include <linux/types.h>

enum mali_vpu_codec_fmt {
	MALI_VPU_FORMAT_CODEC_H264,
};

struct mali_vpu_format_desc {
	u32 fourcc;
	u16 mve_frame_fmt;
	u16 codec_fmt;
	u32 flags;
};

struct mali_vpu_format_list {
	const struct mali_vpu_format_desc *fmts;
	u32 num_fmts;
};

#endif /* __MALI_VPU_FORMATS_H__ */
