// SPDX-License-Identifier: GPL-2.0
/*
 * ARM Mali-C55 ISP Driver - Video capture devices
 *
 * Copyright (C) 2023 Ideas on Board Oy
 */

#include <linux/minmax.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>
#include <linux/videodev2.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "mali-c55-common.h"
#include "mali-c55-registers.h"

/*
 * The Mali-C55 ISP has up to two output pipes; known as full resolution and
 * down scaled. The register space for these is laid out identically, but offset
 * by 372 bytes.
 */
#define MALI_C55_CAP_DEV_FR_REG_OFFSET		0x0
#define MALI_C55_CAP_DEV_DS_REG_OFFSET		0x174

static const struct mali_c55_fmt mali_c55_fmts[] = {
	/*
	 * This table is missing some entries which need further work or
	 * investigation:
	 *
	 * Base mode 1 is a backwards V4L2_PIX_FMT_XRGB32 with no V4L2 equivalent
	 * Base mode 5 is "Generic Data"
	 * Base mode 8 is a backwards V4L2_PIX_FMT_XYUV32 - no V4L2 equivalent
	 * Base mode 9 seems to have no V4L2 equivalent
	 * Base mode 17, 19 and 20 describe formats which seem to have no V4L2
	 * equivalent
	 */
	{
		.fourcc = V4L2_PIX_FMT_ARGB2101010,
		.mbus_codes = {
			MEDIA_BUS_FMT_RGB121212_1X36,
			MEDIA_BUS_FMT_RGB202020_1X60,
		},
		.enumerate = true,
		.is_raw = false,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_A2R10G10B10,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_RGB565,
		.mbus_codes = {
			MEDIA_BUS_FMT_RGB121212_1X36,
			MEDIA_BUS_FMT_RGB202020_1X60,
		},
		.enumerate = false,
		.is_raw = false,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_RGB565,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_BGR24,
		.mbus_codes = {
			MEDIA_BUS_FMT_RGB121212_1X36,
			MEDIA_BUS_FMT_RGB202020_1X60,
		},
		.enumerate = false,
		.is_raw = false,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_RGB24,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.mbus_codes = {
			MEDIA_BUS_FMT_YUV10_1X30,
		},
		.enumerate = true,
		.is_raw = false,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_YUY2,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_UYVY,
		.mbus_codes = {
			MEDIA_BUS_FMT_YUV10_1X30,
		},
		.enumerate = false,
		.is_raw = false,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_UYVY,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_Y210,
		.mbus_codes = {
			MEDIA_BUS_FMT_YUV10_1X30,
		},
		.enumerate = false,
		.is_raw = false,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_Y210,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	/*
	 * This is something of a hack, the ISP thinks it's running NV12M but
	 * by setting uv_plane = 0 we simply discard that planes and only output
	 * the Y-plane.
	 */
	{
		.fourcc = V4L2_PIX_FMT_GREY,
		.mbus_codes = {
			MEDIA_BUS_FMT_YUV10_1X30,
		},
		.enumerate = false,
		.is_raw = false,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_NV12_21,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12M,
		.mbus_codes = {
			MEDIA_BUS_FMT_YUV10_1X30,
		},
		.enumerate = false,
		.is_raw = false,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_NV12_21,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT1
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_NV21M,
		.mbus_codes = {
			MEDIA_BUS_FMT_YUV10_1X30,
		},
		.enumerate = false,
		.is_raw = false,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_NV12_21,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT2
		}
	},
	/*
	 * RAW uncompressed formats are all packed in 16 bpp.
	 * TODO: Expand this list to encompass all possible RAW formats.
	 */
	{
		.fourcc = V4L2_PIX_FMT_SRGGB12,
		.mbus_codes = {
			MEDIA_BUS_FMT_SRGGB12_1X12,
		},
		.enumerate = true,
		.is_raw = true,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_RAW16,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_SBGGR12,
		.mbus_codes = {
			MEDIA_BUS_FMT_SBGGR12_1X12,
		},
		.enumerate = true,
		.is_raw = true,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_RAW16,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_SGBRG12,
		.mbus_codes = {
			MEDIA_BUS_FMT_SGBRG12_1X12,
		},
		.enumerate = true,
		.is_raw = true,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_RAW16,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
	{
		.fourcc = V4L2_PIX_FMT_SGRBG12,
		.mbus_codes = {
			MEDIA_BUS_FMT_SGRBG12_1X12,
		},
		.enumerate = true,
		.is_raw = true,
		.registers = {
			.base_mode = MALI_C55_OUTPUT_RAW16,
			.uv_plane = MALI_C55_OUTPUT_PLANE_ALT0
		}
	},
};

static bool mali_c55_mbus_code_can_produce_fmt(const struct mali_c55_fmt *fmt,
					       u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fmt->mbus_codes); i++) {
		if (fmt->mbus_codes[i] == code)
			return true;
	}

	return false;
}

