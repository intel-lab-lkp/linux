// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 Allegro DVT.
 * Author: Yassine OUAISSA <yassine.ouaissa@allegrodvt.fr>
 *
 * Allegro DVT stateful video decoder driver for the IP Gen 3
 */

#include <asm-generic/errno-base.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>
#include <linux/v4l2-controls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "al_vdec_drv.h"

/* default decoder params */
#define DECODER_WIDTH_DEFAULT 640
#define DECODER_HEIGHT_DEFAULT 480
#define DECODER_WIDTH_MAX 3840
#define DECODER_HEIGHT_MAX 2160
#define DECODER_WIDTH_MIN 16
#define DECODER_HEIGHT_MIN 16
#define DEC_REQ_TIMEOUT msecs_to_jiffies(1000)
#define DEC_RES_EVT_TIMEOUT DEC_REQ_TIMEOUT

/* Supported formats */
static const struct al_fmt al_src_formats[] = {
	{
		.pixelformat = V4L2_PIX_FMT_H264,
		.bpp = 20,
	},
	{
		.pixelformat = V4L2_PIX_FMT_HEVC,
		.bpp = 20,
	},
	{
		.pixelformat = V4L2_PIX_FMT_JPEG,
		.bpp = 8,
	}
};

static const struct al_fmt al_dst_formats[] = {
	{
		.pixelformat = V4L2_PIX_FMT_NV12,
		.bpp = 12,
	},
	{
		.pixelformat = V4L2_PIX_FMT_P010,
		.bpp = 12,
	},
	{
		.pixelformat = V4L2_PIX_FMT_NV16,
		.bpp = 16,
	},
	{
		.pixelformat = V4L2_PIX_FMT_YUV420, /* YUV 4:2:0 */
		.bpp = 12,
	},
	{
		.pixelformat = V4L2_PIX_FMT_YVU420, /* YVU 4:2:0 */
		.bpp = 12,
	},
};

/* Default format */
static const struct al_frame al_default_fmt = {

	.width = DECODER_WIDTH_DEFAULT,
	.height = DECODER_HEIGHT_DEFAULT,
	.bytesperline = DECODER_WIDTH_MAX * 4,
	.sizeimage = DECODER_WIDTH_DEFAULT * DECODER_HEIGHT_DEFAULT * 4,
	.nbuffers = 1,
	.fmt = &al_dst_formats[0],
	.field = V4L2_FIELD_NONE,
	.colorspace = V4L2_COLORSPACE_REC709,
	.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
	.quantization = V4L2_QUANTIZATION_DEFAULT,
	.xfer_func = V4L2_XFER_FUNC_DEFAULT
};

static struct al_frame *al_get_frame(struct al_dec_ctx *ctx,
				     enum v4l2_buf_type type)
{
	if (WARN_ON(!ctx))
		return ERR_PTR(-EINVAL);

	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return &ctx->src;
	else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return &ctx->dst;

	al_v4l2_err(ctx->codec, "Unsupported type (%d)", type);

	return ERR_PTR(-EINVAL);
}

static const struct al_fmt *al_find_fmt(u32 pixelformat)
{
	const struct al_fmt *fmt;
	unsigned int i;

	/* check if the pixelformat exist in the src formats list */
	for (i = 0; i < ARRAY_SIZE(al_src_formats); i++) {
		fmt = &al_src_formats[i];
		if (fmt->pixelformat == pixelformat)
			return fmt;
	}

	/* check if the pixelformat exist in the dst formats list */
	for (i = 0; i < ARRAY_SIZE(al_dst_formats); i++) {
		fmt = &al_dst_formats[i];
		if (fmt->pixelformat == pixelformat)
			return fmt;
	}

	return NULL;
}

static int dec_fw_create_decoder(struct al_dec_ctx *ctx)
{
	struct msg_itf_create_decoder_req_full req;
	struct msg_itf_create_codec_reply reply;
	const struct al_frame *frame = &ctx->src;
	struct al_common_mcu_req mreq = {
		.pCtx = al_virt_to_phys(ctx),
		.req_type = MSG_ITF_TYPE_CREATE_INST_REQ,
		.req_size = sizeof(req.req),
		.reply_size = sizeof(reply),
		.reply = &reply,
	};
	int ret;

	if (ctx->hDec) {
		al_v4l2_dbg(ctx->codec, 3, "fw decoder already exist\n");
		return 0;
	}

	req.req.codec = frame->fmt->pixelformat;

	ret = al_common_send_req_reply(ctx->codec, &ctx->cmd_q_list, &req.hdr,
				       &mreq);

	if (!ret && !reply.ret)
		ctx->hDec = reply.hCodec;
	else if (reply.ret)
		ret = -ENODEV;

	return ret;
}

static void dec_fw_destroy_decoder(struct al_dec_ctx *ctx)
{
	struct msg_itf_destroy_codec_req_full req;
	struct msg_itf_destroy_codec_reply reply;
	struct al_common_mcu_req mreq = {
		.req_type = MSG_ITF_TYPE_DESTROY_INST_REQ,
		.pCtx = al_virt_to_phys(ctx),
		.req_size = sizeof(req.req),
		.reply_size = sizeof(reply),
		.reply = &reply,
	};
	int ret;

	if (WARN(!ctx->hDec, "NULL Decoder to destroy !"))
		return;

	al_v4l2_dbg(ctx->codec, 3, "Destroy decoder %lld ", ctx->hDec);

	req.req.hCodec = ctx->hDec;

	ret = al_common_send_req_reply(ctx->codec, &ctx->cmd_q_list, &req.hdr,
				       &mreq);

	if (!ret)
		ctx->hDec = 0;
}

static int al_dec_fw_push_frame_buf(struct al_dec_ctx *ctx,
				    struct vb2_v4l2_buffer *vbuf)
{
	struct msg_itf_push_dst_buf_req_full req;
	struct v4l2_m2m_buffer *m2m_buf;
	struct al_common_mcu_req mreq = {
		.req_type = MSG_ITF_TYPE_PUT_DISPLAY_PICTURE_REQ,
		.pCtx = al_virt_to_phys(ctx),
		.req_size = sizeof(req.req),
	};
	int ret;

