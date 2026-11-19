// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/errno.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-sg.h>

#include "mali-vpu-buffer.h"
#include "mali-vpu-session.h"
#include "mali-vpu-vb2.h"

#define MALI_VPU_MIN_BUFFERS 1

static int mali_vpu_vb2_queue_setup(struct vb2_queue *q,
				    unsigned int *num_buffers __always_unused,
				    unsigned int *num_planes,
				    unsigned int sizes[],
				    struct device *alloc_devs[] __always_unused)
{
	struct mali_vpu_session *sess = vb2_get_drv_priv(q);
	struct mali_vpu_port *port = V4L2_TYPE_IS_OUTPUT(q->type) ?
					&sess->port_src :
					&sess->port_dst;
	struct v4l2_pix_format_mplane *pix_mp = &port->curr_fmt.fmt.pix_mp;
	u32 i;

	if (!pix_mp->num_planes || pix_mp->num_planes > MALI_VPU_BUFFER_MAX_PLANES)
		return -EINVAL;

	for (i = 0; i < pix_mp->num_planes; i++) {
		if (!pix_mp->plane_fmt[i].sizeimage)
			return -EINVAL;
	}

	if (*num_planes) {
		if (*num_planes != pix_mp->num_planes)
			return -EINVAL;

		for (i = 0; i < pix_mp->num_planes; i++) {
			if (sizes[i] < pix_mp->plane_fmt[i].sizeimage)
				return -EINVAL;
		}
	} else {
		*num_planes = pix_mp->num_planes;
		for (i = 0; i < pix_mp->num_planes; i++)
			sizes[i] = pix_mp->plane_fmt[i].sizeimage;
	}

	return 0;
}

static void mali_vpu_vb2_buffer_queue(struct vb2_buffer *vb2_buf)
{
	struct mali_vpu_session *sess = vb2_get_drv_priv(vb2_buf->vb2_queue);
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb2_buf);
	int ret;

	/* Update vb2 metadata and payload info onto mali_vpu_buffer. */
	ret = mali_vpu_buffer_update_info_from_v4l2(vb2_buf);
	if (ret)
		goto err_buf_done;

	v4l2_m2m_buf_queue(sess->m2m_ctx, v4l2_buf);

	ret = mali_vpu_session_qbuf(sess, vb2_buf);
	if (ret)
		goto err_m2m_buf_remove;

	return;

err_m2m_buf_remove:
	if (V4L2_TYPE_IS_OUTPUT(vb2_buf->type))
		v4l2_m2m_src_buf_remove_by_buf(sess->m2m_ctx, v4l2_buf);
	else
		v4l2_m2m_dst_buf_remove_by_buf(sess->m2m_ctx, v4l2_buf);

err_buf_done:
	v4l2_m2m_buf_done(v4l2_buf, VB2_BUF_STATE_ERROR);

}

static int mali_vpu_vb2_prepare_streaming(struct vb2_queue *q)
{
	struct mali_vpu_session *sess = vb2_get_drv_priv(q);
	int ret;

	if (mali_vpu_is_coded_q(sess, q->type)) {
		ret = mali_vpu_session_request_fw_load(sess);
		if (ret)
			return ret;
	}

	return mali_vpu_session_prepare_streaming(sess, q->type);
}

static int mali_vpu_vb2_start_streaming(struct vb2_queue *q,
					unsigned int count __always_unused)
{
	struct mali_vpu_session *sess = vb2_get_drv_priv(q);

	v4l2_m2m_update_start_streaming_state(sess->m2m_ctx, q);
	return 0;
}

static void mali_vpu_vb2_stop_streaming(struct vb2_queue *q)
{
	struct mali_vpu_session *sess = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *v4l2_buf;
	int ret;

	ret = mali_vpu_session_streamoff(sess, q->type);
	if (ret)
		dev_warn(sess->vpu_dev->dev, "failed to stop streaming: %d\n", ret);

	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		while ((v4l2_buf = v4l2_m2m_src_buf_remove(sess->m2m_ctx)))
			v4l2_m2m_buf_done(v4l2_buf, VB2_BUF_STATE_ERROR);
	} else {
		while ((v4l2_buf = v4l2_m2m_dst_buf_remove(sess->m2m_ctx)))
			v4l2_m2m_buf_done(v4l2_buf, VB2_BUF_STATE_ERROR);
	}

	v4l2_m2m_update_stop_streaming_state(sess->m2m_ctx, q);
}

static void mali_vpu_vb2_unprepare_streaming(struct vb2_queue *q)
{
	struct mali_vpu_session *sess = vb2_get_drv_priv(q);
	int ret;

	ret = mali_vpu_session_streamoff(sess, q->type);
	if (ret)
		dev_warn(sess->vpu_dev->dev,
			 "failed to unprepare streaming: %d\n", ret);

	if (!vb2_start_streaming_called(q))
		v4l2_m2m_update_stop_streaming_state(sess->m2m_ctx, q);
}

static const struct vb2_ops mali_vpu_vb2_ops = {
	.queue_setup		= mali_vpu_vb2_queue_setup,
	.buf_out_validate	= mali_vpu_vb2_buffer_out_validate,
	.buf_init		= mali_vpu_vb2_buffer_init,
	.buf_prepare		= mali_vpu_vb2_buffer_prepare,
	.buf_queue		= mali_vpu_vb2_buffer_queue,
	.buf_cleanup		= mali_vpu_vb2_buffer_cleanup,
	.prepare_streaming	= mali_vpu_vb2_prepare_streaming,
	.start_streaming	= mali_vpu_vb2_start_streaming,
	.stop_streaming		= mali_vpu_vb2_stop_streaming,
	.unprepare_streaming	= mali_vpu_vb2_unprepare_streaming,
};

static void mali_vpu_queue_init_type(struct mali_vpu_session *sess,
				     struct vb2_queue *vq,
				     enum v4l2_buf_type type)
{
	vq->type = type;
	vq->io_modes = VB2_MMAP | VB2_DMABUF;
	vq->dev = sess->vpu_dev->dev;
	vq->lock = &sess->vq_lock;
	vq->ops = &mali_vpu_vb2_ops;
	vq->mem_ops = &vb2_dma_sg_memops;
	vq->drv_priv = sess;
	vq->buf_struct_size = sizeof(struct mali_vpu_buffer);
	vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	vq->min_queued_buffers = MALI_VPU_MIN_BUFFERS;
}

int mali_vpu_queue_init(void *priv, struct vb2_queue *src_vq,
			struct vb2_queue *dst_vq)
{
	struct mali_vpu_session *sess = priv;
	int ret;

	mali_vpu_queue_init_type(sess, src_vq, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	mali_vpu_queue_init_type(sess, dst_vq, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	return vb2_queue_init(dst_vq);
}
