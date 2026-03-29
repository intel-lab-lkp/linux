// SPDX-License-Identifier: GPL-2.0
/*
 * USB3HDCAP video streaming - stream parser, ISO URB handling, vb2 ops
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-v4l2.h>

#include "usb3hdcap.h"

/* ------------------------------------------------------------------ */
/* Stream parser                                                      */
/* ------------------------------------------------------------------ */

static void vyuy_to_yuyv(u8 *dst, const u8 *src, int len)
{
	int k;

	/* VYUY: V Y0 U Y1 -> YUYV: Y0 U Y1 V */
	for (k = 0; k < len; k += 4) {
		dst[k + 0] = src[k + 1]; /* Y0 */
		dst[k + 1] = src[k + 2]; /* U */
		dst[k + 2] = src[k + 3]; /* Y1 */
		dst[k + 3] = src[k + 0]; /* V */
	}
}

static struct hdcap_buf *get_next_buf(struct usb3hdcap *hdcap)
{
	struct hdcap_buf *buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&hdcap->buflock, flags);
	if (!list_empty(&hdcap->bufs)) {
		buf = list_first_entry(&hdcap->bufs, struct hdcap_buf, list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&hdcap->buflock, flags);

	return buf;
}

static void deliver_frame(struct usb3hdcap *hdcap)
{
	struct hdcap_buf *buf = hdcap->cur_buf;
	u8 *vaddr;
	int k;

	if (!buf)
		return;

	hdcap->frames_delivered++;

	/* Pad remaining lines with black */
	vaddr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	while (hdcap->frame_line < hdcap->height) {
		u8 *row = vaddr + hdcap->frame_line * hdcap->bpl;

		for (k = 0; k < hdcap->bpl; k += 4) {
			row[k + 0] = 0x10;
			row[k + 1] = 0x80;
			row[k + 2] = 0x10;
			row[k + 3] = 0x80;
		}
		hdcap->frame_line++;
	}

	vb2_set_plane_payload(&buf->vb.vb2_buf, 0,
			      hdcap->bpl * hdcap->height);
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	if (hdcap->interlaced) {
		buf->vb.field = hdcap->is_second_field ? V4L2_FIELD_BOTTOM : V4L2_FIELD_TOP;
		buf->vb.sequence = hdcap->sequence;
		if (hdcap->is_second_field)
			hdcap->sequence++;
		hdcap->is_second_field ^= 1;
	} else {
		buf->vb.field = V4L2_FIELD_NONE;
		buf->vb.sequence = hdcap->sequence++;
	}
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	hdcap->cur_buf = NULL;
}

/*
 * Process one video line (shared between composite and HDMI).
 * 'video' points to the XY byte followed by pixel data.
 */
static void process_video_line(struct usb3hdcap *hdcap, const u8 *video)
{
	u8 sav_xy = video[0];
	int blanking = sav_xy & BT656_V_BIT;

	/* Blanking -> active transition */
	if (!blanking && hdcap->was_blanking) {
		if (!hdcap->synced)
			hdcap->synced = 1;
		if (hdcap->cur_buf && hdcap->frame_line >= 100)
			deliver_frame(hdcap);
		hdcap->frame_line = 0;
		if (!hdcap->cur_buf)
			hdcap->cur_buf = get_next_buf(hdcap);
	}
	hdcap->was_blanking = blanking;

	if (!hdcap->synced || !hdcap->cur_buf || blanking)
		return;

	/* Deliver when field is full */
	if (hdcap->frame_line >= hdcap->height) {
		deliver_frame(hdcap);
		hdcap->frame_line = 0;
		hdcap->cur_buf = get_next_buf(hdcap);
		if (!hdcap->cur_buf)
			return;
	}

	vyuy_to_yuyv(vb2_plane_vaddr(&hdcap->cur_buf->vb.vb2_buf, 0)
			+ hdcap->frame_line * hdcap->bpl,
		     video, hdcap->bpl);
	hdcap->frame_line++;
}

