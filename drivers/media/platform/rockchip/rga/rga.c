// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Jacob Chen <jacob-chen@iotwrt.com>
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/timer.h>

#include <linux/platform_device.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-sg.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "rga.h"

static int debug;
module_param(debug, int, 0644);

static void device_run(void *prv)
{
	struct rga_ctx *ctx = prv;
	struct rockchip_rga *rga = ctx->rga;
	struct rga_core *core = rga->cores[0];
	struct vb2_v4l2_buffer *src, *dst;
	unsigned long flags;
	int ret;

	ret = pm_runtime_resume_and_get(core->dev);
	if (ret < 0) {
		v4l2_m2m_buf_done_and_job_finish(rga->m2m_dev, ctx->fh.m2m_ctx,
						 VB2_BUF_STATE_ERROR);
		return;
	}

	spin_lock_irqsave(&rga->ctrl_lock, flags);
	if (ctx->cmdbuf_dirty) {
		ctx->cmdbuf_dirty = false;
		memset(ctx->cmdbuf_virt, 0, rga->hw->cmdbuf_size);
		rga->hw->setup_cmdbuf(ctx);
	}
	spin_unlock_irqrestore(&rga->ctrl_lock, flags);

	core->curr = ctx;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	src->sequence = ctx->osequence++;

	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	rga->hw->start(core, vb_to_rga(src), vb_to_rga(dst));
}

static irqreturn_t rga_isr(int irq, void *prv)
{
	struct rga_core *core = prv;
	struct rockchip_rga *rga = core->rga;

	if (rga->hw->handle_irq(core)) {
		struct vb2_v4l2_buffer *src, *dst;
		struct rga_ctx *ctx = core->curr;

		WARN_ON(!ctx);

		core->curr = NULL;

		src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		WARN_ON(!src);
		WARN_ON(!dst);

		v4l2_m2m_buf_copy_metadata(src, dst);

		dst->sequence = ctx->csequence++;

		v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
		v4l2_m2m_job_finish(rga->m2m_dev, ctx->fh.m2m_ctx);

		pm_runtime_put_autosuspend(core->dev);
	}

	return IRQ_HANDLED;
}

static const struct v4l2_m2m_ops rga_m2m_ops = {
	.device_run = device_run,
};

static int
queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct rga_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &rga_qops;
	if (rga_has_internal_iommu(ctx->rga))
		src_vq->mem_ops = &vb2_dma_sg_memops;
	else
		src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->gfp_flags = __GFP_DMA32;
	src_vq->buf_struct_size = sizeof(struct rga_vb_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->rga->mutex;
	src_vq->dev = ctx->rga->cores[0]->dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &rga_qops;
	if (rga_has_internal_iommu(ctx->rga))
		dst_vq->mem_ops = &vb2_dma_sg_memops;
	else
		dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->gfp_flags = __GFP_DMA32;
	dst_vq->buf_struct_size = sizeof(struct rga_vb_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->rga->mutex;
	dst_vq->dev = ctx->rga->cores[0]->dev;

	return vb2_queue_init(dst_vq);
}

static int rga_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rga_ctx *ctx = container_of(ctrl->handler, struct rga_ctx,
					   ctrl_handler);
	const struct rga_hw *hw = ctx->rga->hw;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&ctx->rga->ctrl_lock, flags);
	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
		ctx->hflip = ctrl->val;
		break;
	case V4L2_CID_VFLIP:
		ctx->vflip = ctrl->val;
		break;
	case V4L2_CID_ROTATE:
		if (vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)) &&
		    vb2_is_streaming(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx))) {
			ret = rga_check_scaling(hw, &ctx->in.crop,
						&ctx->out.crop, ctrl->val);
			if (ret < 0)
				goto s_ctrl_done;
		}
		ctx->rotate = ctrl->val;
		break;
	case V4L2_CID_BG_COLOR:
		ctx->fill_color = ctrl->val;
		break;
	}
	ctx->cmdbuf_dirty = true;