const struct mali_c55_fmt *mali_c55_cap_fmt_next(const struct mali_c55_fmt *fmt,
						 bool allow_raw, bool unique)
{
	if (!fmt)
		fmt = &mali_c55_fmts[0];
	else
		++fmt;

	for (; fmt < &mali_c55_fmts[ARRAY_SIZE(mali_c55_fmts)]; ++fmt) {
		if (!allow_raw && fmt->is_raw) {
			fmt++;
			continue;
		}

		if (unique && !fmt->enumerate) {
			fmt++;
			continue;
		}

		return fmt;
	}

	return NULL;
}

bool mali_c55_format_is_raw(unsigned int mbus_code)
{
	const struct mali_c55_fmt *fmt;

	for_each_mali_cap_fmt(fmt, true) {
		if (fmt->mbus_codes[0] == mbus_code)
			return fmt->is_raw;
	}

	return false;
}

static const struct mali_c55_fmt *mali_c55_format_from_pix(const u32 pixelformat)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mali_c55_fmts); i++) {
		if (mali_c55_fmts[i].fourcc == pixelformat)
			return &mali_c55_fmts[i];
	}

	/*
	 * If we find no matching pixelformat, we'll just default to the first
	 * one for now.
	 */

	return &mali_c55_fmts[0];
}

static const char * const capture_device_names[] = {
	"mali-c55 fr",
	"mali-c55 ds",
	"mali-c55 3a stats",
	"mali-c55 params",
};

static const char *mali_c55_cap_dev_to_name(struct mali_c55_cap_dev *cap)
{
	if (cap->reg_offset == MALI_C55_CAP_DEV_FR_REG_OFFSET)
		return capture_device_names[0];
	else if (cap->reg_offset == MALI_C55_CAP_DEV_DS_REG_OFFSET)
		return capture_device_names[1];
	else
		return "params/stat not supported yet";
}

static int mali_c55_link_validate(struct media_link *link)
{
	struct video_device *vdev =
		media_entity_to_video_device(link->sink->entity);
	struct mali_c55_cap_dev *cap_dev = video_get_drvdata(vdev);
	struct v4l2_subdev *sd =
		media_entity_to_v4l2_subdev(link->source->entity);
	const struct v4l2_pix_format_mplane *pix_mp;
	const struct mali_c55_fmt *cap_fmt;
	struct v4l2_subdev_format sd_fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = link->source->index,
	};
	int ret;

	ret = v4l2_subdev_call(sd, pad, get_fmt, NULL, &sd_fmt);
	if (ret)
		return ret;

	pix_mp = &cap_dev->mode.pix_mp;
	cap_fmt = cap_dev->mode.capture_fmt;

	if (sd_fmt.format.width != pix_mp->width ||
	    sd_fmt.format.height != pix_mp->height) {
		dev_dbg(cap_dev->mali_c55->dev,
			"link '%s':%u -> '%s':%u not valid: %ux%u != %ux%u\n",
			link->source->entity->name, link->source->index,
			link->sink->entity->name, link->sink->index,
			sd_fmt.format.width, sd_fmt.format.height,
			pix_mp->width, pix_mp->height);
		return -EPIPE;
	}

	if (!mali_c55_mbus_code_can_produce_fmt(cap_fmt, sd_fmt.format.code)) {
		dev_dbg(cap_dev->mali_c55->dev,
			"link '%s':%u -> '%s':%u not valid: mbus_code 0x%04x cannot produce pixel format %p4cc\n",
			link->source->entity->name, link->source->index,
			link->sink->entity->name, link->sink->index,
			sd_fmt.format.code, &pix_mp->pixelformat);
		return -EPIPE;
	}

	return 0;
}

static const struct media_entity_operations mali_c55_media_ops = {
	.link_validate = mali_c55_link_validate,
};

