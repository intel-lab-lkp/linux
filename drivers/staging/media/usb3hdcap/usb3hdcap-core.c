// SPDX-License-Identifier: GPL-2.0
/*
 * StarTech USB3HDCAP USB 3.0 HD Video Capture Driver
 * a.k.a. YUAN High-Tech Development Co., Ltd UB530
 *
 * Supported inputs are composite, S-Video, component, and HDMI. I haven't
 * attempted to support DVI or VGA.
 *
 * Tested video modes (all at 60fps):
 *     composite 240p and 480i
 *     S-Video 240p and 480i
 *     component 240p, 480i, 480p, 720p, 1080i
 *     HDMI 1080p
 *
 * Other video modes will probably "just work" if the appropriate entries
 * are added in the mode tables and code is added to disambiguate using
 * htotal if necessary.
 *
 * Audio is supported and should work with all inputs.
 *
 * Also partially supports the Micomsoft XCAPTURE-1 for composite and
 * S-Video only... HDMI is unusable, component is untested.
 *
 * Based on USBPcap analysis and reverse engineering of CY3014.X64.SYS
 *   (version 1.1.0.193 of Fri, 04 Jun 2021 09:37:45 UTC)
 * MST3367 HDMI path based on hdcapm
 * Driver structure based heavily on usbtv
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <media/v4l2-common.h>
#include <linux/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "usb3hdcap.h"

#define USB3HDCAP_VID 0x1164
#define USB3HDCAP_PID 0xf533
#define XCAPTURE1_PID 0xf531

/* ------------------------------------------------------------------ */
/* USB control transfer helpers                                       */
/* ------------------------------------------------------------------ */

