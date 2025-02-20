// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 * Author: Fish Wu <fish.wu@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/mtk_aie_v4l2_controls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include "mtk_aie.h"

#define V4L2_CID_MTK_AIE_MAX 2
#define Y2R_CFG_SIZE 34
#define RS_CFG_SIZE 30
#define FD_CFG_SIZE 56

static const struct mtk_aie_variant aie_31_drvdata = {
	.y2r_cfg_size = Y2R_CFG_SIZE,
	.rs_cfg_size = RS_CFG_SIZE,
	.fd_cfg_size = FD_CFG_SIZE,
};

static const struct of_device_id mtk_aie_of_ids[] = {
	{
		.compatible = "mediatek,mt8188-aie",
		.data = &aie_31_drvdata,
	},
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, mtk_aie_of_ids);

static const struct v4l2_pix_format_mplane mtk_aie_img_fmts[] = {
	{
		.pixelformat = V4L2_PIX_FMT_NV16M,
		.num_planes = 2,
	},
	{
		.pixelformat = V4L2_PIX_FMT_NV61M,
		.num_planes = 2,
	},
	{
		.pixelformat = V4L2_PIX_FMT_YUYV,
		.num_planes = 1,
	},
	{
		.pixelformat = V4L2_PIX_FMT_YVYU,
		.num_planes = 1,
	},
	{
		.pixelformat = V4L2_PIX_FMT_UYVY,
		.num_planes = 1,
	},
	{
		.pixelformat = V4L2_PIX_FMT_VYUY,
		.num_planes = 1,
	},
	{
		.pixelformat = V4L2_PIX_FMT_GREY,
		.num_planes = 1,
	},
	{
		.pixelformat = V4L2_PIX_FMT_NV12M,
		.num_planes = 2,
	},
	{
		.pixelformat = V4L2_PIX_FMT_NV12,
		.num_planes = 1,
	},
};

#define NUM_FORMATS ARRAY_SIZE(mtk_aie_img_fmts)

static inline struct mtk_aie_ctx *fh_to_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct mtk_aie_ctx, fh);
}

static inline struct mtk_aie_ctx *ctrl_to_ctx(const struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct mtk_aie_ctx, hdl);
}

static void mtk_aie_hw_job_finish(struct mtk_aie_dev *fd,
				  enum vb2_buffer_state vb_state)
{
	struct vb2_v4l2_buffer *src_vbuf, *dst_vbuf;
	struct mtk_aie_ctx *ctx;

	pm_runtime_put(fd->dev);
	ctx = v4l2_m2m_get_curr_priv(fd->m2m_dev);
	if (!ctx) {
		dev_err(fd->dev, "Failed to do v4l2_m2m_get_curr_priv!\n");
	} else {
		src_vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		dst_vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (src_vbuf && dst_vbuf)
			v4l2_m2m_buf_copy_metadata(src_vbuf, dst_vbuf, true);
		if (src_vbuf)
			v4l2_m2m_buf_done(src_vbuf, vb_state);
		if (dst_vbuf)
			v4l2_m2m_buf_done(dst_vbuf, vb_state);
		if (src_vbuf && dst_vbuf)
			v4l2_m2m_job_finish(fd->m2m_dev, ctx->fh.m2m_ctx);
	}
	complete_all(&fd->fd_job_finished);
}

static int mtk_aie_hw_job_exec(struct mtk_aie_dev *fd)
{
	pm_runtime_get_sync(fd->dev);

	reinit_completion(&fd->fd_job_finished);
	schedule_delayed_work(&fd->job_timeout_work,
			      msecs_to_jiffies(MTK_FD_HW_TIMEOUT_IN_MSEC));

	return 0;
}

static int mtk_aie_vb2_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *v4l2_buf = to_vb2_v4l2_buffer(vb);

	if (v4l2_buf->field == V4L2_FIELD_ANY)
		v4l2_buf->field = V4L2_FIELD_NONE;
	if (v4l2_buf->field != V4L2_FIELD_NONE)
		return -EINVAL;

	return 0;
}

static int mtk_aie_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vb2_queue *vq = vb->vb2_queue;
	struct mtk_aie_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format_mplane *pixfmt;
	struct device *dev = ctx->dev;

	switch (vq->type) {
	case V4L2_BUF_TYPE_META_CAPTURE:
		if (vb2_plane_size(vb, 0) < ctx->dst_fmt.buffersize) {
			dev_err(dev, "meta size %lu is too small\n", vb2_plane_size(vb, 0));
			return -EINVAL;
		}
		vb2_set_plane_payload(vb, 0, ctx->dst_fmt.buffersize);
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		pixfmt = &ctx->src_fmt;

		if (vbuf->field == V4L2_FIELD_ANY)
			vbuf->field = V4L2_FIELD_NONE;

		if (vb->num_planes > 2 || vbuf->field != V4L2_FIELD_NONE) {
			dev_dbg(dev, "plane %d or field %d not supported\n",
				vb->num_planes, vbuf->field);
			return -EINVAL;
		}

		if (vb2_plane_size(vb, 0) < pixfmt->plane_fmt[0].sizeimage) {
			dev_dbg(dev, "plane 0 %lu is too small than %x\n",
				vb2_plane_size(vb, 0), pixfmt->plane_fmt[0].sizeimage);
			return -EINVAL;
		}
		vb2_set_plane_payload(vb, 0, pixfmt->plane_fmt[0].sizeimage);

		if (pixfmt->num_planes == 2 &&
		    vb2_plane_size(vb, 1) < pixfmt->plane_fmt[1].sizeimage) {
			dev_dbg(dev, "plane 1 %lu is too small than %x\n",
				vb2_plane_size(vb, 1), pixfmt->plane_fmt[1].sizeimage);
			return -EINVAL;
		}
		vb2_set_plane_payload(vb, 1, pixfmt->plane_fmt[1].sizeimage);
		break;
	}

	return 0;
}

