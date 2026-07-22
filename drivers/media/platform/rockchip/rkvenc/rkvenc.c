// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VEPU510 (RK3576) hardware video encoder driver.
 *
 * Copyright (C) 2026 Jiaxing Hu <gahing@gahingwoo.com>
 *
 * Architecture notes (see also rkvenc-regs.h):
 *
 *  - This is a *stateful* V4L2 mem2mem encoder (raw NV12 in on OUTPUT,
 *    H.264 Annex-B out on CAPTURE) modelled on
 *    drivers/media/platform/verisilicon's hantro_drv.c device_run()/
 *    codec_ops{run,done} split, NOT on rkvdec's stateless request-API
 *    decoder pattern — an encoder has no per-frame bitstream to parse,
 *    so there is nothing analogous to rkvdec_run_preamble/postamble here.
 *
 *  - The vendor downstream driver (rockchip-linux/kernel,
 *    drivers/video/rockchip/mpp/mpp_rkvenc2.c) groups rkvenc0/rkvenc1
 *    under a "CCU" (rockchip,rkv-encoder-rk3576-ccu) that does pure
 *    software task-queue load balancing across both cores, plus an
 *    optional DCHS (dual-core-handshake) register protocol used only
 *    when *deliberately* splitting one frame's rows across both cores.
 *    There is no hardware descriptor/link-list engine behind it (unlike
 *    the decoder's CCU). v1 of this driver does not implement either:
 *    rkvenc0 and rkvenc1 are exposed as two independent V4L2 M2M device
 *    nodes, each driving one physical core standalone — the same
 *    simplification rkvdec itself makes for multi-core VDPU hardware
 *    (see rkvdec_disable_multicore()).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "rkvenc.h"
#include "rkvenc-regs.h"

#define RKVENC_WATCHDOG_TIMEOUT_MS	800
#define RKVENC_DEFAULT_WIDTH		176
#define RKVENC_DEFAULT_HEIGHT		144
#define RKVENC_MAX_WIDTH		4096
#define RKVENC_MAX_HEIGHT		2560

#define RKVENC_AUTOSUSPEND_DELAY_MS		100

/* Per-resolution hardware watchdog threshold table, ported from the
 * downstream vendor driver's rkvenc2_calc_timeout_thd() /
 * rkvenc2_timeout_thd_by_rsl[]. VEPU510 scales the ENC_WDG register in
 * units of 256 core-clock cycles (other VEPU variants use 1024) — this
 * ×256 factor is confirmed from the vendor kernel source. The field
 * itself (vs_load_thd, bits[23:0] of ENC_WDG per mpp's Vepu510ControlCfg)
 * is 24 bits wide, not "the register's top byte" as an earlier reading of
 * the vendor kernel driver's comments suggested; that earlier reading
 * produced a wrong shift (see git history).
 */
struct rkvenc_wdg_entry {
	u32 pixels;
	u32 ms;
};

static const struct rkvenc_wdg_entry rkvenc_wdg_table[] = {
	{ 1920 * 1088, 50 },
	{ 2560 * 1440, 100 },
	{ 4096 * 2304, 200 },
	{ 8192 * 8192, 400 },
	{ 15360 * 8640, 800 },
};

u32 rkvenc_calc_wdg(struct rkvenc_dev *dev, unsigned int width,
		    unsigned int height)
{
	unsigned int pixels = width * height;
	unsigned int ms = rkvenc_wdg_table[ARRAY_SIZE(rkvenc_wdg_table) - 1].ms;
	u64 cycles;
	u32 thd;
	int i;

	for (i = 0; i < ARRAY_SIZE(rkvenc_wdg_table); i++) {
		if (pixels <= rkvenc_wdg_table[i].pixels) {
			ms = rkvenc_wdg_table[i].ms;
			break;
		}
	}

	cycles = (u64)ms * (dev->core_clk_rate / 1000);
	do_div(cycles, 256);
	thd = min_t(u64, cycles, 0xffffff);

	return thd;
}

/* ---- format helpers ---- */

static const u32 rkvenc_src_fourcc = V4L2_PIX_FMT_NV12;

static const struct rkvenc_coded_fmt_desc *rkvenc_coded_descs[] = {
	&rkvenc_h264_fmt_desc,
};

static const struct rkvenc_coded_fmt_desc *rkvenc_find_coded_desc(u32 fourcc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rkvenc_coded_descs); i++) {
		if (rkvenc_coded_descs[i]->fourcc == fourcc)
			return rkvenc_coded_descs[i];
	}

	return NULL;
}