s_ctrl_done:
	spin_unlock_irqrestore(&ctx->rga->ctrl_lock, flags);
	return ret;
}

static const struct v4l2_ctrl_ops rga_ctrl_ops = {
	.s_ctrl = rga_s_ctrl,
};

static int rga_setup_ctrls(struct rga_ctx *ctx)
{
	struct rockchip_rga *rga = ctx->rga;

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 4);

	if (rga->hw->features & RGA_FEATURE_FLIP) {
		v4l2_ctrl_new_std(&ctx->ctrl_handler, &rga_ctrl_ops,
				  V4L2_CID_HFLIP, 0, 1, 1, 0);

		v4l2_ctrl_new_std(&ctx->ctrl_handler, &rga_ctrl_ops,
				  V4L2_CID_VFLIP, 0, 1, 1, 0);
	}

	if (rga->hw->features & RGA_FEATURE_ROTATE)
		v4l2_ctrl_new_std(&ctx->ctrl_handler, &rga_ctrl_ops,
				  V4L2_CID_ROTATE, 0, 270, 90, 0);

	if (rga->hw->features & RGA_FEATURE_BG_COLOR)
		v4l2_ctrl_new_std(&ctx->ctrl_handler, &rga_ctrl_ops,
				  V4L2_CID_BG_COLOR, 0, 0xffffffff, 1, 0);

	if (ctx->ctrl_handler.error) {
		int err = ctx->ctrl_handler.error;

		v4l2_err(&rga->v4l2_dev, "%s failed\n", __func__);
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		return err;
	}

	return 0;
}

static bool check_scaling_factor(const struct rga_hw *hw, u32 src_size,
				 u32 dst_size)
{
	if (src_size < dst_size)
		return src_size * hw->max_scaling_factor >= dst_size;
	else
		return dst_size * hw->max_scaling_factor >= src_size;
}

int rga_check_scaling(const struct rga_hw *hw, const struct v4l2_rect *crop_in,
		      const struct v4l2_rect *crop_out, u32 rotate)
{
	u32 scaled_width;
	u32 scaled_height;

	if (rotate == 90 || rotate == 270) {
		scaled_width = crop_out->height;
		scaled_height = crop_out->width;
	} else {
		scaled_width = crop_out->width;
		scaled_height = crop_out->height;
	}

	if (!check_scaling_factor(hw, crop_in->width, scaled_width))
		return -EINVAL;

	if (!check_scaling_factor(hw, crop_in->height, scaled_height))
		return -EINVAL;

	return 0;
}

struct rga_frame *rga_get_frame(struct rga_ctx *ctx, enum v4l2_buf_type type)
{
	if (V4L2_TYPE_IS_OUTPUT(type))
		return &ctx->in;
	if (V4L2_TYPE_IS_CAPTURE(type))
		return &ctx->out;
	return ERR_PTR(-EINVAL);
}

