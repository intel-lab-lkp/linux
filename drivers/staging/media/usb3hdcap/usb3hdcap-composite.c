// SPDX-License-Identifier: GPL-2.0
/*
 * USB3HDCAP composite video init - CPLD + TW9900 setup, signal lock
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/usb.h>
#include <media/v4l2-ctrls.h>

#include "usb3hdcap.h"

/* Detect PAL vs NTSC from TW9900 auto-detection result */
static void usb3hdcap_detect_std(struct usb3hdcap *hdcap)
{
	int k, sdt, detected;

	/* 0x80 = start detection */
	/* 0x03 = enable recognition of PAL and NTSC only */
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_SDTR, 0x83);

	for (k = 0; k < 10; k++) {
		sdt = u3hc_i2c_read(hdcap, ADDR_TW9900, TW9900_SDT);
		if (!(sdt & 0x80))
			break;
		msleep(100);
	}

	if (sdt & 0x80) {
		dev_warn(hdcap->dev, "Detection not done after 1s, using NTSC\n");
		hdcap->std = V4L2_STD_NTSC;
		return;
	}

	detected = (sdt & TW9900_STDNOW_MASK) >> TW9900_STDNOW_SHIFT;
	switch (detected) {
	case TW9900_STD_NTSC_M:
		hdcap->std = V4L2_STD_NTSC;
		dev_info(hdcap->dev, "NTSC detected (SDT=0x%02x)\n", sdt);
		break;
	case TW9900_STD_PAL_BDGHI:
		hdcap->std = V4L2_STD_PAL;
		dev_info(hdcap->dev, "PAL detected (SDT=0x%02x)\n", sdt);
		break;
	default: /* not sure if this will be hit, because others are disabled */
		hdcap->std = V4L2_STD_NTSC;
		dev_info(hdcap->dev, "Non-PAL/NTSC detected (SDT=0x%02x)\n", sdt);
		break;
	}

	hdcap->detected_timings_present = 0;
}

/* Detect 240p/288p (non-interlaced) vs 480i/576i */
static void usb3hdcap_detect_size(struct usb3hdcap *hdcap)
{
	int status;
	int full_h = (hdcap->std & V4L2_STD_PAL) ? PAL_HEIGHT : NTSC_HEIGHT;
	int half_h = full_h / 2;

	hdcap->width = SD_WIDTH;
	hdcap->height = half_h;

	status = u3hc_i2c_read(hdcap, ADDR_TW9900, TW9900_CSTATUS_II);
	if (status >= 0 && (status & TW9900_NINTL)) {
		hdcap->interlaced = 0;
		dev_info(hdcap->dev, "NINTL=1: %dp detected (SDTR=0x%02x)\n",
			half_h, status);
	} else {
		hdcap->interlaced = 1;
		dev_info(hdcap->dev, "NINTL=0: %di detected (SDTR=0x%02x)\n",
			full_h, status);
	}
}

int usb3hdcap_composite_init(struct usb3hdcap *hdcap)
{
	int k, status;

	/* disable MST3367 */
	u3hc_i2c_write(hdcap, ADDR_MST3367, 0x00, 0x00); /* bank 0 */
	u3hc_i2c_write(hdcap, ADDR_MST3367, 0x51, 0x80);

	/* disable TW9900 */
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_ACNTL_I, 0x0e);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_ACNTL_II, 0x40);

	/* select TW9900 input of cpld? */
	/* from windows driver */
	status = u3hc_i2c_read(hdcap, ADDR_CPLD, 0x3b);
	if (status != 0)
		u3hc_i2c_write(hdcap, ADDR_CPLD, 0x3b, 0x80);
	u3hc_i2c_write(hdcap, ADDR_CPLD, 0x20, 0x05);
	u3hc_i2c_write(hdcap, ADDR_CPLD, 0x00, 0x01);
	u3hc_i2c_write(hdcap, ADDR_CPLD, 0x10, 0xfe);
	u3hc_i2c_write(hdcap, ADDR_CPLD, 0x20, 0x75);

	/* Main TW9900 configuration */
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_ACNTL_I, 0x00);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_OPFORM, 0xa2);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_OPCNTL_I, 0x01);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VDELAY_LO, 0x14);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VACTIVE_LO, 0xf2);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_HDELAY_LO, 0x0b);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_HACTIVE_LO, 0xd2);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VBICNTL, 0x57);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_ACNTL_II, 0x0f);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_OPCNTL_II, 0x00);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_SDT, 0x07); /* auto-detect */
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VCNTL1, 0x0c);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VCNTL2, 0x03);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_MISC1, 0x07);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_MISC2, 0x06);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_ANAPLLCTL, 0x01);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VVBI, 0x00);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_HSBEGIN, 0x26);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_HSEND, 0x36);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_OVSDLY, 0xf0);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_OVSEND, 0x28);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_ACNTL_I, 0x80);
	if (hdcap->input == INPUT_SVIDEO) {
		/* "Input crystal clock frequency is 27MHz" */
		/* IFSEL 01 for S-Video, YSEL 01 for Y on Mux1 */
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_INFORM, 0x54);
		/* "internal current reference 2" + both luma and chroma ADCs on */
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_ACNTL_I, 0x40);

	} else {
		/* IFSEL 00 for composite, YSEL 00 for Y on Mux0 */
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_INFORM, 0x40);
		/* "internal current reference 2" + chroma ADC off */
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_ACNTL_I, 0x42);
	}
	v4l2_ctrl_handler_setup(&hdcap->ctrl);

	vendor_out(hdcap, REQ_STREAM, 0x0000, 0, NULL, 0);

	for (k = 0; k < 10; k++) {
		status = u3hc_i2c_read(hdcap, ADDR_TW9900, TW9900_CSTATUS);

		/*
		 * 0x40 = "Horizontal sync PLL is locked to the incoming video source"
		 * 0x08 = "Vertical logic is locked to the incoming video source"
		 */
		if (status >= 0 && (status & 0x48) == 0x48) {
			dev_info(hdcap->dev, "Signal locked (%d ms)\n", k * 100);
			break;
		}
		msleep(100);
	}

	if (k == 10) {
		dev_err(hdcap->dev, "No signal lock after 1s (last status=0x%02x)\n",
			status);
		return -ETIMEDOUT;
	}

	usb3hdcap_detect_std(hdcap);
	usb3hdcap_detect_size(hdcap);

	/* Adjust for PAL vs NTSC */
	if (hdcap->std & V4L2_STD_PAL) {
		u3hc_i2c_rmw(hdcap, ADDR_TW9900, TW9900_CROP_HI, 0x0f, 0x10);
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VDELAY_LO, 0x19);
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VACTIVE_LO, 0x20);
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_HDELAY_LO, 0x0a);
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_HACTIVE_LO, 0xd0);
		u3hc_i2c_rmw(hdcap, ADDR_TW9900, TW9900_VVBI, 0xef, 0x00);
	} else {
		u3hc_i2c_rmw(hdcap, ADDR_TW9900, TW9900_CROP_HI, 0x0f, 0x00);
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VDELAY_LO, 0x14);
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_VACTIVE_LO, 0xf2);
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_HDELAY_LO, 0x10);
		u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_HACTIVE_LO, 0xd0);
		u3hc_i2c_rmw(hdcap, ADDR_TW9900, TW9900_VVBI, 0xef, 0x10);
	}
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_CNTRL2, 0x00);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_SDT_NOISE, 0x11);
	u3hc_i2c_write(hdcap, ADDR_TW9900, TW9900_LUMA_CTL, 0x00);

	return 0;
}
