// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-common.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-vmalloc.h>

#include "viif.h"
#include "viif_csi2rx.h"
#include "viif_isp.h"
#include "viif_common.h"
#include "viif_regs.h"
#include "viif_stats.h"

struct viif_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head queue;
};

static void read_isp_capture_regs(struct viif_l1_info *l1_info, struct viif_device *viif_dev)
{
	unsigned long irqflags;
	int i, j;
	u32 val;

	spin_lock_irqsave(&viif_dev->regbuf_lock, irqflags);
	hwd_viif_isp_guard_start(viif_dev);

	/* change register buffer to regbuf0 where driver gets information */
	viif_capture_write(viif_dev, REG_L1_CRGBF_ACC_CONF, VAL_L1_CRGBF_ACC_CONF_MODE_BUFFER0);

	/* get AWB info */
	l1_info->awb_ave_u = viif_capture_read(viif_dev, REG_L1_AWHB_AVE_USIG);
	l1_info->awb_ave_v = viif_capture_read(viif_dev, REG_L1_AWHB_AVE_VSIG);
	l1_info->awb_accumulated_pixel = viif_capture_read(viif_dev, REG_L1_AWHB_NUM_UVON);
	l1_info->awb_gain_r = viif_capture_read(viif_dev, REG_L1_AWHB_AWBGAINR);
	l1_info->awb_gain_g = viif_capture_read(viif_dev, REG_L1_AWHB_AWBGAING);
	l1_info->awb_gain_b = viif_capture_read(viif_dev, REG_L1_AWHB_AWBGAINB);
	val = viif_capture_read(viif_dev, REG_L1_AWHB_R_CTR_STOP);
	l1_info->awb_status_u = (FIELD_GET(BIT(1), val) != 0);
	l1_info->awb_status_v = (FIELD_GET(BIT(0), val) != 0);

	/* get average luminance info */
	l1_info->avg_lum_weight = viif_capture_read(viif_dev, REG_L1_AEXP_RESULT_AVE);
	val = viif_capture_read(viif_dev, REG_L1_AEXP_SATUR_BLACK_PIXNUM);
	l1_info->avg_satur_pixnum = FIELD_GET(GENMASK(31, 16), val);
	l1_info->avg_black_pixnum = FIELD_GET(GENMASK(15, 0), val);
	for (i = 0; i < 8; i++) {
		for (j = 0; j < 8; j++) {
			l1_info->avg_lum_block[i][j] =
				viif_capture_read(viif_dev, REG_L1_AEXP_AVE(i, j));
		}
	}
	l1_info->avg_lum_four_line_lum[0] = viif_capture_read(viif_dev, REG_L1_AEXP_AVE4LINES0);
	l1_info->avg_lum_four_line_lum[1] = viif_capture_read(viif_dev, REG_L1_AEXP_AVE4LINES1);
	l1_info->avg_lum_four_line_lum[2] = viif_capture_read(viif_dev, REG_L1_AEXP_AVE4LINES2);
	l1_info->avg_lum_four_line_lum[3] = viif_capture_read(viif_dev, REG_L1_AEXP_AVE4LINES3);

	/* revert to register access from register buffer access */
	viif_capture_write(viif_dev, REG_L1_CRGBF_ACC_CONF, VAL_L1_CRGBF_ACC_CONF_MODE_BYPASS);

	hwd_viif_isp_guard_end(viif_dev);
	spin_unlock_irqrestore(&viif_dev->regbuf_lock, irqflags);
}

static const struct viif_csi2rx_dphy_calibration_status calib_status_not_streaming = {
	.term_cal_with_rext = -EAGAIN,
	.clock_lane_offset_cal = -EAGAIN,
	.data_lane0_offset_cal = -EAGAIN,
	.data_lane1_offset_cal = -EAGAIN,
	.data_lane2_offset_cal = -EAGAIN,
	.data_lane3_offset_cal = -EAGAIN,
	.data_lane0_ddl_tuning_cal = -EAGAIN,
	.data_lane1_ddl_tuning_cal = -EAGAIN,
	.data_lane2_ddl_tuning_cal = -EAGAIN,
	.data_lane3_ddl_tuning_cal = -EAGAIN,
};

