// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/errno.h>

#include "mali-vpu-buffer.h"
#include "mali-vpu-session.h"

int mali_vpu_vb2_buffer_init(struct vb2_buffer *vb2_buf)
{
	struct mali_vpu_session *sess = vb2_get_drv_priv(vb2_buf->vb2_queue);

	if (vb2_buf->num_planes > MALI_VPU_BUFFER_MAX_PLANES) {
		dev_err(sess->vpu_dev->dev,
			"buffer has too many planes: %u\n",
			vb2_buf->num_planes);
		return -EINVAL;
	}

	return 0;
}

int mali_vpu_vb2_buffer_prepare(struct vb2_buffer *vb2_buf)
{
	struct mali_vpu_session *sess = vb2_get_drv_priv(vb2_buf->vb2_queue);
	struct mali_vpu_port *port;
	struct v4l2_pix_format_mplane *pix_mp;
	u32 i;

	port = V4L2_TYPE_IS_OUTPUT(vb2_buf->vb2_queue->type) ? &sess->port_src :
							       &sess->port_dst;
	pix_mp = &port->curr_fmt.fmt.pix_mp;

	for (i = 0; i < pix_mp->num_planes; i++) {
		if (vb2_plane_size(vb2_buf, i) < pix_mp->plane_fmt[i].sizeimage)
			return -EINVAL;
	}

	return 0;
}

int mali_vpu_vb2_buffer_out_validate(struct vb2_buffer *vb2_buf)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb2_buf);

	v4l2_buf->field = V4L2_FIELD_NONE;

	return 0;
}
