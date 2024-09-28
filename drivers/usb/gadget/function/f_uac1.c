// SPDX-License-Identifier: GPL-2.0+
/*
 * f_uac1.c -- USB Audio Class 1.0 Function (using u_audio API)
 *
 * Copyright (C) 2016 Ruslan Bilovol <ruslan.bilovol@gmail.com>
 * Copyright (C) 2021 Julian Scheel <julian@jusst.de>
 *
 * This driver doesn't expect any real Audio codec to be present
 * on the device - the audio streams are simply sinked to and
 * sourced from a virtual ALSA sound card created.
 *
 * This file is based on f_uac1.c which is
 *   Copyright (C) 2008 Bryan Wu <cooloney@kernel.org>
 *   Copyright (C) 2008 Analog Devices, Inc
 */

#include <linux/usb/audio.h>
#include <linux/module.h>

#include "u_audio.h"
#include "u_uac1.h"
#include "u_uac_utils.h"

#define HOST_TO_DEVICE 0
#define DEVICE_TO_HOST 1

/* UAC1 spec: 3.7.2.3 Audio Channel Cluster Format */
#define UAC1_CHANNEL_MASK 0x0FFF

#define USB_OUT_FU_ID(_opts)	(_opts->c_alt_1_opts.fu_id)
#define USB_IN_FU_ID(_opts)	(_opts->p_alt_1_opts.fu_id)

#define EP_EN(_alt_opts) ((_alt_opts) && ((_alt_opts)->chmask != 0))
#define FUIN_EN(_opts) ((_opts)->p_mute_present \
			|| (_opts)->p_volume_present)
#define FUOUT_EN(_opts) ((_opts)->c_mute_present \
			|| (_opts)->c_volume_present)
#define EPOUT_FBACK_IN_EN(_alt_opts) ((_alt_opts)->sync == USB_ENDPOINT_SYNC_ASYNC)

/* Check if any alt mode has option enabled */
#define EN_ANY(single, fn, cp)						\
static int fn(struct f_uac1_opts *opts)					\
{									\
	struct f_uac1_alt_opts *alt_opts;				\
									\
	if (single(&opts->cp##_alt_1_opts))				\
		return 1;						\
									\
	list_for_each_entry(alt_opts, &opts->cp##_alt_opts, list) {	\
		if (single(alt_opts))					\
			return 1;					\
	}								\
									\
	return 0;							\
}

EN_ANY(EP_EN, epout_en_any, c)
EN_ANY(EP_EN, epin_en_any, p)
EN_ANY(EPOUT_FBACK_IN_EN, epout_fback_in_en_any, p)

struct f_uac1 {
	struct g_audio g_audio;
	u8 ac_intf, as_in_intf, as_out_intf;
	u8 ac_alt, as_in_alt, as_out_alt;	/* needed for get_alt() */

	struct usb_ctrlrequest setup_cr;	/* will be used in data stage */

	/* Interrupt IN endpoint of AC interface */
	struct usb_ep	*int_ep;
	atomic_t	int_count;
	int ctl_id;		/* EP id */
	int c_srate;	/* current capture srate */
	int p_srate;	/* current playback prate */
};

static inline struct f_uac1 *func_to_uac1(struct usb_function *f)
{
	return container_of(f, struct f_uac1, g_audio.func);
}

static inline struct f_uac1_opts *g_audio_to_uac1_opts(struct g_audio *audio)
{
	return container_of(audio->func.fi, struct f_uac1_opts, func_inst);
}

/*
 * DESCRIPTORS ... most are static, but strings and full
 * configuration descriptors are built on demand.
 */

/*
 * We have three interfaces - one AudioControl and two AudioStreaming
 *
 * The driver implements a simple UAC_1 topology.
 * USB-OUT -> IT_1 -> OT_2 -> ALSA_Capture
 * ALSA_Playback -> IT_3 -> OT_4 -> USB-IN
 */

/* B.3.1  Standard AC Interface Descriptor */
static struct usb_interface_descriptor ac_interface_desc = {
	.bLength =		USB_DT_INTERFACE_SIZE,
	.bDescriptorType =	USB_DT_INTERFACE,
	/* .bNumEndpoints =	DYNAMIC */
	.bInterfaceClass =	USB_CLASS_AUDIO,
	.bInterfaceSubClass =	USB_SUBCLASS_AUDIOCONTROL,
};

/* B.3.2  Class-Specific AC Interface Descriptor */
static struct uac1_ac_header_descriptor *ac_header_desc;

/* AC IN Interrupt Endpoint */
static struct usb_endpoint_descriptor fs_ac_int_ep_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(2),
	.bInterval = 1,
};

static struct usb_endpoint_descriptor hs_ac_int_ep_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(2),
	.bInterval = 4,
};

static struct usb_endpoint_descriptor ss_ac_int_ep_desc = {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize = cpu_to_le16(2),
	.bInterval = 4,
};

static struct usb_ss_ep_comp_descriptor ss_ac_int_ep_desc_comp = {
	.bLength = sizeof(ss_ac_int_ep_desc_comp),
	.bDescriptorType = USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst = 0,
	.bmAttributes = 0,
	.wBytesPerInterval = cpu_to_le16(2),
};

/* Class-specific AS ISO OUT Endpoint Descriptor */
static struct uac_iso_endpoint_descriptor as_iso_out_desc = {
	.bLength =		UAC_ISO_ENDPOINT_DESC_SIZE,
	.bDescriptorType =	USB_DT_CS_ENDPOINT,
	.bDescriptorSubtype =	UAC_EP_GENERAL,
	.bmAttributes =		1,
	.bLockDelayUnits =	1,
	.wLockDelay =		cpu_to_le16(1),
};

/* Class-specific AS ISO IN Endpoint Descriptor */
static struct uac_iso_endpoint_descriptor as_iso_in_desc = {
	.bLength =		UAC_ISO_ENDPOINT_DESC_SIZE,
	.bDescriptorType =	USB_DT_CS_ENDPOINT,
	.bDescriptorSubtype =	UAC_EP_GENERAL,
	.bmAttributes =		1,
	.bLockDelayUnits =	0,
	.wLockDelay =		0,
};

/* STD AS ISO IN Feedback Endpoint */
static struct usb_endpoint_descriptor fs_as_in_fback_desc = {
	.bLength = USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_USAGE_FEEDBACK,
	.wMaxPacketSize = cpu_to_le16(3),
	.bInterval = 1,
	.bRefresh = 0,
	.bSynchAddress = 0,
};

static struct usb_endpoint_descriptor hs_as_in_fback_desc = {
	.bLength = USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_USAGE_FEEDBACK,
	.wMaxPacketSize = cpu_to_le16(4),
	.bInterval = 4,
	.bRefresh = 0,
	.bSynchAddress = 0,
};

static struct usb_endpoint_descriptor ss_as_in_fback_desc = {
	.bLength = USB_DT_ENDPOINT_AUDIO_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = USB_DIR_IN,
	.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_USAGE_FEEDBACK,
	.wMaxPacketSize = cpu_to_le16(4),
	.bInterval = 4,
	.bRefresh = 0,
	.bSynchAddress = 0,
};

static struct usb_ss_ep_comp_descriptor ss_as_in_fback_desc_comp = {
	.bLength		= sizeof(ss_as_in_fback_desc_comp),
	.bDescriptorType	= USB_DT_SS_ENDPOINT_COMP,
	.bMaxBurst		= 0,
	.bmAttributes		= 0,
	.wBytesPerInterval	= cpu_to_le16(4),
};

/*
 * This function is an ALSA sound card following USB Audio Class Spec 1.0.
 */

static void uac_cs_attr_sample_rate(struct usb_ep *ep, struct usb_request *req)
{
	struct usb_function *fn = ep->driver_data;
	struct usb_composite_dev *cdev = fn->config->cdev;
	struct g_audio *agdev = func_to_g_audio(fn);
	struct f_uac1 *uac1 = func_to_uac1(fn);
	u8 *buf = (u8 *)req->buf;
	u32 val = 0;

	if (req->actual != 3) {
		WARN(cdev, "Invalid data size for UAC_EP_CS_ATTR_SAMPLE_RATE.\n");
		return;
	}

	val = buf[0] | (buf[1] << 8) | (buf[2] << 16);
	if (uac1->ctl_id == (USB_DIR_IN | 2)) {
		uac1->p_srate = val;
		u_audio_set_playback_srate(agdev, uac1->p_srate);
	} else if (uac1->ctl_id == (USB_DIR_OUT | 1)) {
		uac1->c_srate = val;
		u_audio_set_capture_srate(agdev, uac1->c_srate);
	}
}

static void audio_notify_complete(struct usb_ep *_ep, struct usb_request *req)
{
	struct g_audio *audio = req->context;
	struct f_uac1 *uac1 = func_to_uac1(&audio->func);

	atomic_dec(&uac1->int_count);
	kfree(req->buf);
	usb_ep_free_request(_ep, req);
}

static int audio_notify(struct g_audio *audio, int unit_id, int cs)
{
	struct f_uac1 *uac1 = func_to_uac1(&audio->func);
	struct usb_request *req;
	struct uac1_status_word *msg;
	int ret;

	if (!uac1->int_ep->enabled)
		return 0;

	if (atomic_inc_return(&uac1->int_count) > UAC1_DEF_INT_REQ_NUM) {
		atomic_dec(&uac1->int_count);
		return 0;
	}

	req = usb_ep_alloc_request(uac1->int_ep, GFP_ATOMIC);
	if (req == NULL) {
		ret = -ENOMEM;
		goto err_dec_int_count;
	}

	msg = kmalloc(sizeof(*msg), GFP_ATOMIC);
	if (msg == NULL) {
		ret = -ENOMEM;
		goto err_free_request;
	}

	msg->bStatusType = UAC1_STATUS_TYPE_IRQ_PENDING
				| UAC1_STATUS_TYPE_ORIG_AUDIO_CONTROL_IF;
	msg->bOriginator = unit_id;

	req->length = sizeof(*msg);
	req->buf = msg;
	req->context = audio;
	req->complete = audio_notify_complete;

	ret = usb_ep_queue(uac1->int_ep, req, GFP_ATOMIC);

	if (ret)
		goto err_free_msg;

	return 0;

err_free_msg:
	kfree(msg);
err_free_request:
	usb_ep_free_request(uac1->int_ep, req);
err_dec_int_count:
	atomic_dec(&uac1->int_count);

	return ret;
}

static int
in_rq_cur(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *audio = func_to_g_audio(fn);
	struct f_uac1_opts *opts = g_audio_to_uac1_opts(audio);
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	int value = -EOPNOTSUPP;

	if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts))) ||
			(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID(opts)))) {
		unsigned int is_playback = 0;

		if (FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts)))
			is_playback = 1;

		if (control_selector == UAC_FU_MUTE) {
			unsigned int mute;

			u_audio_get_mute(audio, is_playback, &mute);

			*(u8 *)req->buf = mute;
			value = min_t(unsigned int, w_length, 1);
		} else if (control_selector == UAC_FU_VOLUME) {
			__le16 c;
			s16 volume;

			u_audio_get_volume(audio, is_playback, &volume);

			c = cpu_to_le16(volume);

			value = min_t(unsigned int, w_length, sizeof(c));
			memcpy(req->buf, &c, value);
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
	} else {
		dev_err(&audio->gadget->dev,
			"%s:%d entity_id=%d control_selector=%d TODO!\n",
			__func__, __LINE__, entity_id, control_selector);
	}

	return value;
}