	if (WARN(!vbuf, "NULL frame Buffer to push!!"))
		return -EINVAL;

	m2m_buf = container_of(vbuf, struct v4l2_m2m_buffer, vb);
	req.req.hCodec = ctx->hDec;
	req.req.bufferHandle = al_virt_to_phys(m2m_buf);
	req.req.phyAddr = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 0);
	req.req.size = vb2_plane_size(&vbuf->vb2_buf, 0);

	ret = al_common_send_req_reply(ctx->codec, &ctx->cmd_q_list, &req.hdr,
				       &mreq);
	if (ret)
		al_v4l2_err(ctx->codec, "Failed to push frame buffer %p %d",
			    m2m_buf, ret);

	return ret;
}

static int al_dec_fw_push_bitstream_buf(struct al_dec_ctx *ctx,
					struct vb2_v4l2_buffer *vbuf)
{
	struct msg_itf_push_src_buf_req_full req;
	struct v4l2_m2m_buffer *m2m_buf;
	struct al_common_mcu_req mreq = {
		.req_type = MSG_ITF_TYPE_PUSH_BITSTREAM_BUFFER_REQ,
		.pCtx = al_virt_to_phys(ctx),
		.req_size = sizeof(req.req),
	};
	int ret;

	if (WARN(!vbuf, "NULL Buffer to push!!"))
		return -EINVAL;

	m2m_buf = container_of(vbuf, struct v4l2_m2m_buffer, vb);
	req.req.hCodec = ctx->hDec;
	req.req.bufferHandle = al_virt_to_phys(m2m_buf);
	req.req.phyAddr = vb2_dma_contig_plane_dma_addr(&vbuf->vb2_buf, 0);
	req.req.size = vb2_plane_size(&vbuf->vb2_buf, 0);

	/* Fill the v4l2 metadata*/
	req.req.meta.timestamp = vbuf->vb2_buf.timestamp;
	req.req.meta.timecode = vbuf->timecode;
	req.req.meta.last = vbuf->flags & V4L2_BUF_FLAG_LAST;

	ret = al_common_send_req_reply(ctx->codec, &ctx->cmd_q_list, &req.hdr,
				       &mreq);
	if (ret)
		al_v4l2_err(ctx->codec, "Failed to push bitstream buffer %p %d",
			    m2m_buf, ret);

	return ret;
}

static int dec_fw_flush_req(struct al_dec_ctx *ctx)
{
	struct msg_itf_flush_req_full req;
	struct msg_itf_flush_reply reply;
	struct al_common_mcu_req mreq = {
		.req_type = MSG_ITF_TYPE_FLUSH_REQ,
		.pCtx = al_virt_to_phys(ctx),
		.req_size = sizeof(req.req),
		.reply_size = sizeof(reply),
		.reply = &reply,
	};
	int ret;

	req.req.hCodec = ctx->hDec;

	ret = al_common_send_req_reply(ctx->codec, &ctx->cmd_q_list, &req.hdr,
				       &mreq);

	if (ret)
		al_v4l2_err(ctx->codec, "Failed to flush the decoder %d", ret);

	return ret;
}

static struct vb2_v4l2_buffer *al_dec_dequeue_buf(struct al_dec_ctx *ctx,
						  uint64_t hdl,
						  struct list_head *buffer_list)
{
	struct v4l2_m2m_buffer *buf, *tmp;
	struct vb2_v4l2_buffer *ret = NULL;

	guard(mutex)(&ctx->buf_q_mlock);
	list_for_each_entry_safe(buf, tmp, buffer_list, list) {
		if (buf == al_phys_to_virt(hdl)) {
			list_del(&buf->list);
			ret = &buf->vb;
			break;
		}
	}

	return ret;
}

static struct vb2_v4l2_buffer *al_dec_dequeue_src_buf(struct al_dec_ctx *ctx,
						      uint64_t hdl)
{
	return al_dec_dequeue_buf(ctx, hdl, &ctx->stream_q_list);
}

static struct vb2_v4l2_buffer *al_dec_dequeue_dst_buf(struct al_dec_ctx *ctx,
						      uint64_t hdl)
{
	return al_dec_dequeue_buf(ctx, hdl, &ctx->frame_q_list);
}

static void al_ctx_cleanup(struct kref *ref)
{
	struct al_dec_ctx *ctx = container_of(ref, struct al_dec_ctx, refcount);

	kfree(ctx);
}

static struct al_dec_ctx *al_ctx_get(struct al_codec_dev *codec, uint64_t hdl)
{
	struct al_dec_ctx *ctx;
	struct al_dec_ctx *ret = NULL;

	guard(mutex)(&codec->ctx_mlock);
	list_for_each_entry(ctx, &codec->ctx_q_list, list) {
		if (ctx == al_phys_to_virt(hdl)) {
			kref_get(&ctx->refcount);
			ret = ctx;
			break;
		}
	}

	return ret;
}

static void al_ctx_put(struct al_dec_ctx *ctx)
{
	kref_put(&ctx->refcount, al_ctx_cleanup);
}

static int al_dec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct al_dec_ctx *ctx = vb2_get_drv_priv(q);
	struct al_codec_dev *codec = ctx->codec;

	v4l2_m2m_update_start_streaming_state(ctx->fh.m2m_ctx, q);

	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		struct v4l2_m2m_buffer *buf;
		int ret;

		if (list_empty(&ctx->stream_q_list)) {
			al_v4l2_err(codec, "Empty stream list.");
			return -EINVAL;
		}

		if (!al_common_mcu_is_alive(codec)) {
			al_v4l2_err(codec, "Unable to ping the mcu");
			return -ENODEV;
		}

		ret = dec_fw_create_decoder(ctx);
		if (ret) {
			al_v4l2_err(codec, "Unable to create the fw decoder %d",
				    ret);
			return ret;
		}

		/* Get the first vid-out queued buffer */
		buf = list_first_entry(&ctx->stream_q_list,
				       struct v4l2_m2m_buffer, list);

		if (al_dec_fw_push_bitstream_buf(ctx, &buf->vb)) {
			al_v4l2_err(codec,
				    "Unable to push the bitstream buffer");
			return -EINVAL;
		}

		/* Wait until the mcu detect the resolution of the stream */
		ret = wait_for_completion_timeout(&ctx->res_done,
						  DEC_RES_EVT_TIMEOUT);
		if (!ret) {
			al_v4l2_err(codec, "unsupported stream");
			ctx->aborting = true;
		}
	}

	return 0;
}