static u32 rkvenc_nv12_sizeimage(unsigned int width, unsigned int height)
{
	return width * height + 2 * ALIGN(width, 2) / 2 * ALIGN(height, 2) / 2 * 2;
}

static u32 rkvenc_coded_sizeimage(unsigned int width, unsigned int height)
{
	/* Generous fixed upper bound for a single fixed-QP baseline frame;
	 * revisit once real CBR/VBR budget control lands.
	 */
	return max_t(u32, width * height, SZ_128K);
}

static void rkvenc_fill_src_fmt(struct v4l2_pix_format *fmt)
{
	fmt->pixelformat = rkvenc_src_fourcc;
	fmt->width = clamp_t(u32, fmt->width, RKVENC_DEFAULT_WIDTH, RKVENC_MAX_WIDTH);
	fmt->height = clamp_t(u32, fmt->height, RKVENC_DEFAULT_HEIGHT, RKVENC_MAX_HEIGHT);
	fmt->width = ALIGN(fmt->width, 2);
	fmt->height = ALIGN(fmt->height, 2);
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_REC709;
	fmt->bytesperline = fmt->width;
	fmt->sizeimage = rkvenc_nv12_sizeimage(fmt->width, fmt->height);
}

static void rkvenc_fill_coded_fmt(struct v4l2_pix_format *fmt,
				  const struct rkvenc_coded_fmt_desc *desc)
{
	fmt->pixelformat = desc->fourcc;
	fmt->width = clamp_t(u32, fmt->width, desc->frmsize.frmsize_min_width,
			      desc->frmsize.frmsize_max_width);
	fmt->height = clamp_t(u32, fmt->height, desc->frmsize.frmsize_min_height,
			       desc->frmsize.frmsize_max_height);
	fmt->width = ALIGN(fmt->width, 2);
	fmt->height = ALIGN(fmt->height, 2);
	fmt->field = V4L2_FIELD_NONE;
	fmt->bytesperline = 0;
	fmt->sizeimage = rkvenc_coded_sizeimage(fmt->width, fmt->height);
}

/* ---- ioctl_ops ---- */

static int rkvenc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strscpy(cap->driver, "rkvenc", sizeof(cap->driver));
	strscpy(cap->card, "rockchip,rkv-encoder-rk3576", sizeof(cap->card));
	return 0;
}

static int rkvenc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(rkvenc_coded_descs))
		return -EINVAL;

	f->pixelformat = rkvenc_coded_descs[f->index]->fourcc;
	f->flags = V4L2_FMT_FLAG_COMPRESSED;
	return 0;
}

static int rkvenc_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;

	f->pixelformat = rkvenc_src_fourcc;
	return 0;
}

static int rkvenc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rkvenc_ctx *ctx = fh_to_rkvenc_ctx(file_to_v4l2_fh(file));

	f->fmt.pix = ctx->coded_fmt;
	return 0;
}

static int rkvenc_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rkvenc_ctx *ctx = fh_to_rkvenc_ctx(file_to_v4l2_fh(file));

	f->fmt.pix = ctx->src_fmt;
	return 0;
}

static int rkvenc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	const struct rkvenc_coded_fmt_desc *desc;

	desc = rkvenc_find_coded_desc(f->fmt.pix.pixelformat);
	if (!desc)
		desc = rkvenc_coded_descs[0];

	rkvenc_fill_coded_fmt(&f->fmt.pix, desc);
	return 0;
}

static int rkvenc_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	rkvenc_fill_src_fmt(&f->fmt.pix);
	return 0;
}