int vendor_out(struct usb3hdcap *hdcap, u8 request, u16 value, u16 index,
	       u8 *data, u16 len)
{
	int ret;
	u8 *buf = NULL;

	if (len > 0) {
		buf = kmemdup(data, len, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
	}

	ret = usb_control_msg(hdcap->usb_dev,
			      usb_sndctrlpipe(hdcap->usb_dev, 0),
			      request,
			      USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      value, index, buf, len, 1000);

	kfree(buf);

	if (ret < 0) {
		dev_err(hdcap->dev, "%s 0x%02x val=0x%04x idx=0x%04x: %d\n",
			__func__, request, value, index, ret);
		return ret;
	}

	return 0;
}

static int vendor_in(struct usb3hdcap *hdcap, u8 request, u16 value, u16 index,
		     u8 *data, u16 len)
{
	u8 *buf;
	int ret;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = usb_control_msg(hdcap->usb_dev,
			      usb_rcvctrlpipe(hdcap->usb_dev, 0),
			      request,
			      USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      value, index, buf, len, 1000);

	if (ret >= 0)
		memcpy(data, buf, min_t(int, ret, len));

	kfree(buf);
	return ret;
}

/* ------------------------------------------------------------------ */
/* MCU I2C tunnel (XCAPTURE-1)                                        */
/*                                                                    */
/* The Nuvoton NUC100 MCU intercepts I2C traffic. Transactions are    */
/* tunneled through a different USB vendor protocol:                  */
/*   Write: bRequest=0xC0, wValue=0x5066 or addr|0x5000               */
/*   Read:  two-phase write+read, always through wValue=0x5066        */
/*                                                                    */
/* Some addresses need the 0x66 "gateway" (target addr in payload),   */
/* others use the I2C addr directly in wValue with 0x5000 flag.       */
/* ------------------------------------------------------------------ */

static bool mcu_use_gateway(u8 addr)
{
	/* Matches Windows driver address routing logic */
	return ((u8)(addr + 0x78) & 0xeb) || addr == 0x8c;
}

static int mcu_i2c_write(struct usb3hdcap *hdcap, u8 addr, u8 reg, u8 val)
{
	u8 buf[3];

	if (mcu_use_gateway(addr)) {
		buf[0] = addr;
		buf[1] = reg;
		buf[2] = val;
		return vendor_out(hdcap, REQ_I2C, 0x5066, 0, buf, 3);
	}

	buf[0] = reg;
	buf[1] = val;
	return vendor_out(hdcap, REQ_I2C, addr | 0x5000, 0, buf, 2);
}

static int mcu_i2c_read(struct usb3hdcap *hdcap, u8 addr, u8 reg)
{
	u8 tx[3] = { addr | 1, 1, reg };
	u8 val;
	int ret;

	/* send target addr, rx length, and register */
	ret = vendor_out(hdcap, REQ_I2C, 0x5066, 0, tx, 3);
	if (ret < 0)
		return ret;

	/* Retrieve the result */
	ret = vendor_in(hdcap, REQ_I2C, 0x5066, 0, &val, 1);
	if (ret < 0) {
		dev_err(hdcap->dev, "%s 0x%02x reg 0x%02x: %d\n",
			__func__, addr, reg, ret);
		return ret;
	}

	return val;
}

int u3hc_i2c_write(struct usb3hdcap *hdcap, u8 addr, u8 reg, u8 val)
{
	if (hdcap->has_mcu)
		return mcu_i2c_write(hdcap, addr, reg, val);
	return vendor_out(hdcap, REQ_I2C, addr, reg, &val, 1);
}

int u3hc_i2c_read(struct usb3hdcap *hdcap, u8 addr, u8 reg)
{
	u8 val;
	int ret;

	if (hdcap->has_mcu)
		return mcu_i2c_read(hdcap, addr, reg);

	ret = vendor_in(hdcap, REQ_I2C, addr, reg, &val, 1);
	if (ret < 0) {
		dev_err(hdcap->dev, "%s 0x%02x reg 0x%02x: %d\n",
			__func__, addr, reg, ret);
		return ret;
	}

	return val;
}

int u3hc_i2c_rmw(struct usb3hdcap *hdcap, u8 addr, u8 reg, u8 mask, u8 bits)
{
	int val = u3hc_i2c_read(hdcap, addr, reg);

	if (val < 0)
		return val;
	return u3hc_i2c_write(hdcap, addr, reg, (val & mask) | bits);
}

int u3hc_i2c_rmw_get_old(struct usb3hdcap *hdcap, u8 addr, u8 reg, u8 mask, u8 bits, u8 *old)
{
	int val = u3hc_i2c_read(hdcap, addr, reg);

	if (val < 0)
		return val;
	*old = val;
	return u3hc_i2c_write(hdcap, addr, reg, (val & mask) | bits);
}

/* CheckIsMCUExist() */
static void usb3hdcap_probe_mcu(struct usb3hdcap *hdcap)
{
	u8 tx[5] = { 0xab, 0x03, 0x12, 0x34, 0x57 };
	u8 rx[3] = {};
	int k, ret;

	hdcap->has_mcu = 0;

	/* Assert CPLD GPIO 0x39 to wake MCU? */
	vendor_out(hdcap, REQ_GPIO, 0xc039, 0, NULL, 0);
	vendor_out(hdcap, REQ_GPIO, 0x4134, 0, NULL, 0);

	/* Poll GPIO 0x39 until MCU signals ready */
	for (k = 0; k < 50; k++) {
		msleep(20);
		ret = vendor_in(hdcap, REQ_GPIO, 0x39, 0, rx, 1);
		if (ret >= 0 && rx[0] == 1)
			break;
	}

	if (k == 50) {
		dev_info(hdcap->dev, "MCU not detected (GPIO timeout)\n");
		return;
	}

	dev_dbg(hdcap->dev, "MCU freed, wait count=%d\n", k);

	/* Send probe: magic {0x12, 0x34, 0x57} to MCU at I2C 0xAA */
	ret = vendor_out(hdcap, REQ_I2C, 0x5066, 0, tx, sizeof(tx));
	if (ret < 0)
		return;

	memset(rx, 0, sizeof(rx));
	ret = vendor_in(hdcap, REQ_I2C, 0x5066, 0, rx, 3);
	if (ret < 0)
		return;

	if (rx[0] == 'Q' && rx[1] == 0x10) {
		hdcap->has_mcu = 1;
		dev_info(hdcap->dev, "Nuvoton NUC100 MCU detected\n");
	} else {
		dev_info(hdcap->dev,
			 "Unrecognized MCU probe response: %02x %02x %02x\n",
			 rx[0], rx[1], rx[2]);
	}
}

/* Device-level init + GPIO pin state? shared between all inputs */
static int usb3hdcap_device_init(struct usb3hdcap *hdcap)
{
	vendor_out(hdcap, REQ_INIT,   0x0000, 0, NULL, 0);
	vendor_out(hdcap, REQ_UNK_C7, 0x0064, 0, NULL, 0);
	/* Windows driver does this but is always fails? */
	/* vendor_out(hdcap, 0xc5, 0x0000, 0, NULL, 0); */
	vendor_out(hdcap, REQ_STREAM, 0x0000, 0, NULL, 0);
	vendor_out(hdcap, REQ_STREAM, 0x0000, 0, NULL, 0);

	/*
	 * just replay what the windows driver does:
	 *	FUN_14022e3f4(param_1,0x402d);
	 *	FUN_14022e3f4(param_1,0x12d);
	 *	FUN_140001220(0x32);
	 *	FUN_14022e3f4(param_1,0x2d);
	 *	FUN_140001220(0x32);
	 *	FUN_14022e3f4(param_1,0x12d);
	 *	FUN_140001220(0x32);
	 *	FUN_14022e3f4(param_1,0x802d);
	 */

	vendor_out(hdcap, REQ_GPIO, 0x402d, 0, NULL, 0);
	vendor_out(hdcap, REQ_GPIO, 0x012d, 0, NULL, 0);
	msleep(50);
	vendor_out(hdcap, REQ_GPIO, 0x002d, 0, NULL, 0);
	msleep(50);
	vendor_out(hdcap, REQ_GPIO, 0x012d, 0, NULL, 0);
	msleep(50);
	vendor_out(hdcap, REQ_GPIO, 0x802d, 0, NULL, 0);

	/* Probe for MCU after GPIO reset (XCAPTURE-1 only) */
	if (!hdcap->has_mcu &&
	    le16_to_cpu(hdcap->usb_dev->descriptor.idProduct) == XCAPTURE1_PID)
		usb3hdcap_probe_mcu(hdcap);

	return 0;
}

int usb3hdcap_hw_init(struct usb3hdcap *hdcap)
{
	/* Clear stale detection state from previous input */
	hdcap->std = 0;
	hdcap->requested_std = 0;
	hdcap->detected_timings_present = 0;
	hdcap->requested_timings_present = 0;

	usb3hdcap_device_init(hdcap);
	usb3hdcap_cs53l21_init(hdcap);

	switch (hdcap->input) {
	case INPUT_HDMI:
		return usb3hdcap_hdmi_init(hdcap);
	case INPUT_COMPONENT:
		return usb3hdcap_component_init(hdcap);
	case INPUT_COMPOSITE:
	case INPUT_SVIDEO:
	default:
		return usb3hdcap_composite_init(hdcap);
	}
}

/* ------------------------------------------------------------------ */
/* V4L2 ioctls                                                        */
/* ------------------------------------------------------------------ */

static int usb3hdcap_querycap(struct file *file, void *priv,
			      struct v4l2_capability *cap)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	strscpy(cap->driver, "usb3hdcap", sizeof(cap->driver));
	strscpy(cap->card, hdcap->has_mcu ?
		"Micomsoft XCAPTURE-1" : "StarTech USB3HDCAP",
		sizeof(cap->card));
	usb_make_path(hdcap->usb_dev, cap->bus_info, sizeof(cap->bus_info));
	return 0;
}