static void process_data(struct usb3hdcap *hdcap)
{
	u8 *buf = hdcap->parse_buf;
	int line_size = SAV_LEN + hdcap->bpl;
	int pos = 0;

	while (pos + 3 <= hdcap->parse_len) {
		if (buf[pos] != 0xff || buf[pos + 1] != 0x00) {
			pos++;
			continue;
		}

		if (buf[pos + 2] == 0x00) {
			/* SAV: video line */
			if (pos + line_size > hdcap->parse_len)
				break;
			/*
			 * -1 because on this hardware, XY byte occupies the first pixel's
			 * V position
			 */
			process_video_line(hdcap, buf + pos + SAV_LEN - 1);
			pos += line_size;
		} else if (buf[pos + 2] == 0xff) {
			u8 type;
			int alen;

			if (pos + MARKER_LEN > hdcap->parse_len)
				break;
			type = buf[pos + 3];
			if (type < 0x02 || type > 0x04) {
				pos++;
				continue;
			}
			alen = type * 4;
			if (pos + MARKER_LEN + alen > hdcap->parse_len)
				break;
			hdcap->markers_found++;
			usb3hdcap_audio_data(hdcap, buf + pos + MARKER_LEN, alen);
			pos += MARKER_LEN + alen;
		} else {
			pos++;
		}
	}

	/* Shift unconsumed data to front */
	if (pos > 0) {
		hdcap->parse_len -= pos;
		if (hdcap->parse_len > 0)
			memmove(buf, buf + pos, hdcap->parse_len);
	}
}

/* ------------------------------------------------------------------ */
/* Isochronous URB handling                                           */
/* ------------------------------------------------------------------ */

static void usb3hdcap_iso_cb(struct urb *urb)
{
	struct usb3hdcap *hdcap = urb->context;
	int k, ret;

	switch (urb->status) {
	case 0:
		break;
	case -ENODEV:
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		return;
	default:
		dev_warn(hdcap->dev, "ISO URB status %d\n", urb->status);
		goto resubmit;
	}

	hdcap->iso_cb_count++;

	for (k = 0; k < urb->number_of_packets; k++) {
		int status = urb->iso_frame_desc[k].status;
		int len = urb->iso_frame_desc[k].actual_length;
		u8 *data = urb->transfer_buffer +
			   urb->iso_frame_desc[k].offset;

		if (status != 0 || len == 0)
			continue;

		if (hdcap->parse_len + len > PARSE_BUF_SIZE) {
			hdcap->parse_len = 0;
			continue;
		}

		memcpy(hdcap->parse_buf + hdcap->parse_len, data, len);
		hdcap->parse_len += len;
		hdcap->iso_bytes += len;
	}

	process_data(hdcap);

resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0)
		dev_warn(hdcap->dev, "ISO URB resubmit: %d\n", ret);
}

static struct urb *usb3hdcap_alloc_urb(struct usb3hdcap *hdcap)
{
	struct urb *urb;
	int size = hdcap->iso_size;
	int k;

	urb = usb_alloc_urb(NUM_ISO_PKTS, GFP_KERNEL);
	if (!urb)
		return NULL;

	urb->dev = hdcap->usb_dev;
	urb->context = hdcap;
	urb->pipe = usb_rcvisocpipe(hdcap->usb_dev, EP_VIDEO);
	urb->interval = 1;
	urb->transfer_flags = URB_ISO_ASAP;
	urb->transfer_buffer = kcalloc(NUM_ISO_PKTS, size, GFP_KERNEL);
	if (!urb->transfer_buffer) {
		usb_free_urb(urb);
		return NULL;
	}
	urb->complete = usb3hdcap_iso_cb;
	urb->number_of_packets = NUM_ISO_PKTS;
	urb->transfer_buffer_length = size * NUM_ISO_PKTS;
	for (k = 0; k < NUM_ISO_PKTS; k++) {
		urb->iso_frame_desc[k].offset = size * k;
		urb->iso_frame_desc[k].length = size;
	}

	return urb;
}