static int mali_c55_vb2_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
				    unsigned int *num_planes, unsigned int sizes[],
				    struct device *alloc_devs[])
{
	struct mali_c55_cap_dev *cap_dev = q->drv_priv;
	unsigned int i;

	if (*num_planes) {
		if (*num_planes != cap_dev->mode.pix_mp.num_planes)
			return -EINVAL;

		for (i = 0; i < cap_dev->mode.pix_mp.num_planes; i++)
			if (sizes[i] < cap_dev->mode.pix_mp.plane_fmt[i].sizeimage)
				return -EINVAL;
	} else {
		*num_planes = cap_dev->mode.pix_mp.num_planes;
		for (i = 0; i < cap_dev->mode.pix_mp.num_planes; i++)
			sizes[i] = cap_dev->mode.pix_mp.plane_fmt[i].sizeimage;
	}

	return 0;
}

static void mali_c55_buf_queue(struct vb2_buffer *vb)
{
	struct mali_c55_cap_dev *cap_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c55_buffer *buf = container_of(vbuf,
						   struct mali_c55_buffer, vb);
	unsigned int i;

	buf->plane_done[MALI_C55_PLANE_Y] = false;

	/*
	 * If we're in a single-plane format we flag the other plane as done
	 * already so it's dequeued appropriately later
	 */
	if (cap_dev->mode.pix_mp.num_planes > 1)
		buf->plane_done[MALI_C55_PLANE_UV] = false;
	else
		buf->plane_done[MALI_C55_PLANE_UV] = true;

	for (i = 0; i < cap_dev->mode.pix_mp.num_planes; i++) {
		unsigned long size = cap_dev->mode.pix_mp.plane_fmt[i].sizeimage;

		vb2_set_plane_payload(vb, i, size);
	}

	spin_lock(&cap_dev->buffers.lock);
	list_add_tail(&buf->queue, &cap_dev->buffers.queue);
	spin_unlock(&cap_dev->buffers.lock);
}

static int mali_c55_buf_init(struct vb2_buffer *vb)
{
	struct mali_c55_cap_dev *cap_dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mali_c55_buffer *buf = container_of(vbuf,
						   struct mali_c55_buffer, vb);
	unsigned int i;

	for (i = 0; i < cap_dev->mode.pix_mp.num_planes; i++)
		buf->addrs[i] = vb2_dma_contig_plane_dma_addr(vb, i);

	return 0;
}

void mali_c55_set_next_buffer(struct mali_c55_cap_dev *cap_dev)
{
	struct mali_c55 *mali_c55 = cap_dev->mali_c55;

	spin_lock(&cap_dev->buffers.lock);

	cap_dev->buffers.curr = cap_dev->buffers.next;
	cap_dev->buffers.next = NULL;

	if (!list_empty(&cap_dev->buffers.queue)) {
		struct v4l2_pix_format_mplane *pix_mp;
		const struct v4l2_format_info *info;
		u32 *addrs;

		pix_mp = &cap_dev->mode.pix_mp;
		info = v4l2_format_info(pix_mp->pixelformat);

		mali_c55_update_bits(mali_c55,
				MALI_C55_REG_Y_WRITER_MODE(cap_dev->reg_offset),
				MALI_C55_WRITER_FRAME_WRITE_MASK,
				MALI_C55_WRITER_FRAME_WRITE_MASK);
		if (cap_dev->mode.capture_fmt->registers.uv_plane)
			mali_c55_update_bits(mali_c55,
				MALI_C55_REG_UV_WRITER_MODE(cap_dev->reg_offset),
				MALI_C55_WRITER_FRAME_WRITE_MASK,
				MALI_C55_WRITER_FRAME_WRITE_MASK);

		cap_dev->buffers.next = list_first_entry(&cap_dev->buffers.queue,
							 struct mali_c55_buffer,
							 queue);
		list_del(&cap_dev->buffers.next->queue);

		addrs = cap_dev->buffers.next->addrs;
		mali_c55_write(mali_c55,
			MALI_C55_REG_Y_WRITER_BANKS_BASE(cap_dev->reg_offset),
			addrs[MALI_C55_PLANE_Y]);
		mali_c55_write(mali_c55,
			MALI_C55_REG_UV_WRITER_BANKS_BASE(cap_dev->reg_offset),
			addrs[MALI_C55_PLANE_UV]);
		mali_c55_write(mali_c55,
			MALI_C55_REG_Y_WRITER_OFFSET(cap_dev->reg_offset),
			pix_mp->width * info->bpp[MALI_C55_PLANE_Y]);
		mali_c55_write(mali_c55,
			MALI_C55_REG_UV_WRITER_OFFSET(cap_dev->reg_offset),
			pix_mp->width * info->bpp[MALI_C55_PLANE_UV]
			/ info->hdiv);
	} else {
		/*
		 * If we underflow then we can tell the ISP that we don't want
		 * to write out the next frame.
		 */
		mali_c55_update_bits(mali_c55,
				MALI_C55_REG_Y_WRITER_MODE(cap_dev->reg_offset),
				MALI_C55_WRITER_FRAME_WRITE_MASK, 0x00);
		mali_c55_update_bits(mali_c55,
				MALI_C55_REG_UV_WRITER_MODE(cap_dev->reg_offset),
				MALI_C55_WRITER_FRAME_WRITE_MASK, 0x00);
	}

	spin_unlock(&cap_dev->buffers.lock);
}