static int rkvenc_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rkvenc_ctx *ctx = fh_to_rkvenc_ctx(file_to_v4l2_fh(file));
	const struct rkvenc_coded_fmt_desc *desc;

	if (vb2_is_busy(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;

	desc = rkvenc_find_coded_desc(f->fmt.pix.pixelformat);
	if (!desc)
		desc = rkvenc_coded_descs[0];

	rkvenc_fill_coded_fmt(&f->fmt.pix, desc);
	ctx->coded_fmt = f->fmt.pix;
	ctx->coded_desc = desc;
	return 0;
}

static int rkvenc_s_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rkvenc_ctx *ctx = fh_to_rkvenc_ctx(file_to_v4l2_fh(file));

	if (vb2_is_busy(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx)))
		return -EBUSY;

	rkvenc_fill_src_fmt(&f->fmt.pix);
	ctx->src_fmt = f->fmt.pix;
	return 0;
}

static const struct v4l2_ioctl_ops rkvenc_ioctl_ops = {
	.vidioc_querycap		= rkvenc_querycap,

	.vidioc_enum_fmt_vid_cap	= rkvenc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= rkvenc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= rkvenc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= rkvenc_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out	= rkvenc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out		= rkvenc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out		= rkvenc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out		= rkvenc_s_fmt_vid_out,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_try_encoder_cmd		= v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd		= v4l2_m2m_ioctl_encoder_cmd,

	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

/* ---- vb2_ops ---- */

static int rkvenc_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
			      unsigned int *num_planes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct rkvenc_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *fmt;

	fmt = V4L2_TYPE_IS_OUTPUT(vq->type) ? &ctx->src_fmt : &ctx->coded_fmt;

	if (*num_planes) {
		if (sizes[0] < fmt->sizeimage)
			return -EINVAL;
	} else {
		*num_planes = 1;
		sizes[0] = fmt->sizeimage;
	}

	return 0;
}

static int rkvenc_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rkvenc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format *fmt;

	fmt = V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type) ? &ctx->src_fmt : &ctx->coded_fmt;

	if (vb2_plane_size(vb, 0) < fmt->sizeimage)
		return -EINVAL;

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
		vb2_set_plane_payload(vb, 0, fmt->sizeimage);
	else
		vbuf->field = V4L2_FIELD_NONE;

	return 0;
}

static void rkvenc_buf_queue(struct vb2_buffer *vb)
{
	struct rkvenc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int rkvenc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct rkvenc_ctx *ctx = vb2_get_drv_priv(q);
	int ret = 0;

	if (V4L2_TYPE_IS_OUTPUT(q->type) && ctx->coded_desc && ctx->coded_desc->ops->start)
		ret = ctx->coded_desc->ops->start(ctx);

	if (ret) {
		struct vb2_v4l2_buffer *vbuf;

		while ((vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_QUEUED);
	}

	return ret;
}

static void rkvenc_stop_streaming(struct vb2_queue *q)
{
	struct rkvenc_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (!vbuf)
			break;

		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}

	if (V4L2_TYPE_IS_OUTPUT(q->type) && ctx->coded_desc && ctx->coded_desc->ops->stop)
		ctx->coded_desc->ops->stop(ctx);
}

static const struct vb2_ops rkvenc_qops = {
	.queue_setup	= rkvenc_queue_setup,
	.buf_prepare	= rkvenc_buf_prepare,
	.buf_queue	= rkvenc_buf_queue,
	.start_streaming = rkvenc_start_streaming,
	.stop_streaming	= rkvenc_stop_streaming,
};

static int rkvenc_queue_init(void *priv, struct vb2_queue *src_vq,
			     struct vb2_queue *dst_vq)
{
	struct rkvenc_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &rkvenc_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->vfd_mutex;
	src_vq->dev = ctx->dev->v4l2_dev.dev;
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &rkvenc_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->vfd_mutex;
	dst_vq->dev = ctx->dev->v4l2_dev.dev;
	return vb2_queue_init(dst_vq);
}

/* ---- v4l2_m2m_ops ---- */

void rkvenc_job_finish(struct rkvenc_dev *dev, struct rkvenc_ctx *ctx,
		       enum vb2_buffer_state state)
{
	clk_bulk_disable(dev->num_clocks, dev->clocks);
	pm_runtime_mark_last_busy(dev->dev);
	pm_runtime_put_autosuspend(dev->dev);

	dev->cur_ctx = NULL;
	v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx, state);
}