static int rga_open(struct file *file)
{
	struct rockchip_rga *rga = video_drvdata(file);
	struct rga_ctx *ctx = NULL;
	int ret = 0;
	u32 def_width = clamp(DEFAULT_WIDTH, rga->hw->min_width, rga->hw->max_width);
	u32 def_height = clamp(DEFAULT_HEIGHT, rga->hw->min_height, rga->hw->max_height);
	struct rga_frame def_frame = {
		.crop.left = 0,
		.crop.top = 0,
		.crop.width = def_width,
		.crop.height = def_height,
	};

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	/* Create CMD buffer */
	ctx->cmdbuf_virt = dma_alloc_attrs(rga->cores[0]->dev, rga->hw->cmdbuf_size,
					   &ctx->cmdbuf_phy, GFP_KERNEL,
					   DMA_ATTR_WRITE_COMBINE);
	if (!ctx->cmdbuf_virt) {
		ret = -ENOMEM;
		goto rel_ctx;
	}
	ctx->cmdbuf_dirty = true;

	ctx->rga = rga;
	/* Set default formats */
	ctx->in = def_frame;
	ctx->out = def_frame;

	ctx->in.fmt = rga->hw->adjust_and_map_format(ctx, &ctx->in.pix, true);
	v4l2_fill_pixfmt_mp_aligned(&ctx->in.pix, ctx->in.pix.pixelformat,
				    def_width, def_height, rga->hw->stride_alignment);
	ctx->out.fmt =
		rga->hw->adjust_and_map_format(ctx, &ctx->out.pix, false);
	v4l2_fill_pixfmt_mp_aligned(&ctx->out.pix, ctx->out.pix.pixelformat,
				    def_width, def_height, rga->hw->stride_alignment);

	if (mutex_lock_interruptible(&rga->mutex)) {
		ret = -ERESTARTSYS;
		goto rel_cmdbuf;
	}
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(rga->m2m_dev, ctx, &queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto unlock_mutex;
	}
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	v4l2_fh_add(&ctx->fh, file);

	rga_setup_ctrls(ctx);

	/* Write the default values to the ctx struct */
	v4l2_ctrl_handler_setup(&ctx->ctrl_handler);

	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	mutex_unlock(&rga->mutex);

	return 0;

unlock_mutex:
	mutex_unlock(&rga->mutex);
rel_cmdbuf:
	dma_free_attrs(rga->cores[0]->dev, rga->hw->cmdbuf_size, ctx->cmdbuf_virt,
		       ctx->cmdbuf_phy, DMA_ATTR_WRITE_COMBINE);
rel_ctx:
	kfree(ctx);
	return ret;
}

static int rga_release(struct file *file)
{
	struct rga_ctx *ctx = file_to_rga_ctx(file);
	struct rockchip_rga *rga = ctx->rga;

	mutex_lock(&rga->mutex);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);

	dma_free_attrs(rga->cores[0]->dev, rga->hw->cmdbuf_size, ctx->cmdbuf_virt,
		       ctx->cmdbuf_phy, DMA_ATTR_WRITE_COMBINE);

	kfree(ctx);

	mutex_unlock(&rga->mutex);

	return 0;
}

static const struct v4l2_file_operations rga_fops = {
	.owner = THIS_MODULE,
	.open = rga_open,
	.release = rga_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int
vidioc_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct rockchip_rga *rga = video_drvdata(file);

	strscpy(cap->driver, RGA_NAME, sizeof(cap->driver));
	strscpy(cap->card, rga->hw->card_type, sizeof(cap->card));
	strscpy(cap->bus_info, "platform:rga", sizeof(cap->bus_info));

	return 0;
}

static int vidioc_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct rockchip_rga *rga = video_drvdata(file);
	int ret;

	ret = rga->hw->enum_format(f);
	if (ret != 0)
		return ret;

	/*
	 * Allow changing the quantization and ycbcr_enc func for YUV formats
	 * on the capture side for RGB -> YUV conversions.
	 *
	 * These flags are only relevant for capture devices.
	 */
	if (V4L2_TYPE_IS_CAPTURE(f->type) &&
	    v4l2_is_format_yuv(v4l2_format_info(f->pixelformat)))
		f->flags |= V4L2_FMT_FLAG_CSC_QUANTIZATION |
			    V4L2_FMT_FLAG_CSC_YCBCR_ENC;

	return 0;
}

static int vidioc_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_fmt = &f->fmt.pix_mp;
	struct rga_ctx *ctx = file_to_rga_ctx(file);
	struct rga_frame *frm;

	frm = rga_get_frame(ctx, f->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);

	*pix_fmt = frm->pix;
	pix_fmt->field = V4L2_FIELD_NONE;

	return 0;
}