static void mtk_aie_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct mtk_aie_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int mtk_aie_vb2_queue_setup(struct vb2_queue *vq,
				   unsigned int *num_buffers,
				   unsigned int *num_planes,
				   unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct mtk_aie_ctx *ctx = vb2_get_drv_priv(vq);
	struct device *dev = ctx->dev;
	unsigned int size[2];
	unsigned int plane;

	switch (vq->type) {
	case V4L2_BUF_TYPE_META_CAPTURE:
		size[0] = ctx->dst_fmt.buffersize;
		size[1] = 0;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		size[0] = ctx->src_fmt.plane_fmt[0].sizeimage;
		size[1] = ctx->src_fmt.plane_fmt[1].sizeimage;
		break;
	default:
		size[0] = 0;
		size[1] = 0;
	}

	dev_dbg(dev, "vq type = %d, size[0] = %d, size[1] = %d\n",
		vq->type, size[0], size[1]);

	if (*num_planes > 2)
		return -EINVAL;

	*num_buffers = clamp_val(*num_buffers, 1, VB2_MAX_FRAME);

	if (*num_planes == 0) {
		if (vq->type == V4L2_BUF_TYPE_META_CAPTURE) {
			sizes[0] = ctx->dst_fmt.buffersize;
			*num_planes = 1;
			return 0;
		}

		*num_planes = ctx->src_fmt.num_planes;
		if (*num_planes > 2)
			return -EINVAL;
		for (plane = 0; plane < *num_planes; plane++)
			sizes[plane] = ctx->src_fmt.plane_fmt[plane].sizeimage;

		return 0;
	}

	return 0;
}

static int mtk_aie_vb2_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mtk_aie_ctx *ctx = vb2_get_drv_priv(vq);
	struct mtk_aie_dev *fd;

	if (!ctx)
		return -EINVAL;

	fd = ctx->fd_dev;
	if (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && (++fd->fd_stream_count == 1))
		return aie_init(ctx->fd_dev, &ctx->fd_dev->ctx->user_init);

	return 0;
}

static void mtk_aie_job_timeout_work(struct work_struct *work)
{
	struct mtk_aie_dev *fd =
		container_of(work, struct mtk_aie_dev, job_timeout_work.work);

	dev_err(fd->dev, "FD Job timeout!");

	dev_dbg(fd->dev, "%s result result1: %x, %x, %x", __func__,
		readl(fd->fd_base + AIE_RESULT_0_REG),
		readl(fd->fd_base + AIE_RESULT_1_REG),
		readl(fd->fd_base + AIE_DMA_CTL_REG));

	fd->aie_cfg->irq_status = readl(fd->fd_base + AIE_INT_EN_REG);

	if (fd->aie_cfg->sel_mode == ATTRIBUTEMODE) {
		dev_dbg(fd->dev, "w_idx = %d, r_idx = %d\n",
			fd->attr_para->w_idx, fd->attr_para->r_idx);
	}

	aie_irqhandle(fd);
	aie_reset(fd);
	atomic_dec(&fd->num_composing);
	mtk_aie_hw_job_finish(fd, VB2_BUF_STATE_ERROR);
	wake_up(&fd->flushing_waitq);
}

static void mtk_aie_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct mtk_aie_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	struct v4l2_m2m_queue_ctx *queue_ctx;
	struct mtk_aie_dev *fd = ctx->fd_dev;
	struct vb2_v4l2_buffer *vb = NULL;
	int ret;

	/* Waiting Job Finish */
	ret = wait_for_completion_timeout(&fd->fd_job_finished, msecs_to_jiffies(1000));
	if (!ret)
		dev_err(fd->dev, "Wait job finish timeout\n");

	if (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		fd->fd_stream_count--;
		if (fd->fd_stream_count > 0)
			dev_dbg(fd->dev, "Stop: fd_stream_count = %d\n", fd->fd_stream_count);
		else
			aie_uninit(fd);
	}

	queue_ctx = V4L2_TYPE_IS_OUTPUT(vq->type) ? &m2m_ctx->out_q_ctx :
		&m2m_ctx->cap_q_ctx;
	while ((vb = v4l2_m2m_buf_remove(queue_ctx)))
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
}

static void mtk_aie_vb2_request_complete(struct vb2_buffer *vb)
{
	struct mtk_aie_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->hdl);
}

static int mtk_aie_querycap(struct file *file, void *fh,
			    struct v4l2_capability *cap)
{
	struct mtk_aie_dev *fd = video_drvdata(file);
	struct device *dev = fd->dev;

	strscpy(cap->driver, dev_driver_string(dev), sizeof(cap->driver));
	strscpy(cap->card, dev_driver_string(dev), sizeof(cap->card));

