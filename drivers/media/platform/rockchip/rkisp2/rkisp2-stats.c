// SPDX-License-Identifier: (GPL-2.0-or-later OR MIT)
/*
 * Rockchip ISP2 Driver - Stats subdevice
 *
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2026 Ideas on Board Oy.
 */

#include <media/v4l2-common.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-isp.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>

#include "rkisp2-common.h"

#define RKISP2_STATS_DEV_NAME	RKISP2_DRIVER_NAME "_stats"

#define RKISP2_STATS_BLOCK_INFO(block, data) \
	[RKISP2_STATS_BLOCK_ ## block] = { \
		.size = sizeof(struct rkisp2_stats_ ## data), \
	}

static const struct v4l2_isp_stats_block_type_info
rkisp2_stats_blocks_info[] = {
	RKISP2_STATS_BLOCK_INFO(AE_LITE, ae_lite),
	RKISP2_STATS_BLOCK_INFO(HIST_LITE, hist),
	RKISP2_STATS_BLOCK_INFO(HIST_BIG0, hist),
	RKISP2_STATS_BLOCK_INFO(HIST_BIG1, hist),
	RKISP2_STATS_BLOCK_INFO(HIST_BIG1, hist),
	RKISP2_STATS_BLOCK_INFO(AWB, awb),
};

/*
 * We only need one instance of hist here since it's only for accessing the
 * members of the union
 */
union rkisp2_stats_block {
	struct v4l2_isp_block_header header;
	struct rkisp2_stats_ae_lite ae_lite;
	struct rkisp2_stats_hist hist;
	struct rkisp2_stats_awb awb;
};

#define rkisp2_init_stats_block(rkisp2, buf, type)			\
	((union rkisp2_stats_block *)					\
	v4l2_isp_stats_init_block((rkisp2)->dev, (buf),			\
				  rkisp2_stats_blocks_info,		\
				  ARRAY_SIZE(rkisp2_stats_blocks_info),	\
				  (type), RKISP2_STATS_MAX_SIZE))

static int rkisp2_stats_enum_fmt_meta_cap(struct file *file, void *priv,
					  struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_RKISP2_STATS;

	return 0;
}

static int rkisp2_stats_g_fmt_meta_cap(struct file *file, void *priv,
				       struct v4l2_format *f)
{
	static const struct v4l2_meta_format mfmt = {
		.dataformat = V4L2_META_FMT_RKISP2_STATS,
		.buffersize = v4l2_isp_buffer_size(RKISP2_STATS_MAX_SIZE)
	};

	f->fmt.meta = mfmt;

	return 0;
}

static int rkisp2_stats_querycap(struct file *file,
				 void *priv, struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, RKISP2_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));

	return 0;
}

static const struct v4l2_ioctl_ops rkisp2_stats_ioctl = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_fmt_meta_cap = rkisp2_stats_enum_fmt_meta_cap,
	.vidioc_g_fmt_meta_cap = rkisp2_stats_g_fmt_meta_cap,
	.vidioc_s_fmt_meta_cap = rkisp2_stats_g_fmt_meta_cap,
	.vidioc_try_fmt_meta_cap = rkisp2_stats_g_fmt_meta_cap,
	.vidioc_querycap = rkisp2_stats_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations rkisp2_stats_fops = {
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
	.poll = vb2_fop_poll,
	.open = v4l2_fh_open,
	.release = vb2_fop_release
};

static int rkisp2_stats_vb2_queue_setup(struct vb2_queue *vq,
					unsigned int *num_buffers,
					unsigned int *num_planes,
					unsigned int sizes[],
					struct device *alloc_devs[])
{
	/* TODO num_buffers */

	*num_planes = 1;

	sizes[0] = v4l2_isp_buffer_size(RKISP2_STATS_MAX_SIZE);

	return 0;
}

static void rkisp2_stats_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct rkisp2_buffer *stats_buf =
		container_of(vbuf, struct rkisp2_buffer, vb);
	struct vb2_queue *vq = vb->vb2_queue;
	struct rkisp2_stats *stats_dev = vq->drv_priv;

	spin_lock_irq(&stats_dev->lock);
	list_add_tail(&stats_buf->queue, &stats_dev->stat);
	spin_unlock_irq(&stats_dev->lock);
}

static int rkisp2_stats_vb2_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < v4l2_isp_buffer_size(RKISP2_STATS_MAX_SIZE))
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, v4l2_isp_buffer_size(RKISP2_STATS_MAX_SIZE));

	return 0;
}