static void al_dec_stop_streaming_cap(struct al_dec_ctx *ctx)
{
	struct vb2_v4l2_buffer *vbuf;
	struct v4l2_m2m_buffer *entry, *tmp;

	mutex_lock(&ctx->buf_q_mlock);
	if (!list_empty(&ctx->frame_q_list))
		list_for_each_entry_safe(entry, tmp, &ctx->frame_q_list, list) {
			list_del(&entry->list);
			vbuf = &entry->vb;
			vb2_set_plane_payload(&vbuf->vb2_buf, 0, 0);
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
		}
	mutex_unlock(&ctx->buf_q_mlock);

	while (v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx)) {
		vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (vbuf) {
			vb2_set_plane_payload(&vbuf->vb2_buf, 0, 0);
			v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
		}
	}

	v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
}

static void al_dec_stop_streaming_out(struct al_dec_ctx *ctx)
{
	struct vb2_v4l2_buffer *vbuf;
	struct v4l2_m2m_buffer *entry, *tmp;

	mutex_lock(&ctx->buf_q_mlock);
	if (!list_empty(&ctx->stream_q_list))
		list_for_each_entry_safe(entry, tmp, &ctx->stream_q_list,
					 list) {
			list_del(&entry->list);
			v4l2_m2m_buf_done(&entry->vb, VB2_BUF_STATE_ERROR);
		}
	mutex_unlock(&ctx->buf_q_mlock);

	if (v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx)) {
		while ((vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
			if (vbuf->vb2_buf.state == VB2_BUF_STATE_ACTIVE)
				v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}

	dec_fw_destroy_decoder(ctx);
}

static void al_dec_stop_streaming(struct vb2_queue *q)
{
	struct al_dec_ctx *ctx = vb2_get_drv_priv(q);

	v4l2_m2m_update_stop_streaming_state(ctx->fh.m2m_ctx, q);

	/* Releasing the dst and src buffers */
	ctx->stopped = true;

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		al_dec_stop_streaming_out(ctx);
	else
		al_dec_stop_streaming_cap(ctx);
}

static int al_dec_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			      unsigned int *nplanes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct al_dec_ctx *ctx = vb2_get_drv_priv(vq);
	struct al_frame *format = al_get_frame(ctx, vq->type);

	if (IS_ERR(format)) {
		al_v4l2_err(ctx->codec, "Invalid format %p", format);
		return PTR_ERR(format);
	}

	if (*nplanes)
		return ((sizes[0] < format->sizeimage) ? -EINVAL : 0);

	/* update queue num buffers */
	format->nbuffers = max(*nbuffers, format->nbuffers);

	*nplanes = 1;
	sizes[0] = format->sizeimage;
	*nbuffers = format->nbuffers;

	al_v4l2_dbg(ctx->codec, 2, "%s: Get %d buffers of size %d each ",
		    (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) ? "OUT" : "CAP",
		    *nbuffers, sizes[0]);

	return 0;
}

static int al_dec_buf_prepare(struct vb2_buffer *vb)
{
	struct al_dec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	if (ctx->aborting)
		return -EINVAL;

	if (V4L2_TYPE_IS_CAPTURE(vb->type)) {
		if (vbuf->field == V4L2_FIELD_ANY)
			vbuf->field = V4L2_FIELD_NONE;
		if (vbuf->field != V4L2_FIELD_NONE)
			return -EINVAL;
	}

	al_v4l2_dbg(ctx->codec, 3, "%s : Buffer (%p) prepared ",
		    (V4L2_TYPE_IS_OUTPUT(vb->type) ? "OUT" : "CAP"), vbuf);

	return 0;
}

static inline void al_dec_fill_bitstream(struct al_dec_ctx *ctx)
{
	struct vb2_v4l2_buffer *src_buf;
	struct v4l2_m2m_buffer *m2m_buf;
	struct vb2_queue *src_vq;

	lockdep_assert_held(&ctx->buf_q_mlock);

	if (v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) > 0) {
		src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
		if (!src_buf)
			return;

		/* Dump empty buffers */
		if (!vb2_get_plane_payload(&src_buf->vb2_buf, 0)) {
			src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
			v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_DONE);
			return;
		}

		src_vq = v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx);
		src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);

		if (src_buf) {
			src_buf->sequence = ctx->osequence++;

			if (vb2_is_streaming(src_vq) &&
			    al_dec_fw_push_bitstream_buf(ctx, src_buf)) {
				v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
				return;
			}

			m2m_buf = container_of(src_buf, struct v4l2_m2m_buffer,
					       vb);
			list_add_tail(&m2m_buf->list, &ctx->stream_q_list);
		}
	}
}

static void al_dec_buf_queue(struct vb2_buffer *vb)
{
	struct al_dec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);

	if (V4L2_TYPE_IS_OUTPUT(vb->type)) {
		mutex_lock(&ctx->buf_q_mlock);
		al_dec_fill_bitstream(ctx);
		mutex_unlock(&ctx->buf_q_mlock);
	}

	al_v4l2_dbg(ctx->codec, 3, "%s queued (%p) - (%d)",
		    V4L2_TYPE_IS_OUTPUT(vb->type) ? "OUT" : "CAP", vbuf,
		    vb->num_planes);
}