static int vidioc_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_fmt = &f->fmt.pix_mp;
	struct rga_ctx *ctx = file_to_rga_ctx(file);
	const struct rga_hw *hw = ctx->rga->hw;
	struct v4l2_frmsize_stepwise frmsize = {
		.min_width = hw->min_width,
		.max_width = hw->max_width,
		.min_height = hw->min_height,
		.max_height = hw->max_height,
		.step_width = 1,
		.step_height = 1,
	};

	/*
	 * Technically 4:2:2 YUV formats don't need a step_height of 2.
	 * But for the RGA3 this is explicitly documented in  section 5.6.3
	 * of the RK3588 TRM Part 2.
	 * And the RGA2 vendor driver also checks that the height (and width)
	 * is aligned to 2 when a YUV format is used.
	 *
	 * Therefore be safe and always align width and height to 2
	 * when a YUV format is used.
	 */
	if (v4l2_is_format_yuv(v4l2_format_info(pix_fmt->pixelformat))) {
		frmsize.step_width = 2;
		frmsize.step_height = 2;
	}

	if (V4L2_TYPE_IS_CAPTURE(f->type)) {
		const struct rga_frame *frm;

		frm = rga_get_frame(ctx, f->type);
		if (IS_ERR(frm))
			return PTR_ERR(frm);

		if (!(pix_fmt->flags & V4L2_PIX_FMT_FLAG_SET_CSC)) {
			pix_fmt->quantization = frm->pix.quantization;
			pix_fmt->ycbcr_enc = frm->pix.ycbcr_enc;
		}
		/* disallow values not announced in vidioc_enum_fmt */
		pix_fmt->colorspace = frm->pix.colorspace;
		pix_fmt->xfer_func = frm->pix.xfer_func;
	}

	hw->adjust_and_map_format(ctx, pix_fmt, V4L2_TYPE_IS_OUTPUT(f->type));

	v4l2_apply_frmsize_constraints(&pix_fmt->width, &pix_fmt->height, &frmsize);
	v4l2_fill_pixfmt_mp_aligned(pix_fmt, pix_fmt->pixelformat,
				    pix_fmt->width, pix_fmt->height, hw->stride_alignment);
	pix_fmt->field = V4L2_FIELD_NONE;

	return 0;
}

static int vidioc_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_fmt = &f->fmt.pix_mp;
	struct rga_ctx *ctx = file_to_rga_ctx(file);
	struct rockchip_rga *rga = ctx->rga;
	struct vb2_queue *vq;
	struct rga_frame *frm;
	int ret = 0;
	int i;

	/* Adjust all values accordingly to the hardware capabilities
	 * and chosen format.
	 */
	ret = vidioc_try_fmt(file, priv, f);
	if (ret)
		return ret;
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq)) {
		v4l2_err(&rga->v4l2_dev, "queue (%d) bust\n", f->type);
		return -EBUSY;
	}
	frm = rga_get_frame(ctx, f->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);
	frm->fmt = rga->hw->adjust_and_map_format(ctx, pix_fmt,
						  V4L2_TYPE_IS_OUTPUT(f->type));

	/*
	 * Copy colorimetry from output to capture as required by the
	 * v4l2-compliance tests
	 */
	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		ctx->out.pix.colorspace = pix_fmt->colorspace;
		ctx->out.pix.ycbcr_enc = pix_fmt->ycbcr_enc;
		ctx->out.pix.quantization = pix_fmt->quantization;
		ctx->out.pix.xfer_func = pix_fmt->xfer_func;
	}

	/* Reset crop settings */
	frm->crop.left = 0;
	frm->crop.top = 0;
	frm->crop.width = pix_fmt->width;
	frm->crop.height = pix_fmt->height;

	frm->pix = *pix_fmt;
	ctx->cmdbuf_dirty = true;

	v4l2_dbg(debug, 1, &rga->v4l2_dev,
		 "[%s] fmt - %p4cc %dx%d (stride %d)\n",
		  V4L2_TYPE_IS_OUTPUT(f->type) ? "OUTPUT" : "CAPTURE",
		  &pix_fmt->pixelformat, pix_fmt->width, pix_fmt->height,
		  pix_fmt->plane_fmt[0].bytesperline);

	for (i = 0; i < pix_fmt->num_planes; i++) {
		v4l2_dbg(debug, 1, &rga->v4l2_dev,
			 "plane[%d]: size %d, bytesperline %d\n",
			 i, pix_fmt->plane_fmt[i].sizeimage,
			 pix_fmt->plane_fmt[i].bytesperline);
	}

	return 0;
}