static int rkisp2_stats_vb2_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct rkisp2_stats *stats = vq->drv_priv;
	stats->icr = 0;
	stats->cur_buf = NULL;
	stats->awb_window_offset = 0;

	return 0;
}

static void rkisp2_stats_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct rkisp2_stats *stats = vq->drv_priv;
	struct rkisp2_buffer *buf;
	LIST_HEAD(tmp_list);

	spin_lock_irq(&stats->lock);
	list_splice_init(&stats->stat, &tmp_list);
	spin_unlock_irq(&stats->lock);

	list_for_each_entry(buf, &tmp_list, queue)
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);

	if (stats->cur_buf != NULL) {
		vb2_buffer_done(&stats->cur_buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		stats->cur_buf = NULL;
	}
}

static const struct vb2_ops rkisp2_stats_vb2_ops = {
	.queue_setup = rkisp2_stats_vb2_queue_setup,
	.buf_queue = rkisp2_stats_vb2_buf_queue,
	.buf_prepare = rkisp2_stats_vb2_buf_prepare,
	.start_streaming = rkisp2_stats_vb2_start_streaming,
	.stop_streaming = rkisp2_stats_vb2_stop_streaming,
};

static int
rkisp2_stats_init_vb2_queue(struct vb2_queue *q, struct rkisp2_stats *stats)
{
	struct rkisp2_vdev_node *node;

	node = container_of(q, struct rkisp2_vdev_node, buf_queue);

	q->type = V4L2_BUF_TYPE_META_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = stats;
	q->ops = &rkisp2_stats_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->buf_struct_size = sizeof(struct rkisp2_buffer);
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &node->vlock;
	q->dev = stats->rkisp2->dev;

	return vb2_queue_init(q);
}

static void rkisp2_stats_get_rawae_lite(struct rkisp2_stats *stats,
					u32 status,
					struct v4l2_isp_buffer *pbuf)
{
	struct rkisp2_device *rkisp2 = stats->rkisp2;
	union rkisp2_stats_block *block;
	struct rkisp2_stats_ae_lite *ae_lite;
	unsigned int i;
	u32 val;

	block = rkisp2_init_stats_block(rkisp2, pbuf,
					RKISP2_STATS_BLOCK_AE_LITE);
	if (IS_ERR(block))
		return;

	ae_lite = &block->ae_lite;

	/* TODO figure out what the other channels are for */
	if (!(status & ISP2X_3A_RAWAE_CH0))
		return;
	stats->icr |= ISP2X_3A_RAWAE_CH0;

	for (i = 0; i < RKISP2_ISP_AE_MEAN_MAX_LITE; i++) {
		val = rkisp2_read(rkisp2, ISP_RAWAE_LITE_RO_MEAN(i));
		ae_lite->exp_mean_r[i] = ISP3X_RAWAE_LITE_RO_MEAN_R(val);
		ae_lite->exp_mean_g[i] = ISP3X_RAWAE_LITE_RO_MEAN_G(val);
		ae_lite->exp_mean_b[i] = ISP3X_RAWAE_LITE_RO_MEAN_B(val);
	}

	/*
	 * The done bit is never set in the register; set it here to signal
	 * done to userspace
	 */
	ae_lite->done = 1;
}

static void rkisp2_stats_hist_read(struct rkisp2_stats *stats, u32 reg_base,
				   struct v4l2_isp_buffer *pbuf,
				   enum rkisp2_stats_block_type type)
{
	struct rkisp2_device *rkisp2 = stats->rkisp2;
	unsigned int i;
	u32 val, done;
	unsigned int ctrl = reg_base + ISP_RAWHIST_BIG_CTRL;
	union rkisp2_stats_block *block;
	struct rkisp2_stats_hist *hist;

	block = rkisp2_init_stats_block(rkisp2, pbuf, type);
	if (IS_ERR(block))
		return;

	hist = &block->hist;

	/*
	 * TODO Figure out how the done bit works (it doesn't seem to ever be
	 * set)
	 */
	done = rkisp2_read(rkisp2, ctrl) & ISP_RAWHIST_CTRL_MEAS_DONE;
	hist->done = done ? 1 : 0;
	if (!done)
		return;

	rkisp2_write(rkisp2, reg_base + ISP_RAWHIST_BIG_HRAM_CTRL,
		     ISP_RAWHIST_RAM_OFFSET(0));