static const struct vb2_ops dec_queue_ops = {
	.queue_setup = al_dec_queue_setup,
	.buf_prepare = al_dec_buf_prepare,
	.buf_queue = al_dec_buf_queue,
	.start_streaming = al_dec_start_streaming,
	.stop_streaming = al_dec_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int al_dec_queue_init(void *priv, struct vb2_queue *src_vq,
			     struct vb2_queue *dst_vq)
{
	struct al_dec_ctx *ctx = priv;
	struct al_codec_dev *codec = ctx->codec;
	int ret;

	src_vq->dev = &codec->pdev->dev;
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->non_coherent_mem = false;
	src_vq->dma_attrs |= DMA_ATTR_FORCE_CONTIGUOUS;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->drv_priv = ctx;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->ops = &dec_queue_ops;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->lock = &ctx->codec->lock;
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->dev = &codec->pdev->dev;
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->non_coherent_mem = false;
	dst_vq->dma_attrs |= DMA_ATTR_FORCE_CONTIGUOUS;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->drv_priv = ctx;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->ops = &dec_queue_ops;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->lock = &ctx->codec->lock;
	ret = vb2_queue_init(dst_vq);
	if (ret) {
		vb2_queue_release(src_vq);
		return ret;
	}

	return 0;
}

static int al_dec_querycap(struct file *file, void *fh,
			   struct v4l2_capability *cap)
{
	struct al_codec_dev *codec = video_drvdata(file);

	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strscpy(cap->card, "Allegro DVT Video Decoder", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(&codec->pdev->dev));

	return 0;
}

static int al_dec_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	const struct al_fmt *fmt;

	if (f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT &&
	    f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		if (f->index >= ARRAY_SIZE(al_src_formats))
			return -EINVAL;

		fmt = &al_src_formats[f->index];
	} else {
		if (f->index >= ARRAY_SIZE(al_dst_formats))
			return -EINVAL;

		fmt = &al_dst_formats[f->index];
	}

	f->pixelformat = fmt->pixelformat;
	return 0;
}

static int al_dec_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct al_dec_ctx *ctx = fh_to_ctx(fh, struct al_dec_ctx);
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct al_frame *pix_fmt;

	pix_fmt = al_get_frame(ctx, f->type);
	if (IS_ERR(pix_fmt)) {
		al_v4l2_err(ctx->codec, "Invalid frame (%p)", pix_fmt);
		return PTR_ERR(pix_fmt);
	}

	pix_fmt->fmt = al_find_fmt(pix->pixelformat);
	if (!pix_fmt->fmt) {
		al_v4l2_err(ctx->codec, "Unknown format 0x%x",
			    pix->pixelformat);
		return -EINVAL;
	}
	pix->field = V4L2_FIELD_NONE;
	pix->width = clamp_t(__u32, pix->width, DECODER_WIDTH_MIN,
			     DECODER_WIDTH_MAX);
	pix->height = clamp_t(__u32, pix->height, DECODER_HEIGHT_MIN,
			      DECODER_HEIGHT_MAX);

	pix->bytesperline = pix->width;
	pix->sizeimage = (pix->width * pix->height * pix_fmt->fmt->bpp) / 8;

	if (V4L2_TYPE_IS_CAPTURE(f->type))
		if (pix->sizeimage < pix_fmt->sizeimage)
			pix->sizeimage = pix_fmt->sizeimage;

	al_v4l2_dbg(
		ctx->codec, 3,
		"%s : width (%d) , height (%d), bytesperline (%d), sizeimage (%d) ",
		(f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) ? "CAP" : "OUT",
		pix->width, pix->height, pix->bytesperline, pix->sizeimage);

	return 0;
}

static int al_dec_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct al_dec_ctx *ctx = fh_to_ctx(fh, struct al_dec_ctx);
	struct al_frame *pix_fmt = al_get_frame(ctx, f->type);
	struct v4l2_pix_format *pix;

	if (IS_ERR(pix_fmt)) {
		al_v4l2_err(ctx->codec, "Invalid pixel format %p", pix_fmt);
		return PTR_ERR(pix_fmt);
	}

	if (!pix_fmt->fmt) {
		al_v4l2_err(ctx->codec, "Unknown format for %d", f->type);
		return -EINVAL;
	}

	pix = &f->fmt.pix;
	pix->width = pix_fmt->width;
	pix->height = pix_fmt->height;
	pix->bytesperline = pix_fmt->bytesperline;
	pix->sizeimage = pix_fmt->sizeimage;
	pix->pixelformat = pix_fmt->fmt->pixelformat;
	pix->field = V4L2_FIELD_NONE;

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		pix->bytesperline = 0;

	pix->ycbcr_enc = pix_fmt->ycbcr_enc;
	pix->quantization = pix_fmt->quantization;
	pix->xfer_func = pix_fmt->xfer_func;
	pix->colorspace = pix_fmt->colorspace;

	al_v4l2_dbg(
		ctx->codec, 3,
		"%s : width (%d) , height (%d), bytesperline (%d) , sizeimage (%d)",
		(f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) ? "CAP" : "OUT",
		pix->width, pix->height, pix->bytesperline, pix->sizeimage);

	return 0;
}

static int al_dec_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct al_dec_ctx *ctx = fh_to_ctx(fh, struct al_dec_ctx);
	struct v4l2_pix_format *pix;
	struct al_frame *frame;
	struct vb2_queue *vq;
	int ret;

	ret = al_dec_try_fmt(file, fh, f);
	if (ret) {
		al_v4l2_err(ctx->codec, "Cannot set format (%d)", f->type);
		return ret;
	}

	frame = (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) ? &ctx->src : &ctx->dst;

	pix = &f->fmt.pix;
	frame->fmt = al_find_fmt(pix->pixelformat);
	if (!frame->fmt) {
		al_v4l2_err(ctx->codec, "Unknown format for %d",
			    pix->pixelformat);
		return -EINVAL;
	}

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq)) {
		al_v4l2_err(ctx->codec, "queue %d busy", f->type);
		return -EBUSY;
	}

	frame->width = pix->width;
	frame->height = pix->height;
	frame->bytesperline = pix->bytesperline;
	frame->sizeimage = pix->sizeimage;
	frame->field = pix->field;

	frame->ycbcr_enc = pix->ycbcr_enc;
	frame->quantization = pix->quantization;
	frame->xfer_func = pix->xfer_func;
	frame->colorspace = pix->colorspace;

	al_v4l2_dbg(
		ctx->codec, 3,
		" %s : width (%d) , height (%d), bytesperline (%d), sizeimage (%d)",
		(f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) ? "CAP" : "OUT",
		pix->width, pix->height, pix->bytesperline, pix->sizeimage);

	return 0;
}