	cap->device_caps = V4L2_CAP_VIDEO_OUTPUT_MPLANE |
			   V4L2_CAP_STREAMING | V4L2_CAP_META_CAPTURE;
	cap->capabilities = V4L2_CAP_DEVICE_CAPS | cap->device_caps;

	return 0;
}

static int mtk_aie_enum_fmt_out_mp(struct file *file, void *fh,
				   struct v4l2_fmtdesc *f)
{
	if (f->index >= NUM_FORMATS)
		return -EINVAL;

	f->pixelformat = mtk_aie_img_fmts[f->index].pixelformat;
	return 0;
}

static void mtk_aie_fill_pixfmt_mp(struct v4l2_pix_format_mplane *dfmt,
				   const struct v4l2_pix_format_mplane *sfmt)
{
	dfmt->field = V4L2_FIELD_NONE;
	dfmt->colorspace = V4L2_COLORSPACE_BT2020;
	dfmt->num_planes = sfmt->num_planes;
	dfmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	dfmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	dfmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(dfmt->colorspace);
	dfmt->pixelformat = sfmt->pixelformat;

	/* Keep user setting as possible */
	dfmt->width = clamp(dfmt->width, MTK_FD_OUTPUT_MIN_WIDTH,
			    MTK_FD_OUTPUT_MAX_WIDTH);
	dfmt->height = clamp(dfmt->height, MTK_FD_OUTPUT_MIN_HEIGHT,
			     MTK_FD_OUTPUT_MAX_HEIGHT);

	dfmt->plane_fmt[0].bytesperline = ALIGN(dfmt->width, 16);
	dfmt->plane_fmt[1].bytesperline = ALIGN(dfmt->width, 16);

	dfmt->plane_fmt[0].sizeimage = dfmt->height * dfmt->plane_fmt[0].bytesperline;
	dfmt->plane_fmt[1].sizeimage = dfmt->height * dfmt->plane_fmt[1].bytesperline;
	if (sfmt->num_planes == 2 && sfmt->pixelformat == V4L2_PIX_FMT_NV12M) {
		dfmt->plane_fmt[1].sizeimage /= 2;
	} else if (sfmt->pixelformat == V4L2_PIX_FMT_NV12) {
		dfmt->plane_fmt[0].sizeimage *= 3;
		dfmt->plane_fmt[0].sizeimage /= 2;
	}
}

static const struct v4l2_pix_format_mplane *mtk_aie_find_fmt(u32 format)
{
	unsigned int i;

	for (i = 0; i < NUM_FORMATS; i++) {
		if (mtk_aie_img_fmts[i].pixelformat == format)
			return &mtk_aie_img_fmts[i];
	}

	return NULL;
}

static int mtk_aie_try_fmt_out_mp(struct file *file, void *fh,
				  struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct v4l2_pix_format_mplane *fmt;

	fmt = mtk_aie_find_fmt(pix_mp->pixelformat);
	if (!fmt)
		fmt = &mtk_aie_img_fmts[0]; /* Get default img fmt */

	mtk_aie_fill_pixfmt_mp(pix_mp, fmt);
	return 0;
}

static int mtk_aie_g_fmt_out_mp(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct mtk_aie_ctx *ctx = fh_to_ctx(fh);

	f->fmt.pix_mp = ctx->src_fmt;

	return 0;
}

static int mtk_aie_s_fmt_out_mp(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct mtk_aie_ctx *ctx = fh_to_ctx(fh);
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	const struct v4l2_pix_format_mplane *fmt;
	struct mtk_aie_dev *fd = ctx->fd_dev;

	if (!vq) {
		dev_err(fd->dev, "%s vq is NULL!\n", __func__);
		return -EINVAL;
	}

	/* Change not allowed if queue is streaming. */
	if (vb2_is_streaming(vq))
		return -EBUSY;

	fmt = mtk_aie_find_fmt(f->fmt.pix_mp.pixelformat);
	if (!fmt)
		fmt = &mtk_aie_img_fmts[0]; /* Get default img fmt */
	else if (&fd->ctx->fh != file->private_data)
		return -EBUSY;

	fd->ctx = ctx;
	mtk_aie_fill_pixfmt_mp(&f->fmt.pix_mp, fmt);
	ctx->src_fmt = f->fmt.pix_mp;

	return 0;
}

static int mtk_aie_enum_fmt_meta_cap(struct file *file, void *fh,
				     struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	strscpy(f->description, "Face detection result", sizeof(f->description));
	f->pixelformat = V4L2_META_FMT_MTFD_RESULT;
	f->flags = 0;

	return 0;
}

static int mtk_aie_g_fmt_meta_cap(struct file *file, void *fh,
				  struct v4l2_format *f)
{
	f->fmt.meta.dataformat = V4L2_META_FMT_MTFD_RESULT;
	f->fmt.meta.buffersize = sizeof(struct aie_enq_info);

	return 0;
}

static const struct vb2_ops mtk_aie_vb2_ops = {
	.queue_setup = mtk_aie_vb2_queue_setup,
	.buf_out_validate = mtk_aie_vb2_buf_out_validate,
	.buf_prepare = mtk_aie_vb2_buf_prepare,
	.buf_queue = mtk_aie_vb2_buf_queue,
	.start_streaming = mtk_aie_vb2_start_streaming,
	.stop_streaming = mtk_aie_vb2_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.buf_request_complete = mtk_aie_vb2_request_complete,
};

