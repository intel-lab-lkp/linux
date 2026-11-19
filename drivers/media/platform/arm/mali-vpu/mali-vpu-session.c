// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <media/v4l2-common.h>
#include <media/v4l2-mem2mem.h>

#include "mali-vpu-session.h"
#include "mali-vpu-dec.h"
#include "mali-vpu-variant.h"
#include "mali-vpu-vb2.h"

static int mali_vpu_set_port_fmt(struct mali_vpu_port *port,
				 enum v4l2_buf_type type,
				 u32 width, u32 height)
{
	struct v4l2_pix_format_mplane *pix_mp = &port->curr_fmt.fmt.pix_mp;
	const struct mali_vpu_format_desc *fmt_default;
	int ret;

	if (!port->supported_fmts.fmts || !port->supported_fmts.num_fmts)
		return -EINVAL;

	fmt_default = &port->supported_fmts.fmts[0];

	memset(&port->curr_fmt, 0, sizeof(port->curr_fmt));
	port->curr_fmt.type = type;

	if (fmt_default->flags & V4L2_FMT_FLAG_COMPRESSED) {
		pix_mp->pixelformat = fmt_default->fourcc;
		pix_mp->num_planes = 1;
		pix_mp->width = width;
		pix_mp->height = height;
		pix_mp->plane_fmt[0].bytesperline = 0;

		/*
		 * Use one byte per pixel as the default compressed sizeimage,
		 * with a minimum of one page, aligned to a page boundary.
		 */
		pix_mp->plane_fmt[0].sizeimage = PAGE_ALIGN(max_t(u32, PAGE_SIZE, width * height));
	} else {
		ret = v4l2_fill_pixfmt_mp(pix_mp, fmt_default->fourcc, width, height);
		if (ret)
			return ret;
	}

	pix_mp->field = V4L2_FIELD_NONE;

	return 0;
}

static int mali_vpu_set_port_defaults(struct mali_vpu_session *sess)
{
	const struct mali_vpu_framesize_caps *fmsz = &sess->vpu_dev->variant->frmsz_caps;
	int ret;

	ret = mali_vpu_set_port_fmt(&sess->port_src, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				    fmsz->width_default, fmsz->height_default);
	if (ret)
		return ret;

	return mali_vpu_set_port_fmt(&sess->port_dst, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				    fmsz->width_default, fmsz->height_default);
}

int mali_vpu_session_init(struct mali_vpu_session *sess)
{
	int ret;

	mutex_init(&sess->vq_lock);

	if (sess->role != MALI_VPU_DECODER) {
		ret = -EINVAL;
		goto err_mutex_destroy;
	}

	mali_vpu_session_init_dec(sess);

	ret = mali_vpu_set_port_defaults(sess);
	if (ret)
		goto err_mutex_destroy;

	sess->m2m_ctx = v4l2_m2m_ctx_init(sess->vpu_dev->m2m_dev, sess, mali_vpu_queue_init);
	if (IS_ERR(sess->m2m_ctx)) {
		ret = PTR_ERR(sess->m2m_ctx);
		goto err_mutex_destroy;
	}

	return 0;

err_mutex_destroy:
	mutex_destroy(&sess->vq_lock);
	return ret;
}

void mali_vpu_session_cleanup(struct mali_vpu_session *sess)
{
	mutex_lock(&sess->vq_lock);

	if (sess->m2m_ctx) {
		v4l2_m2m_ctx_release(sess->m2m_ctx);
		sess->m2m_ctx = NULL;
		sess->fh.m2m_ctx = NULL;
	}

	mutex_unlock(&sess->vq_lock);
	mutex_destroy(&sess->vq_lock);
}