static int
in_rq_min(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *audio = func_to_g_audio(fn);
	struct f_uac1_opts *opts = g_audio_to_uac1_opts(audio);
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	int value = -EOPNOTSUPP;

	if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts))) ||
			(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID(opts)))) {
		unsigned int is_playback = 0;

		if (FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts)))
			is_playback = 1;

		if (control_selector == UAC_FU_VOLUME) {
			__le16 r;
			s16 min_db;

			if (is_playback)
				min_db = opts->p_volume_min;
			else
				min_db = opts->c_volume_min;

			r = cpu_to_le16(min_db);

			value = min_t(unsigned int, w_length, sizeof(r));
			memcpy(req->buf, &r, value);
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
	} else {
		dev_err(&audio->gadget->dev,
			"%s:%d entity_id=%d control_selector=%d TODO!\n",
			__func__, __LINE__, entity_id, control_selector);
	}

	return value;
}

static int
in_rq_max(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *audio = func_to_g_audio(fn);
	struct f_uac1_opts *opts = g_audio_to_uac1_opts(audio);
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	int value = -EOPNOTSUPP;

	if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts))) ||
			(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID(opts)))) {
		unsigned int is_playback = 0;

		if (FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts)))
			is_playback = 1;

		if (control_selector == UAC_FU_VOLUME) {
			__le16 r;
			s16 max_db;

			if (is_playback)
				max_db = opts->p_volume_max;
			else
				max_db = opts->c_volume_max;

			r = cpu_to_le16(max_db);

			value = min_t(unsigned int, w_length, sizeof(r));
			memcpy(req->buf, &r, value);
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
	} else {
		dev_err(&audio->gadget->dev,
			"%s:%d entity_id=%d control_selector=%d TODO!\n",
			__func__, __LINE__, entity_id, control_selector);
	}

	return value;
}

static int
in_rq_res(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *audio = func_to_g_audio(fn);
	struct f_uac1_opts *opts = g_audio_to_uac1_opts(audio);
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;
	int value = -EOPNOTSUPP;

	if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts))) ||
			(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID(opts)))) {
		unsigned int is_playback = 0;

		if (FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts)))
			is_playback = 1;

		if (control_selector == UAC_FU_VOLUME) {
			__le16 r;
			s16 res_db;

			if (is_playback)
				res_db = opts->p_volume_res;
			else
				res_db = opts->c_volume_res;

			r = cpu_to_le16(res_db);

			value = min_t(unsigned int, w_length, sizeof(r));
			memcpy(req->buf, &r, value);
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
		}
	} else {
		dev_err(&audio->gadget->dev,
			"%s:%d entity_id=%d control_selector=%d TODO!\n",
			__func__, __LINE__, entity_id, control_selector);
	}

	return value;
}

static void
out_rq_cur_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct g_audio *audio = req->context;
	struct usb_composite_dev *cdev = audio->func.config->cdev;
	struct f_uac1_opts *opts = g_audio_to_uac1_opts(audio);
	struct f_uac1 *uac1 = func_to_uac1(&audio->func);
	struct usb_ctrlrequest *cr = &uac1->setup_cr;
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;

	if (req->status != 0) {
		dev_dbg(&cdev->gadget->dev, "completion err %d\n", req->status);
		return;
	}

	if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts))) ||
			(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID(opts)))) {
		unsigned int is_playback = 0;

		if (FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts)))
			is_playback = 1;

		if (control_selector == UAC_FU_MUTE) {
			u8 mute = *(u8 *)req->buf;

			u_audio_set_mute(audio, is_playback, mute);

			return;
		} else if (control_selector == UAC_FU_VOLUME) {
			__le16 *c = req->buf;
			s16 volume;

			volume = le16_to_cpu(*c);
			u_audio_set_volume(audio, is_playback, volume);

			return;
		} else {
			dev_err(&audio->gadget->dev,
				"%s:%d control_selector=%d TODO!\n",
				__func__, __LINE__, control_selector);
			usb_ep_set_halt(ep);
		}
	} else {
		dev_err(&audio->gadget->dev,
			"%s:%d entity_id=%d control_selector=%d TODO!\n",
			__func__, __LINE__, entity_id, control_selector);
		usb_ep_set_halt(ep);

	}
}

static int
out_rq_cur(struct usb_function *fn, const struct usb_ctrlrequest *cr)
{
	struct usb_request *req = fn->config->cdev->req;
	struct g_audio *audio = func_to_g_audio(fn);
	struct f_uac1_opts *opts = g_audio_to_uac1_opts(audio);
	struct f_uac1 *uac1 = func_to_uac1(&audio->func);
	u16 w_length = le16_to_cpu(cr->wLength);
	u16 w_index = le16_to_cpu(cr->wIndex);
	u16 w_value = le16_to_cpu(cr->wValue);
	u8 entity_id = (w_index >> 8) & 0xff;
	u8 control_selector = w_value >> 8;

	if ((FUIN_EN(opts) && (entity_id == USB_IN_FU_ID(opts))) ||
			(FUOUT_EN(opts) && (entity_id == USB_OUT_FU_ID(opts)))) {
		memcpy(&uac1->setup_cr, cr, sizeof(*cr));
		req->context = audio;
		req->complete = out_rq_cur_complete;

		return w_length;
	} else {
		dev_err(&audio->gadget->dev,
			"%s:%d entity_id=%d control_selector=%d TODO!\n",
			__func__, __LINE__, entity_id, control_selector);
	}
	return -EOPNOTSUPP;
}

static int ac_rq_in(struct usb_function *f,
		const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	int value = -EOPNOTSUPP;
	u8 ep = ((le16_to_cpu(ctrl->wIndex) >> 8) & 0xFF);
	u16 len = le16_to_cpu(ctrl->wLength);
	u16 w_value = le16_to_cpu(ctrl->wValue);

	DBG(cdev, "bRequest 0x%x, w_value 0x%04x, len %d, endpoint %d\n",
			ctrl->bRequest, w_value, len, ep);

	switch (ctrl->bRequest) {
	case UAC_GET_CUR:
		return in_rq_cur(f, ctrl);
	case UAC_GET_MIN:
		return in_rq_min(f, ctrl);
	case UAC_GET_MAX:
		return in_rq_max(f, ctrl);
	case UAC_GET_RES:
		return in_rq_res(f, ctrl);
	case UAC_GET_MEM:
		break;
	case UAC_GET_STAT:
		value = len;
		break;
	default:
		break;
	}

	return value;
}

static int audio_set_endpoint_req(struct usb_function *f,
		const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_request	*req = f->config->cdev->req;
	struct f_uac1		*uac1 = func_to_uac1(f);
	int			value = -EOPNOTSUPP;
	u16			ep = le16_to_cpu(ctrl->wIndex);
	u16			len = le16_to_cpu(ctrl->wLength);
	u16			w_value = le16_to_cpu(ctrl->wValue);
	u8			cs = w_value >> 8;

	DBG(cdev, "bRequest 0x%x, w_value 0x%04x, len %d, endpoint %d\n",
			ctrl->bRequest, w_value, len, ep);

	switch (ctrl->bRequest) {
	case UAC_SET_CUR: {
		if (cs == UAC_EP_CS_ATTR_SAMPLE_RATE) {
			cdev->gadget->ep0->driver_data = f;
			uac1->ctl_id = ep;
			req->complete = uac_cs_attr_sample_rate;
		}
		value = len;
		break;
	}

	case UAC_SET_MIN:
		break;

	case UAC_SET_MAX:
		break;

	case UAC_SET_RES:
		break;

	case UAC_SET_MEM:
		break;

	default:
		break;
	}

	return value;
}

static int audio_get_endpoint_req(struct usb_function *f,
		const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_request *req = f->config->cdev->req;
	struct f_uac1 *uac1 = func_to_uac1(f);
	u8 *buf = (u8 *)req->buf;
	int value = -EOPNOTSUPP;
	u8 ep = le16_to_cpu(ctrl->wIndex);
	u16 len = le16_to_cpu(ctrl->wLength);
	u16 w_value = le16_to_cpu(ctrl->wValue);
	u8 cs = w_value >> 8;
	u32 val = 0;

	DBG(cdev, "bRequest 0x%x, w_value 0x%04x, len %d, endpoint %d\n",
			ctrl->bRequest, w_value, len, ep);

	switch (ctrl->bRequest) {
	case UAC_GET_CUR: {
		if (cs == UAC_EP_CS_ATTR_SAMPLE_RATE) {
			if (ep == (USB_DIR_IN | 2))
				val = uac1->p_srate;
			else if (ep == (USB_DIR_OUT | 1))
				val = uac1->c_srate;
			buf[2] = (val >> 16) & 0xff;
			buf[1] = (val >> 8) & 0xff;
			buf[0] = val & 0xff;
		}
		value = len;
		break;
	}
	case UAC_GET_MIN:
	case UAC_GET_MAX:
	case UAC_GET_RES:
		value = len;
		break;
	case UAC_GET_MEM:
		break;
	default:
		break;
	}

	return value;
}