static int vidioc_g_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct rga_ctx *ctx = file_to_rga_ctx(file);
	struct rga_frame *f;
	bool use_frame = false;

	f = rga_get_frame(ctx, s->type);
	if (IS_ERR(f))
		return PTR_ERR(f);

	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		if (!V4L2_TYPE_IS_CAPTURE(s->type))
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		if (!V4L2_TYPE_IS_OUTPUT(s->type))
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_COMPOSE:
		if (!V4L2_TYPE_IS_CAPTURE(s->type))
			return -EINVAL;
		use_frame = true;
		break;
	case V4L2_SEL_TGT_CROP:
		if (!V4L2_TYPE_IS_OUTPUT(s->type))
			return -EINVAL;
		use_frame = true;
		break;
	default:
		return -EINVAL;
	}

	if (use_frame) {
		s->r = f->crop;
	} else {
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = f->pix.width;
		s->r.height = f->pix.height;
	}

	return 0;
}

static int vidioc_s_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct rga_ctx *ctx = file_to_rga_ctx(file);
	struct rockchip_rga *rga = ctx->rga;
	struct rga_frame *f;

	f = rga_get_frame(ctx, s->type);
	if (IS_ERR(f))
		return PTR_ERR(f);

	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE:
		/*
		 * COMPOSE target is only valid for capture buffer type, return
		 * error for output buffer type
		 */
		if (!V4L2_TYPE_IS_CAPTURE(s->type))
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_CROP:
		/*
		 * CROP target is only valid for output buffer type, return
		 * error for capture buffer type
		 */
		if (!V4L2_TYPE_IS_OUTPUT(s->type))
			return -EINVAL;
		break;
	/*
	 * bound and default crop/compose targets are invalid targets to
	 * try/set
	 */
	default:
		return -EINVAL;
	}

	if (s->r.top < 0 || s->r.left < 0) {
		v4l2_dbg(debug, 1, &rga->v4l2_dev,
			 "doesn't support negative values for top & left.\n");
		return -EINVAL;
	}

	if (s->r.left + s->r.width > f->pix.width ||
	    s->r.top + s->r.height > f->pix.height ||
	    s->r.width < rga->hw->min_width || s->r.height < rga->hw->min_height) {
		v4l2_dbg(debug, 1, &rga->v4l2_dev, "unsupported crop value.\n");
		return -EINVAL;
	}

	if (vb2_is_streaming(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)) &&
	    vb2_is_streaming(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx))) {
		int ret = 0;

		if (V4L2_TYPE_IS_OUTPUT(s->type))
			ret = rga_check_scaling(rga->hw, &s->r, &ctx->out.crop,
						ctx->rotate);
		else
			ret = rga_check_scaling(rga->hw, &ctx->in.crop, &s->r,
						ctx->rotate);

		if (ret < 0)
			return ret;
	}

	f->crop = s->r;
	ctx->cmdbuf_dirty = true;

	return 0;
}

static const struct v4l2_ioctl_ops rga_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,

	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt,
	.vidioc_g_fmt_vid_cap_mplane = vidioc_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = vidioc_try_fmt,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt,

	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt,
	.vidioc_g_fmt_vid_out_mplane = vidioc_g_fmt,
	.vidioc_try_fmt_vid_out_mplane = vidioc_try_fmt,
	.vidioc_s_fmt_vid_out_mplane = vidioc_s_fmt,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_g_selection = vidioc_g_selection,
	.vidioc_s_selection = vidioc_s_selection,
};

