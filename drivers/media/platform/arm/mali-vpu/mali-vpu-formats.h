/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_FORMATS_H__
#define __MALI_VPU_FORMATS_H__

#include <linux/types.h>
#include <linux/videodev2.h>

struct mali_vpu_session;

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

const struct mali_vpu_format_desc *mali_vpu_find_format(struct mali_vpu_session *sess,
							u32 pixfmt,
							enum v4l2_buf_type type);
int mali_vpu_fill_pixmp_fmt(const struct mali_vpu_format_desc *fmt,
			    u32 width, u32 height,
			    struct v4l2_pix_format_mplane *pix_mp);
bool mali_vpu_is_coded_q(const struct mali_vpu_session *sess, enum v4l2_buf_type type);
bool mali_vpu_is_raw_q(const struct mali_vpu_session *sess, enum v4l2_buf_type type);

#endif /* __MALI_VPU_FORMATS_H__ */