static void al_queue_eos_event(struct al_dec_ctx *ctx)
{
	const struct v4l2_event eos_event = {
		.id = 0,
		.type = V4L2_EVENT_EOS,
	};

	v4l2_event_queue_fh(&ctx->fh, &eos_event);
}

static void al_queue_res_chg_event(struct al_dec_ctx *ctx)
{
	static const struct v4l2_event ev_src_ch = {
		.id = 0,
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	v4l2_event_queue_fh(&ctx->fh, &ev_src_ch);
}

static int al_dec_decoder_cmd(struct file *file, void *fh,
			      struct v4l2_decoder_cmd *dcmd)
{
	struct al_dec_ctx *ctx = fh_to_ctx(fh, struct al_dec_ctx);
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	struct vb2_v4l2_buffer *vbuf;
	struct vb2_queue *dst_vq;
	int ret;

	ret = v4l2_m2m_ioctl_try_decoder_cmd(file, fh, dcmd);
	if (ret)
		return ret;

	/* Get the vb2 queue for the Capture */
	dst_vq = v4l2_m2m_get_dst_vq(m2m_ctx);

	switch (dcmd->cmd) {
	case V4L2_DEC_CMD_START:
		vb2_clear_last_buffer_dequeued(dst_vq);
		break;
	case V4L2_DEC_CMD_STOP:
		vbuf = v4l2_m2m_last_src_buf(m2m_ctx);
		if (vbuf) {
			al_v4l2_dbg(ctx->codec, 1,
				    "marking last pending buffer");

			vbuf->flags |= V4L2_BUF_FLAG_LAST;
			if (v4l2_m2m_num_src_bufs_ready(m2m_ctx) == 0) {
				al_v4l2_dbg(ctx->codec, 1,
					    "all remaining buffers queued");
				v4l2_m2m_try_schedule(m2m_ctx);
			}
		}
		dec_fw_flush_req(ctx);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int al_dec_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *fsize)
{
	if (!al_find_fmt(fsize->pixel_format))
		return -EINVAL;

	/* FIXME : check step size */
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = DECODER_WIDTH_MIN;
	fsize->stepwise.max_width = DECODER_WIDTH_MAX;
	fsize->stepwise.step_width = 8;
	fsize->stepwise.min_height = DECODER_HEIGHT_MIN;
	fsize->stepwise.max_height = DECODER_HEIGHT_MAX;
	fsize->stepwise.step_height = 8;

	return 0;
}

static int al_dec_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	default:
		return -EINVAL;
	}

	return 0;
}

static int al_dec_log_status(struct file *file, void *fh)
{
	struct al_codec_dev *codec = video_drvdata(file);

	v4l2_device_call_all(&codec->v4l2_dev, 0, core, log_status);
	return 0;
}

static const struct v4l2_ioctl_ops al_dec_ioctl_ops = {
	.vidioc_querycap = al_dec_querycap,
	.vidioc_enum_fmt_vid_cap = al_dec_enum_fmt,
	.vidioc_enum_fmt_vid_out = al_dec_enum_fmt,
	.vidioc_g_fmt_vid_cap = al_dec_g_fmt,
	.vidioc_g_fmt_vid_out = al_dec_g_fmt,
	.vidioc_try_fmt_vid_cap = al_dec_try_fmt,
	.vidioc_try_fmt_vid_out = al_dec_try_fmt,
	.vidioc_s_fmt_vid_cap = al_dec_s_fmt,
	.vidioc_s_fmt_vid_out = al_dec_s_fmt,

	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,

	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_log_status = al_dec_log_status,

	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_try_decoder_cmd,
	.vidioc_decoder_cmd = al_dec_decoder_cmd,
	.vidioc_enum_framesizes = al_dec_enum_framesizes,

	.vidioc_subscribe_event = al_dec_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static void al_device_run(void *priv)
{
	struct al_dec_ctx *ctx = priv;
	struct vb2_v4l2_buffer *dst_buf;
	struct v4l2_m2m_buffer *m2m_buf;

	if (unlikely(!ctx))
		return;

	if (ctx->aborting) {
		vb2_queue_error(v4l2_m2m_get_src_vq(ctx->fh.m2m_ctx));
		vb2_queue_error(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx));
		return;
	}

	if (!v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx))
		goto job_finish;

	dst_buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (!dst_buf)
		goto job_finish;

	if (!al_common_mcu_is_alive(ctx->codec) ||
	    al_dec_fw_push_frame_buf(ctx, dst_buf)) {
		vb2_set_plane_payload(&dst_buf->vb2_buf, 0, 0);
		v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
		goto job_finish;
	}

	mutex_lock(&ctx->buf_q_mlock);
	m2m_buf = container_of(dst_buf, struct v4l2_m2m_buffer, vb);
	list_add_tail(&m2m_buf->list, &ctx->frame_q_list);
	mutex_unlock(&ctx->buf_q_mlock);

job_finish:
	v4l2_m2m_job_finish(ctx->codec->m2m_dev, ctx->fh.m2m_ctx);
}

static const struct v4l2_m2m_ops al_dec_m2m_ops = {
	.device_run = al_device_run,
};