static int
f_audio_setup(struct usb_function *f, const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_request	*req = cdev->req;
	int			value = -EOPNOTSUPP;
	u16			w_index = le16_to_cpu(ctrl->wIndex);
	u16			w_value = le16_to_cpu(ctrl->wValue);
	u16			w_length = le16_to_cpu(ctrl->wLength);

	/* composite driver infrastructure handles everything; interface
	 * activation uses set_alt().
	 */
	switch (ctrl->bRequestType) {
	case USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_ENDPOINT:
		value = audio_set_endpoint_req(f, ctrl);
		break;

	case USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT:
		value = audio_get_endpoint_req(f, ctrl);
		break;
	case USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE:
		if (ctrl->bRequest == UAC_SET_CUR)
			value = out_rq_cur(f, ctrl);
		break;
	case USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE:
		value = ac_rq_in(f, ctrl);
		break;
	default:
		ERROR(cdev, "invalid control req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
	}

	/* respond with data transfer or status phase? */
	if (value >= 0) {
		DBG(cdev, "audio req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			w_value, w_index, w_length);
		req->zero = 0;
		req->length = value;
		value = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
		if (value < 0)
			ERROR(cdev, "audio response on err %d\n", value);
	}

	/* device either stalls (value < 0) or reports success */
	return value;
}

static int f_audio_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_gadget *gadget = cdev->gadget;
	struct device *dev = &gadget->dev;
	struct g_audio *audio = func_to_g_audio(f);
	struct f_uac1 *uac1 = func_to_uac1(f);
	int ret = 0;

	/* No i/f has more than 2 alt settings */
	if (alt > 1) {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return -EINVAL;
	}

	if (intf == uac1->ac_intf) {
		/* Control I/f has only 1 AltSetting - 0 */
		if (alt) {
			dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
			return -EINVAL;
		}

		/* restart interrupt endpoint */
		if (uac1->int_ep) {
			usb_ep_disable(uac1->int_ep);
			config_ep_by_speed(gadget, &audio->func, uac1->int_ep);
			usb_ep_enable(uac1->int_ep);
		}

		return 0;
	}

	if (intf == uac1->as_out_intf) {
		uac1->as_out_alt = alt;

		if (alt)
			ret = u_audio_start_capture(&uac1->g_audio);
		else
			u_audio_stop_capture(&uac1->g_audio);
	} else if (intf == uac1->as_in_intf) {
		uac1->as_in_alt = alt;

		if (alt)
			ret = u_audio_start_playback(&uac1->g_audio);
		else
			u_audio_stop_playback(&uac1->g_audio);
	} else {
		dev_err(dev, "%s:%d Error!\n", __func__, __LINE__);
		return -EINVAL;
	}

	return ret;
}

static int f_audio_get_alt(struct usb_function *f, unsigned intf)
{
	struct usb_composite_dev *cdev = f->config->cdev;
	struct usb_gadget *gadget = cdev->gadget;
	struct device *dev = &gadget->dev;
	struct f_uac1 *uac1 = func_to_uac1(f);

	if (intf == uac1->ac_intf)
		return uac1->ac_alt;
	else if (intf == uac1->as_out_intf)
		return uac1->as_out_alt;
	else if (intf == uac1->as_in_intf)
		return uac1->as_in_alt;
	else
		dev_err(dev, "%s:%d Invalid Interface %d!\n",
			__func__, __LINE__, intf);

	return -EINVAL;
}


static void f_audio_disable(struct usb_function *f)
{
	struct f_uac1 *uac1 = func_to_uac1(f);

	uac1->as_out_alt = 0;
	uac1->as_in_alt = 0;

	u_audio_stop_playback(&uac1->g_audio);
	u_audio_stop_capture(&uac1->g_audio);
	if (uac1->int_ep)
		usb_ep_disable(uac1->int_ep);
}

static void
f_audio_suspend(struct usb_function *f)
{
	struct f_uac1 *uac1 = func_to_uac1(f);

	u_audio_suspend(&uac1->g_audio);
}

/*-------------------------------------------------------------------------*/

/*
 * String handling
 */

#define MAX_STRINGS 256

static int add_string(struct usb_string *strings, const char *s)
{
	int i;

	if (!s || s[0] == '\0')
		return 0;

	for (i = 0; i < MAX_STRINGS; i++) {
		if (!strings[i].s) {
			strings[i].s = s;
			return 0; /* IDs aren't allocated yet */
		}

		if (!strcmp(s, strings[i].s))
			return strings[i].id;
	}

	return -1;
}

static void add_alt_strings(struct usb_string *strings, struct f_uac1_alt_opts *alt_opts, bool fu)
{
	add_string(strings, alt_opts->name);
	add_string(strings, alt_opts->it_name);
	add_string(strings, alt_opts->it_ch_name);
	add_string(strings, alt_opts->ot_name);
	if (fu)
		add_string(strings, alt_opts->fu_vol_name);
}

static struct usb_string *attach_strings(struct usb_composite_dev *cdev,
					 struct f_uac1_opts *audio_opts)
{
	struct usb_string	*strings = kzalloc(sizeof(struct usb_string) * MAX_STRINGS,
						   GFP_KERNEL);
	struct f_uac1_alt_opts	*alt_opts;
	struct usb_string	*us;
	int			strings_uac1_length;

	struct usb_gadget_strings str_uac1 = {
		.language = 0x0409,	/* en-us */
		.strings = strings
	};

	struct usb_gadget_strings *uac1_strings[] = {
		&str_uac1,
		NULL,
	};

	if (!strings)
		return ERR_PTR(-ENOMEM);

	/* Add all the strings from all the alt mode options */
	add_string(strings, audio_opts->function_name);
	add_string(strings, audio_opts->c_alt_0_opts.name);
	add_string(strings, audio_opts->p_alt_0_opts.name);
	add_alt_strings(strings, &audio_opts->c_alt_1_opts, FUOUT_EN(audio_opts));
	add_alt_strings(strings, &audio_opts->p_alt_1_opts, FUIN_EN(audio_opts));
	list_for_each_entry(alt_opts, &audio_opts->c_alt_opts, list) {
		add_alt_strings(strings, alt_opts, FUOUT_EN(audio_opts));
	}
	list_for_each_entry(alt_opts, &audio_opts->p_alt_opts, list) {
		add_alt_strings(strings, alt_opts, FUIN_EN(audio_opts));
	}

	for (strings_uac1_length = 0; strings[strings_uac1_length].s; strings_uac1_length++)
		;

	/* Attach strings to the composite device and get string IDs assigned */
	us = usb_gstrings_attach(cdev, uac1_strings, strings_uac1_length);

	/* Strings are now copied to the composite device and we use the
	 * copy in "us" going forward, that has all the string IDs.
	 */
	kfree(strings);

	return us;
}

/*-------------------------------------------------------------------------*/

/*
 * Descriptor building functions
 */

static int set_ep_max_packet_size_bint(struct device *dev, const struct f_uac1_alt_opts *alt_opts,
					struct usb_endpoint_descriptor *ep_desc,
					enum usb_device_speed speed, bool is_playback)
{
	return uac_set_ep_max_packet_size_bint(
		dev, ep_desc, speed, is_playback, alt_opts->hs_bint,
		alt_opts->chmask, get_max_srate(alt_opts->srates),
		alt_opts->ssize, alt_opts->sync, alt_opts->c.opts->fb_max);
}

struct path_params {
	int dir;
	int id;
	struct f_uac1_opts *opts;
	struct usb_string *strings;
};

/* 4.3.2.1 Input Terminal Descriptor */
static void init_it_desc(struct uac_input_terminal_descriptor *it_desc,
			 struct f_uac1_alt_opts *alt_opts,
			 struct path_params *params)
{
	it_desc->bLength =		UAC_DT_INPUT_TERMINAL_SIZE;
	it_desc->bDescriptorType =	USB_DT_CS_INTERFACE;
	it_desc->bDescriptorSubtype =	UAC_INPUT_TERMINAL;
	it_desc->bTerminalID =		params->id++;
	it_desc->wTerminalType =	cpu_to_le16((params->dir == HOST_TO_DEVICE)
							?  UAC_TERMINAL_STREAMING
							: alt_opts->terminal_type);
	it_desc->bAssocTerminal =	0;
	it_desc->bNrChannels =		num_channels(alt_opts->chmask);
	it_desc->wChannelConfig =	cpu_to_le16(alt_opts->chmask);
	it_desc->iTerminal =		add_string(params->strings, alt_opts->it_name);
	it_desc->iChannelNames =	add_string(params->strings, alt_opts->it_ch_name);
}

/* 4.3.2.2 Output Terminal Descriptor */
static void init_ot_desc(struct uac1_output_terminal_descriptor *ot_desc,
			 struct f_uac1_alt_opts *alt_opts,
			 struct path_params *params, int src_id)
{
	ot_desc->bLength =		UAC_DT_OUTPUT_TERMINAL_SIZE;
	ot_desc->bDescriptorType =	USB_DT_CS_INTERFACE;
	ot_desc->bDescriptorSubtype =	UAC_OUTPUT_TERMINAL;
	ot_desc->bTerminalID =		params->id++;
	ot_desc->wTerminalType =	cpu_to_le16((params->dir == HOST_TO_DEVICE)
							? alt_opts->terminal_type
							: UAC_TERMINAL_STREAMING);
	ot_desc->bAssocTerminal =	0;
	ot_desc->bSourceID =		src_id;
	ot_desc->iTerminal =		add_string(params->strings, alt_opts->ot_name);
}

/* 4.3.2.5 Feature Unit Descriptor */
static struct uac_feature_unit_descriptor *build_fu_desc(struct f_uac1_alt_opts *alt_opts,
							 struct path_params *params,
							 int src_id)
{
	struct uac_feature_unit_descriptor *fu_desc;
	int channels = num_channels(alt_opts->chmask);
	int fu_desc_size = UAC_DT_FEATURE_UNIT_SIZE(channels);
	u32 control = 0;
	u8 *i_feature;

	fu_desc = kzalloc(fu_desc_size, GFP_KERNEL);
	if (!fu_desc)
		return NULL;

	fu_desc->bLength = fu_desc_size;
	fu_desc->bDescriptorType = USB_DT_CS_INTERFACE;
	fu_desc->bDescriptorSubtype = UAC_FEATURE_UNIT;
	fu_desc->bUnitID = params->id++;
	fu_desc->bSourceID = src_id;
	fu_desc->bControlSize  = 2;

	if (params->dir == HOST_TO_DEVICE) {
		if (params->opts->c_mute_present)
			control |= UAC_FU_MUTE;
		if (params->opts->c_volume_present)
			control |= UAC_FU_VOLUME;
	}

	if (params->dir == DEVICE_TO_HOST) {
		if (params->opts->p_mute_present)
			control |= UAC_FU_MUTE;
		if (params->opts->p_volume_present)
			control |= UAC_FU_VOLUME;
	}

	/* Only master volume/mute is supported. Per-channel controls are all zero. */
	fu_desc->bmaControls[0] = cpu_to_le16(control);

	/* iFeature is located after all channel controls */
	i_feature = (u8 *)fu_desc + fu_desc->bLength - 1;
	*i_feature = add_string(params->strings, alt_opts->fu_vol_name);

	return fu_desc;
}

/* 4.3.2  Class-Specific AC Interface Descriptor */
static struct
uac1_ac_header_descriptor *build_ac_header_desc(struct f_uac1 *uac1, struct f_uac1_opts *opts)
{
	struct uac1_ac_header_descriptor *ac_desc;
	int ac_header_desc_size;
	int capture = epout_en_any(opts);
	int playback = epin_en_any(opts);
	int ba_iface_id = 0;

	ac_header_desc_size = UAC_DT_AC_HEADER_SIZE(capture + playback);

	ac_desc = kzalloc(ac_header_desc_size, GFP_KERNEL);
	if (!ac_desc)
		return NULL;

	ac_desc->bLength = ac_header_desc_size;
	ac_desc->bDescriptorType = USB_DT_CS_INTERFACE;
	ac_desc->bDescriptorSubtype = UAC_HEADER;
	ac_desc->bcdADC = cpu_to_le16(0x0100);
	ac_desc->bInCollection = capture + playback;

	if (capture)
		ac_desc->baInterfaceNr[ba_iface_id++] = uac1->as_out_intf;

	if (playback)
		ac_desc->baInterfaceNr[ba_iface_id++] = uac1->as_in_intf;

	/* wTotalLength will be defined later */

	return ac_desc;
}

/* 4.5.1  Standard AS Interface Descriptor */
static void init_as_interface_desc(struct usb_interface_descriptor *iface_desc,
				   u8 ifnum, u8 alt, u8 endpoints, const char *name,
				   struct usb_string *strings)
{
	iface_desc->bLength =			USB_DT_INTERFACE_SIZE;
	iface_desc->bDescriptorType =		USB_DT_INTERFACE;
	iface_desc->bInterfaceNumber =		ifnum;
	iface_desc->bAlternateSetting =		alt;
	iface_desc->bNumEndpoints =		endpoints;
	iface_desc->bInterfaceClass =		USB_CLASS_AUDIO;
	iface_desc->bInterfaceSubClass =	USB_SUBCLASS_AUDIOSTREAMING;
	iface_desc->bInterfaceProtocol =	0;
	iface_desc->iInterface =		add_string(strings, name);
}

/* 4.5.2  Class-Specific AS Interface Descriptor */
static void init_as_header_desc(struct uac1_as_header_descriptor *as_header_desc, int terminalId)
{
	as_header_desc->bLength =		UAC_DT_AS_HEADER_SIZE;
	as_header_desc->bDescriptorType =	USB_DT_CS_INTERFACE;
	as_header_desc->bDescriptorSubtype =	UAC_AS_GENERAL;
	as_header_desc->bTerminalLink =		terminalId;
	as_header_desc->bDelay =		1;
	as_header_desc->wFormatTag =		cpu_to_le16(UAC_FORMAT_TYPE_I_PCM);
}

/* 4.5.3 Class-Specific AS Format Type Descriptor */
static void init_uac_format_type_i_discrete_desc(struct f_uac1_alt_opts *alt_opts)
{
	int idx, i;

	alt_opts->fmt_desc.bDescriptorType = USB_DT_CS_INTERFACE;
	alt_opts->fmt_desc.bDescriptorSubtype = UAC_FORMAT_TYPE;
	alt_opts->fmt_desc.bFormatType = UAC_FORMAT_TYPE_I;
	alt_opts->fmt_desc.bNrChannels = num_channels(alt_opts->chmask);
	alt_opts->fmt_desc.bSubframeSize = alt_opts->ssize;
	alt_opts->fmt_desc.bBitResolution = alt_opts->ssize * 8;

	/* Set sample rates */
	for (i = 0, idx = 0; i < UAC_MAX_RATES; i++) {
		if (alt_opts->srates[i] == 0)
			break;
		memcpy(alt_opts->fmt_desc.tSamFreq[idx++],
			&alt_opts->srates[i], 3);
	}
	alt_opts->fmt_desc.bLength = UAC_FORMAT_TYPE_I_DISCRETE_DESC_SIZE(idx);
	alt_opts->fmt_desc.bSamFreqType = idx;
}

static int init_isoc_ep_descriptor(struct device *dev, struct usb_endpoint_descriptor *ep_desc,
				   struct f_uac1_alt_opts *alt_opts, int dir,
				   enum usb_device_speed speed, u8 addr)
{
	ep_desc->bLength =		USB_DT_ENDPOINT_AUDIO_SIZE;
	ep_desc->bDescriptorType =	USB_DT_ENDPOINT;
	ep_desc->bEndpointAddress =	addr;
	ep_desc->bmAttributes =		USB_ENDPOINT_XFER_ISOC |
					(((dir == HOST_TO_DEVICE) && !EPOUT_FBACK_IN_EN(alt_opts))
						? USB_ENDPOINT_SYNC_ADAPTIVE
						: USB_ENDPOINT_SYNC_ASYNC);
	ep_desc->bInterval =		1; /* For FS. For HS/SS, this is set later from hs_bint. */
	ep_desc->bRefresh =		0;
	ep_desc->bSynchAddress =	EPOUT_FBACK_IN_EN(alt_opts)
						? fs_as_in_fback_desc.bEndpointAddress : 0;

	return set_ep_max_packet_size_bint(dev, alt_opts, ep_desc, speed, (dir == DEVICE_TO_HOST));
}

static void init_isoc_ep_descriptor_comp(struct usb_ss_ep_comp_descriptor *ep_desc_comp,
					 struct usb_endpoint_descriptor *ep_desc)
{
	ep_desc_comp->bLength		= sizeof(*ep_desc_comp),
	ep_desc_comp->bDescriptorType	= USB_DT_SS_ENDPOINT_COMP,
	ep_desc_comp->bMaxBurst		= 0,
	ep_desc_comp->bmAttributes	= 0,
	ep_desc_comp->wBytesPerInterval = ep_desc->wMaxPacketSize;
}

static int init_alt_descriptors(struct device *dev, struct f_uac1_alt_opts *alt_opts, int ifnum,
				u8 epaddr, int endpoints, int terminalID, int dir,
				struct usb_string *strings)
{
	int status = 0;

	init_as_header_desc(&alt_opts->as_header_desc, terminalID);
	init_as_interface_desc(&alt_opts->intf_desc, ifnum, alt_opts->c.alt_num, endpoints,
			       alt_opts->name, strings);
	init_uac_format_type_i_discrete_desc(alt_opts);

	status = init_isoc_ep_descriptor(dev, &alt_opts->fs_iso_ep_desc, alt_opts, dir,
					 USB_SPEED_FULL, epaddr);
	if (!status)
		status = init_isoc_ep_descriptor(dev, &alt_opts->hs_iso_ep_desc, alt_opts, dir,
						 USB_SPEED_HIGH, epaddr);
	if (!status)
		status = init_isoc_ep_descriptor(dev, &alt_opts->ss_iso_ep_desc, alt_opts, dir,
						 USB_SPEED_SUPER, epaddr);

	init_isoc_ep_descriptor_comp(&alt_opts->ss_iso_ep_desc_comp, &alt_opts->ss_iso_ep_desc);

	return status;
}

static struct f_uac1_path_descriptors *
build_path_descriptors(struct path_params *params, struct f_uac1_alt_opts *alt_opts)
{
	struct f_uac1_path_descriptors *path_descs;
	u8 srcId;

	path_descs = kzalloc(sizeof(*path_descs), GFP_KERNEL);
	if (!path_descs)
		return NULL;

	path_descs->dir = params->dir;
	path_descs->alt_opts = alt_opts;

	init_it_desc(&path_descs->it_desc, alt_opts, params);
	srcId = path_descs->it_desc.bTerminalID;

	if (((params->dir == HOST_TO_DEVICE) && FUOUT_EN(params->opts)) ||
	    ((params->dir == DEVICE_TO_HOST) && FUIN_EN(params->opts))) {
		path_descs->fu_desc = build_fu_desc(alt_opts, params,
						    path_descs->it_desc.bTerminalID);
		srcId = path_descs->fu_desc->bUnitID;
	}

	init_ot_desc(&path_descs->ot_desc, alt_opts, params, srcId);

	return path_descs;
}

static void free_path_descriptors(struct f_uac1_path_descriptors *path_descs)
{
	kfree(path_descs->fu_desc);
	kfree(path_descs);
}

static struct f_uac1_path_descriptors *find_path_descriptors(struct list_head *list,
							     struct f_uac1_alt_opts *alt_opts,
							     int dir)
{
	struct f_uac1_path_descriptors *path_descs;

	list_for_each_entry(path_descs, list, list) {
		/* Check that all options used in the path descriptors are the same */
		if ((path_descs->dir == dir) &&
		    (!strncmp(path_descs->alt_opts->name, alt_opts->name,
			      sizeof(alt_opts->name))) &&
		    (!strncmp(path_descs->alt_opts->it_name, alt_opts->it_name,
			      sizeof(alt_opts->it_name))) &&
		    (!strncmp(path_descs->alt_opts->it_ch_name, alt_opts->it_ch_name,
			      sizeof(alt_opts->it_ch_name))) &&
		    (!strncmp(path_descs->alt_opts->ot_name, alt_opts->ot_name,
			      sizeof(alt_opts->ot_name))) &&
		    (path_descs->alt_opts->chmask == alt_opts->chmask) &&
		    (path_descs->alt_opts->terminal_type == alt_opts->terminal_type))
			return path_descs;
	}
	return NULL;
}

static int add_path_descriptors(struct list_head *list,
				struct path_params *params,
				struct f_uac1_alt_opts *alt_opts)
{
	int len = 0;
	struct f_uac1_path_descriptors *path_descs;

	if (!EP_EN(alt_opts))
		return 0;

	path_descs = find_path_descriptors(list, alt_opts, params->dir);

	if (!path_descs) {
		path_descs = build_path_descriptors(params, alt_opts);
		if (path_descs) {
			list_add_tail(&path_descs->list, list);
			len += sizeof(path_descs->it_desc);
			len += sizeof(path_descs->ot_desc);
			if (path_descs->fu_desc)
				len += path_descs->fu_desc->bLength;
		}
	}

	if (path_descs) {
		alt_opts->as_header_desc.bTerminalLink =
			(params->dir == HOST_TO_DEVICE) ? path_descs->it_desc.bTerminalID
							: path_descs->ot_desc.bTerminalID;
		alt_opts->it_id = path_descs->it_desc.bTerminalID;
		alt_opts->fu_id = path_descs->fu_desc ? path_descs->fu_desc->bUnitID : 0;
		alt_opts->ot_id = path_descs->ot_desc.bTerminalID;
	}

	return len;
}

/* Use macro to overcome line length limitation */
#define USBDHDR(p) (struct usb_descriptor_header *)(p)

static int setup_headers(struct usb_descriptor_header **desc_list,
			 struct f_uac1 *uac1,
			 struct f_uac1_opts *opts,
			 struct list_head *path_descs,
			 enum usb_device_speed speed);

static int setup_descriptor(struct device *dev, struct f_uac1 *uac1, struct f_uac1_opts *opts,
			    struct usb_string *strings)
{
	int status;
	struct usb_descriptor_header **fs_desc_list, **hs_desc_list, **ss_ssp_desc_list;

	/* patch descriptors */
	int len;
	struct list_head path_descs = LIST_HEAD_INIT(path_descs);
	int fs_num, hs_num, ss_ssp_num;
	struct f_uac1_alt_opts *alt_opts;
	struct list_head *path_desc, *tmp;
	struct path_params params;

	params.id = 1; /* ID's start with 1 */
	params.opts = opts;
	params.strings = strings;

	ac_header_desc = build_ac_header_desc(uac1, opts);
	if (!ac_header_desc)
		return -ENOMEM;

	len = ac_header_desc->bLength;

	if (uac1->g_audio.out_ep) {
		params.dir = HOST_TO_DEVICE;
		init_as_interface_desc(&opts->c_alt_0_opts.intf_desc, uac1->as_out_intf, 0, 0,
				       opts->c_alt_0_opts.name, strings);

		/* Audio path descriptors (input terminal -> <feature unit> -> output terminal) */
		len += add_path_descriptors(&path_descs, &params, &opts->c_alt_1_opts);

		status = init_alt_descriptors(dev, &opts->c_alt_1_opts, uac1->as_out_intf,
					      uac1->g_audio.out_ep->address,
					      EPOUT_FBACK_IN_EN(&opts->c_alt_1_opts) ? 2 : 1,
					      opts->c_alt_1_opts.it_id, HOST_TO_DEVICE, strings);
		if (status) {
			dev_err(dev, "Failed to init alt descs for capture alt %d (%d)\n",
				1, status);
			goto cleanup;
		}

		list_for_each_entry(alt_opts, &opts->c_alt_opts, list) {
			len += add_path_descriptors(&path_descs, &params, alt_opts);

			status = init_alt_descriptors(dev, alt_opts, uac1->as_out_intf,
						      uac1->g_audio.out_ep->address,
						      EPOUT_FBACK_IN_EN(alt_opts) ? 2 : 1,
						      alt_opts->it_id, HOST_TO_DEVICE, strings);
			if (status) {
				dev_err(dev, "Failed to init alt descs for capture alt %d (%d)\n",
					alt_opts->c.alt_num, status);
				goto cleanup;
			}
		}
	}

	if (uac1->g_audio.in_ep) {
		params.dir = DEVICE_TO_HOST;
		init_as_interface_desc(&opts->p_alt_0_opts.intf_desc, uac1->as_in_intf, 0, 0,
				       opts->p_alt_0_opts.name, strings);

		/* Audio path descriptors (input terminal -> <feature unit> -> output terminal) */
		len += add_path_descriptors(&path_descs, &params, &opts->p_alt_1_opts);

		status = init_alt_descriptors(dev, &opts->p_alt_1_opts, uac1->as_in_intf,
					      uac1->g_audio.in_ep->address, 1,
					      opts->p_alt_1_opts.ot_id, DEVICE_TO_HOST, strings);
		if (status) {
			dev_err(dev, "Failed to init alt descs for playback alt %d (%d)\n",
				1, status);
			goto cleanup;
		}

		list_for_each_entry(alt_opts, &opts->p_alt_opts, list) {
			len += add_path_descriptors(&path_descs, &params, alt_opts);

			status = init_alt_descriptors(dev, alt_opts, uac1->as_in_intf,
						      uac1->g_audio.in_ep->address, 1,
						      alt_opts->ot_id, DEVICE_TO_HOST, strings);
			if (status) {
				dev_err(dev, "Failed to init alt descs for playback alt %d (%d)\n",
					alt_opts->c.alt_num, status);
				goto cleanup;
			}
		}
	}

	ac_header_desc->wTotalLength = cpu_to_le16(len);

	/* Count how many descriptors we have and then allocate and populate */
	fs_num = setup_headers(NULL, uac1, opts, &path_descs, USB_SPEED_FULL);
	hs_num = setup_headers(NULL, uac1, opts, &path_descs, USB_SPEED_HIGH);
	ss_ssp_num = setup_headers(NULL, uac1, opts, &path_descs, USB_SPEED_SUPER);

	fs_desc_list = kzalloc((fs_num + hs_num + ss_ssp_num) * sizeof(*fs_desc_list), GFP_KERNEL);
	if (!fs_desc_list) {
		status = -ENOMEM;
		goto cleanup;
	}
	hs_desc_list = fs_desc_list + fs_num;
	ss_ssp_desc_list = hs_desc_list + hs_num;

	(void) setup_headers(fs_desc_list, uac1, opts, &path_descs, USB_SPEED_FULL);
	(void) setup_headers(hs_desc_list, uac1, opts, &path_descs, USB_SPEED_HIGH);
	(void) setup_headers(ss_ssp_desc_list, uac1, opts, &path_descs, USB_SPEED_SUPER);

	/* copy descriptors, and track endpoint copies */
	status = usb_assign_descriptors(&uac1->g_audio.func, fs_desc_list, hs_desc_list,
					ss_ssp_desc_list, ss_ssp_desc_list);

	if (status)
		dev_err(dev, "Failed to assign descriptors (%d)\n", status);

	kfree(fs_desc_list);

cleanup:
	list_for_each_safe(path_desc, tmp, &path_descs) {
		free_path_descriptors(
			container_of(path_desc, struct f_uac1_path_descriptors, list));
	}
	kfree(ac_header_desc);
	ac_header_desc = NULL;

	return status;
}

static inline void add_descriptor(int i, struct usb_descriptor_header **desc_list,
				  struct usb_descriptor_header *desc)
{
	if (desc_list)
		desc_list[i] = desc;
}

static int add_alt_descriptors(int i, struct usb_descriptor_header **desc_list,
			       struct f_uac1_alt_opts *alt_opts, enum usb_device_speed speed)
{
	add_descriptor(i++, desc_list, USBDHDR(&alt_opts->intf_desc));
	add_descriptor(i++, desc_list, USBDHDR(&alt_opts->as_header_desc));
	add_descriptor(i++, desc_list, USBDHDR(&alt_opts->fmt_desc));
	if (speed == USB_SPEED_FULL)
		add_descriptor(i++, desc_list, USBDHDR(&alt_opts->fs_iso_ep_desc));
	else if (speed == USB_SPEED_HIGH)
		add_descriptor(i++, desc_list, USBDHDR(&alt_opts->hs_iso_ep_desc));
	else if (speed == USB_SPEED_SUPER || speed == USB_SPEED_SUPER_PLUS) {
		add_descriptor(i++, desc_list, USBDHDR(&alt_opts->ss_iso_ep_desc));
		add_descriptor(i++, desc_list, USBDHDR(&alt_opts->ss_iso_ep_desc_comp));
	}

	return i;
}

static int setup_headers(struct usb_descriptor_header **desc_list,
			 struct f_uac1 *uac1,
			 struct f_uac1_opts *opts,
			 struct list_head *path_descs,
			 enum usb_device_speed speed)
{
	struct usb_ss_ep_comp_descriptor *epin_fback_desc_comp = NULL;
	struct usb_ss_ep_comp_descriptor *ep_int_desc_comp = NULL;
	struct usb_endpoint_descriptor *epin_fback_desc;
	struct usb_endpoint_descriptor *ep_int_desc;

	int i = 0;
	struct list_head *pos;

	switch (speed) {
	case USB_SPEED_FULL:
		epin_fback_desc = &fs_as_in_fback_desc;
		ep_int_desc = &fs_ac_int_ep_desc;
		break;
	case USB_SPEED_HIGH:
		epin_fback_desc = &hs_as_in_fback_desc;
		ep_int_desc = &hs_ac_int_ep_desc;
		break;
	default:
		epin_fback_desc = &ss_as_in_fback_desc;
		epin_fback_desc_comp = &ss_as_in_fback_desc_comp;
		ep_int_desc = &ss_ac_int_ep_desc;
		ep_int_desc_comp = &ss_ac_int_ep_desc_comp;
	}

	add_descriptor(i++, desc_list, USBDHDR(&ac_interface_desc));
	add_descriptor(i++, desc_list, USBDHDR(ac_header_desc));

	list_for_each(pos, path_descs) {
		struct f_uac1_path_descriptors *path_desc =
			container_of(pos, struct f_uac1_path_descriptors, list);
		add_descriptor(i++, desc_list, USBDHDR(&path_desc->it_desc));
		add_descriptor(i++, desc_list, USBDHDR(&path_desc->ot_desc));
		if (path_desc->fu_desc)
			add_descriptor(i++, desc_list, USBDHDR(path_desc->fu_desc));
	}

	// If any FU exists, add the interrupt endpoint descriptor
	if (FUOUT_EN(opts) || FUIN_EN(opts)) {
		add_descriptor(i++, desc_list, USBDHDR(ep_int_desc));
		if (ep_int_desc_comp)
			add_descriptor(i++, desc_list, USBDHDR(ep_int_desc_comp));
	}

	// If any capture interface is active
	if (epout_en_any(opts)) {
		struct f_uac1_alt_opts *alt_opts;

		add_descriptor(i++, desc_list, USBDHDR(&opts->c_alt_0_opts.intf_desc));

		if (EP_EN(&opts->c_alt_1_opts)) {
			i = add_alt_descriptors(i, desc_list, &opts->c_alt_1_opts, speed);

			add_descriptor(i++, desc_list, USBDHDR(&as_iso_out_desc));
			if (EPOUT_FBACK_IN_EN(&opts->c_alt_1_opts)) {
				add_descriptor(i++, desc_list, USBDHDR(epin_fback_desc));
				if (epin_fback_desc_comp)
					add_descriptor(i++, desc_list,
						       USBDHDR(epin_fback_desc_comp));
			}
		}

		list_for_each_entry(alt_opts, &opts->c_alt_opts, list) {
			if (EP_EN(alt_opts)) {
				i = add_alt_descriptors(i, desc_list, alt_opts, speed);

				add_descriptor(i++, desc_list, USBDHDR(&as_iso_out_desc));
				if (EPOUT_FBACK_IN_EN(alt_opts)) {
					add_descriptor(i++, desc_list, USBDHDR(epin_fback_desc));
					if (epin_fback_desc_comp)
						add_descriptor(i++, desc_list,
							       USBDHDR(epin_fback_desc_comp));
				}
			}
		}
	}

	// If any playback interface is active
	if (epin_en_any(opts)) {
		struct f_uac1_alt_opts *alt_opts;

		add_descriptor(i++, desc_list, USBDHDR(&opts->p_alt_0_opts.intf_desc));

		if (EP_EN(&opts->p_alt_1_opts)) {
			i = add_alt_descriptors(i, desc_list, &opts->p_alt_1_opts, speed);

			add_descriptor(i++, desc_list, USBDHDR(&as_iso_in_desc));
		}

		list_for_each_entry(alt_opts, &opts->p_alt_opts, list) {
			if (EP_EN(alt_opts)) {
				i = add_alt_descriptors(i, desc_list, alt_opts, speed);

				add_descriptor(i++, desc_list, USBDHDR(&as_iso_in_desc));
			}
		}
	}

	add_descriptor(i++, desc_list, NULL);

	return i;
}

static int f_audio_validate_opts(struct g_audio *audio, struct device *dev)
{
	struct f_uac1_opts *opts = g_audio_to_uac1_opts(audio);
	struct f_uac1_alt_opts *alt_opts;

	if (!epin_en_any(opts) && !epout_en_any(opts)) {
		dev_err(dev, "Error: no playback and capture channels\n");
		return -EINVAL;
	}

	list_for_each_entry(alt_opts, &opts->p_alt_opts, list) {
		if (alt_opts->chmask & ~UAC1_CHANNEL_MASK) {
			dev_err(dev, "Error: unsupported playback channels mask for alt %d\n",
				alt_opts->c.alt_num);
			return -EINVAL;
		} else if ((alt_opts->ssize < 1) || (alt_opts->ssize > 4)) {
			dev_err(dev, "Error: incorrect playback sample size for alt %d\n",
				alt_opts->c.alt_num);
			return -EINVAL;
		} else if ((alt_opts->hs_bint < 0) || (alt_opts->hs_bint > 4)) {
			dev_err(dev, "Error: incorrect playback HS/SS bInterval (1-4: fixed, 0: auto) for alt %d\n",
				alt_opts->c.alt_num);

			return -EINVAL;
		}
	}

	list_for_each_entry(alt_opts, &opts->c_alt_opts, list) {
		if (alt_opts->chmask & ~UAC1_CHANNEL_MASK) {
			dev_err(dev, "Error: unsupported capture channels mask for alt %d\n",
				alt_opts->c.alt_num);
			return -EINVAL;
		} else if ((alt_opts->ssize < 1) || (alt_opts->ssize > 4)) {
			dev_err(dev, "Error: incorrect capture sample size for alt %d\n",
				alt_opts->c.alt_num);
			return -EINVAL;
		} else if ((alt_opts->hs_bint < 0) || (alt_opts->hs_bint > 4)) {
			dev_err(dev, "Error: incorrect capture HS/SS bInterval (1-4: fixed, 0: auto) for alt %d\n",
				alt_opts->c.alt_num);

			return -EINVAL;
		}
	}

	if (!opts->p_srates[0]) {
		dev_err(dev, "Error: incorrect playback sampling rate\n");
		return -EINVAL;
	} else if (!opts->c_srates[0]) {
		dev_err(dev, "Error: incorrect capture sampling rate\n");
		return -EINVAL;
	}

	if (opts->p_volume_max <= opts->p_volume_min) {
		dev_err(dev, "Error: incorrect playback volume max/min\n");
		return -EINVAL;
	} else if (opts->c_volume_max <= opts->c_volume_min) {
		dev_err(dev, "Error: incorrect capture volume max/min\n");
		return -EINVAL;
	} else if (opts->p_volume_res <= 0) {
		dev_err(dev, "Error: negative/zero playback volume resolution\n");
		return -EINVAL;
	} else if (opts->c_volume_res <= 0) {
		dev_err(dev, "Error: negative/zero capture volume resolution\n");
		return -EINVAL;
	}

	if ((opts->p_volume_max - opts->p_volume_min) % opts->p_volume_res) {
		dev_err(dev, "Error: incorrect playback volume resolution\n");
		return -EINVAL;
	} else if ((opts->c_volume_max - opts->c_volume_min) % opts->c_volume_res) {
		dev_err(dev, "Error: incorrect capture volume resolution\n");
		return -EINVAL;
	}

	return 0;
}

/*-------------------------------------------------------------------------*/

/*
 * Configfs alt mode handling
 */

static void init_alt_0_opts(struct f_uac1_alt_0_opts *alt_0_opts,
			    struct f_uac1_opts *opts, int playback)
{
	alt_0_opts->c.opts = opts;
	alt_0_opts->c.alt_num = 0;

	// Note: Strings are from the host perspective, opt prefixes are from the device perspective
	scnprintf(alt_0_opts->name, sizeof(alt_0_opts->name),
		  (!playback) ? "Playback Inactive" : "Capture Inactive");
}

static void init_alt_opts(struct f_uac1_alt_opts *alt_opts, struct f_uac1_opts *opts,
			  int alt_num, int playback)
{
	alt_opts->c.opts = opts;
	alt_opts->c.alt_num = alt_num;

	INIT_LIST_HEAD(&alt_opts->list);

	// Note: Strings are from the host perspective, opt prefixes are from the device perspective
	scnprintf(alt_opts->name, sizeof(alt_opts->name),
		  (!playback) ? "Playback Active" : "Capture Active");
	strscpy(alt_opts->it_name, (playback) ? opts->p_it_name : opts->c_it_name,
		sizeof(alt_opts->it_name));
	strscpy(alt_opts->it_ch_name, (playback) ? opts->p_it_ch_name : opts->c_it_ch_name,
		sizeof(alt_opts->it_ch_name));
	strscpy(alt_opts->ot_name, (playback) ? opts->p_ot_name : opts->c_ot_name,
		sizeof(alt_opts->ot_name));
	strscpy(alt_opts->fu_vol_name, (playback) ? opts->p_fu_vol_name : opts->c_fu_vol_name,
		sizeof(alt_opts->fu_vol_name));

	/* Copy default options from the main opts */
	alt_opts->chmask = (playback) ? opts->p_chmask : opts->c_chmask;
	alt_opts->ssize = (playback) ? opts->p_ssize : opts->c_ssize;
	alt_opts->hs_bint = (playback) ? opts->p_hs_bint : opts->c_hs_bint;
	alt_opts->srates = (playback) ? opts->p_srates : opts->c_srates;
	alt_opts->sync = (playback) ? 0 : opts->c_sync;
	alt_opts->terminal_type = (playback) ? opts->p_terminal_type : opts->c_terminal_type;
}

static u16 get_max_packet_size(struct f_uac1_alt_opts *alt_opts, struct list_head *list)
{
	u16 max_psize = max_t(u16,
				le16_to_cpu(alt_opts->fs_iso_ep_desc.wMaxPacketSize),
				le16_to_cpu(alt_opts->hs_iso_ep_desc.wMaxPacketSize));
	max_psize = max_t(u16, max_psize,
			le16_to_cpu(alt_opts->ss_iso_ep_desc.wMaxPacketSize));

	list_for_each_entry(alt_opts, list, list) {
		max_psize = max_t(u16, max_psize,
				 le16_to_cpu(alt_opts->fs_iso_ep_desc.wMaxPacketSize));
		max_psize = max_t(u16, max_psize,
				 le16_to_cpu(alt_opts->hs_iso_ep_desc.wMaxPacketSize));
		max_psize = max_t(u16, max_psize,
				 le16_to_cpu(alt_opts->ss_iso_ep_desc.wMaxPacketSize));
	}

	return max_psize;
}

/* audio function driver setup/binding */
static int f_audio_bind(struct usb_configuration *c, struct usb_function *f)
{
	struct usb_composite_dev	*cdev = c->cdev;
	struct usb_gadget		*gadget = cdev->gadget;
	struct device			*dev = &gadget->dev;
	struct f_uac1			*uac1 = func_to_uac1(f);
	struct g_audio			*audio = func_to_g_audio(f);
	struct f_uac1_opts		*audio_opts;
	struct usb_ep			*ep = NULL;
	struct usb_string		*us;
	int				status;
	struct list_head		strings = LIST_HEAD_INIT(strings);

	audio_opts = container_of(f->fi, struct f_uac1_opts, func_inst);

	/* Copy main options to alt modes 0/1 if the groups don't exist
	 * before validation since they will be checked.
	 */
	if (!audio_opts->c_alt_0_opts.c.group.cg_item.ci_name)
		init_alt_0_opts(&audio_opts->c_alt_0_opts, audio_opts, HOST_TO_DEVICE);
	if (!audio_opts->p_alt_0_opts.c.group.cg_item.ci_name)
		init_alt_0_opts(&audio_opts->p_alt_0_opts, audio_opts, DEVICE_TO_HOST);
	if (!audio_opts->c_alt_1_opts.c.group.cg_item.ci_name)
		init_alt_opts(&audio_opts->c_alt_1_opts, audio_opts, 1, HOST_TO_DEVICE);
	if (!audio_opts->p_alt_1_opts.c.group.cg_item.ci_name)
		init_alt_opts(&audio_opts->p_alt_1_opts, audio_opts, 1, DEVICE_TO_HOST);

	status = f_audio_validate_opts(audio, dev);
	if (status)
		return status;

	/* Past this point, all settings that apply to an alt mode should
	 * be used from their alt mode opts.
	 */

	us = attach_strings(cdev, audio_opts);
	if (IS_ERR(us)) {
		status = PTR_ERR(us);
		goto fail;
	}

	ac_interface_desc.iInterface = add_string(us, audio_opts->function_name);

	uac1->p_srate = audio_opts->p_srates[0];
	uac1->c_srate = audio_opts->c_srates[0];

	/* allocate instance-specific interface IDs */
	status = usb_interface_id(c, f);
	if (status < 0)
		goto fail;
	ac_interface_desc.bInterfaceNumber = status;
	uac1->ac_intf = status;
	uac1->ac_alt = 0;

	if (epout_en_any(audio_opts)) {
		status = usb_interface_id(c, f);
		if (status < 0)
			goto fail;
		uac1->as_out_intf = status;
		uac1->as_out_alt = 0;
	}

	if (epin_en_any(audio_opts)) {
		status = usb_interface_id(c, f);
		if (status < 0)
			goto fail;
		uac1->as_in_intf = status;
		uac1->as_in_alt = 0;
	}

	audio->gadget = gadget;

	status = -ENODEV;

	ac_interface_desc.bNumEndpoints = 0;

	/* allocate AC interrupt endpoint */
	if (FUOUT_EN(audio_opts) || FUIN_EN(audio_opts)) {
		ep = usb_ep_autoconfig(cdev->gadget, &fs_ac_int_ep_desc);
		if (!ep) {
			status = -EINVAL;
			dev_err(dev, "Failed to allocate interrupt endpoint\n");
			goto fail;
		}
		hs_ac_int_ep_desc.bEndpointAddress = fs_ac_int_ep_desc.bEndpointAddress;
		ss_ac_int_ep_desc.bEndpointAddress = fs_ac_int_ep_desc.bEndpointAddress;

		uac1->int_ep = ep;

		ac_interface_desc.bNumEndpoints = 1;
	}

	/* Allocate instance-specific endpoints. These use the FS version for alt mode 1.
	 * All other alt modes and speeds will be initialized to the same endpoint address
	 * during the setup_descriptor() call. The u_audio code will update the currently
	 * selected endpoint descriptor when the alt mode changes.
	 */
	if (epout_en_any(audio_opts)) {
		status = init_isoc_ep_descriptor(dev, &audio_opts->c_alt_1_opts.fs_iso_ep_desc,
						 &audio_opts->c_alt_1_opts, HOST_TO_DEVICE,
						 USB_SPEED_FULL, USB_DIR_OUT);
		if (status) {
			dev_err(dev, "Failed to init FS isoc ep descriptor for capture (%d)\n",
				status);
			goto fail;
		}

		ep = usb_ep_autoconfig(cdev->gadget, &audio_opts->c_alt_1_opts.fs_iso_ep_desc);
		if (!ep) {
			status = -EINVAL;
			dev_err(dev, "Failed to allocate isoc endpoint for capture\n");
			goto fail;
		}
		audio->out_ep = ep;

		if (epout_fback_in_en_any(audio_opts)) {
			ep = usb_ep_autoconfig(cdev->gadget, &fs_as_in_fback_desc);
			if (!ep) {
				status = -EINVAL;
				dev_err(dev, "Failed to allocate feedback endpoint for capture\n");
				goto fail;
			}
			hs_as_in_fback_desc.bEndpointAddress = fs_as_in_fback_desc.bEndpointAddress;
			ss_as_in_fback_desc.bEndpointAddress = fs_as_in_fback_desc.bEndpointAddress;

			audio->in_ep_fback = ep;
		}
	}

	if (epin_en_any(audio_opts)) {
		status = init_isoc_ep_descriptor(dev, &audio_opts->p_alt_1_opts.fs_iso_ep_desc,
						 &audio_opts->p_alt_1_opts, HOST_TO_DEVICE,
						 USB_SPEED_FULL, USB_DIR_IN);
		if (status) {
			dev_err(dev, "Failed to init FS isoc ep descriptor for playback (%d)\n",
				status);
			goto fail;
		}

		ep = usb_ep_autoconfig(cdev->gadget, &audio_opts->p_alt_1_opts.fs_iso_ep_desc);
		if (!ep) {
			status = -EINVAL;
			dev_err(dev, "Failed to allocate isoc endpoint for playback\n");
			goto fail;
		}
		audio->in_ep = ep;
	}

	status = setup_descriptor(dev, uac1, audio_opts, us);

	if (status)
		goto fail;

	// Set max packet size for all alt modes. These are used to allocate the buffers in u_audio.
	audio->out_ep_maxpsize = get_max_packet_size(&audio_opts->c_alt_1_opts,
						     &audio_opts->c_alt_opts);
	audio->in_ep_maxpsize = get_max_packet_size(&audio_opts->p_alt_1_opts,
						    &audio_opts->p_alt_opts);

	// TODO: This may need some change with the audio params for the current alt mode
	audio->params.c_chmask = audio_opts->c_chmask;
	memcpy(audio->params.c_srates, audio_opts->c_srates,
			sizeof(audio->params.c_srates));
	audio->params.c_ssize = audio_opts->c_ssize;

	if (FUIN_EN(audio_opts)) {
		audio->params.p_fu.id = USB_IN_FU_ID(audio_opts);
		audio->params.p_fu.mute_present = audio_opts->p_mute_present;
		audio->params.p_fu.volume_present =
				audio_opts->p_volume_present;
		audio->params.p_fu.volume_min = audio_opts->p_volume_min;
		audio->params.p_fu.volume_max = audio_opts->p_volume_max;
		audio->params.p_fu.volume_res = audio_opts->p_volume_res;
	}

	// TODO: This may need some change with the audio params for the current alt mode
	audio->params.p_chmask = audio_opts->p_chmask;
	memcpy(audio->params.p_srates, audio_opts->p_srates,
			sizeof(audio->params.p_srates));
	audio->params.p_ssize = audio_opts->p_ssize;

	if (FUOUT_EN(audio_opts)) {
		audio->params.c_fu.id = USB_OUT_FU_ID(audio_opts);
		audio->params.c_fu.mute_present = audio_opts->c_mute_present;
		audio->params.c_fu.volume_present =
				audio_opts->c_volume_present;
		audio->params.c_fu.volume_min = audio_opts->c_volume_min;
		audio->params.c_fu.volume_max = audio_opts->c_volume_max;
		audio->params.c_fu.volume_res = audio_opts->c_volume_res;
	}
	audio->params.req_number = audio_opts->req_number;
	audio->params.fb_max = audio_opts->fb_max;
	if (FUOUT_EN(audio_opts) || FUIN_EN(audio_opts))
		audio->notify = audio_notify;

	status = g_audio_setup(audio, "UAC1_PCM", "UAC1_Gadget");
	if (status)
		goto err_card_register;

	return 0;

err_card_register:
	usb_free_all_descriptors(f);
fail:
	return status;
}

/*-------------------------------------------------------------------------*/

static inline struct f_uac1_opts *to_f_uac1_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_uac1_opts,
			    func_inst.group);
}