static const struct video_device rga_videodev = {
	.name = "rockchip-rga",
	.fops = &rga_fops,
	.ioctl_ops = &rga_ioctl_ops,
	.minor = -1,
	.release = video_device_release,
	.vfl_dir = VFL_DIR_M2M,
	.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING,
};

static int rga_parse_dt(struct rga_core *core)
{
	struct reset_control *core_rst, *axi_rst, *ahb_rst;
	int ret;

	core_rst = devm_reset_control_get(core->dev, "core");
	if (IS_ERR(core_rst)) {
		dev_err(core->dev, "failed to get core reset controller\n");
		return PTR_ERR(core_rst);
	}

	axi_rst = devm_reset_control_get(core->dev, "axi");
	if (IS_ERR(axi_rst)) {
		dev_err(core->dev, "failed to get axi reset controller\n");
		return PTR_ERR(axi_rst);
	}

	ahb_rst = devm_reset_control_get(core->dev, "ahb");
	if (IS_ERR(ahb_rst)) {
		dev_err(core->dev, "failed to get ahb reset controller\n");
		return PTR_ERR(ahb_rst);
	}

	reset_control_assert(core_rst);
	udelay(1);
	reset_control_deassert(core_rst);

	reset_control_assert(axi_rst);
	udelay(1);
	reset_control_deassert(axi_rst);

	reset_control_assert(ahb_rst);
	udelay(1);
	reset_control_deassert(ahb_rst);

	ret = devm_clk_bulk_get_all(core->dev, &core->clks);
	if (ret < 0) {
		dev_err(core->dev, "failed to get clocks\n");
		return ret;
	}
	core->num_clks = ret;

	return 0;
}

static int rga_core_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct rockchip_rga *rga;
	struct rga_core *core;
	struct video_device *vfd;
	int ret = 0;
	int irq;

	if (!pdev->dev.of_node)
		return -ENODEV;

	rga = devm_kzalloc(&pdev->dev, sizeof(*rga) + 1 * sizeof(*rga->cores), GFP_KERNEL);
	if (!rga)
		return -ENOMEM;

	rga->hw = of_device_get_match_data(&pdev->dev);
	if (!rga->hw)
		return dev_err_probe(&pdev->dev, -ENODEV, "failed to get match data\n");

	spin_lock_init(&rga->ctrl_lock);
	mutex_init(&rga->mutex);

	core = devm_kzalloc(&pdev->dev, sizeof(*core), GFP_KERNEL);
	core->rga = rga;
	core->dev = &pdev->dev;

	rga->cores[0] = core;

	ret = rga_parse_dt(core);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Unable to parse OF data\n");

	pm_runtime_set_autosuspend_delay(core->dev, 50);
	pm_runtime_enable(core->dev);

	core->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(core->regs)) {
		ret = PTR_ERR(core->regs);
		goto err_put_clk;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_put_clk;
	}

	ret = devm_request_irq(core->dev, irq, rga_isr,
			       rga_has_internal_iommu(rga) ? 0 : IRQF_SHARED,
			       dev_name(core->dev), core);
	if (ret < 0) {
		dev_err(core->dev, "failed to request irq\n");
		goto err_put_clk;
	}

	ret = dma_set_mask_and_coherent(core->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(core->dev, "32-bit DMA not supported");
		goto err_put_clk;
	}

	ret = v4l2_device_register(&pdev->dev, &rga->v4l2_dev);
	if (ret)
		goto err_put_clk;
	vfd = video_device_alloc();
	if (!vfd) {
		v4l2_err(&rga->v4l2_dev, "Failed to allocate video device\n");
		ret = -ENOMEM;
		goto unreg_v4l2_dev;
	}
	*vfd = rga_videodev;
	vfd->lock = &rga->mutex;
	vfd->v4l2_dev = &rga->v4l2_dev;

	video_set_drvdata(vfd, rga);
	rga->vfd = vfd;

	platform_set_drvdata(pdev, core);
	rga->m2m_dev = v4l2_m2m_init(&rga_m2m_ops);
	if (IS_ERR(rga->m2m_dev)) {
		v4l2_err(&rga->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(rga->m2m_dev);
		goto rel_vdev;
	}

	ret = pm_runtime_resume_and_get(core->dev);
	if (ret < 0)
		goto rel_m2m;

	rga->version = rga->hw->get_version(core);

	v4l2_info(&rga->v4l2_dev, "HW Version: 0x%02x.%02x\n",
		  rga->version.major, rga->version.minor);

	pm_runtime_put(core->dev);

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&rga->v4l2_dev, "Failed to register video device\n");
		goto rel_m2m;
	}

	v4l2_info(&rga->v4l2_dev, "Registered %s as /dev/%s\n",
		  vfd->name, video_device_node_name(vfd));

	return 0;