static int usb3hdcap_enum_input(struct file *file, void *priv,
				struct v4l2_input *i)
{
	if (i->index > INPUT_HDMI)
		return -EINVAL;

	i->type = V4L2_INPUT_TYPE_CAMERA;
	switch (i->index) {
	case INPUT_COMPOSITE:
		strscpy(i->name, "Composite", sizeof(i->name));
		i->std = USB3HDCAP_V4L2_STDS;
		i->capabilities = V4L2_IN_CAP_STD;
		break;
	case INPUT_SVIDEO:
		strscpy(i->name, "S-Video", sizeof(i->name));
		i->std = USB3HDCAP_V4L2_STDS;
		i->capabilities = V4L2_IN_CAP_STD;
		break;
	case INPUT_COMPONENT:
		strscpy(i->name, "Component", sizeof(i->name));
		i->std = 0;
		i->capabilities = V4L2_IN_CAP_DV_TIMINGS;
		break;
	case INPUT_HDMI:
		strscpy(i->name, "HDMI", sizeof(i->name));
		i->std = 0;
		i->capabilities = V4L2_IN_CAP_DV_TIMINGS;
		break;
	}
	return 0;
}

static int usb3hdcap_enum_fmt_vid_cap(struct file *file, void  *priv,
				      struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int usb3hdcap_fmt_vid_cap(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	int width = hdcap->width;
	int height = hdcap->height;
	int interlaced = hdcap->interlaced;

	/*
	 * If timings were explicitly set, report format matching those
	 * timings instead of what was detected from the signal
	 */
	if (hdcap->requested_timings_present) {
		const struct v4l2_bt_timings *bt = &hdcap->requested_timings.bt;

		width = bt->width;
		height = bt->interlaced ? bt->height / 2 : bt->height;
		interlaced = bt->interlaced;
	}

	f->fmt.pix.width = width;
	f->fmt.pix.height = height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	f->fmt.pix.field = interlaced ? V4L2_FIELD_ALTERNATE : V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = width * 2;
	f->fmt.pix.sizeimage = width * 2 * height;
	f->fmt.pix.colorspace = (hdcap->input == INPUT_HDMI) ?
		V4L2_COLORSPACE_REC709 : V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int usb3hdcap_g_std(struct file *file, void *priv, v4l2_std_id *norm)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	if (!(hdcap->input == INPUT_COMPOSITE || hdcap->input == INPUT_SVIDEO))
		return -ENODATA;

	/* try what was explicitly set, if unavailable, use detected */
	if (hdcap->requested_std)
		*norm = hdcap->requested_std;
	else if (hdcap->std)
		*norm = hdcap->std;
	else
		*norm = V4L2_STD_NTSC;
	return 0;
}

static int usb3hdcap_s_std(struct file *file, void *priv, v4l2_std_id norm)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	if (!(hdcap->input == INPUT_COMPOSITE || hdcap->input == INPUT_SVIDEO))
		return -ENODATA;

	if (!(norm & USB3HDCAP_V4L2_STDS))
		return -EINVAL;

	hdcap->requested_std = norm;
	return 0;
}