static inline struct f_uac1_alt_opts *to_f_uac1_alt_opts(struct config_item *item)
{
	return container_of(to_config_group(item), struct f_uac1_alt_opts,
			    c.group);
}

#define UAC1_ALT_ATTR_TO_OPTS struct f_uac1_alt_opts *alt_opts = to_f_uac1_alt_opts(item)
#define UAC1_ALT_ATTRIBUTE(type, name)					\
	UAC_ATTRIBUTE(f_uac1_alt_opts, UAC1_ALT_ATTR_TO_OPTS, alt_opts,	\
		      alt_opts->c.opts->lock, alt_opts->c.opts->refcnt,	\
		      type, name)

#define UAC1_ALT_ATTRIBUTE_SYNC(name)					\
	UAC_ATTRIBUTE_SYNC(f_uac1_alt_opts, UAC1_ALT_ATTR_TO_OPTS,	\
			   alt_opts, alt_opts->c.opts->lock,		\
			   alt_opts->c.opts->refcnt, name)

#define UAC1_ALT_ATTRIBUTE_STRING(name)					\
	UAC_ATTRIBUTE_STRING(f_uac1_alt_opts, UAC1_ALT_ATTR_TO_OPTS,	\
			     alt_opts, alt_opts->c.opts->lock,		\
			     alt_opts->c.opts->refcnt, name)