static const struct viif_csi2rx_err_status csi_err_not_streaming;

void visconti_viif_stats_isr(struct viif_device *viif_dev, unsigned int sequence, u64 timestamp)
{
	struct visconti_viif_isp_stat *cur_stat_buf;
	struct stats_dev *stats_dev = &viif_dev->stats_dev;
	struct viif_buffer *cur_buf;

	spin_lock(&stats_dev->stats_lock);

	if (list_empty(&stats_dev->stats_queue))
		goto done;

	cur_buf = list_first_entry(&stats_dev->stats_queue, struct viif_buffer, queue);
	list_del(&cur_buf->queue);
	cur_stat_buf = (struct visconti_viif_isp_stat *)vb2_plane_vaddr(&cur_buf->vb.vb2_buf, 0);

	if (!vb2_start_streaming_called(&viif_dev->cap_dev0.vb2_vq)) {
		cur_stat_buf->csi2rx_dphy_calibration = calib_status_not_streaming;
	} else {
		visconti_viif_csi2rx_get_calibration_status(viif_dev,
							    &cur_stat_buf->csi2rx_dphy_calibration);
	}

	if (!vb2_is_streaming(&viif_dev->cap_dev0.vb2_vq))
		cur_stat_buf->csi2rx_err = csi_err_not_streaming;
	else
		visconti_viif_csi2rx_get_err_status(viif_dev, &cur_stat_buf->csi2rx_err);

	read_isp_capture_regs(&cur_stat_buf->isp_capture.l1_info, viif_dev);

	cur_stat_buf->errors.main = viif_dev->reported_err_main;
	cur_stat_buf->errors.sub = viif_dev->reported_err_sub;
	cur_stat_buf->errors.csi2rx = viif_dev->reported_err_csi2rx;
	viif_dev->reported_err_main = 0;
	viif_dev->reported_err_sub = 0;
	viif_dev->reported_err_csi2rx = 0;

	vb2_set_plane_payload(&cur_buf->vb.vb2_buf, 0, sizeof(struct visconti_viif_isp_stat));

	cur_buf->vb.sequence = sequence;
	cur_buf->vb.vb2_buf.timestamp = timestamp;
	vb2_buffer_done(&cur_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

done:
	spin_unlock(&stats_dev->stats_lock);
}

static int viif_stats_enum_fmt_meta_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct video_device *vdev = video_devdata(file);

	if (f->index > 0 || f->type != vdev->queue->type)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_VISCONTI_VIIF_STATS;

	return 0;
}

static int viif_stats_g_fmt_meta_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct video_device *vdev = video_devdata(file);
	struct v4l2_meta_format *meta = &f->fmt.meta;

	if (f->type != vdev->queue->type)
		return -EINVAL;

	memset(meta, 0, sizeof(*meta));
	meta->dataformat = V4L2_META_FMT_VISCONTI_VIIF_STATS;
	meta->buffersize = sizeof(struct visconti_viif_isp_stat);

	return 0;
}

static int viif_stats_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, VIIF_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));
	strscpy(cap->bus_info, VIIF_BUS_INFO_BASE "-0", sizeof(cap->bus_info));

	return 0;
}

static const struct v4l2_ioctl_ops viif_stats_ioctl = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_fmt_meta_cap = viif_stats_enum_fmt_meta_cap,
	.vidioc_g_fmt_meta_cap = viif_stats_g_fmt_meta_cap,
	.vidioc_s_fmt_meta_cap = viif_stats_g_fmt_meta_cap,
	.vidioc_try_fmt_meta_cap = viif_stats_g_fmt_meta_cap,
	.vidioc_querycap = viif_stats_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations viif_stats_fops = { .mmap = vb2_fop_mmap,
							     .unlocked_ioctl = video_ioctl2,
							     .poll = vb2_fop_poll,
							     .open = v4l2_fh_open,
							     .release = vb2_fop_release };