/* ------------------------------------------------------------------ */
/* DV timings (HDMI + HD component)                                   */
/* ------------------------------------------------------------------ */

static const struct v4l2_dv_timings_cap usb3hdcap_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.bt = {
		.min_width = 720,
		.max_width = 1920,
		.min_height = 480,
		.max_height = 1080,
		.min_pixelclock = 13500000, /* 480i */
		.max_pixelclock = 148500000, /* 1080p60 */
		.standards = V4L2_DV_BT_STD_CEA861,
		.capabilities = V4L2_DV_BT_CAP_PROGRESSIVE |
				V4L2_DV_BT_CAP_INTERLACED,
	},
};

const struct v4l2_dv_timings all_supported_dv_timings[] = {
	/* component only */
	V4L2_DV_BT_CEA_720X480I59_94,
	V4L2_DV_BT_CEA_720X576I50,
	V4L2_DV_BT_CEA_1920X1080I60,

	/* hdmi only */
	V4L2_DV_BT_CEA_640X480P59_94,
	V4L2_DV_BT_CEA_1280X720P50,

	/* both */
	V4L2_DV_BT_CEA_720X480P59_94,
	V4L2_DV_BT_CEA_720X576P50,
	V4L2_DV_BT_CEA_1280X720P60,
	V4L2_DV_BT_CEA_1920X1080P60,
};

static int usb3hdcap_g_dv_timings(struct file *file, void *priv,
				  struct v4l2_dv_timings *timings)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	/* composite/S-Video -> no DV timings */
	if (!(hdcap->input == INPUT_COMPONENT || hdcap->input == INPUT_HDMI))
		return -ENODATA;

	/* nothing set with s_dv_timings and nothing detected? */
	if (!(hdcap->requested_timings_present || hdcap->detected_timings_present))
		return -ENODATA;

	if (hdcap->requested_timings_present)
		*timings = hdcap->requested_timings;
	else
		*timings = hdcap->detected_timings;
	return 0;
}

static int usb3hdcap_s_dv_timings(struct file *file, void *priv,
				  struct v4l2_dv_timings *timings)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	if (!(hdcap->input == INPUT_COMPONENT || hdcap->input == INPUT_HDMI))
		return -ENODATA;

	hdcap->requested_timings = *timings;
	hdcap->requested_timings_present = 1;
	return 0;
}