UAC1_ALT_ATTRIBUTE_STRING(name);
UAC1_ALT_ATTRIBUTE_STRING(it_name);
UAC1_ALT_ATTRIBUTE_STRING(it_ch_name);
UAC1_ALT_ATTRIBUTE_STRING(ot_name);
UAC1_ALT_ATTRIBUTE_STRING(fu_vol_name);

UAC1_ALT_ATTRIBUTE(u32, ssize);
UAC1_ALT_ATTRIBUTE(u8, hs_bint);
UAC1_ALT_ATTRIBUTE(u32, chmask);
UAC1_ALT_ATTRIBUTE_SYNC(sync);
UAC1_ALT_ATTRIBUTE(s16, terminal_type);

static struct configfs_attribute *f_uac1_alt_0_attrs[] = {
	&f_uac1_alt_opts_attr_name,

	NULL,
};

static const struct config_item_type alt_mode_0_type = {
	.ct_attrs	= f_uac1_alt_0_attrs,
	.ct_owner       = THIS_MODULE,
};

static struct configfs_attribute *f_uac1_alt_attrs_c[] = {
	&f_uac1_alt_opts_attr_name,
	&f_uac1_alt_opts_attr_it_name,
	&f_uac1_alt_opts_attr_it_ch_name,
	&f_uac1_alt_opts_attr_ot_name,
	&f_uac1_alt_opts_attr_fu_vol_name,
	&f_uac1_alt_opts_attr_ssize,
	&f_uac1_alt_opts_attr_hs_bint,
	&f_uac1_alt_opts_attr_chmask,
	&f_uac1_alt_opts_attr_sync,
	&f_uac1_alt_opts_attr_terminal_type,