static void rkvenc_device_run(void *priv)
{
	struct rkvenc_ctx *ctx = priv;
	struct rkvenc_dev *dev = ctx->dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev->dev);
	if (ret < 0) {
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
						  VB2_BUF_STATE_ERROR);
		return;
	}

	ret = clk_bulk_enable(dev->num_clocks, dev->clocks);
	if (ret) {
		pm_runtime_put_autosuspend(dev->dev);
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
						  VB2_BUF_STATE_ERROR);
		return;
	}

	v4l2_m2m_buf_copy_metadata(rkvenc_get_src_buf(ctx), rkvenc_get_dst_buf(ctx));

	dev->cur_ctx = ctx;
	dev->irq_status = 0;
	schedule_delayed_work(&dev->watchdog_work,
			      msecs_to_jiffies(RKVENC_WATCHDOG_TIMEOUT_MS));

	ctx->coded_desc->ops->run(ctx);
}

static const struct v4l2_m2m_ops rkvenc_m2m_ops = {
	.device_run = rkvenc_device_run,
};

/* ---- IRQ handling ---- */

static bool rkvenc_drain_slices(struct rkvenc_dev *dev)
{
	unsigned int count = rkvenc_read(dev, RKVENC_REG_ST_SLICE_NUM) & 0x3f;
	bool last = false;
	unsigned int i;
	u32 val;

	for (i = 0; i < count; i++) {
		val = rkvenc_read(dev, RKVENC_REG_ST_SLICE_LEN);
		if (val & BIT(31))
			last = true;
	}

	return last;
}

static irqreturn_t rkvenc_irq(int irq, void *priv)
{
	struct rkvenc_dev *dev = priv;
	u32 status = rkvenc_read(dev, RKVENC_REG_INT_STA);
	bool done = false;

	rkvenc_write(dev, RKVENC_REG_INT_CLR, status);

	if (status & BIT(8) /* wdg_sta */) {
		u32 mask = rkvenc_read(dev, RKVENC_REG_INT_MASK);

		rkvenc_write(dev, RKVENC_REG_INT_MASK, mask | BIT(8));
	}

	if (!dev->cur_ctx)
		return status ? IRQ_HANDLED : IRQ_NONE;

	/* VEPU510 errata (vendor comment): int_sta ENC_DONE is not reliable
	 * on its own — the hardware has been observed to skip raising it
	 * even though the frame is actually finished. The slice-length
	 * FIFO's "last" flag (always populated because ENC_PIC.SLEN_FIFO is
	 * forced on for every VEPU510 task, see rkvenc_vepu510_quirk()) is
	 * the authoritative completion signal instead.
	 */
	if (rkvenc_drain_slices(dev)) {
		rkvenc_write(dev, RKVENC_REG_ENC_ID, 0);
		udelay(5);
		status |= rkvenc_read(dev, RKVENC_REG_INT_STA);
		done = true;
	}

	if (status & (BIT(0) /* enc_done_sta */ | RKVENC_INT_ERROR_MASK))
		done = true;

	dev->irq_status |= status;

	return done ? IRQ_WAKE_THREAD : IRQ_HANDLED;
}

/* Graceful core recovery after an error/timeout, mirroring the downstream
 * vendor kernel's rkvenc_soft_reset() (mpp_rkvenc2.c): a SAFE clear via
 * ENC_CLR drains the core's outstanding AXI transactions so the IOMMU can
 * disable cleanly, then a FORCE clear + interrupt-state wipe returns the core
 * to a defined state. Without this, a hung P-frame leaves the core wedged and
 * an unacknowledged write fault stuck active, which cascades into the NEXT
 * encode failing ("rk_iommu Disable paging request timed out"). Only ever
 * called from the IRQ error path, so it can never touch a good frame; clocks
 * are still enabled here (rkvenc_job_finish disables them afterwards).
 */