static int al_dec_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct al_codec_dev *codec = video_get_drvdata(vdev);
	struct al_dec_ctx *ctx = NULL;
	int ret;

	if (mutex_lock_interruptible(&codec->ctx_mlock))
		return -ERESTARTSYS;

	/* Aloocate memory for the dec ctx */
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto unlock;
	}

	ctx->codec = codec;
	/* Init ctx mutex */
	mutex_init(&ctx->buf_q_mlock);
	/* Init ctx LISTHEADs*/
	INIT_LIST_HEAD(&ctx->cmd_q_list);
	INIT_LIST_HEAD(&ctx->frame_q_list);
	INIT_LIST_HEAD(&ctx->stream_q_list);

	/* Init the irq queue */
	init_completion(&ctx->res_done);

	v4l2_fh_init(&ctx->fh, vdev);

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 0);
	if (ctx->ctrl_handler.error) {
		ret = ctx->ctrl_handler.error;
		al_v4l2_err(codec, "Failed to create control %d", ret);
		goto handler_error;
	}

	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	v4l2_ctrl_handler_setup(&ctx->ctrl_handler);

	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh, file);

	/* Set default formats */
	ctx->src = ctx->dst = al_default_fmt;
	ctx->csequence = ctx->osequence = 0;

	ctx->stopped = false;
	ctx->aborting = false;
	ctx->id = codec->ctx_counter++;

	/* Setup the ctx for m2m mode */
	ctx->fh.m2m_ctx =
		v4l2_m2m_ctx_init(codec->m2m_dev, ctx, al_dec_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		al_v4l2_err(codec, "Failed to initialize m2m mode %d", ret);
		goto error_ctrls;
	}

	v4l2_m2m_set_src_buffered(ctx->fh.m2m_ctx, true);

	/* Add ctx to the LIST */
	kref_init(&ctx->refcount);
	list_add(&ctx->list, &codec->ctx_q_list);
	al_codec_dbgfs_create(ctx);

	mutex_unlock(&codec->ctx_mlock);

	return 0;

error_ctrls:
	v4l2_fh_del(&ctx->fh, file);
handler_error:
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

unlock:
	mutex_unlock(&codec->ctx_mlock);
	return ret;
}

static int al_dec_release(struct file *file)
{
	struct al_dec_ctx *ctx =
		fh_to_ctx(file->private_data, struct al_dec_ctx);
	struct al_codec_dev *codec = ctx->codec;

	guard(mutex)(&codec->ctx_mlock);

	/* It is important to do this before removing ctx from dev list.
	 * Those commands will trigger some traffic towards fw and so we
	 * need completion to avoid deadlock if cmds can't find ctx.
	 */
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);

	list_del(&ctx->list);
	al_codec_dbgfs_remove(codec, ctx->id);
	al_ctx_put(ctx);

	return 0;
}

static inline bool al_mark_last_dst_buf(struct al_dec_ctx *ctx)
{
	struct vb2_v4l2_buffer *buf;
	struct vb2_buffer *dst_vb;
	struct vb2_queue *dst_vq;
	unsigned long flags;

	al_v4l2_dbg(ctx->codec, 1, "marking last capture buffer");

	dst_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	spin_lock_irqsave(&dst_vq->done_lock, flags);
	if (list_empty(&dst_vq->done_list)) {
		spin_unlock_irqrestore(&dst_vq->done_lock, flags);
		return false;
	}

	dst_vb = list_last_entry(&dst_vq->done_list, struct vb2_buffer,
				 done_entry);
	buf = to_vb2_v4l2_buffer(dst_vb);
	buf->flags |= V4L2_BUF_FLAG_LAST;

	spin_unlock_irqrestore(&dst_vq->done_lock, flags);
	return true;
}

static const struct v4l2_file_operations al_dec_file_ops = {
	.owner = THIS_MODULE,
	.open = al_dec_open,
	.release = al_dec_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static void handle_error_evt(struct al_dec_ctx *ctx, struct msg_itf_header *hdr)
{
	struct al_codec_dev *codec = ctx->codec;
	struct msg_itf_evt_error evt;
	struct v4l2_m2m_buffer *vbuf;

	if (al_common_get_data(codec, (char *)&evt, hdr->payload_len)) {
		al_v4l2_err(codec, "Unable to get resolution found event");
		return;
	}

	al_v4l2_err(codec, "Decoding error  %d", evt.errno);

	guard(mutex)(&ctx->buf_q_mlock);
	if (!list_empty(&ctx->stream_q_list)) {
		vbuf = list_last_entry(&ctx->frame_q_list,
				       struct v4l2_m2m_buffer, list);
		list_del(&vbuf->list);
		v4l2_m2m_buf_done(&vbuf->vb, VB2_BUF_STATE_ERROR);
	}
}

static void handle_resolution_found_evt(struct al_dec_ctx *ctx,
					struct msg_itf_header *hdr)
{
	struct msg_itf_evt_resolution_found evt;
	struct al_codec_dev *codec = ctx->codec;
	struct al_frame *frame;
	struct vb2_queue *dst_vq;

	if (al_common_get_data(codec, (char *)&evt, hdr->payload_len)) {
		al_v4l2_err(codec, "Unable to get resolution found event");
		return;
	}

	frame = &ctx->dst;

	if (frame->width != evt.width || frame->height != evt.height ||
	    frame->nbuffers < evt.buffer_nb) {
		/* Update frame properties */
		frame->width = evt.width;
		frame->height = evt.height;
		frame->bytesperline = evt.bytesperline;
		frame->sizeimage = evt.sizeimage;
		frame->nbuffers = evt.buffer_nb;
		frame->fmt = al_find_fmt(evt.pixelformat);

		/* This has to be changed */
		if (!frame->fmt)
			return;

		al_queue_res_chg_event(ctx);
	}

	dst_vq = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	if (!vb2_is_streaming(dst_vq))
		complete(&ctx->res_done);

	al_v4l2_dbg(
		codec, 3,
		"width(%d) , height(%d), bytesperline(%d), sizeimage(%d), n_bufs(%d)",
		frame->width, frame->height, frame->bytesperline,
		frame->sizeimage, frame->nbuffers);
}

static void handle_bitstream_buffer_release_evt(struct al_dec_ctx *ctx,
						struct msg_itf_header *hdr)
{
	struct msg_itf_evt_bitstream_buffer_release evt;
	struct al_codec_dev *codec = ctx->codec;
	struct vb2_v4l2_buffer *vbuf;

	if (al_common_get_data(codec, (char *)&evt, hdr->payload_len)) {
		al_v4l2_err(codec, "Unable to get buffer release event");
		return;
	}

	if (ctx->stopped)
		return;

	vbuf = al_dec_dequeue_src_buf(ctx, evt.bufferHandle);
	if (!vbuf) {
		al_v4l2_err(codec, "Unable to find bitsream buffer 0x%llx",
			    evt.bufferHandle);
		return;
	}

	al_v4l2_dbg(codec, 3, "Release bitstream buffer %p", vbuf);
	v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_DONE);
}

