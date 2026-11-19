// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <media/v4l2-common.h>

#include "mali-vpu-formats.h"
#include "mali-vpu-session.h"

bool mali_vpu_is_coded_q(const struct mali_vpu_session *sess, enum v4l2_buf_type type)
{
	if (sess->role == MALI_VPU_DECODER)
		return V4L2_TYPE_IS_OUTPUT(type);
	return V4L2_TYPE_IS_CAPTURE(type);
}

bool mali_vpu_is_raw_q(const struct mali_vpu_session *sess, enum v4l2_buf_type type)
{
	if (sess->role == MALI_VPU_DECODER)
		return V4L2_TYPE_IS_CAPTURE(type);
	return V4L2_TYPE_IS_OUTPUT(type);
}

const struct mali_vpu_format_desc *
mali_vpu_find_format(struct mali_vpu_session *sess, u32 pixfmt,
		     enum v4l2_buf_type type)
{
	const struct mali_vpu_format_list *fmt_list;
	u32 i;

	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		fmt_list = &sess->port_src.supported_fmts;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		fmt_list = &sess->port_dst.supported_fmts;
		break;
	default:
		return NULL;
	}

	for (i = 0; i < fmt_list->num_fmts; i++) {
		if (fmt_list->fmts[i].fourcc == pixfmt)
			return &fmt_list->fmts[i];
	}

	return NULL;
}

int mali_vpu_fill_pixmp_fmt(const struct mali_vpu_format_desc *fmt,
			    u32 width, u32 height,
			    struct v4l2_pix_format_mplane *pix_mp)
{
	u32 sizeimage;
	int ret;

	memset(pix_mp->reserved, 0, sizeof(pix_mp->reserved));
	memset(pix_mp->plane_fmt, 0, sizeof(pix_mp->plane_fmt));
	pix_mp->flags = 0;

	if (fmt->flags & V4L2_FMT_FLAG_COMPRESSED) {
		pix_mp->width = width;
		pix_mp->height = height;
		pix_mp->pixelformat = fmt->fourcc;
		pix_mp->num_planes = 1;
		pix_mp->plane_fmt[0].bytesperline = 0;

		/*
		 * Use one byte per pixel as the default compressed sizeimage,
		 * with a minimum of one page, aligned to a page boundary.
		 */
		if (check_mul_overflow(width, height, &sizeimage))
			return -EOVERFLOW;

		sizeimage = max_t(u32, PAGE_SIZE, sizeimage);

		if (sizeimage > U32_MAX - (PAGE_SIZE - 1))
			return -EOVERFLOW;

		pix_mp->plane_fmt[0].sizeimage = PAGE_ALIGN(sizeimage);

	} else {
		ret = v4l2_fill_pixfmt_mp(pix_mp, fmt->fourcc, width, height);
		if (ret)
			return ret;
	}

	return 0;
}