static void usb3hdcap_stop(struct usb3hdcap *hdcap)
{
	int k;
	unsigned long flags;

	for (k = 0; k < NUM_XFERS; k++) {
		struct urb *urb = hdcap->isoc_urbs[k];

		if (!urb)
			continue;
		usb_kill_urb(urb);
		kfree(urb->transfer_buffer);
		usb_free_urb(urb);
		hdcap->isoc_urbs[k] = NULL;
	}

	/* already NULL if this stop came from a disconnect */
	if (hdcap->usb_dev) {
		vendor_out(hdcap, REQ_STREAM, 0x0000, 0, NULL, 0);
		usb_set_interface(hdcap->usb_dev, 0, 0);
	}

	/* Return any in-progress buffer */
	if (hdcap->cur_buf) {
		vb2_buffer_done(&hdcap->cur_buf->vb.vb2_buf,
				VB2_BUF_STATE_ERROR);
		hdcap->cur_buf = NULL;
	}

	spin_lock_irqsave(&hdcap->buflock, flags);
	while (!list_empty(&hdcap->bufs)) {
		struct hdcap_buf *buf = list_first_entry(&hdcap->bufs,
						struct hdcap_buf, list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&hdcap->buflock, flags);

	vfree(hdcap->parse_buf);
	hdcap->parse_buf = NULL;
}

static int usb3hdcap_start(struct usb3hdcap *hdcap)
{
	struct usb_host_endpoint *ep;
	int maxp, burst, mult;
	int k, ret, alt;

	if (!hdcap->hw_inited) {
		ret = usb3hdcap_hw_init(hdcap);
		if (ret < 0) {
			dev_err(hdcap->dev, "open: hw_init failed: %d\n", ret);
			return ret;
		}
		hdcap->hw_inited = 1;
	}

	hdcap->parse_buf = vzalloc(PARSE_BUF_SIZE);
	if (!hdcap->parse_buf)
		return -ENOMEM;
	hdcap->parse_len = 0;
	hdcap->frame_line = 0;
	hdcap->synced = 0;
	hdcap->was_blanking = 0;
	hdcap->is_second_field = 0;
	hdcap->cur_buf = NULL;

	if (hdcap->input == INPUT_HDMI) {
		/*
		 * this is a register watchlist, interrupt generated n endpoint 1 when
		 * (value & mask) changes.
		 *
		 * struct reg_watch {
		 *   uint8_t reg;
		 *   // 0x80-0x82: MST3367 bank n&0x7f, 0x40: look at I2C addr
		 *   uint8_t bank_or_source;
		 *   uint8_t value_mask;
		 *   uint8_t i2c_addr; // for bank_or_source == 0x40
		 * };
		 */
		u8 stream_payload[] = {
			0x55, 0x80, 0x3c, 0x00,  0x6a, 0x80, 0x0f, 0x00,
			0x6b, 0x80, 0xfe, 0x00,  0x57, 0x80, 0x3f, 0x00,
			0x58, 0x80, 0x80, 0x00,  0x59, 0x80, 0x3f, 0x00,
			0x5a, 0x80, 0xf0, 0x00,  0x5b, 0x80, 0x07, 0x00,
			0x5c, 0x80, 0xfe, 0x00,  0x5f, 0x80, 0x02, 0x00,
			0x01, 0x81, 0x07, 0x00,  0x29, 0x82, 0x1f, 0x00,
			0x28, 0x82, 0xff, 0x00,  0x0b, 0x82, 0xdf, 0x00,
			0x0c, 0x82, 0x3f, 0x00,  0x0e, 0x82, 0x08, 0x00,
			0x48, 0x82, 0x60, 0x00,  0x9b, 0x82, 0xf0, 0x00,
			0x11, 0x82, 0xff, 0x00,  0x12, 0x82, 0xff, 0x00,
			0x11, 0x82, 0xff, 0x00,  0x12, 0x82, 0xff, 0x00,
		};
		ret = vendor_out(hdcap, REQ_STREAM, 0x0032, 1,
				 stream_payload, sizeof(stream_payload));
	} else if (hdcap->input == INPUT_COMPONENT) {
		/* Component: monitor MST3367 ADC registers */
		u8 stream_payload[] = {
			0x14, 0x80, 0x90, 0x00,  0x15, 0x80, 0x46, 0x00,
			0x5b, 0x80, 0x07, 0x00,  0x5c, 0x80, 0xf8, 0x00,
			0x57, 0x80, 0x3f, 0x00,  0x5f, 0x80, 0x12, 0x00,
			0x11, 0x82, 0xff, 0x00,  0x12, 0x82, 0xff, 0x00,
		};
		ret = vendor_out(hdcap, REQ_STREAM, 0x0032, 1,
				 stream_payload, sizeof(stream_payload));
	} else {
		u8 stream_payload[] = {
			0x01, 0x40, 0xe9, 0x88,
			0x1c, 0x40, 0xf0, 0x88,
			0x30, 0x40, 0x0f, 0x88,
		};
		v4l2_ctrl_handler_setup(&hdcap->ctrl);
		ret = vendor_out(hdcap, REQ_STREAM, 0x0032, 1,
				 stream_payload, sizeof(stream_payload));
	}

	if (ret < 0) {
		vfree(hdcap->parse_buf);
		hdcap->parse_buf = NULL;
		return ret;
	}

	/*
	 * Select alt setting based on bandwidth:
	 * alt 1: maxp=1024 burst=16 mult=1 -> 16KB/pkt (SD)
	 * alt 2: maxp=1024 burst=16 mult=2 -> 32KB/pkt (HD)
	 */
	alt = (hdcap->width > 720 || hdcap->height > 576) ? 2 : 1;
	dev_info(hdcap->dev, "%s: setting alt %d\n", __func__, alt);
	ret = usb_set_interface(hdcap->usb_dev, 0, alt);

	if (ret < 0) {
		dev_err(hdcap->dev, "%s: usb_set_interface failed: %d\n",
			__func__, ret);
		vfree(hdcap->parse_buf);
		hdcap->parse_buf = NULL;
		return ret;
	}

	/* Compute ISO packet size from SS endpoint companion descriptor */
	ep = usb_pipe_endpoint(hdcap->usb_dev,
			       usb_rcvisocpipe(hdcap->usb_dev, EP_VIDEO));
	if (!ep) {
		dev_err(hdcap->dev, "ISO endpoint 0x%02x not found\n",
			EP_VIDEO);
		return -ENODEV;
	}

	maxp = usb_endpoint_maxp(&ep->desc);
	burst = ep->ss_ep_comp.bMaxBurst;
	mult = USB_SS_MULT(ep->ss_ep_comp.bmAttributes);
	hdcap->iso_size = maxp * (burst + 1) * mult;

	for (k = 0; k < NUM_XFERS; k++) {
		struct urb *urb = usb3hdcap_alloc_urb(hdcap);

		if (!urb) {
			ret = -ENOMEM;
			goto fail;
		}
		hdcap->isoc_urbs[k] = urb;

		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret < 0)
			goto fail;
	}

	return 0;

fail:
	usb3hdcap_stop(hdcap);
	return ret;
}