static void handle_eos_evt(struct al_dec_ctx *ctx, struct msg_itf_header *hdr)
{
	struct msg_itf_evt_frame_buffer_decode evt;
	struct al_codec_dev *codec = ctx->codec;

	if (al_common_get_data(codec, (char *)&evt, hdr->payload_len)) {
		al_v4l2_err(codec, "Unable to get frame buffer event");
		return;
	}

	/* set LAST_FLAG to the last done CAPTURE buffer*/
	al_mark_last_dst_buf(ctx);
	/* Set eos event */
	al_queue_eos_event(ctx);
}

static void handle_frame_buffer_decode_evt(struct al_dec_ctx *ctx,
					   struct msg_itf_header *hdr)
{
	struct msg_itf_evt_frame_buffer_decode evt;
	struct al_codec_dev *codec = ctx->codec;
	struct vb2_v4l2_buffer *vbuf;
	struct al_buffer_meta *meta;

	if (al_common_get_data(codec, (char *)&evt, hdr->payload_len)) {
		al_v4l2_err(codec, "Unable to get frame buffer event");
		return;
	}

	vbuf = al_dec_dequeue_dst_buf(ctx, evt.bufferHandle);
	if (!vbuf) {
		al_v4l2_err(codec, "Unable to find frame buffer 0x%llx",
			    evt.bufferHandle);
		return;
	}

	meta = &evt.meta;
	al_v4l2_dbg(codec, 3, "Decoded frame done for buffer %p (%d) (%lld)",
		    vbuf, meta->last, evt.size);

	vb2_set_plane_payload(&vbuf->vb2_buf, 0, evt.size);
	vbuf->field = V4L2_FIELD_NONE;
	vbuf->sequence = ctx->csequence++;
	vbuf->timecode = meta->timecode;
	vbuf->vb2_buf.timestamp = meta->timestamp;

	if (meta->last || (vbuf->flags & V4L2_BUF_FLAG_LAST)) {
		vbuf->flags |= V4L2_BUF_FLAG_LAST;
		v4l2_m2m_mark_stopped(ctx->fh.m2m_ctx);
	}

	v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_DONE);
}

static int al_handle_cmd_reply(struct al_codec_dev *codec,
			       struct msg_itf_header *hdr)
{
	struct al_dec_ctx *ctx;
	struct al_codec_cmd *cmd = NULL;
	int ret = 0;

	ctx = al_ctx_get(codec, hdr->drv_ctx_hdl);
	if (IS_ERR_OR_NULL(ctx)) {
		al_v4l2_err(codec, "Unable to find ctx %p for reply %d",
			    al_phys_to_virt(hdr->drv_ctx_hdl), hdr->type);
		return -EINVAL;
	}

	cmd = al_codec_cmd_get(&ctx->cmd_q_list, hdr->drv_cmd_hdl);
	if (!cmd) {
		al_v4l2_err(codec, "Unable to find command %p for reply %d",
			    al_phys_to_virt(hdr->drv_cmd_hdl), hdr->type);
		ret = -EINVAL;
		goto ctx_put;
	}

	if (cmd->reply_size != hdr->payload_len) {
		al_v4l2_err(codec, "mismatch size %d %d", cmd->reply_size,
			    hdr->payload_len);
		ret = -EINVAL;
		goto cmd_put;
	}

	ret = al_common_get_data(codec, cmd->reply, hdr->payload_len);
	if (ret)
		al_v4l2_err(codec, "Unable to copy reply");

	complete(&cmd->done);
	ret = 0;

cmd_put:
	al_codec_cmd_put(cmd);
ctx_put:
	al_ctx_put(ctx);

	return ret;
}

static int al_handle_cmd_evt(struct al_codec_dev *dev,
			     struct msg_itf_header *hdr, int type)
{
	static u32 evt_sizes[] = {
		sizeof(struct msg_itf_evt_error),
		sizeof(struct msg_itf_evt_resolution_found),
		sizeof(struct msg_itf_evt_bitstream_buffer_release),
		sizeof(struct msg_itf_evt_frame_buffer_decode),
		sizeof(struct msg_itf_evt_eos),
	};

	u32 evt_size;
	struct al_dec_ctx *ctx = NULL;
	int ret = 0;

	if (type < MSG_ITF_TYPE_NEXT_EVT || type > MSG_ITF_TYPE_END_EVT) {
		al_v4l2_err(dev, "Unsupporting event type %d", type);
		return -EINVAL;
	}

	ctx = al_ctx_get(dev, hdr->drv_ctx_hdl);
	if (!ctx) {
		al_v4l2_err(dev, "Unable to find ctx %p for evt %d",
			    al_phys_to_virt(hdr->drv_ctx_hdl), type);
		return -EINVAL;
	}

	// Check the received event size and the expected one
	evt_size = evt_sizes[type - MSG_ITF_TYPE_NEXT_EVT];
	if (hdr->payload_len != evt_size) {
		al_v4l2_err(
			dev,
			"Invalid event size for client (%p) for evt (%d) : Got (%d), expected (%d)",
			al_phys_to_virt(hdr->drv_ctx_hdl), type,
			hdr->payload_len, evt_size);
		ret = -EINVAL;
		goto clean_ctx;
	}