	for (i = 0; i < RKISP2_ISP_HIST_BIN_N_MAX; i++) {
		val = rkisp2_read(rkisp2, reg_base + ISP_RAWHIST_BIG_RO_BASE_BIN);
		hist->hist_bins[i] = val;
	}

	/* Set the done bit */
	val = rkisp2_read(rkisp2, ctrl);
	rkisp2_write(rkisp2, ctrl, val | ISP_RAWHIST_CTRL_MEAS_DONE);
}

static void rkisp2_stats_get_hist(struct rkisp2_stats *stats,
				  u32 status,
				  struct v4l2_isp_buffer *pbuf)
{
	/*
	 * It seems that big only triggers the big interrupt, and lite only
	 * triggers the ch0 interrupts. ch1 and ch2 interrupts are therefore
	 * unknown, and we need to check the done bits of every big module to
	 * determine which to process
	 */

	/*
	 * It looks like we get separate interrupts for big in big-mode (ie.
	 * using the 15x15 grid) and for non-big stuff
	 */

	if (status & ISP2X_3A_RAWHIST_CH0) {
		stats->rkisp2->debug.stats3a_hist_ch0_count++;
		stats->icr |= ISP2X_3A_RAWHIST_CH0;
		rkisp2_stats_hist_read(stats, ISP_RAWHIST_LITE_BASE, pbuf,
				       RKISP2_STATS_BLOCK_HIST_LITE);
	}

	if (status & ISP2X_3A_RAWHIST_BIG) {
		stats->rkisp2->debug.stats3a_hist_big_count++;
		stats->icr |= ISP2X_3A_RAWHIST_BIG;
		rkisp2_stats_hist_read(stats, ISP_RAWHIST_BIG1_BASE, pbuf,
				       RKISP2_STATS_BLOCK_HIST_BIG0);
		rkisp2_stats_hist_read(stats, ISP_RAWHIST_BIG2_BASE, pbuf,
				       RKISP2_STATS_BLOCK_HIST_BIG1);
		rkisp2_stats_hist_read(stats, ISP_RAWHIST_BIG3_BASE, pbuf,
				       RKISP2_STATS_BLOCK_HIST_BIG2);
	}

}

static void rkisp2_stats_get_rawawb(struct rkisp2_stats *stats,
				    u32 status,
				    struct v4l2_isp_buffer *pbuf)
{
	struct rkisp2_device *rkisp2 = stats->rkisp2;
	union rkisp2_stats_block *block;
	struct rkisp2_stats_awb *awb;
	unsigned int ctrl = ISP21_RAWAWB_CTRL;
	unsigned int i;
	u32 val1, val2;
	u32 done;

	block = rkisp2_init_stats_block(rkisp2, pbuf, RKISP2_STATS_BLOCK_AWB);
	if (IS_ERR(block))
		return;

	awb = &block->awb;

	if (!(status & ISP2X_3A_RAWAWB))
		return;
	stats->icr |= ISP2X_3A_RAWAWB;

	rkisp2->debug.stats3a_awb_count++;

	done = rkisp2_read(rkisp2, ctrl) & ISP3X_RAWAWB_CTRL_MEAS_DONE;
	awb->done = done ? 1 : 0;
	if (!done)
		return;

	rkisp2->debug.stats3a_awb_done_count++;

	for (i = 0; i < RKISP2_ISP_AWB_COUNTS_SIZE; i++) {
		val1 = rkisp2_read(rkisp2, ISP21_RAWAWB_RAM_DATA_BASE);
		val2 = rkisp2_read(rkisp2, ISP21_RAWAWB_RAM_DATA_BASE);
		awb->counts_r[i] = (val2 >> 4) & 0x3ffff;
		awb->counts_g[i] = (val1 >> 18) | ((val2 & 0xf) << 14);
		awb->counts_b[i] = val1 & 0x3ffff;
		awb->counts_w[i] = val2 >> 22;
	}

	/*
	 * Clear the done bit (reference says write 0 but writing 1 seems to be
	 * the correct reset, plus all the other stats 3a write 1 to reset)
	 */
	val1 = rkisp2_read(rkisp2, ctrl);
	rkisp2_write(rkisp2, ctrl, val1 | ISP3X_RAWAWB_CTRL_MEAS_DONE);
}

/* This is always called in an intterupt context */
static struct rkisp2_buffer *rkisp2_stats_get_buf(struct rkisp2_stats *stats)
{
	struct rkisp2_buffer *ret = NULL;

	if (stats->cur_buf != NULL)
		return stats->cur_buf;