static int usb3hdcap_enum_dv_timings(struct file *file, void *priv,
				     struct v4l2_enum_dv_timings *timings)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	if (timings->index >= ARRAY_SIZE(all_supported_dv_timings))
		return -EINVAL;
	if (!(hdcap->input == INPUT_COMPONENT || hdcap->input == INPUT_HDMI))
		return -ENODATA;

	timings->timings = all_supported_dv_timings[timings->index];
	return 0;
}

static int usb3hdcap_dv_timings_cap(struct file *file, void *priv,
				    struct v4l2_dv_timings_cap *cap)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	/* if you're not on HDMI/component, DV timings are not supported */
	if (!(hdcap->input == INPUT_COMPONENT || hdcap->input == INPUT_HDMI))
		return -ENODATA;

	*cap = usb3hdcap_timings_cap;
	return 0;
}

static int usb3hdcap_query_dv_timings(struct file *file, void *priv,
				      struct v4l2_dv_timings *timings)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	if (!hdcap->detected_timings_present)
		return -ENODATA;

	*timings = hdcap->detected_timings;
	return 0;
}

static void usb3hdcap_activate_ctrls(struct usb3hdcap *hdcap,
				     enum usb3hdcap_input input)
{
	bool is_tw9900 = input == INPUT_COMPOSITE || input == INPUT_SVIDEO;

	v4l2_ctrl_activate(hdcap->ctrl_brightness, is_tw9900);
	v4l2_ctrl_activate(hdcap->ctrl_contrast, is_tw9900);
	v4l2_ctrl_activate(hdcap->ctrl_saturation, is_tw9900);
	v4l2_ctrl_activate(hdcap->ctrl_hue, is_tw9900);
	v4l2_ctrl_activate(hdcap->ctrl_sharpness, is_tw9900);
}

static int usb3hdcap_g_input(struct file *file, void *priv, unsigned int *i)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	*i = hdcap->input;
	return 0;
}

static int usb3hdcap_s_input(struct file *file, void *priv, unsigned int i)
{
	struct usb3hdcap *hdcap = video_drvdata(file);
	enum usb3hdcap_input old_input = hdcap->input;
	int ret;

	if (i > INPUT_HDMI)
		return -EINVAL;
	if (vb2_is_busy(&hdcap->vb2q))
		return -EBUSY;

	hdcap->input = i;

	usb3hdcap_activate_ctrls(hdcap, i);

	ret = usb3hdcap_hw_init(hdcap);
	if (ret < 0) {
		hdcap->input = old_input;
		hdcap->hw_inited = 0;
		return ret;
	}

	hdcap->bpl = hdcap->width * 2;
	hdcap->hw_inited = 1;

	if (i == INPUT_COMPOSITE || i == INPUT_SVIDEO)
		hdcap->video_dev.tvnorms = USB3HDCAP_V4L2_STDS;
	else
		hdcap->video_dev.tvnorms = 0;

	return 0;
}

static int usb3hdcap_enum_framesizes(struct file *file, void *priv,
				     struct v4l2_frmsizeenum *fsize)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	if (fsize->index > 0)
		return -EINVAL;
	if (fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = hdcap->width;
	fsize->discrete.height = hdcap->height;
	return 0;
}

static void fill_timeperframe(struct usb3hdcap *hdcap, struct v4l2_fract *tf)
{
	/*
	 * using the frame rate here even for interlaced, spec isn't super
	 * clear on what to do, but adv7180.c does it this way
	 */
	if (hdcap->detected_timings.type) {
		const struct v4l2_bt_timings *bt = &hdcap->detected_timings.bt;
		u32 htotal = V4L2_DV_BT_FRAME_WIDTH(bt);
		u32 vtotal = V4L2_DV_BT_FRAME_HEIGHT(bt);

		tf->numerator = htotal * vtotal;
		tf->denominator = (u32)bt->pixelclock;
		v4l2_simplify_fraction(&tf->numerator, &tf->denominator, 8, 333);
	} else if (hdcap->std & V4L2_STD_625_50) {
		/* PAL: 25fps */
		tf->numerator = 1;
		tf->denominator = 25;
	} else {
		/* NTSC: 29.97fps */
		tf->numerator = 1001;
		tf->denominator = 30000;
	}
}