	al_v4l2_dbg(dev, 3, "Event received from MCU (%d)", type);

	switch (type) {
	case MSG_ITF_TYPE_EVT_ERROR:
		handle_error_evt(ctx, hdr);
		break;
	case MSG_ITF_TYPE_EVT_RESOLUTION_FOUND:
		handle_resolution_found_evt(ctx, hdr);
		break;
	case MSG_ITF_TYPE_EVT_BITSTREAM_BUFFER_RELEASE:
		handle_bitstream_buffer_release_evt(ctx, hdr);
		break;
	case MSG_ITF_TYPE_EVT_FRAME_BUFFER_DECODE:
		handle_frame_buffer_decode_evt(ctx, hdr);
		break;
	case MSG_ITF_TYPE_EVT_EOS:
		handle_eos_evt(ctx, hdr);
		break;
	default:
		break;
	}

clean_ctx:
	al_ctx_put(ctx);
	return ret;
}

static void al_dec_process_msg(void *cb_arg, struct msg_itf_header *hdr)
{
	struct al_codec_dev *dev = cb_arg;
	int ret;

	if (is_type_reply(hdr->type))
		ret = al_handle_cmd_reply(dev, hdr);
	else if (is_type_event(hdr->type))
		ret = al_handle_cmd_evt(dev, hdr, hdr->type);
	else {
		al_v4l2_err(dev, "Unsupported message type %d", hdr->type);
		ret = -EINVAL;
	}

	if (ret) {
		al_v4l2_err(dev, "Skip received data");
		al_common_skip_data(dev, hdr->payload_len);
	}
}

static const struct video_device al_videodev = {
	.name = "allegro-decoder",
	.fops = &al_dec_file_ops,
	.ioctl_ops = &al_dec_ioctl_ops,
	.minor = -1,
	.release = video_device_release_empty,
	.vfl_dir = VFL_DIR_M2M,
	.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

static void al_dec_register_v4l2(void *cb_arg)
{
	struct al_codec_dev *dev = cb_arg;
	struct video_device *video_dev = NULL;
	int ret;

	ret = v4l2_device_register(&dev->pdev->dev, &dev->v4l2_dev);
	if (ret) {
		al_v4l2_err(dev, "Unable to register v4l2 device %d", ret);
		return;
	}

	dev->m2m_dev = v4l2_m2m_init(&al_dec_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		ret = PTR_ERR(dev->m2m_dev);
		al_v4l2_err(dev, "failed to init mem2mem device %d", ret);
		goto v4l2_m2m_init_error;
	}

	video_dev = &dev->video_dev;
	*video_dev = al_videodev;

	video_dev->lock = &dev->lock;
	video_dev->v4l2_dev = &dev->v4l2_dev;

	video_set_drvdata(video_dev, dev);
	ret = video_register_device(video_dev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		al_v4l2_err(dev, "failed to register video device %d", ret);
		goto video_register_device_error;
	}

	v4l2_info(&dev->v4l2_dev, "registered as /dev/video%d\n",
		  dev->video_dev.num);

	dev->init_done = true;

	return;

video_register_device_error:
	v4l2_m2m_release(dev->m2m_dev);
v4l2_m2m_init_error:
	v4l2_device_unregister(&dev->v4l2_dev);
}

static int al_dec_probe(struct platform_device *pdev)
{
	struct al_codec_dev *codec;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct al_match_data *match_data;
	const char *firmware;
	int ret;

	dev_info(dev, "Probing ...\n");

	match_data = device_get_match_data(dev);
	if (!match_data) {
		dev_err(dev, "Missing device match data\n");
		return -EINVAL;
	}

	codec = devm_kzalloc(dev, sizeof(*codec), GFP_KERNEL);
	if (!codec)
		return -ENOMEM;

	codec->pdev = pdev;
	codec->init_done = false;
	codec->ctx_counter = 0;
	mutex_init(&codec->lock);
	mutex_init(&codec->ctx_mlock);
	INIT_LIST_HEAD(&codec->ctx_q_list);

	codec->cb_arg = codec;
	codec->process_msg_cb = al_dec_process_msg;
	codec->fw_ready_cb = al_dec_register_v4l2;

	/* firmware-name is optional in DT */
	of_property_read_string(np, "firmware-name", &firmware);
	if (!firmware)
		firmware = match_data->fw_name;

	ret = al_common_probe(codec, firmware);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, codec);
	al_codec_dbgfs_init(codec);
	dev_info(dev, "Probing done successfully %p\n", codec);

	return 0;
}

static void al_dec_remove(struct platform_device *pdev)
{
	struct al_codec_dev *dev = platform_get_drvdata(pdev);

	dev_info(&pdev->dev, "remove %p\n", dev);

	if (dev->init_done) {
		video_unregister_device(&dev->video_dev);
		if (dev->m2m_dev)
			v4l2_m2m_release(dev->m2m_dev);
		v4l2_device_unregister(&dev->v4l2_dev);
	}
	al_codec_dbgfs_deinit(&dev->dbgfs);
	al_common_remove(dev);
}

static const struct al_match_data ald300_data = {
	.fw_name = "al300-vdec.fw",
};

static const struct of_device_id __maybe_unused v4l2_al_dec_dt_match[] = {
	{ .compatible = "allegro,al300-vdec", .data = &ald300_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, v4l2_al_dec_dt_match);

static struct platform_driver al300_vdec_drv = {
	.probe = al_dec_probe,
	.remove = al_dec_remove,
	.driver = {
		.name = "al300_vdec",
		.of_match_table = of_match_ptr(v4l2_al_dec_dt_match),
	},
};

module_platform_driver(al300_vdec_drv);

MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:al300-vdec");
MODULE_AUTHOR("Yassine OUAISSA <yassine.ouaissa@allegrodvt.com>");
MODULE_DESCRIPTION("Allegro DVT V4l2 decoder driver gen 3");