/* ------------------------------------------------------------------ */
/* vb2 ops                                                            */
/* ------------------------------------------------------------------ */

static int usb3hdcap_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				 unsigned int *nplanes, unsigned int sizes[],
	struct device *alloc_devs[])
{
	struct usb3hdcap *hdcap = vb2_get_drv_priv(vq);
	unsigned int img_size = hdcap->bpl * hdcap->height;

	if (*nplanes) {
		dev_info(hdcap->dev, "queue_setup: recheck sizes[0]=%u vs %u\n",
			 sizes[0], img_size);
		return sizes[0] < img_size ? -EINVAL : 0;
	}
	*nplanes = 1;
	sizes[0] = img_size;

	return 0;
}

static void usb3hdcap_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct usb3hdcap *hdcap = vb2_get_drv_priv(vb->vb2_queue);
	struct hdcap_buf *buf = container_of(vbuf, struct hdcap_buf, vb);
	unsigned long flags;

	if (!hdcap->usb_dev) {
		dev_warn(hdcap->dev, "buf_queue: usb_dev is NULL\n");
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}

	spin_lock_irqsave(&hdcap->buflock, flags);
	list_add_tail(&buf->list, &hdcap->bufs);
	spin_unlock_irqrestore(&hdcap->buflock, flags);
}

static int usb3hdcap_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct usb3hdcap *hdcap = vb2_get_drv_priv(vq);
	int ret;

	if (!hdcap->usb_dev) {
		dev_err(hdcap->dev, "start_streaming: usb_dev is NULL\n");
		return -ENODEV;
	}

	hdcap->sequence = 0;
	ret = usb3hdcap_start(hdcap);
	return ret;
}

static void usb3hdcap_stop_streaming(struct vb2_queue *vq)
{
	struct usb3hdcap *hdcap = vb2_get_drv_priv(vq);

	if (hdcap->usb_dev)
		usb3hdcap_stop(hdcap);
}

const struct vb2_ops usb3hdcap_vb2_ops = {
	.queue_setup = usb3hdcap_queue_setup,
	.buf_queue = usb3hdcap_buf_queue,
	.start_streaming = usb3hdcap_start_streaming,
	.stop_streaming = usb3hdcap_stop_streaming,
};