static void mali_c55_handle_buffer(struct mali_c55_buffer *curr_buf,
				   unsigned int framecount)
{
	curr_buf->vb.vb2_buf.timestamp = ktime_get_boottime_ns();
	curr_buf->vb.field = V4L2_FIELD_NONE;
	curr_buf->vb.sequence = framecount;
	vb2_buffer_done(&curr_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

/**
 * mali_c55_set_plane_done - mark the plane as written and process the buffer if
 *			     both planes are finished.
 * @cap_dev:  pointer to the fr or ds pipe output
 * @plane:    the plane to mark as completed
 *
 * The Mali C55 ISP has muliplanar outputs for some formats that come with two
 * separate "buffer write completed" interrupts - we need to flag each plane's
 * completion and check whether both planes are done - if so, complete the buf
 * in vb2.
 */
void mali_c55_set_plane_done(struct mali_c55_cap_dev *cap_dev,
			     enum mali_c55_planes plane)
{
	struct mali_c55_buffer *curr_buf;

	spin_lock(&cap_dev->buffers.lock);
	curr_buf = cap_dev->buffers.curr;

	/*
	 * This _should_ never happen. If no buffer was available from vb2 then
	 * we tell the ISP not to bother writing the next frame, which means the
	 * interrupts that call this function should never trigger. If it does
	 * happen then one of our assumptions is horribly wrong - complain
	 * loudly and do nothing.
	 */
	if (!curr_buf) {
		dev_err(cap_dev->mali_c55->dev, "%s null buffer in %s()\n",
			mali_c55_cap_dev_to_name(cap_dev), __func__);
		goto unlock;
	}

	/* If the other plane is also done... */
	if (curr_buf->plane_done[~plane & 1]) {
		mali_c55_handle_buffer(curr_buf, cap_dev->buffers.framecount++);
		cap_dev->buffers.curr = NULL;
	} else {
		curr_buf->plane_done[plane] = true;
	}

unlock:
	spin_unlock(&cap_dev->buffers.lock);
}

static void mali_c55_cap_dev_stream_disable(struct mali_c55_cap_dev *cap_dev)
{
	struct mali_c55 *mali_c55 = cap_dev->mali_c55;

	mali_c55_update_bits(mali_c55,
			     MALI_C55_REG_Y_WRITER_MODE(cap_dev->reg_offset),
			     MALI_C55_WRITER_FRAME_WRITE_MASK, 0x00);
	mali_c55_update_bits(mali_c55,
			     MALI_C55_REG_UV_WRITER_MODE(cap_dev->reg_offset),
			     MALI_C55_WRITER_FRAME_WRITE_MASK, 0x00);
}

static void mali_c55_cap_dev_stream_enable(struct mali_c55_cap_dev *cap_dev)
{
	struct mali_c55 *mali_c55 = cap_dev->mali_c55;

	/*
	 * The Mali ISP can hold up to 5 buffer addresses and simply cycle
	 * through them, but it's not clear to me that the vb2 queue _guarantees_
	 * it will queue buffers to the driver in a fixed order, and ensuring
	 * we call vb2_buffer_done() for the right buffer seems to me to add
	 * pointless complexity given in multi-context mode we'd need to
	 * re-write those registers every frame anyway...so we tell the ISP to
	 * use a single register and update it for each frame.
	 */
	mali_c55_update_bits(mali_c55,
			MALI_C55_REG_Y_WRITER_BANKS_CONFIG(cap_dev->reg_offset),
			MALI_C55_REG_Y_WRITER_MAX_BANKS_MASK, 0);
	mali_c55_update_bits(mali_c55,
			MALI_C55_REG_UV_WRITER_BANKS_CONFIG(cap_dev->reg_offset),
			MALI_C55_REG_UV_WRITER_MAX_BANKS_MASK, 0);
	cap_dev->buffers.framecount = 0;

	/*
	 * We only queue a buffer in the streamon path if this is the first of
	 * the capture devices to start streaming. If the ISP is already running
	 * then we rely on the ISP_START interrupt to queue the first buffer for
	 * this capture device.
	 */
	if (mali_c55->pipe.start_count == 1)
		mali_c55_set_next_buffer(cap_dev);
}

static void mali_c55_cap_dev_return_buffers(struct mali_c55_cap_dev *cap_dev,
					    enum vb2_buffer_state state)
{
	struct mali_c55_buffer *buf, *tmp;

	spin_lock(&cap_dev->buffers.lock);

	if (cap_dev->buffers.curr) {
		vb2_buffer_done(&cap_dev->buffers.curr->vb.vb2_buf,
				state);
		cap_dev->buffers.curr = NULL;
	}

	if (cap_dev->buffers.next) {
		vb2_buffer_done(&cap_dev->buffers.next->vb.vb2_buf,
				state);
		cap_dev->buffers.next = NULL;
	}

	list_for_each_entry_safe(buf, tmp, &cap_dev->buffers.queue, queue) {
		list_del(&buf->queue);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}

	spin_unlock(&cap_dev->buffers.lock);
}

static int mali_c55_vb2_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct mali_c55_cap_dev *cap_dev = q->drv_priv;
	struct mali_c55 *mali_c55 = cap_dev->mali_c55;
	struct mali_c55_resizer *rzr = cap_dev->rzr;
	struct mali_c55_isp *isp = &mali_c55->isp;
	int ret;

	mutex_lock(&isp->lock);

	ret = pm_runtime_resume_and_get(mali_c55->dev);
	if (ret)
		goto err_unlock;

	ret = video_device_pipeline_start(&cap_dev->vdev,
					  &cap_dev->mali_c55->pipe);
	if (ret) {
		dev_err(mali_c55->dev, "%s failed to start media pipeline\n",
			mali_c55_cap_dev_to_name(cap_dev));
		goto err_pm_put;
	}

	mali_c55_cap_dev_stream_enable(cap_dev);
	mali_c55_rzr_start_stream(rzr);

	/*
	 * We only start the ISP if we're the only capture device that's
	 * streaming. Otherwise, it'll already be active.
	 */
	if (mali_c55->pipe.start_count == 1) {
		ret = mali_c55_isp_s_stream(isp, true);
		if (ret)
			goto err_disable_cap_dev;
	}

	mutex_unlock(&isp->lock);

	return 0;

err_disable_cap_dev:
	mali_c55_cap_dev_stream_disable(cap_dev);
	video_device_pipeline_stop(&cap_dev->vdev);
err_pm_put:
	pm_runtime_put(mali_c55->dev);
	mali_c55_cap_dev_return_buffers(cap_dev, VB2_BUF_STATE_QUEUED);
err_unlock:
	mutex_unlock(&isp->lock);

	return ret;
}

static void mali_c55_vb2_stop_streaming(struct vb2_queue *q)
{
	struct mali_c55_cap_dev *cap_dev = q->drv_priv;
	struct mali_c55 *mali_c55 = cap_dev->mali_c55;
	struct mali_c55_isp *isp = &mali_c55->isp;

	mutex_lock(&isp->lock);

	/*
	 * If one of the other capture nodes is streaming, we shouldn't
	 * disable the ISP here.
	 */
	if (mali_c55->pipe.start_count == 1)
		mali_c55_isp_s_stream(&mali_c55->isp, false);

	mali_c55_cap_dev_stream_disable(cap_dev);
	mali_c55_cap_dev_return_buffers(cap_dev, VB2_BUF_STATE_ERROR);
	video_device_pipeline_stop(&cap_dev->vdev);
	pm_runtime_put(mali_c55->dev);

	mutex_unlock(&isp->lock);
}

static const struct vb2_ops mali_c55_vb2_ops = {
	.queue_setup		= &mali_c55_vb2_queue_setup,
	.buf_queue		= &mali_c55_buf_queue,
	.buf_init		= &mali_c55_buf_init,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
	.start_streaming	= &mali_c55_vb2_start_streaming,
	.stop_streaming		= &mali_c55_vb2_stop_streaming,
};

static const struct v4l2_file_operations mali_c55_v4l2_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static void mali_c55_try_fmt(struct v4l2_pix_format_mplane *pix_mp)
{
	const struct mali_c55_fmt *capture_format;
	const struct v4l2_format_info *info;
	struct v4l2_plane_pix_format *plane;
	unsigned int i;

	capture_format = mali_c55_format_from_pix(pix_mp->pixelformat);
	pix_mp->pixelformat = capture_format->fourcc;

	pix_mp->width = clamp_t(u32, pix_mp->width, MALI_C55_MIN_WIDTH,
				MALI_C55_MAX_WIDTH);
	pix_mp->height = clamp_t(u32, pix_mp->height, MALI_C55_MIN_HEIGHT,
				 MALI_C55_MAX_HEIGHT);

	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->colorspace = V4L2_COLORSPACE_DEFAULT;
	pix_mp->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix_mp->quantization = V4L2_QUANTIZATION_DEFAULT;

	info = v4l2_format_info(pix_mp->pixelformat);
	pix_mp->num_planes = info->mem_planes;
	memset(pix_mp->plane_fmt, 0, sizeof(pix_mp->plane_fmt));

	pix_mp->plane_fmt[0].bytesperline = info->bpp[0] * pix_mp->width;
	pix_mp->plane_fmt[0].sizeimage = (info->bpp[0] * pix_mp->width)
				       * pix_mp->height;

	for (i = 1; i < info->comp_planes; i++) {
		plane = &pix_mp->plane_fmt[i];

		plane->bytesperline = DIV_ROUND_UP(info->bpp[i] * pix_mp->width,
						   info->hdiv);
		plane->sizeimage = DIV_ROUND_UP(
					plane->bytesperline * pix_mp->height,
					info->vdiv);
	}

	if (info->mem_planes == 1) {
		for (i = 1; i < info->comp_planes; i++) {
			plane = &pix_mp->plane_fmt[i];
			pix_mp->plane_fmt[0].sizeimage += plane->sizeimage;
		}
	}
}

static int mali_c55_try_fmt_vid_cap_mplane(struct file *file, void *fh,
					   struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	mali_c55_try_fmt(&f->fmt.pix_mp);

	return 0;
}

static void mali_c55_set_format(struct mali_c55_cap_dev *cap_dev,
				struct v4l2_pix_format_mplane *pix_mp)
{
	const struct mali_c55_fmt *capture_format;
	struct mali_c55 *mali_c55 = cap_dev->mali_c55;
	const struct v4l2_format_info *info;

	mali_c55_try_fmt(pix_mp);
	capture_format = mali_c55_format_from_pix(pix_mp->pixelformat);
	info = v4l2_format_info(pix_mp->pixelformat);

	mali_c55_write(mali_c55,
		       MALI_C55_REG_Y_WRITER_MODE(cap_dev->reg_offset),
		       capture_format->registers.base_mode);
	mali_c55_write(mali_c55,
		       MALI_C55_REG_ACTIVE_OUT_Y_SIZE(cap_dev->reg_offset),
		       MALI_C55_REG_ACTIVE_OUT_SIZE_W(pix_mp->width) |
		       MALI_C55_REG_ACTIVE_OUT_SIZE_H(pix_mp->height));

	if (info->mem_planes > 1) {
		mali_c55_write(mali_c55,
			       MALI_C55_REG_UV_WRITER_MODE(cap_dev->reg_offset),
			       capture_format->registers.base_mode);
		mali_c55_update_bits(mali_c55,
				MALI_C55_REG_UV_WRITER_MODE(cap_dev->reg_offset),
				MALI_C55_WRITER_SUBMODE_MASK,
				capture_format->registers.uv_plane << 6);

		mali_c55_write(mali_c55,
			MALI_C55_REG_ACTIVE_OUT_UV_SIZE(cap_dev->reg_offset),
			MALI_C55_REG_ACTIVE_OUT_SIZE_W(pix_mp->width) |
			MALI_C55_REG_ACTIVE_OUT_SIZE_H(pix_mp->height));

		mali_c55_write(mali_c55,
			       MALI_C55_REG_CS_CONV_CONFIG(cap_dev->reg_offset),
			       MALI_C55_CS_CONV_MATRIX_MASK);

		/*
		 * TODO: Figure out the colour matrix coefficients and calculate
		 * and write them here.
		 */

		if (info->hdiv > 1)
			mali_c55_update_bits(mali_c55,
				MALI_C55_REG_CS_CONV_CONFIG(cap_dev->reg_offset),
				MALI_C55_CS_CONV_HORZ_DOWNSAMPLE_MASK,
				MALI_C55_CS_CONV_HORZ_DOWNSAMPLE_MASK);
		if (info->vdiv > 1)
			mali_c55_update_bits(mali_c55,
				MALI_C55_REG_CS_CONV_CONFIG(cap_dev->reg_offset),
				MALI_C55_CS_CONV_VERT_DOWNSAMPLE_MASK,
				MALI_C55_CS_CONV_VERT_DOWNSAMPLE_MASK);
		if (info->hdiv > 1 || info->vdiv > 1)
			mali_c55_update_bits(mali_c55,
				MALI_C55_REG_CS_CONV_CONFIG(cap_dev->reg_offset),
				MALI_C55_CS_CONV_FILTER_MASK,
				MALI_C55_CS_CONV_FILTER_MASK);
	}

	cap_dev->mode.pix_mp = *pix_mp;
	cap_dev->mode.capture_fmt = capture_format;
}

static int mali_c55_s_fmt_vid_cap_mplane(struct file *file, void *fh,
					 struct v4l2_format *f)
{
	struct mali_c55_cap_dev *cap_dev = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	if (vb2_is_busy(&cap_dev->queue))
		return -EBUSY;

	mali_c55_set_format(cap_dev, &f->fmt.pix_mp);

	return 0;
}

static int mali_c55_g_fmt_vid_cap_mplane(struct file *file, void *fh,
					 struct v4l2_format *f)
{
	struct mali_c55_cap_dev *cap_dev = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	f->fmt.pix_mp = cap_dev->mode.pix_mp;

	return 0;
}

static int mali_c55_enum_fmt_vid_cap_mplane(struct file *file, void *fh,
					    struct v4l2_fmtdesc *f)
{
	struct mali_c55_cap_dev *cap_dev = video_drvdata(file);
	unsigned int j = 0;
	unsigned int i;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(mali_c55_fmts); i++) {
		if (f->mbus_code &&
		    !mali_c55_mbus_code_can_produce_fmt(&mali_c55_fmts[i],
						       f->mbus_code))
			continue;

		/* Downscale pipe can't output RAW formats */
		if (mali_c55_fmts[i].is_raw &&
		    cap_dev->reg_offset == MALI_C55_CAP_DEV_DS_REG_OFFSET)
			continue;

		if (j++ == f->index) {
			f->pixelformat = mali_c55_fmts[i].fourcc;
			return 0;
		}
	}

	return -EINVAL;
}