static int viif_stats_vb2_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
				      unsigned int *num_planes, unsigned int sizes[],
				      struct device *alloc_devs[])
{
	*num_planes = 1;
	*num_buffers = clamp_t(u32, *num_buffers, 2, 8);
	sizes[0] = sizeof(struct visconti_viif_isp_stat);

	return 0;
}

static inline struct viif_buffer *vb2_to_viif(struct vb2_v4l2_buffer *vbuf)
{
	return container_of(vbuf, struct viif_buffer, vb);
}

static inline struct stats_dev *vb2queue_to_statsdev(struct vb2_queue *q)
{
	return (struct stats_dev *)vb2_get_drv_priv(q);
}

static void viif_stats_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct stats_dev *stats_dev = vb2queue_to_statsdev(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct viif_buffer *buf = vb2_to_viif(vbuf);

	spin_lock_irq(&stats_dev->stats_lock);
	list_add_tail(&buf->queue, &stats_dev->stats_queue);
	spin_unlock_irq(&stats_dev->stats_lock);
}

static int viif_stats_vb2_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < sizeof(struct visconti_viif_isp_stat))
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, sizeof(struct visconti_viif_isp_stat));

	return 0;
}

static void viif_stats_vb2_stop_streaming(struct vb2_queue *q)
{
	struct stats_dev *stats_dev = vb2queue_to_statsdev(q);
	struct viif_buffer *buf;
	unsigned int i;

	spin_lock_irq(&stats_dev->stats_lock);
	for (i = 0; i < 8; i++) {
		if (list_empty(&stats_dev->stats_queue))
			break;
		buf = list_first_entry(&stats_dev->stats_queue, struct viif_buffer, queue);
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irq(&stats_dev->stats_lock);
}

static const struct vb2_ops viif_stats_vb2_ops = {
	.queue_setup = viif_stats_vb2_queue_setup,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.buf_queue = viif_stats_vb2_buf_queue,
	.buf_prepare = viif_stats_vb2_buf_prepare,
	.stop_streaming = viif_stats_vb2_stop_streaming,
};

int visconti_viif_stats_register(struct viif_device *viif_dev)
{
	struct stats_dev *stats_dev = &viif_dev->stats_dev;
	struct video_device *vdev = &stats_dev->vdev;
	struct vb2_queue *q = &stats_dev->vb2_vq;
	int ret;

	mutex_init(&stats_dev->vlock);
	INIT_LIST_HEAD(&stats_dev->stats_queue);
	spin_lock_init(&stats_dev->stats_lock);

	strscpy(vdev->name, "viif_stats", sizeof(vdev->name));

	/* Register the video device */
	video_set_drvdata(vdev, stats_dev);
	vdev->ioctl_ops = &viif_stats_ioctl;
	vdev->fops = &viif_stats_fops;
	vdev->release = video_device_release_empty;
	vdev->lock = &stats_dev->vlock;
	vdev->v4l2_dev = &viif_dev->v4l2_dev;
	vdev->queue = &stats_dev->vb2_vq;
	vdev->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
	vdev->vfl_dir = VFL_DIR_RX;

	/* Initialize vb2 queue */
	q->type = V4L2_BUF_TYPE_META_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	q->drv_priv = stats_dev;
	q->ops = &viif_stats_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->buf_struct_size = sizeof(struct viif_buffer);
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &stats_dev->vlock;
	q->dev = viif_dev->v4l2_dev.dev;

	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	stats_dev->stats_pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, VIIF_STATS_PAD_NUM, &stats_dev->stats_pad);
	if (ret)
		goto error;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(viif_dev->v4l2_dev.dev, "video_register_device failed: %d\n", ret);
		goto error;
	}

	return 0;

error:
	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&stats_dev->vlock);

	return ret;
}

void visconti_viif_stats_unregister(struct viif_device *viif_dev)
{
	struct stats_dev *stats_dev = &viif_dev->stats_dev;
	struct video_device *vdev = &stats_dev->vdev;

	if (!video_is_registered(vdev))
		return;

	vb2_video_unregister_device(vdev);
	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&stats_dev->vlock);
}