static int usb3hdcap_g_parm(struct file *file, void *priv,
			    struct v4l2_streamparm *sp)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	fill_timeperframe(hdcap, &sp->parm.capture.timeperframe);
	sp->parm.capture.readbuffers = 4;
	return 0;
}

static int usb3hdcap_s_parm(struct file *file, void *priv,
			    struct v4l2_streamparm *sp)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/* fixed framerate - just report what we have */
	sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	fill_timeperframe(hdcap, &sp->parm.capture.timeperframe);
	sp->parm.capture.readbuffers = 4;
	return 0;
}

static int usb3hdcap_enum_frameintervals(struct file *file, void *priv,
					 struct v4l2_frmivalenum *fival)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	if (fival->index > 0)
		return -EINVAL;
	if (fival->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	if (fival->width != hdcap->width || fival->height != hdcap->height)
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fill_timeperframe(hdcap, &fival->discrete);
	return 0;
}

static int usb3hdcap_log_status(struct file *file, void *priv)
{
	struct usb3hdcap *hdcap = video_drvdata(file);

	dev_info(hdcap->dev,
		 "status: iso_cbs=%u iso_bytes=%lu markers=%u frames=%u ",
		 hdcap->iso_cb_count, hdcap->iso_bytes,
		 hdcap->markers_found, hdcap->frames_delivered);
	dev_info(hdcap->dev,
		 "parse_len=%d frame_line=%d synced=%d was_blanking=%d ",
		 hdcap->parse_len, hdcap->frame_line,
		 hdcap->synced, hdcap->was_blanking);
	dev_info(hdcap->dev, "cur_buf=%p\n", hdcap->cur_buf);
	return 0;
}

static int usb3hdcap_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct usb3hdcap *hdcap = container_of(ctrl->handler,
		struct usb3hdcap, ctrl);

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		return u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_BRIGHT, (u8)ctrl->val);
	case V4L2_CID_CONTRAST:
		return u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_CONTRAST, ctrl->val);
	case V4L2_CID_SATURATION:
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_SAT_U, ctrl->val);
		return u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_SAT_V, ctrl->val);
	case V4L2_CID_HUE:
		return u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_HUE, (u8)ctrl->val);
	case V4L2_CID_SHARPNESS:
		return u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_SHARPNESS, ctrl->val);
	default:
		return -EINVAL;
	}
}

static int usb3hdcap_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	switch (ctrl->id) {
	case V4L2_CID_DV_RX_POWER_PRESENT:
		ctrl->val = 1;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops usb3hdcap_ctrl_ops = {
	.s_ctrl = usb3hdcap_s_ctrl,
	.g_volatile_ctrl = usb3hdcap_g_volatile_ctrl,
};

static int usb3hdcap_subscribe_event(struct v4l2_fh *fh,
				     const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	default:
		return v4l2_ctrl_subscribe_event(fh, sub);
	}
}

static const struct v4l2_ioctl_ops usb3hdcap_ioctl_ops = {
	.vidioc_querycap = usb3hdcap_querycap,
	.vidioc_enum_input = usb3hdcap_enum_input,
	.vidioc_enum_fmt_vid_cap = usb3hdcap_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = usb3hdcap_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = usb3hdcap_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = usb3hdcap_fmt_vid_cap,
	.vidioc_g_std = usb3hdcap_g_std,
	.vidioc_s_std = usb3hdcap_s_std,
	.vidioc_g_dv_timings = usb3hdcap_g_dv_timings,
	.vidioc_s_dv_timings = usb3hdcap_s_dv_timings,
	.vidioc_enum_dv_timings = usb3hdcap_enum_dv_timings,
	.vidioc_dv_timings_cap = usb3hdcap_dv_timings_cap,
	.vidioc_query_dv_timings = usb3hdcap_query_dv_timings,
	.vidioc_g_input = usb3hdcap_g_input,
	.vidioc_s_input = usb3hdcap_s_input,
	.vidioc_g_parm = usb3hdcap_g_parm,
	.vidioc_s_parm = usb3hdcap_s_parm,
	.vidioc_enum_framesizes = usb3hdcap_enum_framesizes,
	.vidioc_enum_frameintervals = usb3hdcap_enum_frameintervals,
	.vidioc_log_status = usb3hdcap_log_status,

	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,

	.vidioc_subscribe_event = usb3hdcap_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations usb3hdcap_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
};