static int mali_c55_querycap(struct file *file, void *fh,
			     struct v4l2_capability *cap)
{
	strscpy(cap->driver, MALI_C55_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "ARM Mali-C55 ISP", sizeof(cap->card));

	return 0;
}

static const struct v4l2_ioctl_ops mali_c55_v4l2_ioctl_ops = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_try_fmt_vid_cap_mplane = mali_c55_try_fmt_vid_cap_mplane,
	.vidioc_s_fmt_vid_cap_mplane = mali_c55_s_fmt_vid_cap_mplane,
	.vidioc_g_fmt_vid_cap_mplane = mali_c55_g_fmt_vid_cap_mplane,
	.vidioc_enum_fmt_vid_cap = mali_c55_enum_fmt_vid_cap_mplane,
	.vidioc_querycap = mali_c55_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

int mali_c55_register_capture_devs(struct mali_c55 *mali_c55)
{
	struct v4l2_pix_format_mplane pix_mp;
	struct mali_c55_cap_dev *cap_dev;
	struct video_device *vdev;
	struct vb2_queue *vb2q;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(mali_c55->cap_devs); i++) {
		cap_dev = &mali_c55->cap_devs[i];
		vdev = &cap_dev->vdev;
		vb2q = &cap_dev->queue;

		/*
		 * The downscale output pipe is an optional block within the ISP
		 * so we need to check whether it's actually been fitted or not.
		 */

		if (i == MALI_C55_CAP_DEV_DS &&
		    !(mali_c55->capabilities & MALI_C55_GPS_DS_PIPE_FITTED))
			continue;

		cap_dev->mali_c55 = mali_c55;
		mutex_init(&cap_dev->lock);
		INIT_LIST_HEAD(&cap_dev->buffers.queue);

		switch (i) {
		case MALI_C55_CAP_DEV_FR:
			cap_dev->rzr = &mali_c55->resizers[MALI_C55_RZR_FR];
			cap_dev->reg_offset = MALI_C55_CAP_DEV_FR_REG_OFFSET;
			break;
		case MALI_C55_CAP_DEV_DS:
			cap_dev->rzr = &mali_c55->resizers[MALI_C55_RZR_DS];
			cap_dev->reg_offset = MALI_C55_CAP_DEV_DS_REG_OFFSET;
			break;
		default:
			mutex_destroy(&cap_dev->lock);
			ret = -EINVAL;
			goto err_destroy_mutex;
		}

		cap_dev->pad.flags = MEDIA_PAD_FL_SINK;
		ret = media_entity_pads_init(&cap_dev->vdev.entity, 1, &cap_dev->pad);
		if (ret) {
			mutex_destroy(&cap_dev->lock);
			goto err_destroy_mutex;
		}

		vb2q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		vb2q->io_modes = VB2_MMAP | VB2_DMABUF;
		vb2q->drv_priv = cap_dev;
		vb2q->mem_ops = &vb2_dma_contig_memops;
		vb2q->ops = &mali_c55_vb2_ops;
		vb2q->buf_struct_size = sizeof(struct mali_c55_buffer);
		vb2q->min_queued_buffers = 1;
		vb2q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
		vb2q->lock = &cap_dev->lock;
		vb2q->dev = mali_c55->dev;

		ret = vb2_queue_init(vb2q);
		if (ret) {
			dev_err(mali_c55->dev, "%s vb2 queue init failed\n",
				mali_c55_cap_dev_to_name(cap_dev));
			goto err_cleanup_media_entity;
		}

		strscpy(cap_dev->vdev.name, capture_device_names[i],
			sizeof(cap_dev->vdev.name));
		vdev->release = video_device_release_empty;
		vdev->fops = &mali_c55_v4l2_fops;
		vdev->ioctl_ops = &mali_c55_v4l2_ioctl_ops;
		vdev->lock = &cap_dev->lock;
		vdev->v4l2_dev = &mali_c55->v4l2_dev;
		vdev->queue = &cap_dev->queue;
		vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE |
				    V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
		vdev->entity.ops = &mali_c55_media_ops;
		video_set_drvdata(vdev, cap_dev);

		memset(&pix_mp, 0, sizeof(pix_mp));
		pix_mp.pixelformat = V4L2_PIX_FMT_RGB565;
		pix_mp.width = MALI_C55_DEFAULT_WIDTH;
		pix_mp.height = MALI_C55_DEFAULT_HEIGHT;
		mali_c55_set_format(cap_dev, &pix_mp);

		ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
		if (ret) {
			dev_err(mali_c55->dev,
				"%s failed to register video device\n",
				mali_c55_cap_dev_to_name(cap_dev));
			goto err_release_vb2q;
		}
	}

	return 0;

err_release_vb2q:
	vb2_queue_release(vb2q);
err_cleanup_media_entity:
	media_entity_cleanup(&cap_dev->vdev.entity);
err_destroy_mutex:
	mutex_destroy(&cap_dev->lock);
	mali_c55_unregister_capture_devs(mali_c55);

	return ret;
}

void mali_c55_unregister_capture_devs(struct mali_c55 *mali_c55)
{
	struct mali_c55_cap_dev *cap_dev;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mali_c55->cap_devs); i++) {
		cap_dev = &mali_c55->cap_devs[i];

		if (!video_is_registered(&cap_dev->vdev))
			continue;

		vb2_video_unregister_device(&cap_dev->vdev);
		media_entity_cleanup(&cap_dev->vdev.entity);
		mutex_destroy(&cap_dev->lock);
	}
}