	NULL,
};

static struct configfs_attribute *f_uac1_alt_attrs_p[] = {
	&f_uac1_alt_opts_attr_name,
	&f_uac1_alt_opts_attr_it_name,
	&f_uac1_alt_opts_attr_it_ch_name,
	&f_uac1_alt_opts_attr_ot_name,
	&f_uac1_alt_opts_attr_fu_vol_name,
	&f_uac1_alt_opts_attr_ssize,
	&f_uac1_alt_opts_attr_hs_bint,
	&f_uac1_alt_opts_attr_chmask,
	&f_uac1_alt_opts_attr_terminal_type,

	NULL,
};

static void f_uac1_alt_attr_release(struct config_item *item)
{
	struct f_uac1_alt_opts *alt_opts = to_f_uac1_alt_opts(item);

	/* Opts 0 and 1 are fixed structures, 2+ are kzalloc'd */
	if (alt_opts->c.alt_num > 1)
		kfree(alt_opts);
}

static struct configfs_item_operations f_uac1_alt_item_ops = {
	.release	= f_uac1_alt_attr_release,
};

static const struct config_item_type alt_mode_c_type = {
	.ct_item_ops	= &f_uac1_alt_item_ops,
	.ct_attrs	= f_uac1_alt_attrs_c,
	.ct_owner	= THIS_MODULE,
};