static void usb3hdcap_release(struct v4l2_device *v4l2_dev)
{
	struct usb3hdcap *hdcap = container_of(v4l2_dev,
		struct usb3hdcap, v4l2_dev);

	v4l2_device_unregister(&hdcap->v4l2_dev);
	v4l2_ctrl_handler_free(&hdcap->ctrl);
	kfree(hdcap);
}

static int video_init(struct usb3hdcap *hdcap)
{
	int ret;

	mutex_init(&hdcap->v4l2_lock);
	mutex_init(&hdcap->vb2q_lock);
	spin_lock_init(&hdcap->buflock);
	INIT_LIST_HEAD(&hdcap->bufs);

	hdcap->vb2q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	hdcap->vb2q.io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	hdcap->vb2q.drv_priv = hdcap;
	hdcap->vb2q.buf_struct_size = sizeof(struct hdcap_buf);
	hdcap->vb2q.ops = &usb3hdcap_vb2_ops;
	hdcap->vb2q.mem_ops = &vb2_vmalloc_memops;
	hdcap->vb2q.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	hdcap->vb2q.min_queued_buffers = 2;
	hdcap->vb2q.lock = &hdcap->vb2q_lock;
	ret = vb2_queue_init(&hdcap->vb2q);
	if (ret < 0) {
		dev_warn(hdcap->dev, "Could not initialize videobuf2 queue\n");
		return ret;
	}

	v4l2_ctrl_handler_init(&hdcap->ctrl, 7);
	hdcap->ctrl_brightness = v4l2_ctrl_new_std(&hdcap->ctrl,
						   &usb3hdcap_ctrl_ops,
						   V4L2_CID_BRIGHTNESS,
						   -128, 127, 1, -9);
	hdcap->ctrl_contrast = v4l2_ctrl_new_std(&hdcap->ctrl,
						 &usb3hdcap_ctrl_ops,
						 V4L2_CID_CONTRAST,
						 0, 255, 1, 0x79);
	hdcap->ctrl_saturation = v4l2_ctrl_new_std(&hdcap->ctrl,
						   &usb3hdcap_ctrl_ops,
						   V4L2_CID_SATURATION,
						   0, 255, 1, 0x80);
	hdcap->ctrl_hue = v4l2_ctrl_new_std(&hdcap->ctrl,
					    &usb3hdcap_ctrl_ops,
					    V4L2_CID_HUE,
					    -128, 127, 1, 0);
	hdcap->ctrl_sharpness = v4l2_ctrl_new_std(&hdcap->ctrl,
						  &usb3hdcap_ctrl_ops,
						  V4L2_CID_SHARPNESS,
						  0, 255, 1, 0x52);
	hdcap->ctrl_rx_power = v4l2_ctrl_new_std(&hdcap->ctrl,
						 &usb3hdcap_ctrl_ops,
						 V4L2_CID_DV_RX_POWER_PRESENT,
						 0, 1, 0, 1);
	if (hdcap->ctrl_rx_power)
		hdcap->ctrl_rx_power->flags |= V4L2_CTRL_FLAG_VOLATILE |
						V4L2_CTRL_FLAG_READ_ONLY;
	ret = hdcap->ctrl.error;
	if (ret < 0) {
		dev_warn(hdcap->dev, "Could not initialize controls\n");
		goto ctrl_fail;
	}

	hdcap->v4l2_dev.ctrl_handler = &hdcap->ctrl;
	hdcap->v4l2_dev.release = usb3hdcap_release;
	ret = v4l2_device_register(hdcap->dev, &hdcap->v4l2_dev);
	if (ret < 0) {
		dev_warn(hdcap->dev, "Could not register v4l2 device\n");
		goto v4l2_fail;
	}

	strscpy(hdcap->video_dev.name, "usb3hdcap", sizeof(hdcap->video_dev.name));
	hdcap->video_dev.v4l2_dev = &hdcap->v4l2_dev;
	hdcap->video_dev.release = video_device_release_empty;
	hdcap->video_dev.fops = &usb3hdcap_fops;
	hdcap->video_dev.ioctl_ops = &usb3hdcap_ioctl_ops;
	hdcap->video_dev.tvnorms = USB3HDCAP_V4L2_STDS;
	hdcap->video_dev.queue = &hdcap->vb2q;
	hdcap->video_dev.lock = &hdcap->v4l2_lock;
	hdcap->video_dev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
				  V4L2_CAP_STREAMING;
	video_set_drvdata(&hdcap->video_dev, hdcap);
	ret = video_register_device(&hdcap->video_dev, VFL_TYPE_VIDEO, -1);
	if (ret < 0) {
		dev_warn(hdcap->dev, "Could not register video device\n");
		goto vdev_fail;
	}

	dev_info(hdcap->dev, "%s: registered as %s\n",
		 __func__, video_device_node_name(&hdcap->video_dev));

	return 0;

vdev_fail:
	v4l2_device_unregister(&hdcap->v4l2_dev);

v4l2_fail:
	v4l2_ctrl_handler_free(&hdcap->ctrl);

ctrl_fail:
	return ret;
}