static void rkvenc_soft_reset(struct rkvenc_dev *dev)
{
	u32 sta;
	int ret;

	rkvenc_write(dev, RKVENC_REG_INT_MASK, 0x3ff);
	rkvenc_write(dev, RKVENC_REG_ENC_CLR, RKVENC_ENC_CLR_SAFE);
	ret = readl_relaxed_poll_timeout(dev->regs + RKVENC_REG_INT_STA, sta,
					 sta & RKVENC_SCLR_DONE_STA, 0, 1000);
	rkvenc_write(dev, RKVENC_REG_ENC_CLR, RKVENC_ENC_CLR_FORCE);
	udelay(5);
	rkvenc_write(dev, RKVENC_REG_ENC_CLR, 0);
	rkvenc_write(dev, RKVENC_REG_INT_CLR, 0xffffffff);
	rkvenc_write(dev, RKVENC_REG_INT_STA, 0);

	if (ret) {
		/* Safe clear never completed -- fall back to a full core reset. */
		dev_warn(dev->dev, "safe reset timed out, forcing core reset\n");
		reset_control_assert(dev->rst);
		udelay(5);
		reset_control_deassert(dev->rst);
	}
}

static irqreturn_t rkvenc_irq_thread(int irq, void *priv)
{
	struct rkvenc_dev *dev = priv;
	struct rkvenc_ctx *ctx = dev->cur_ctx;
	enum vb2_buffer_state state;

	if (!ctx)
		return IRQ_HANDLED;

	if (!cancel_delayed_work(&dev->watchdog_work))
		return IRQ_HANDLED;

	state = (dev->irq_status & RKVENC_INT_ERROR_MASK) ?
		VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;

	if (state == VB2_BUF_STATE_ERROR) {
		dev_warn(dev->dev,
			 "encode error, int_sta=0x%08x (error bits: %s%s%s%s%s%s%s%s) "
			 "version=0x%08x enc_strt=0x%08x int_en=0x%08x int_msk=0x%08x "
			 "enc_wdg=0x%08x enc_rsl=0x%08x enc_pic=0x%08x synt_sli0=0x%08x "
			 "bs_lgth=0x%08x slice_num=0x%08x st_bsb=0x%08x\n",
			 dev->irq_status,
			 dev->irq_status & BIT(4) ? "vbsf_oflw " : "",
			 dev->irq_status & BIT(6) ? "enc_err " : "",
			 dev->irq_status & BIT(7) ? "vsrc_err " : "",
			 dev->irq_status & BIT(8) ? "wdg " : "",
			 dev->irq_status & BIT(9) ? "lkt_err_int " : "",
			 dev->irq_status & BIT(10) ? "lkt_err_stop " : "",
			 dev->irq_status & BIT(11) ? "lkt_force_stop " : "",
			 dev->irq_status & BIT(15) ? "dvbm_err " : "",
			 /* version is a fixed IP-identification readback: if this
			  * comes back 0x00000000 or 0xffffffff, the MMIO region
			  * itself is dead (clock/power/mapping), not an encode
			  * config problem — a much more fundamental class of bug
			  * than anything register-value-level we've audited so far.
			  */
			 rkvenc_read(dev, RKVENC_REG_VERSION),
			 rkvenc_read(dev, RKVENC_REG_ENC_START),
			 rkvenc_read(dev, RKVENC_REG_INT_EN),
			 rkvenc_read(dev, RKVENC_REG_INT_MASK),
			 rkvenc_read(dev, RKVENC_REG_ENC_WDG),
			 rkvenc_read(dev, RKVENC_REG_ENC_RSL),
			 rkvenc_read(dev, RKVENC_REG_ENC_PIC),
			 rkvenc_read(dev, RKVENC_REG_SYNT_SLI0),
			 rkvenc_read(dev, RKVENC_REG_ST_BS_LENGTH),
			 rkvenc_read(dev, RKVENC_REG_ST_SLICE_NUM),
			 rkvenc_read(dev, RKVENC_REG_ST_BSB));

		/* Release the DVBM/encoder hold (only the drain-slices completion
		 * path did this before) and gracefully reset the wedged core so
		 * the fault is drained and the NEXT encode starts clean.
		 */
		rkvenc_write(dev, RKVENC_REG_ENC_ID, 0);
		rkvenc_soft_reset(dev);
	}

	ctx->coded_desc->ops->done(ctx, state);
	rkvenc_job_finish(dev, ctx, state);

	return IRQ_HANDLED;
}