rel_m2m:
	v4l2_m2m_release(rga->m2m_dev);
rel_vdev:
	video_device_release(vfd);
unreg_v4l2_dev:
	v4l2_device_unregister(&rga->v4l2_dev);
err_put_clk:
	pm_runtime_disable(core->dev);

	return ret;
}

static void rga_core_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct rga_core *core = dev_get_drvdata(dev);
	struct rockchip_rga *rga = core->rga;

	v4l2_info(&rga->v4l2_dev, "Removing\n");

	v4l2_m2m_release(rga->m2m_dev);
	video_unregister_device(rga->vfd);
	v4l2_device_unregister(&rga->v4l2_dev);

	pm_runtime_disable(core->dev);
}

static const struct component_ops rga_core_ops = {
	.bind = rga_core_bind,
	.unbind = rga_core_unbind,
};

static int rga_core_probe(struct platform_device *pdev)
{
	int ret = 0;

	ret = component_add(&pdev->dev, &rga_core_ops);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to register component: %d", ret);
		return ret;
	}

	return 0;
}

static void rga_core_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &rga_core_ops);
}

static int __maybe_unused rga_runtime_suspend(struct device *dev)
{
	struct rga_core *core = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(core->num_clks, core->clks);

	return 0;
}

static int __maybe_unused rga_runtime_resume(struct device *dev)
{
	struct rga_core *core = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(core->num_clks, core->clks);
}

static const struct dev_pm_ops rga_core_pm = {
	SET_RUNTIME_PM_OPS(rga_runtime_suspend,
			   rga_runtime_resume, NULL)
};

static const struct of_device_id rockchip_rga_match[] = {
	{
		.compatible = "rockchip,rk3288-rga",
		.data = &rga2_hw,
	},
	{
		.compatible = "rockchip,rk3399-rga",
		.data = &rga2_hw,
	},
	{
		.compatible = "rockchip,rk3588-rga3",
		.data = &rga3_hw,
	},
	{},
};

MODULE_DEVICE_TABLE(of, rockchip_rga_match);

static struct platform_driver rga_core_pdrv = {
	.probe = rga_core_probe,
	.remove = rga_core_remove,
	.driver = {
		.name = RGA_NAME "-core",
		.pm = &rga_core_pm,
		.of_match_table = rockchip_rga_match,
	},
};

static int rga_bind(struct device *dev)
{
	int ret;

	ret = component_bind_all(dev, NULL);
	if (ret) {
		dev_err(dev, "component bind failed\n");
		return ret;
	}

	return 0;
}

static void rga_unbind(struct device *dev)
{
	component_unbind_all(dev, NULL);
}

struct component_master_ops rga_master_ops = {
	.bind = rga_bind,
	.unbind = rga_unbind,
};