	/* get one empty buffer */
	if (!list_empty(&stats->stat)) {
		ret = list_first_entry(&stats->stat, struct rkisp2_buffer, queue);
		list_del(&ret->queue);
	}

	stats->cur_buf = ret;

	return ret;
}

static void rkisp2_stats_complete_buf(struct rkisp2_stats *stats, struct rkisp2_buffer *buf)
{
	unsigned int frame_sequence = stats->rkisp2->isp.frame_sequence;
	u64 timestamp = ktime_get_ns();

	vb2_set_plane_payload(&buf->vb.vb2_buf, 0,
			      v4l2_isp_buffer_size(RKISP2_STATS_MAX_SIZE));
	buf->vb.sequence = frame_sequence;
	buf->vb.vb2_buf.timestamp = timestamp;
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	stats->cur_buf = NULL;
}

static void rkisp2_stats_send_measurement_3a(struct rkisp2_stats *stats, u32 status)
{
	struct v4l2_isp_buffer *cur_stat_buf;
	struct rkisp2_buffer *cur_buf;

	cur_buf = rkisp2_stats_get_buf(stats);
	if (!cur_buf)
		return;

	cur_stat_buf = (struct v4l2_isp_buffer *)
			vb2_plane_vaddr(&cur_buf->vb.vb2_buf, 0);

	v4l2_isp_stats_init_buffer(cur_stat_buf, V4L2_ISP_VERSION_V1);

	rkisp2_stats_get_rawae_lite(stats, status, cur_stat_buf);

	rkisp2_stats_get_hist(stats, status, cur_stat_buf);

	rkisp2_stats_get_rawawb(stats, status, cur_stat_buf);

	rkisp2_write(stats->rkisp2, ISP_ISP3A_ICR, stats->icr);

	rkisp2_stats_complete_buf(stats, cur_buf);
	stats->icr = 0;
}

irqreturn_t rkisp2_stats_isr_eof(struct rkisp2_stats *stats)
{
	struct rkisp2_device *rkisp2 = stats->rkisp2;
	u32 status = rkisp2_read(rkisp2, ISP_ISP3A_MIS);

	spin_lock(&stats->lock);
	rkisp2_stats_send_measurement_3a(stats, status);
	spin_unlock(&stats->lock);

	return IRQ_HANDLED;
}

static void rkisp2_init_stats(struct rkisp2_stats *stats)
{
	stats->vdev_fmt.fmt.meta.dataformat =
		V4L2_META_FMT_RKISP2_STATS;
	stats->vdev_fmt.fmt.meta.buffersize =
		v4l2_isp_buffer_size(RKISP2_STATS_MAX_SIZE);
}

int rkisp2_stats_register(struct rkisp2_device *rkisp2)
{
	struct rkisp2_stats *stats = &rkisp2->stats;
	struct rkisp2_vdev_node *node = &stats->vnode;
	struct video_device *vdev = &node->vdev;
	int ret;

	stats->rkisp2 = rkisp2;
	mutex_init(&node->vlock);
	INIT_LIST_HEAD(&stats->stat);
	spin_lock_init(&stats->lock);

	strscpy(vdev->name, RKISP2_STATS_DEV_NAME, sizeof(vdev->name));

	video_set_drvdata(vdev, stats);
	vdev->ioctl_ops = &rkisp2_stats_ioctl;
	vdev->fops = &rkisp2_stats_fops;
	vdev->release = video_device_release_empty;
	vdev->lock = &node->vlock;
	vdev->v4l2_dev = &rkisp2->v4l2_dev;
	vdev->queue = &node->buf_queue;
	vdev->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
	vdev->vfl_dir =  VFL_DIR_RX;
	rkisp2_stats_init_vb2_queue(vdev->queue, stats);
	rkisp2_init_stats(stats);
	video_set_drvdata(vdev, stats);

	node->pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, 1, &node->pad);
	if (ret)
		goto error;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&vdev->dev,
			"failed to register %s, ret=%d\n", vdev->name, ret);
		goto error;
	}

	return 0;

error:
	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&node->vlock);
	stats->rkisp2 = NULL;
	return ret;
}

void rkisp2_stats_unregister(struct rkisp2_device *rkisp2)
{
	struct rkisp2_stats *stats = &rkisp2->stats;
	struct rkisp2_vdev_node *node = &stats->vnode;
	struct video_device *vdev = &node->vdev;

	if (!stats->rkisp2)
		return;

	vb2_video_unregister_device(vdev);
	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&node->vlock);
}