static void rkvenc_watchdog(struct work_struct *work)
{
	struct rkvenc_dev *dev = container_of(work, struct rkvenc_dev,
					      watchdog_work.work);
	struct rkvenc_ctx *ctx = dev->cur_ctx;

	if (!ctx)
		return;

	dev_warn(dev->dev, "encode timeout, resetting core\n");

	reset_control_assert(dev->rst);
	udelay(5);
	reset_control_deassert(dev->rst);

	ctx->coded_desc->ops->done(ctx, VB2_BUF_STATE_ERROR);
	rkvenc_job_finish(dev, ctx, VB2_BUF_STATE_ERROR);
}

/* ---- ctrls / file ops ---- */

static int rkvenc_open(struct file *file)
{
	struct rkvenc_dev *dev = video_drvdata(file);
	struct rkvenc_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	v4l2_fh_init(&ctx->fh, &dev->vfd);
	file->private_data = &ctx->fh;
	ctx->dev = dev;

	ctx->coded_desc = rkvenc_coded_descs[0];
	ctx->coded_fmt.width = RKVENC_DEFAULT_WIDTH;
	ctx->coded_fmt.height = RKVENC_DEFAULT_HEIGHT;
	rkvenc_fill_coded_fmt(&ctx->coded_fmt, ctx->coded_desc);
	ctx->src_fmt.width = RKVENC_DEFAULT_WIDTH;
	ctx->src_fmt.height = RKVENC_DEFAULT_HEIGHT;
	rkvenc_fill_src_fmt(&ctx->src_fmt);

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 16);
	ctx->fh.ctrl_handler = &ctx->ctrl_handler;

	ret = ctx->coded_desc->ops->init_ctrls(ctx);
	if (!ret)
		ret = ctx->ctrl_handler.error;
	if (!ret)
		ret = v4l2_ctrl_handler_setup(&ctx->ctrl_handler);
	if (ret)
		goto err_free_handler;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, rkvenc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_free_handler;
	}

	v4l2_fh_add(&ctx->fh, file);

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int rkvenc_release(struct file *file)
{
	struct rkvenc_ctx *ctx = fh_to_rkvenc_ctx(file->private_data);

	v4l2_fh_del(&ctx->fh, file);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations rkvenc_fops = {
	.owner		= THIS_MODULE,
	.open		= rkvenc_open,
	.release	= rkvenc_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

/* ---- probe/remove ---- */

static int rkvenc_runtime_suspend(struct device *dev)
{
	struct rkvenc_dev *rkvenc = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(rkvenc->num_clocks, rkvenc->clocks);
	return 0;
}

static int rkvenc_runtime_resume(struct device *dev)
{
	struct rkvenc_dev *rkvenc = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(rkvenc->num_clocks, rkvenc->clocks);
}

static const struct dev_pm_ops rkvenc_pm_ops = {
	SET_RUNTIME_PM_OPS(rkvenc_runtime_suspend, rkvenc_runtime_resume, NULL)
};

static int rkvenc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rkvenc_dev *rkvenc;
	struct clk_bulk_data *clk;
	int irq, ret, i;

	rkvenc = devm_kzalloc(dev, sizeof(*rkvenc), GFP_KERNEL);
	if (!rkvenc)
		return -ENOMEM;

	rkvenc->dev = dev;
	platform_set_drvdata(pdev, rkvenc);
	mutex_init(&rkvenc->vfd_mutex);
	INIT_DELAYED_WORK(&rkvenc->watchdog_work, rkvenc_watchdog);

	rkvenc->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rkvenc->regs))
		return PTR_ERR(rkvenc->regs);

	/* Buffer pointer registers (adr_src0/1/2, bsb*, rfp*) are 32 bits
	 * wide; each core sits behind its own rkvenc{0,1}_mmu IOMMU, which
	 * this DMA mask keeps within a 32-bit IOVA aperture.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA mask\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_clk_bulk_get_all(dev, &rkvenc->clocks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get clocks\n");
	rkvenc->num_clocks = ret;

	for (i = 0; i < rkvenc->num_clocks; i++) {
		clk = &rkvenc->clocks[i];
		if (!strcmp(clk->id, "clk_core"))
			rkvenc->core_clk_rate = clk_get_rate(clk->clk);
	}
	if (!rkvenc->core_clk_rate)
		rkvenc->core_clk_rate = 702000000;

	rkvenc->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(rkvenc->rst))
		return dev_err_probe(dev, PTR_ERR(rkvenc->rst),
				     "failed to get resets\n");

	ret = devm_request_threaded_irq(dev, irq, rkvenc_irq, rkvenc_irq_thread,
					IRQF_ONESHOT, dev_name(dev), rkvenc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	ret = v4l2_device_register(dev, &rkvenc->v4l2_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register v4l2 device\n");

	rkvenc->m2m_dev = v4l2_m2m_init(&rkvenc_m2m_ops);
	if (IS_ERR(rkvenc->m2m_dev)) {
		ret = PTR_ERR(rkvenc->m2m_dev);
		goto err_unreg_v4l2;
	}

	rkvenc->vfd = (struct video_device){
		.fops		= &rkvenc_fops,
		.ioctl_ops	= &rkvenc_ioctl_ops,
		.minor		= -1,
		.release	= video_device_release_empty,
		.lock		= &rkvenc->vfd_mutex,
		.v4l2_dev	= &rkvenc->v4l2_dev,
		.vfl_dir	= VFL_DIR_M2M,
		.device_caps	= V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
	};
	strscpy(rkvenc->vfd.name, "rkvenc", sizeof(rkvenc->vfd.name));
	video_set_drvdata(&rkvenc->vfd, rkvenc);

	pm_runtime_set_autosuspend_delay(dev, RKVENC_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	devm_pm_runtime_enable(dev);

	/*
	 * One-time reset cycle (via a real pm_runtime activation, so the
	 * power domain and clocks are genuinely on rather than assuming raw
	 * clk_bulk_prepare_enable() alone would power an unattached genpd)
	 * so the core starts from a defined state rather than whatever the
	 * bootloader/ATF left it in -- otherwise reset is only ever touched
	 * by rkvenc_watchdog()'s error-recovery path, never on a normal
	 * first activation. Board bring-up saw an isolated rk_iommu write
	 * fault at an IOVA that changed between boots and matched none of
	 * this driver's own buffer addresses, consistent with undefined
	 * boot-varying hardware state.
	 *
	 * Done once here, not on every rkvenc_runtime_resume(): that was
	 * tried first and made things worse (a hardware-reported "enc_err"
	 * status on the very next encode, with a bitstream length wildly
	 * larger than the same test frame ever produced before) -- something
	 * about this core's state is expected to persist for the life of the
	 * session, not be re-initialized before every single frame.
	 */
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to power up for initial reset\n");
	reset_control_assert(rkvenc->rst);
	udelay(5);
	reset_control_deassert(rkvenc->rst);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	ret = video_register_device(&rkvenc->vfd, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register video device\n");
		goto err_release_m2m;
	}

	dev_info(dev, "VEPU510 encoder core registered as %s\n",
		 video_device_node_name(&rkvenc->vfd));

	return 0;

err_release_m2m:
	v4l2_m2m_release(rkvenc->m2m_dev);
err_unreg_v4l2:
	v4l2_device_unregister(&rkvenc->v4l2_dev);
	return ret;
}

static void rkvenc_remove(struct platform_device *pdev)
{
	struct rkvenc_dev *rkvenc = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&rkvenc->watchdog_work);
	video_unregister_device(&rkvenc->vfd);
	v4l2_m2m_release(rkvenc->m2m_dev);
	v4l2_device_unregister(&rkvenc->v4l2_dev);
}

static const struct of_device_id rkvenc_of_match[] = {
	{ .compatible = "rockchip,rk3576-vepu" },
	{}
};
MODULE_DEVICE_TABLE(of, rkvenc_of_match);

static struct platform_driver rkvenc_driver = {
	.probe = rkvenc_probe,
	.remove = rkvenc_remove,
	.driver = {
		.name = "rkvenc",
		.of_match_table = rkvenc_of_match,
		.pm = &rkvenc_pm_ops,
	},
};
module_platform_driver(rkvenc_driver);

MODULE_DESCRIPTION("Rockchip VEPU510 video encoder driver");
MODULE_LICENSE("GPL");