static int rga_probe(struct platform_device *pdev)
{
	const struct of_device_id *match_desc = pdev->dev.platform_data;
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct device_node *core_node;

	if (!match_desc)
		return dev_err_probe(dev, -ENODEV, "missing platform data\n");

	for_each_compatible_node(core_node, NULL, match_desc->compatible) {
		if (!of_device_is_available(core_node))
			continue;

		of_node_get(core_node);
		component_match_add_release(dev, &match, component_release_of,
					    component_compare_of, core_node);

		/*
		 * As multi core is not implemented yet,
		 * break out of the loop to only have one core per rockchip_rga struct.
		 * Also put the node, which otherwise would've been done by the loop iteration.
		 */
		of_node_put(core_node);
		break;
	}

	if (!match)
		return dev_err_probe(
			dev, -ENODEV,
			"no matching available component devices found\n");

	return component_master_add_with_match(dev, &rga_master_ops, match);
}

static void rga_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &rga_master_ops);
}

static struct platform_driver rga_pdrv = {
	.probe = rga_probe,
	.remove = rga_remove,
	.driver = {
		.name = RGA_NAME,
	},
};

static bool rga_of_has_available_node(const char *compat)
{
	struct device_node *node;

	for_each_compatible_node(node, NULL, compat) {
		if (of_device_is_available(node)) {
			of_node_put(node);
			return true;
		}
	}

	return false;
}

static int rga_create_platform_device(struct platform_device **ppdev,
				      const struct of_device_id *match)
{
	struct platform_device *pdev;
	int ret;

	pdev = platform_device_alloc(match->compatible, PLATFORM_DEVID_NONE);
	if (!pdev)
		return -ENOMEM;

	ret = platform_device_add_data(pdev, match, sizeof(*match));
	if (ret)
		goto free_platform_device;

	ret = platform_device_add(pdev);
	if (ret)
		goto free_platform_device;

	ret = device_driver_attach(&rga_pdrv.driver, &pdev->dev);
	if (ret)
		goto del_platform_device;

	*ppdev = pdev;

	return 0;

del_platform_device:
	platform_device_del(pdev);
free_platform_device:
	platform_device_put(pdev);
	return ret;
}

static struct platform_device *master_pdevs[ARRAY_SIZE(rockchip_rga_match) - 1];

static int __init rga_init(void)
{
	int ret;
	unsigned int i;

	ret = platform_driver_register(&rga_core_pdrv);
	if (ret != 0)
		return ret;

	ret = platform_driver_register(&rga_pdrv);
	if (ret != 0)
		goto unregister_core_driver;

	for (i = 0; i < ARRAY_SIZE(master_pdevs); i++) {
		if (!rga_of_has_available_node(
			    rockchip_rga_match[i].compatible))
			continue;

		ret = rga_create_platform_device(&master_pdevs[i],
						 &rockchip_rga_match[i]);
		if (ret)
			goto unregister_platform_devices;
	}

	return 0;

unregister_platform_devices:
	for (i = 0; i < ARRAY_SIZE(master_pdevs); i++) {
		platform_device_unregister(master_pdevs[i]);
		master_pdevs[i] = NULL;
	}
	platform_driver_unregister(&rga_pdrv);
unregister_core_driver:
	platform_driver_unregister(&rga_core_pdrv);
	return ret;
}
module_init(rga_init);

static void __exit rga_exit(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(master_pdevs); i++) {
		platform_device_unregister(master_pdevs[i]);
		master_pdevs[i] = NULL;
	}
	platform_driver_unregister(&rga_pdrv);
	platform_driver_unregister(&rga_core_pdrv);
}
module_exit(rga_exit);

MODULE_AUTHOR("Jacob Chen <jacob-chen@iotwrt.com>");
MODULE_DESCRIPTION("Rockchip Raster 2d Graphic Acceleration Unit");
MODULE_LICENSE("GPL");