static const struct config_item_type alt_mode_p_type = {
	.ct_item_ops	= &f_uac1_alt_item_ops,
	.ct_attrs	= f_uac1_alt_attrs_p,
	.ct_owner	= THIS_MODULE,
};

/*-------------------------------------------------------------------------*/

static void f_uac1_attr_release(struct config_item *item)
{
	struct f_uac1_opts *opts = to_f_uac1_opts(item);

	usb_put_function_instance(&opts->func_inst);
}

static struct configfs_item_operations f_uac1_item_ops = {
	.release	= f_uac1_attr_release,
};

#define UAC1_ATTR_TO_OPTS struct f_uac1_opts *opts = to_f_uac1_opts(item)
#define UAC1_ATTRIBUTE(type, name)					\
	UAC_ATTRIBUTE(f_uac1_opts, UAC1_ATTR_TO_OPTS, opts,		\
		      opts->lock, opts->refcnt, type, name)

#define UAC1_ATTRIBUTE_SYNC(name)					\
	UAC_ATTRIBUTE_SYNC(f_uac1_opts, UAC1_ATTR_TO_OPTS, opts,	\
			   opts->lock, opts->refcnt, name)

#define UAC1_RATE_ATTRIBUTE(name)					\
	UAC_RATE_ATTRIBUTE(f_uac1_opts, UAC1_ATTR_TO_OPTS, opts,	\
			   opts->lock, opts->refcnt, name)

#define UAC1_ATTRIBUTE_STRING(name)					\
	UAC_ATTRIBUTE_STRING(f_uac1_opts, UAC1_ATTR_TO_OPTS, opts,	\
			     opts->lock, opts->refcnt, name)

UAC1_ATTRIBUTE(u32, c_chmask);
UAC1_RATE_ATTRIBUTE(c_srate);
UAC1_ATTRIBUTE_SYNC(c_sync);
UAC1_ATTRIBUTE(u32, c_ssize);
UAC1_ATTRIBUTE(u8, c_hs_bint);
UAC1_ATTRIBUTE(u32, p_chmask);
UAC1_RATE_ATTRIBUTE(p_srate);
UAC1_ATTRIBUTE(u32, p_ssize);
UAC1_ATTRIBUTE(u8, p_hs_bint);
UAC1_ATTRIBUTE(u32, req_number);

UAC1_ATTRIBUTE(bool, p_mute_present);
UAC1_ATTRIBUTE(bool, p_volume_present);
UAC1_ATTRIBUTE(s16, p_volume_min);
UAC1_ATTRIBUTE(s16, p_volume_max);
UAC1_ATTRIBUTE(s16, p_volume_res);

UAC1_ATTRIBUTE(bool, c_mute_present);
UAC1_ATTRIBUTE(bool, c_volume_present);
UAC1_ATTRIBUTE(s16, c_volume_min);
UAC1_ATTRIBUTE(s16, c_volume_max);
UAC1_ATTRIBUTE(s16, c_volume_res);

UAC1_ATTRIBUTE(u32, fb_max);

UAC1_ATTRIBUTE_STRING(function_name);

UAC1_ATTRIBUTE_STRING(p_it_name);
UAC1_ATTRIBUTE_STRING(p_it_ch_name);
UAC1_ATTRIBUTE_STRING(p_ot_name);
UAC1_ATTRIBUTE_STRING(p_fu_vol_name);

UAC1_ATTRIBUTE_STRING(c_it_name);
UAC1_ATTRIBUTE_STRING(c_it_ch_name);
UAC1_ATTRIBUTE_STRING(c_ot_name);
UAC1_ATTRIBUTE_STRING(c_fu_vol_name);