static const struct v4l2_ioctl_ops mtk_aie_v4l2_video_out_ioctl_ops = {
	.vidioc_querycap = mtk_aie_querycap,
	.vidioc_enum_fmt_vid_out = mtk_aie_enum_fmt_out_mp,
	.vidioc_g_fmt_vid_out_mplane = mtk_aie_g_fmt_out_mp,
	.vidioc_s_fmt_vid_out_mplane = mtk_aie_s_fmt_out_mp,
	.vidioc_try_fmt_vid_out_mplane = mtk_aie_try_fmt_out_mp,
	.vidioc_enum_fmt_meta_cap = mtk_aie_enum_fmt_meta_cap,
	.vidioc_g_fmt_meta_cap = mtk_aie_g_fmt_meta_cap,
	.vidioc_s_fmt_meta_cap = mtk_aie_g_fmt_meta_cap,
	.vidioc_try_fmt_meta_cap = mtk_aie_g_fmt_meta_cap,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int mtk_aie_queue_init(void *priv, struct vb2_queue *src_vq,
			      struct vb2_queue *dst_vq)
{
	struct mtk_aie_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->supports_requests = true;
	src_vq->drv_priv = ctx;
	src_vq->ops = &mtk_aie_vb2_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->fd_dev->vfd_lock;
	src_vq->dev = ctx->fd_dev->v4l2_dev.dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_META_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &mtk_aie_vb2_ops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->fd_dev->vfd_lock;
	dst_vq->dev = ctx->fd_dev->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

static int mtk_aie_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mtk_aie_ctx *ctx = ctrl_to_ctx(ctrl);
	struct v4l2_ctrl_aie_param *p_aie_param;
	struct v4l2_ctrl_aie_init *p_aie_init;

	if (!ctx)
		return -EINVAL;

	switch (ctrl->id) {
	case V4L2_CID_MTK_AIE_INIT:
		p_aie_init = ctrl->p_new.p;
		memcpy(&ctx->user_init, p_aie_init, sizeof(struct v4l2_ctrl_aie_init));
		break;
	case V4L2_CID_MTK_AIE_PARAM:
		p_aie_param = ctrl->p_new.p;
		memcpy(&ctx->user_param, p_aie_param, sizeof(struct v4l2_ctrl_aie_param));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops aie_ctrl_ops = {
	.s_ctrl = mtk_aie_s_ctrl,
};

static void mtk_aie_ctrl_type_op_init(const struct v4l2_ctrl *ctrl,
				      u32 from_idx, union v4l2_ctrl_ptr ptr)
{
	struct v4l2_ctrl_aie_param *p_aie_param;
	struct v4l2_ctrl_aie_init *p_aie_init;

	switch (ctrl->id) {
	case V4L2_CID_MTK_AIE_INIT:
		p_aie_init = ptr.p;
		memset(p_aie_init, 0, sizeof(struct v4l2_ctrl_aie_init));
		break;

	case V4L2_CID_MTK_AIE_PARAM:
		p_aie_param = ptr.p;
		memset(p_aie_param, 0, sizeof(struct v4l2_ctrl_aie_param));
		break;

	default:
		break;
	}
}

static int mtk_aie_ctrl_type_op_validate(const struct v4l2_ctrl *ctrl,
					 union v4l2_ctrl_ptr ptr)
{
	struct mtk_aie_ctx *ctx = ctrl_to_ctx(ctrl);
	struct v4l2_ctrl_aie_param *p_aie_param;
	struct v4l2_ctrl_aie_init *p_aie_init;
	struct mtk_aie_dev *fd;

	if (!ctx)
		return -EINVAL;

	fd = ctx->fd_dev;

	switch (ctrl->id) {
	case V4L2_CID_MTK_AIE_PARAM:
		p_aie_param = ptr.p;

		switch (p_aie_param->fd_mode) {
		case FDMODE:
		case ATTRIBUTEMODE:
		case FLDMODE:
			break;
		default:
			dev_err(ctx->dev, "Requested invalied mode: %d\n", p_aie_param->fd_mode);
			return -EINVAL;
		}

		switch (p_aie_param->src_img_fmt) {
		case FMT_YUV_2P:
		case FMT_YVU_2P:
		case FMT_YUYV:
		case FMT_YVYU:
		case FMT_UYVY:
		case FMT_VYUY:
		case FMT_MONO:
		case FMT_YUV420_2P:
		case FMT_YUV420_1P:
			break;
		default:
			dev_err(ctx->dev, "Requested invalied fmt: %d\n", p_aie_param->src_img_fmt);
			return -EINVAL;
		}

		if (p_aie_param->src_img_width > fd->base_para->max_img_rect.width ||
		    p_aie_param->src_img_height > fd->base_para->max_img_rect.height ||
		    p_aie_param->src_img_width == 0 || p_aie_param->src_img_height == 0) {
			dev_err(fd->dev, "Requested invalied Src_WD: %d Src_HT: %d\n",
				p_aie_param->src_img_width,
				p_aie_param->src_img_height);

			dev_err(fd->dev, "Requested invalied MAX_Src_WD: %d MAX_Src_HT: %d\n",
				fd->base_para->max_img_rect.width,
				fd->base_para->max_img_rect.height);

			return -EINVAL;
		}

		if (p_aie_param->pyramid_base_width > fd->base_para->max_pyramid_rect.width ||
		    p_aie_param->pyramid_base_height > fd->base_para->max_pyramid_rect.height ||
		    p_aie_param->number_of_pyramid > 3 || p_aie_param->number_of_pyramid <= 0) {
			dev_err(fd->dev, "Requested invalied base w: %d h: %d num: %d\n",
				p_aie_param->pyramid_base_width, p_aie_param->pyramid_base_height,
				p_aie_param->number_of_pyramid);

			dev_err(fd->dev, "Requested invalied max w: %d h: %d\n",
				fd->base_para->max_pyramid_rect.width,
				fd->base_para->max_pyramid_rect.height);

			return -EINVAL;
		}

		break;

	case V4L2_CID_MTK_AIE_INIT:
		p_aie_init = ptr.p;
		if (!p_aie_init->max_img_width || !p_aie_init->max_img_height ||
		    !p_aie_init->pyramid_width || !p_aie_init->pyramid_height) {
			dev_err(fd->dev,
				"Requested invalied max_w: %d max_h: %d, p_w: %d p_h: %d\n",
				p_aie_init->max_img_width, p_aie_init->max_img_height,
				p_aie_init->pyramid_width, p_aie_init->pyramid_height);

			return -EINVAL;
		}

		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_type_ops aie_ctrl_type_ops = {
	.equal = v4l2_ctrl_type_op_equal,
	.init = mtk_aie_ctrl_type_op_init,
	.log = v4l2_ctrl_type_op_log,
	.validate = mtk_aie_ctrl_type_op_validate,
};

static struct v4l2_ctrl_config mtk_aie_controls[] = {
	{
		.ops = &aie_ctrl_ops,
		.type_ops = &aie_ctrl_type_ops,
		.id = V4L2_CID_MTK_AIE_INIT,
		.name = "FD detection init",
		.type = V4L2_CTRL_TYPE_AIE_INIT,
		.elem_size = sizeof(struct v4l2_ctrl_aie_init),
	}, {
		.ops = &aie_ctrl_ops,
		.type_ops = &aie_ctrl_type_ops,
		.id = V4L2_CID_MTK_AIE_PARAM,
		.name = "FD detection param",
		.type = V4L2_CTRL_TYPE_AIE_PARAM,
		.elem_size = sizeof(struct v4l2_ctrl_aie_param),
	},
};

static int mtk_aie_ctrls_setup(struct mtk_aie_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;
	int i;

	v4l2_ctrl_handler_init(hdl, V4L2_CID_MTK_AIE_MAX);
	if (hdl->error)
		return hdl->error;

	for (i = 0; i < ARRAY_SIZE(mtk_aie_controls); i++) {
		v4l2_ctrl_new_custom(hdl, &mtk_aie_controls[i], ctx);
		if (hdl->error) {
			v4l2_ctrl_handler_free(hdl);
			dev_err(ctx->dev, "Failed to register controls: %d", i);
			return hdl->error;
		}
	}

	ctx->fh.ctrl_handler = &ctx->hdl;
	v4l2_ctrl_handler_setup(hdl);

	return 0;
}

static void init_ctx_fmt(struct mtk_aie_ctx *ctx)
{
	struct v4l2_pix_format_mplane *src_fmt = &ctx->src_fmt;
	struct v4l2_meta_format *dst_fmt = &ctx->dst_fmt;

	/* Initialize M2M source fmt */
	src_fmt->width = MTK_FD_OUTPUT_MAX_WIDTH;
	src_fmt->height = MTK_FD_OUTPUT_MAX_HEIGHT;
	mtk_aie_fill_pixfmt_mp(src_fmt, &mtk_aie_img_fmts[0]);

	/* Initialize M2M destination fmt */
	dst_fmt->buffersize = sizeof(struct aie_enq_info);
	dst_fmt->dataformat = V4L2_META_FMT_MTFD_RESULT;
}

/*
 * V4L2 file operations.
 */
static int mtk_vfd_open(struct file *filp)
{
	struct video_device *vdev = video_devdata(filp);
	struct mtk_aie_dev *fd = video_drvdata(filp);
	struct mtk_aie_ctx *ctx;
	int ret;

	mutex_lock(&fd->dev_lock);

	if (fd->fd_state & STATE_OPEN) {
		dev_err(fd->dev, "VFD is already open, Only one instance is supported\n");
		ret =  -EBUSY;
		goto err_unlock;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret =  -ENOMEM;
		goto err_unlock;
	}

	ctx->fd_dev = fd;
	ctx->dev = fd->dev;
	fd->ctx = ctx;

	v4l2_fh_init(&ctx->fh, vdev);
	filp->private_data = &ctx->fh;

	init_ctx_fmt(ctx);

	ret = mtk_aie_ctrls_setup(ctx);
	if (ret) {
		dev_err(ctx->dev, "Failed to set up controls: %d\n", ret);
		goto err_fh_exit;
	}
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(fd->m2m_dev, ctx, &mtk_aie_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_free_ctrl_handler;
	}
	v4l2_fh_add(&ctx->fh);
	fd->fd_state |= STATE_OPEN;

	mutex_unlock(&fd->dev_lock);

	return 0;
err_free_ctrl_handler:
	v4l2_ctrl_handler_free(&ctx->hdl);
err_fh_exit:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
err_unlock:
	mutex_unlock(&fd->dev_lock);

	return ret;
}

static int mtk_vfd_release(struct file *filp)
{
	struct mtk_aie_ctx *ctx = container_of(filp->private_data, struct mtk_aie_ctx, fh);
	struct mtk_aie_dev *fd = video_drvdata(filp);

	mutex_lock(&fd->dev_lock);

	fd->fd_state &= ~STATE_OPEN;

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);

	kfree(ctx);

	mutex_unlock(&fd->dev_lock);

	return 0;
}

static __poll_t mtk_vfd_fop_poll(struct file *file, poll_table *wait)
{
	int ret;

	struct mtk_aie_ctx *ctx = container_of(file->private_data, struct mtk_aie_ctx, fh);
	struct mtk_aie_dev *fd = ctx->fd_dev;

	if (fd->fd_state & STATE_INIT) {
		/* Waiting Job Finsh */
		ret = wait_for_completion_timeout(&fd->fd_job_finished, msecs_to_jiffies(1000));
		if (!ret) {
			dev_err(ctx->dev, "Wait job finish timeout from poll\n");
			return EPOLLERR;
		}
	}

	return v4l2_m2m_fop_poll(file, wait);
}

static const struct v4l2_file_operations fd_video_fops = {
	.owner = THIS_MODULE,
	.open = mtk_vfd_open,
	.release = mtk_vfd_release,
	.poll = mtk_vfd_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int mtk_aie_job_ready(void *priv)
{
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct mtk_aie_ctx *ctx = priv;
	struct mtk_aie_dev *fd = ctx->fd_dev;
	struct fd_buffer src_img[2] = {};
	void *plane_vaddr;

	if (!ctx->fh.m2m_ctx) {
		dev_err(fd->dev, "Memory-to-memory context is NULL\n");
		return -1;
	}

	if (!(fd->fd_state & STATE_OPEN)) {
		dev_err(fd->dev, "Job ready with device closed\n");
		return -1;
	}

	mutex_lock(&fd->fd_lock);

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	if (!src_buf || !dst_buf) {
		dev_err(fd->dev, "src or dst buf is NULL\n");
		mutex_unlock(&fd->fd_lock);
		return -1;
	}

	if (!(fd->fd_state & STATE_INIT)) {
		dev_err(fd->dev, "%s Wrong fd state: %d\n", __func__, fd->fd_state);
		mutex_unlock(&fd->fd_lock);
		return -1;
	}

	plane_vaddr = vb2_plane_vaddr(&dst_buf->vb2_buf, 0);
	if (!plane_vaddr) {
		dev_err(fd->dev, "Failed to get plane virtual address\n");
		mutex_unlock(&fd->fd_lock);
		return -1;
	}

	v4l2_ctrl_request_setup(src_buf->vb2_buf.req_obj.req, &ctx->hdl);

	fd->aie_cfg = (struct aie_enq_info *)plane_vaddr;
	fd->aie_cfg->fld_face_num = ctx->user_param.fld_face_num;

	memset(fd->aie_cfg, 0, sizeof(struct aie_enq_info));
	memcpy(fd->aie_cfg, &ctx->user_param, sizeof(struct v4l2_ctrl_aie_param));
	memcpy(fd->aie_cfg->fld_input, ctx->user_param.fld_input,
	       FLD_MAX_FRAME * sizeof(struct fld_crop_rip_rop));

	src_img[0].dma_addr = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);

	if (ctx->src_fmt.num_planes == 2) {
		src_img[1].dma_addr =
			vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 1);
	}

	if ((fd->aie_cfg->sel_mode == FDMODE || fd->aie_cfg->sel_mode == ATTRIBUTEMODE) &&
	    fd->aie_cfg->src_img_fmt == FMT_YUV420_1P) {
		src_img[1].dma_addr = src_img[0].dma_addr + ctx->user_param.src_img_stride *
			ctx->user_param.src_img_height;
	}

	fd->aie_cfg->src_img_addr = src_img[0].dma_addr;
	fd->aie_cfg->src_img_addr_uv = src_img[1].dma_addr;

	aie_prepare(fd, fd->aie_cfg);

	mutex_unlock(&fd->fd_lock);

	if (src_buf) {
		/* Complete request controls if any */
		v4l2_ctrl_request_complete(src_buf->vb2_buf.req_obj.req, &ctx->hdl);
	}

	return 0;
}

static void mtk_aie_device_run(void *priv)
{
	struct mtk_aie_ctx *ctx = priv;
	struct mtk_aie_dev *fd = ctx->fd_dev;
	int ret;

	ret = mtk_aie_job_ready(priv);
	if (ret == -1) {
		dev_err(fd->dev, "Failed to run job ready\n");
		return;
	}

	atomic_inc(&fd->num_composing);
	mtk_aie_hw_job_exec(fd);
	aie_execute(fd, fd->aie_cfg);
}

static struct v4l2_m2m_ops fd_m2m_ops = {
	.device_run = mtk_aie_device_run,
};

static const struct media_device_ops fd_m2m_media_ops = {
	.req_validate = vb2_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static int mtk_aie_video_device_register(struct mtk_aie_dev *fd)
{
	struct v4l2_m2m_dev *m2m_dev = fd->m2m_dev;
	struct video_device *vfd = &fd->vfd;
	struct device *dev = fd->dev;
	int ret;

	vfd->fops = &fd_video_fops;
	vfd->release = video_device_release_empty;
	vfd->lock = &fd->vfd_lock;
	vfd->v4l2_dev = &fd->v4l2_dev;
	vfd->vfl_dir = VFL_DIR_M2M;
	vfd->device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_OUTPUT_MPLANE |
			   V4L2_CAP_META_CAPTURE;
	vfd->ioctl_ops = &mtk_aie_v4l2_video_out_ioctl_ops;

	strscpy(vfd->name, dev_driver_string(dev), sizeof(vfd->name));

	video_set_drvdata(vfd, fd);

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, 0);
	if (ret) {
		dev_err(dev, "Failed to register video device\n");
		return ret;
	}

	ret = v4l2_m2m_register_media_controller(m2m_dev, vfd, MEDIA_ENT_F_PROC_VIDEO_STATISTICS);
	if (ret) {
		dev_err(dev, "Failed to init mem2mem media controller\n");
		video_unregister_device(vfd);
		return ret;
	}

	return 0;
}

static int mtk_aie_dev_v4l2_init(struct mtk_aie_dev *fd)
{
	struct media_device *mdev = &fd->mdev;
	struct device *dev = fd->dev;
	int ret;

	ret = v4l2_device_register(dev, &fd->v4l2_dev);
	if (ret) {
		dev_err(dev, "Failed to register v4l2 device\n");
		return ret;
	}

	fd->m2m_dev = v4l2_m2m_init(&fd_m2m_ops);
	if (IS_ERR(fd->m2m_dev)) {
		dev_err(dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(fd->m2m_dev);
		goto err_unreg_v4l2_dev;
	}

	mdev->dev = dev;
	strscpy(mdev->model, dev_driver_string(dev), sizeof(mdev->model));
	media_device_init(mdev);
	mdev->ops = &fd_m2m_media_ops;
	fd->v4l2_dev.mdev = mdev;

	ret = mtk_aie_video_device_register(fd);
	if (ret)
		goto err_cleanup_mdev;

	ret = media_device_register(mdev);
	if (ret) {
		dev_err(dev, "Failed to register mem2mem media device\n");
		goto err_unreg_vdev;
	}
	return 0;

err_unreg_vdev:
	v4l2_m2m_unregister_media_controller(fd->m2m_dev);
	video_unregister_device(&fd->vfd);
err_cleanup_mdev:
	media_device_cleanup(mdev);
	v4l2_m2m_release(fd->m2m_dev);
err_unreg_v4l2_dev:
	v4l2_device_unregister(&fd->v4l2_dev);
	return ret;
}

static void mtk_aie_video_device_unregister(struct mtk_aie_dev *fd)
{
	v4l2_m2m_unregister_media_controller(fd->m2m_dev);
	video_unregister_device(&fd->vfd);
	media_device_cleanup(&fd->mdev);
	v4l2_m2m_release(fd->m2m_dev);
	v4l2_device_unregister(&fd->v4l2_dev);
}

static void mtk_aie_frame_done_worker(struct work_struct *work)
{
	struct mtk_aie_req_work *req_work = (struct mtk_aie_req_work *)work;
	struct mtk_aie_dev *fd = (struct mtk_aie_dev *)req_work->fd_dev;

	if (fd->reg_cfg.fd_mode == FDMODE) {
		fd->reg_cfg.hw_result = readl(fd->fd_base + AIE_RESULT_0_REG);
		fd->reg_cfg.hw_result1 = readl(fd->fd_base + AIE_RESULT_1_REG);
	}

	mutex_lock(&fd->fd_lock);

	switch (fd->aie_cfg->sel_mode) {
	case FDMODE:
		aie_get_fd_result(fd, fd->aie_cfg);
		break;
	case ATTRIBUTEMODE:
		aie_get_attr_result(fd, fd->aie_cfg);
		break;
	case FLDMODE:
		aie_get_fld_result(fd, fd->aie_cfg);
		break;
	default:
		dev_dbg(fd->dev, "Wrong sel_mode\n");
		break;
	}

	mutex_unlock(&fd->fd_lock);

	if (!cancel_delayed_work(&fd->job_timeout_work))
		return;

	atomic_dec(&fd->num_composing);
	mtk_aie_hw_job_finish(fd, VB2_BUF_STATE_DONE);
	wake_up(&fd->flushing_waitq);
}

static int mtk_aie_resource_init(struct mtk_aie_dev *fd)
{
	mutex_init(&fd->vfd_lock);
	mutex_init(&fd->dev_lock);
	mutex_init(&fd->fd_lock);

	init_completion(&fd->fd_job_finished);
	complete_all(&fd->fd_job_finished);
	INIT_DELAYED_WORK(&fd->job_timeout_work, mtk_aie_job_timeout_work);
	init_waitqueue_head(&fd->flushing_waitq);
	atomic_set(&fd->num_composing, 0);
	fd->fd_stream_count = 0;

	fd->frame_done_wq = alloc_ordered_workqueue(dev_name(fd->dev),
						    WQ_HIGHPRI | WQ_FREEZABLE);
	if (!fd->frame_done_wq) {
		dev_err(fd->dev, "failed to alloc frame_done workqueue\n");
		mutex_destroy(&fd->vfd_lock);
		mutex_destroy(&fd->dev_lock);
		mutex_destroy(&fd->fd_lock);
		return -ENOMEM;
	}

	INIT_WORK(&fd->req_work.work, mtk_aie_frame_done_worker);
	fd->req_work.fd_dev = fd;

	return 0;
}

static void mtk_aie_resource_free(struct platform_device *pdev)
{
	struct mtk_aie_dev *fd = dev_get_drvdata(&pdev->dev);

	if (fd->frame_done_wq)
		destroy_workqueue(fd->frame_done_wq);
	fd->frame_done_wq = NULL;
	mutex_destroy(&fd->vfd_lock);
	mutex_destroy(&fd->dev_lock);
	mutex_destroy(&fd->fd_lock);
}

static irqreturn_t mtk_aie_irq(int irq, void *data)
{
	struct mtk_aie_dev *fd = (struct mtk_aie_dev *)data;

	aie_irqhandle(fd);

	queue_work(fd->frame_done_wq, &fd->req_work.work);

	return IRQ_HANDLED;
}

static int mtk_aie_probe(struct platform_device *pdev)
{
	struct mtk_aie_dev *fd;
	struct device *dev = &pdev->dev;
	const struct mtk_aie_variant *driver_data = NULL;
	const struct of_device_id *match = NULL;
	int irq;
	int ret;

	static struct clk_bulk_data aie_clks[] = {
		{ .id = "img_ipe" },
		{ .id = "ipe_fdvt" },
		{ .id = "ipe_top" },
		{ .id = "ipe_smi_larb12" },
	};

	fd = devm_kzalloc(&pdev->dev, sizeof(*fd), GFP_KERNEL);
	if (!fd)
		return -ENOMEM;

	match = of_match_node(mtk_aie_of_ids, dev->of_node);
	if (match)
		driver_data = (const struct mtk_aie_variant *)match->data;

	fd->variant = driver_data;
	if (!fd->variant)
		return -ENODEV;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));
	if (ret)
		return dev_err_probe(dev, ret, "Cannot set Coherent DMA mask\n");

	dev_set_drvdata(dev, fd);
	fd->dev = dev;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return dev_err_probe(dev, irq, "Failed to get IRQ\n");

	ret = devm_request_irq(dev, irq, mtk_aie_irq, IRQF_SHARED,
			       dev_driver_string(dev), fd);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request irq\n");

	fd->irq = irq;
	fd->fd_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fd->fd_base))
		return dev_err_probe(dev, -EINVAL, "Failed to get fd reg base\n");

	fd->aie_clk.clk_num = ARRAY_SIZE(aie_clks);
	fd->aie_clk.clks = aie_clks;
	ret = devm_clk_bulk_get(&pdev->dev, fd->aie_clk.clk_num, fd->aie_clk.clks);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get raw clock\n");

	ret = mtk_aie_resource_init(fd);
	if (ret) {
		mtk_aie_resource_free(pdev);
		return ret;
	}
	pm_runtime_enable(dev);
	ret = mtk_aie_dev_v4l2_init(fd);
	if (ret) {
		pm_runtime_disable(&pdev->dev);
		return ret;
	}

	return 0;
}