static int usb3hdcap_probe(struct usb_interface *intf,
			   const struct usb_device_id *id)
{
	struct device *dev;
	struct usb3hdcap *hdcap;
	int ret, is_xcapture;

	/* not using the interrupt interface, just data */
	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	/* none, Mult=0 for SD, Mult=1 for HD */
	if (intf->num_altsetting != 3)
		return -ENODEV;

	dev = &intf->dev;

	hdcap = kzalloc_obj(*hdcap, GFP_KERNEL);
	if (!hdcap)
		return -ENOMEM;

	hdcap->dev = dev;
	hdcap->usb_dev = usb_get_dev(interface_to_usbdev(intf));
	hdcap->input = INPUT_COMPOSITE;
	hdcap->std = 0;
	hdcap->width = SD_WIDTH;
	hdcap->height = NTSC_HEIGHT / 2;
	hdcap->bpl = DEFAULT_BPL;

	usb_set_intfdata(intf, hdcap);

	ret = video_init(hdcap);
	if (ret < 0)
		goto video_fail;

	ret = usb3hdcap_audio_init(hdcap);
	if (ret < 0)
		dev_warn(dev, "audio init failed: %d (continuing without audio)\n", ret);

	/*
	 * Take an extra ref so that disconnect's v4l2_device_put doesn't
	 * immediately trigger usb3hdcap_release before cleanup is done - based on
	 * usbtv driver
	 */
	v4l2_device_get(&hdcap->v4l2_dev);

	is_xcapture = le16_to_cpu(hdcap->usb_dev->descriptor.idProduct) == XCAPTURE1_PID;
	dev_info(dev, "%s USB 3.0 HD Video Capture Device",
		 is_xcapture ? "Micomsoft XCAPTURE-1" : "StarTech USB3HDCAP");

	return 0;

video_fail:
	usb_set_intfdata(intf, NULL);
	usb_put_dev(hdcap->usb_dev);
	kfree(hdcap);
	return ret;
}

static void usb3hdcap_disconnect(struct usb_interface *intf)
{
	struct usb3hdcap *hdcap = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);

	if (!hdcap)
		return;

	usb3hdcap_audio_free(hdcap);
	vb2_video_unregister_device(&hdcap->video_dev);
	v4l2_device_disconnect(&hdcap->v4l2_dev);

	usb_put_dev(hdcap->usb_dev);
	hdcap->usb_dev = NULL;

	/* hdcap is freed by usb3hdcap_release when last user closes fd */
	v4l2_device_put(&hdcap->v4l2_dev);
}

static const struct usb_device_id usb3hdcap_id_table[] = {
	{ USB_DEVICE(USB3HDCAP_VID, USB3HDCAP_PID) },
	{ USB_DEVICE(USB3HDCAP_VID, XCAPTURE1_PID) },
	{ },
};
MODULE_DEVICE_TABLE(usb, usb3hdcap_id_table);

MODULE_AUTHOR("Matthew Laux <matthew.laux@gmail.com>");
MODULE_DESCRIPTION("StarTech USB3HDCAP Driver");
MODULE_LICENSE("GPL");

static struct usb_driver usb3hdcap_usb_driver = {
	.name = "usb3hdcap",
	.id_table = usb3hdcap_id_table,
	.probe = usb3hdcap_probe,
	.disconnect = usb3hdcap_disconnect,
};
module_usb_driver(usb3hdcap_usb_driver);