UAC1_ATTRIBUTE(s16, p_terminal_type);
UAC1_ATTRIBUTE(s16, c_terminal_type);

static struct configfs_attribute *f_uac1_attrs[] = {
	&f_uac1_opts_attr_c_chmask,
	&f_uac1_opts_attr_c_srate,
	&f_uac1_opts_attr_c_sync,
	&f_uac1_opts_attr_c_ssize,
	&f_uac1_opts_attr_c_hs_bint,
	&f_uac1_opts_attr_p_chmask,
	&f_uac1_opts_attr_p_srate,
	&f_uac1_opts_attr_p_ssize,
	&f_uac1_opts_attr_p_hs_bint,
	&f_uac1_opts_attr_req_number,
	&f_uac1_opts_attr_fb_max,

	&f_uac1_opts_attr_p_mute_present,
	&f_uac1_opts_attr_p_volume_present,
	&f_uac1_opts_attr_p_volume_min,
	&f_uac1_opts_attr_p_volume_max,
	&f_uac1_opts_attr_p_volume_res,

	&f_uac1_opts_attr_c_mute_present,
	&f_uac1_opts_attr_c_volume_present,
	&f_uac1_opts_attr_c_volume_min,
	&f_uac1_opts_attr_c_volume_max,
	&f_uac1_opts_attr_c_volume_res,

	&f_uac1_opts_attr_function_name,

	&f_uac1_opts_attr_p_it_name,
	&f_uac1_opts_attr_p_it_ch_name,
	&f_uac1_opts_attr_p_ot_name,
	&f_uac1_opts_attr_p_fu_vol_name,

	&f_uac1_opts_attr_c_it_name,
	&f_uac1_opts_attr_c_it_ch_name,
	&f_uac1_opts_attr_c_ot_name,
	&f_uac1_opts_attr_c_fu_vol_name,

	&f_uac1_opts_attr_p_terminal_type,
	&f_uac1_opts_attr_c_terminal_type,

	NULL,
};

static struct config_group *f_uac1_group_make(
		struct config_group *group,
		const char *name)
{
	struct f_uac1_opts *opts = to_f_uac1_opts(&group->cg_item);
	struct f_uac1_alt_opts *alt_opts;
	struct f_uac1_alt_opts *pos;
	struct config_group *ret;
	unsigned int alt_num;
	int playback = 0;

	mutex_lock(&opts->lock);
	if (opts->refcnt) {
		ret = ERR_PTR(-EBUSY);
		goto end;
	}

	if (sscanf(name, "c_alt.%u", &alt_num) != 1) {
		playback = 1;
		if (sscanf(name, "p_alt.%u", &alt_num) != 1) {
			ret = ERR_PTR(-EINVAL);
			goto end;
		}
	}

	if (alt_num > 255) {
		ret = ERR_PTR(-EINVAL);
		goto end;
	}

	/* Alt mode 0 has less properties */
	if (alt_num == 0) {
		struct f_uac1_alt_0_opts *alt_0_opts = (playback) ? &opts->p_alt_0_opts
								  : &opts->c_alt_0_opts;
		init_alt_0_opts(alt_0_opts, opts, playback);
		config_group_init_type_name(&alt_0_opts->c.group, name, &alt_mode_0_type);
		ret = &alt_0_opts->c.group;
		goto end;
	}

	if (alt_num == 1) {
		/* Alt mode 1 always exists */
		alt_opts = (playback) ? &opts->p_alt_1_opts : &opts->c_alt_1_opts;
	} else {
		/* Allocate a structure for alt mode 2+ */
		alt_opts = kzalloc(sizeof(*alt_opts), GFP_KERNEL);
		if (!alt_opts) {
			ret = ERR_PTR(-ENOMEM);
			goto end;
		}
	}

	ret = &alt_opts->c.group;

	config_group_init_type_name(&alt_opts->c.group, name, (playback) ? &alt_mode_p_type
									 : &alt_mode_c_type);

	init_alt_opts(alt_opts, opts, alt_num, playback);

	/* Alt mode 1 doesn't go in the list. It is handled separately to
	 * also handle the case where the alt.1 group is not created.
	 */
	if (alt_num == 1)
		goto end;

	/* Insert the new alt mode in the list, sorted by alt_num */
	list_for_each_entry(pos, (playback) ? &opts->p_alt_opts : &opts->c_alt_opts, list) {
		if (alt_opts->c.alt_num < pos->c.alt_num) {
			list_add_tail(&alt_opts->list, &pos->list);
			goto end;
		}
	}

	list_add_tail(&alt_opts->list, (playback) ? &opts->p_alt_opts : &opts->c_alt_opts);

end:
	mutex_unlock(&opts->lock);

	return ret;
}

static void f_uac1_group_drop(struct config_group *group, struct config_item *item)
{
	struct f_uac1_alt_opts *alt_opts = to_f_uac1_alt_opts(item);

	/* Alt modes 0 and 1 are preallocated and not included in the list */
	if (alt_opts->c.alt_num > 1) {
		mutex_lock(&alt_opts->c.opts->lock);
		list_del(&alt_opts->list);
		mutex_unlock(&alt_opts->c.opts->lock);
	}

	config_item_put(item);
}

static struct configfs_group_operations f_uac1_group_ops = {
	.make_group     = &f_uac1_group_make,
	.drop_item      = &f_uac1_group_drop,
};

static const struct config_item_type f_uac1_func_type = {
	.ct_item_ops	= &f_uac1_item_ops,
	.ct_group_ops   = &f_uac1_group_ops,
	.ct_attrs	= f_uac1_attrs,
	.ct_owner	= THIS_MODULE,
};

static void f_audio_free_inst(struct usb_function_instance *f)
{
	struct f_uac1_opts *opts;

	opts = container_of(f, struct f_uac1_opts, func_inst);
	kfree(opts);
}

static struct usb_function_instance *f_audio_alloc_inst(void)
{
	struct f_uac1_opts *opts;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return ERR_PTR(-ENOMEM);

	mutex_init(&opts->lock);
	opts->func_inst.free_func_inst = f_audio_free_inst;

	INIT_LIST_HEAD(&opts->c_alt_opts);
	INIT_LIST_HEAD(&opts->p_alt_opts);

	config_group_init_type_name(&opts->func_inst.group, "",
				    &f_uac1_func_type);

	opts->c_chmask = UAC1_DEF_CCHMASK;
	opts->c_srates[0] = UAC1_DEF_CSRATE;
	opts->c_sync = UAC1_DEF_CSYNC;
	opts->c_ssize = UAC1_DEF_CSSIZE;
	opts->c_hs_bint = UAC1_DEF_CHSBINT;
	opts->p_chmask = UAC1_DEF_PCHMASK;
	opts->p_srates[0] = UAC1_DEF_PSRATE;
	opts->p_ssize = UAC1_DEF_PSSIZE;
	opts->p_hs_bint = UAC1_DEF_PHSBINT;

	opts->p_mute_present = UAC1_DEF_MUTE_PRESENT;
	opts->p_volume_present = UAC1_DEF_VOLUME_PRESENT;
	opts->p_volume_min = UAC1_DEF_MIN_DB;
	opts->p_volume_max = UAC1_DEF_MAX_DB;
	opts->p_volume_res = UAC1_DEF_RES_DB;

	opts->c_mute_present = UAC1_DEF_MUTE_PRESENT;
	opts->c_volume_present = UAC1_DEF_VOLUME_PRESENT;
	opts->c_volume_min = UAC1_DEF_MIN_DB;
	opts->c_volume_max = UAC1_DEF_MAX_DB;
	opts->c_volume_res = UAC1_DEF_RES_DB;

	opts->req_number = UAC1_DEF_REQ_NUM;
	opts->fb_max = FBACK_FAST_MAX;

	scnprintf(opts->function_name, sizeof(opts->function_name), "AC Interface");

	// Note: Strings are from the host perspective, opt prefixes are from the device perspective
	scnprintf(opts->p_it_name, sizeof(opts->p_it_name), "Capture Input terminal");
	scnprintf(opts->p_it_ch_name, sizeof(opts->p_it_ch_name), "Capture Channels");
	scnprintf(opts->p_ot_name, sizeof(opts->p_ot_name), "Capture Output terminal");
	scnprintf(opts->p_fu_vol_name, sizeof(opts->p_fu_vol_name), "Capture Volume");

	scnprintf(opts->c_it_name, sizeof(opts->c_it_name), "Playback Input terminal");
	scnprintf(opts->c_it_ch_name, sizeof(opts->c_it_ch_name), "Playback Channels");
	scnprintf(opts->c_ot_name, sizeof(opts->c_ot_name), "Playback Output terminal");
	scnprintf(opts->c_fu_vol_name, sizeof(opts->c_fu_vol_name), "Playback Volume");

	opts->p_terminal_type = UAC1_DEF_P_TERM_TYPE;
	opts->c_terminal_type = UAC1_DEF_C_TERM_TYPE;

	return &opts->func_inst;
}

static void f_audio_free(struct usb_function *f)
{
	struct g_audio *audio;
	struct f_uac1_opts *opts;

	audio = func_to_g_audio(f);
	opts = container_of(f->fi, struct f_uac1_opts, func_inst);
	kfree(audio);
	mutex_lock(&opts->lock);
	--opts->refcnt;
	mutex_unlock(&opts->lock);
}

static void f_audio_unbind(struct usb_configuration *c, struct usb_function *f)
{
	struct g_audio *audio = func_to_g_audio(f);

	g_audio_cleanup(audio);
	usb_free_all_descriptors(f);

	kfree(ac_header_desc);
	ac_header_desc = NULL;

	audio->gadget = NULL;
}

static struct usb_function *f_audio_alloc(struct usb_function_instance *fi)
{
	struct f_uac1 *uac1;
	struct f_uac1_opts *opts;

	/* allocate and initialize one new instance */
	uac1 = kzalloc(sizeof(*uac1), GFP_KERNEL);
	if (!uac1)
		return ERR_PTR(-ENOMEM);

	opts = container_of(fi, struct f_uac1_opts, func_inst);
	mutex_lock(&opts->lock);
	++opts->refcnt;
	mutex_unlock(&opts->lock);

	uac1->g_audio.func.name = "uac1_func";
	uac1->g_audio.func.bind = f_audio_bind;
	uac1->g_audio.func.unbind = f_audio_unbind;
	uac1->g_audio.func.set_alt = f_audio_set_alt;
	uac1->g_audio.func.get_alt = f_audio_get_alt;
	uac1->g_audio.func.setup = f_audio_setup;
	uac1->g_audio.func.disable = f_audio_disable;
	uac1->g_audio.func.suspend = f_audio_suspend;
	uac1->g_audio.func.free_func = f_audio_free;

	return &uac1->g_audio.func;
}

DECLARE_USB_FUNCTION_INIT(uac1, f_audio_alloc_inst, f_audio_alloc);
MODULE_DESCRIPTION("USB Audio Class 1.0 Function (using u_audio API)");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ruslan Bilovol");