static void mtk_aie_remove(struct platform_device *pdev)
{
	struct mtk_aie_dev *fd = dev_get_drvdata(&pdev->dev);

	mtk_aie_video_device_unregister(fd);
	pm_runtime_disable(&pdev->dev);
	mtk_aie_resource_free(pdev);
}

static int __maybe_unused mtk_aie_suspend(struct device *dev)
{
	struct mtk_aie_dev *fd = dev_get_drvdata(dev);
	int ret, num;

	if (pm_runtime_suspended(dev))
		return 0;

	num = atomic_read(&fd->num_composing);

	ret = wait_event_timeout(fd->flushing_waitq,
				 !(num = atomic_read(&fd->num_composing)),
				 msecs_to_jiffies(MTK_FD_HW_TIMEOUT_IN_MSEC));
	if (!ret && num) {
		dev_dbg(dev, "%s: flushing aie job timeout num %d\n",
			__func__, num);

		return -EBUSY;
	}

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		return ret;

	return 0;
}

static int __maybe_unused mtk_aie_resume(struct device *dev)
{
	int ret;

	if (pm_runtime_suspended(dev)) {
		dev_dbg(dev, "%s: pm_runtime_suspended is true, no action\n", __func__);
		return 0;
	}

	ret = pm_runtime_force_resume(dev);
	if (ret)
		return ret;

	return 0;
}

static int __maybe_unused mtk_aie_runtime_suspend(struct device *dev)
{
	struct mtk_aie_dev *fd = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(fd->aie_clk.clk_num, fd->aie_clk.clks);

	return 0;
}

static int __maybe_unused mtk_aie_runtime_resume(struct device *dev)
{
	struct mtk_aie_dev *fd = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(fd->aie_clk.clk_num, fd->aie_clk.clks);
	if (ret) {
		dev_err(dev, "Failed to enable clock: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct dev_pm_ops mtk_aie_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_aie_suspend, mtk_aie_resume)
	SET_RUNTIME_PM_OPS(mtk_aie_runtime_suspend, mtk_aie_runtime_resume, NULL)
};

static struct platform_driver mtk_aie_driver = {
	.probe = mtk_aie_probe,
	.remove = mtk_aie_remove,
	.driver = {
		.name = "mtk-aie-5.3",
		.of_match_table = mtk_aie_of_ids,
		.pm = pm_ptr(&mtk_aie_pm_ops),
	}
};

module_platform_driver(mtk_aie_driver);
MODULE_AUTHOR("Bo Kong <bo.kong@mediatek.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek AIE driver");
