// SPDX-License-Identifier: GPL-2.0
/*
 * V4L2 Support for the OS05E10
 *
 * Copyright (C) 2026 Silicon Signals Pvt. Ltd.
 *
 */

#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <linux/types.h>
#include <linux/time.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>

#define OS05E10_XCLK_FREQ_24MHZ			(24 * HZ_PER_MHZ)

#define OS05E10_REG_CHIPID			CCI_REG32(0x000)
#define OS05E10_CHIPID				0x10450553

#define OS05E10_REG_STREAM_CTRL			CCI_REG8(0x020)
#define OS05E10_STREAM_CTRL_ON			0x1f
#define OS05E10_STREAM_CTRL_OFF			0x27

#define OS05E10_REG_FRAME_SYNC			CCI_REG8(0x101)
#define OS05E10_FRAME_SYNC_TRIGGER		0x01

#define OS05E10_REG_EXPOSURE			CCI_REG24(0x102)
#define OS05E10_EXPOSURE_MIN			1
#define OS05E10_EXPOSURE_STEP			1
#define OS05E10_EXPOSURE_MARGIN			33

#define OS05E10_REG_VBLANK			CCI_REG16(0x105)
#define OS05E10_VBLANK_MAX			0xffff

/* Actual VTS = base VTS + dummy lines. */
#define OS05E10_VTS_BASE			2010

#define OS05E10_REG_DIGITAL_GAIN		CCI_REG16(0x121)
#define OS05E10_DIGITAL_GAIN_MIN		0x40
#define OS05E10_DIGITAL_GAIN_MAX		0x7ff
#define OS05E10_DIGITAL_GAIN_STEP		1
#define OS05E10_DIGITAL_GAIN_DEFAULT		0x40

#define OS05E10_REG_ANALOG_GAIN			CCI_REG16_LE(0x124)
#define OS05E10_ANALOG_GAIN_MIN			0x10
#define OS05E10_ANALOG_GAIN_MAX			0x3e0
#define OS05E10_ANALOG_GAIN_STEP		1
#define OS05E10_ANALOG_GAIN_DEFAULT		0x10

/* Read-only registers */
#define OS05E10_REG_VTS				CCI_REG24(0x134)
#define OS05E10_REG_HTS				CCI_REG16(0x137)

#define OS05E10_LINK_FREQ_720MHZ		(720 * HZ_PER_MHZ)
#define OS05E10_DATA_LANES			4

/* OS05E10 native and active pixel array size */
static const struct v4l2_rect os05e10_native_area = {
	.top = 0,
	.left = 0,
	.width = 2608,
	.height = 1960,
};

static const struct v4l2_rect os05e10_active_area = {
	.top = 8,
	.left = 8,
	.width = 2592,
	.height = 1944,
};

static const char * const os05e10_supply_name[] = {
	"avdd",		/* Analog supply */
	"dovdd",	/* Digital I/O */
	"dvdd",		/* Digital core */
};

struct os05e10 {
	struct device *dev;
	struct regmap *cci;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(os05e10_supply_name)];

	/* V4L2 Controls */
	struct v4l2_ctrl_handler handler;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
};

struct os05e10_mode {
	u32 width;
	u32 height;
	u32 vts;
	u32 exp;
	u32 bpp;
};

static const struct cci_reg_sequence os05e10_common_regs[] = {
	{ CCI_REG8(0x021), 0x16 },
	{ CCI_REG8(0x014), 0x11 },
	{ CCI_REG8(0x013), 0x38 },
	{ CCI_REG8(0x01b), 0x96 },
	{ CCI_REG8(0x01d), 0x10 },
	{ CCI_REG8(0x01e), 0x0c },
	{ CCI_REG8(0x01f), 0xb7 },
	{ CCI_REG8(0x026), 0xf7 },
	{ CCI_REG8(0x027), 0x32 },
	{ CCI_REG8(0x021), 0x12 },
	{ CCI_REG8(0x0ff), 0x32 },
	{ CCI_REG8(0xb10), 0x00 },
	{ CCI_REG8(0xb12), 0x00 },
	{ CCI_REG8(0xb13), 0x0b },
	{ CCI_REG8(0xb14), 0xf2 },
	{ CCI_REG8(0x251), 0x10 },
	{ CCI_REG8(0x021), 0x10 },
	{ CCI_REG8(0xf02), 0x10 },
	{ CCI_REG8(0xf03), 0x03 },
	{ CCI_REG8(0xf13), 0x44 },
	{ CCI_REG8(0xf16), 0x44 },
	{ CCI_REG8(0xf24), 0xf7 },
	{ CCI_REG8(0xf2c), 0xe7 },
	{ CCI_REG8(0xf2a), 0x02 },
	{ CCI_REG8(0xf2e), 0x02 },
	{ CCI_REG8(0xf2f), 0x8c },
	{ CCI_REG8(0xf00), 0x50 },
	{ CCI_REG8(0xf21), 0xed },
	{ CCI_REG8(0xf22), 0xed },
	{ CCI_REG8(0xf23), 0xed },
	{ CCI_REG8(0xf37), 0x6c },
	{ CCI_REG8(0xf2d), 0x02 },
	{ CCI_REG8(0xf3a), 0xdd },
	{ CCI_REG8(0xf32), 0xc2 },
	{ CCI_REG8(0x102), 0x00 },
	{ CCI_REG8(0x103), 0x00 },
	{ CCI_REG8(0x104), 0x04 },
	{ CCI_REG8(0x107), 0x08 },
	{ CCI_REG8(0x124), 0x10 },
	{ CCI_REG8(0x125), 0x00 },
	{ CCI_REG8(0x127), 0x00 },
	{ CCI_REG8(0x1e4), 0x0f },
	{ CCI_REG8(0x130), 0x00 },
	{ CCI_REG8(0x2a9), 0x77 },
	{ CCI_REG8(0x2f7), 0x01 },
	{ CCI_REG8(0x2c3), 0x24 },
	{ CCI_REG8(0x2c4), 0x12 },
	{ CCI_REG8(0x2c5), 0x01 },
	{ CCI_REG8(0x2ca), 0x03 },
	{ CCI_REG8(0x2cb), 0x0c },
	{ CCI_REG8(0x2cc), 0x0f },
	{ CCI_REG8(0x2cd), 0x0f },
	{ CCI_REG8(0x2ce), 0xff },
	{ CCI_REG8(0x2cf), 0xff },
	{ CCI_REG8(0x2d0), 0xff },
	{ CCI_REG8(0x236), 0xc1 },
	{ CCI_REG8(0x202), 0x04 },
	{ CCI_REG8(0x204), 0x0f },
	{ CCI_REG8(0x205), 0x00 },
	{ CCI_REG8(0x206), 0x08 },
	{ CCI_REG8(0x207), 0x70 },
	{ CCI_REG8(0x20a), 0x06 },
	{ CCI_REG8(0x20b), 0x06 },
	{ CCI_REG8(0x20c), 0x0e },
	{ CCI_REG8(0x20d), 0x0e },
	{ CCI_REG8(0x20e), 0x02 },
	{ CCI_REG8(0x20f), 0x06 },
	{ CCI_REG8(0x210), 0x10 },
	{ CCI_REG8(0x211), 0x14 },
	{ CCI_REG8(0x217), 0x00 },
	{ CCI_REG8(0x219), 0x00 },
	{ CCI_REG8(0x21a), 0x0f },
	{ CCI_REG8(0x21b), 0x00 },
	{ CCI_REG8(0x21c), 0x08 },
	{ CCI_REG8(0x21d), 0x70 },
	{ CCI_REG8(0x23b), 0x00 },
	{ CCI_REG8(0x23d), 0x00 },
	{ CCI_REG8(0x23e), 0x0f },
	{ CCI_REG8(0x23f), 0x00 },
	{ CCI_REG8(0x240), 0x08 },
	{ CCI_REG8(0x241), 0x70 },
	{ CCI_REG8(0x52f), 0x08 },
	{ CCI_REG8(0x52e), 0x08 },
	{ CCI_REG8(0xa0c), 0xaa },
	{ CCI_REG8(0xa0d), 0x54 },
	{ CCI_REG8(0xa0f), 0x20 },
	{ CCI_REG8(0xa11), 0x44 },
	{ CCI_REG8(0xa13), 0x43 },
	{ CCI_REG8(0xa14), 0x42 },
	{ CCI_REG8(0xa15), 0x42 },
	{ CCI_REG8(0xa16), 0xc2 },
	{ CCI_REG8(0xa17), 0xc2 },
	{ CCI_REG8(0xa18), 0xc2 },
	{ CCI_REG8(0xa30), 0x08 },
	{ CCI_REG8(0xa31), 0x18 },
	{ CCI_REG8(0xa33), 0x50 },
	{ CCI_REG8(0xa34), 0x31 },
	{ CCI_REG8(0xa3a), 0xc0 },
	{ CCI_REG8(0xa35), 0xc3 },
	{ CCI_REG8(0xa36), 0xf6 },
	{ CCI_REG8(0xa1a), 0x46 },
	{ CCI_REG8(0xa1b), 0x4a },
	{ CCI_REG8(0xa1c), 0x4c },
	{ CCI_REG8(0xa1d), 0x96 },
	{ CCI_REG8(0xa1e), 0x92 },
	{ CCI_REG8(0xa41), 0x46 },
	{ CCI_REG8(0xa42), 0x4a },
	{ CCI_REG8(0xa43), 0x4c },
	{ CCI_REG8(0xa45), 0x92 },
	{ CCI_REG8(0xa50), 0x2f },
	{ CCI_REG8(0xa53), 0x2f },
	{ CCI_REG8(0xa51), 0x18 },
	{ CCI_REG8(0xa52), 0x16 },
	{ CCI_REG8(0xa59), 0x10 },
	{ CCI_REG8(0xa5a), 0x10 },
	{ CCI_REG8(0xab8), 0x00 },
	{ CCI_REG8(0xab9), 0x00 },
	{ CCI_REG8(0xa5c), 0x2d },
	{ CCI_REG8(0xa5d), 0x3a },
	{ CCI_REG8(0xa5e), 0x16 },
	{ CCI_REG8(0xa5f), 0x10 },
	{ CCI_REG8(0xa67), 0x01 },
	{ CCI_REG8(0xa7b), 0x27 },
	{ CCI_REG8(0xa7d), 0x1e },
	{ CCI_REG8(0xa8e), 0x2f },
	{ CCI_REG8(0xa8f), 0x02 },
	{ CCI_REG8(0xa90), 0x60 },
	{ CCI_REG8(0xa91), 0x1a },
	{ CCI_REG8(0xa92), 0x30 },
	{ CCI_REG8(0xa93), 0x42 },
	{ CCI_REG8(0xa95), 0x50 },
	{ CCI_REG8(0xa99), 0x2b },
	{ CCI_REG8(0xa9a), 0x1b },
	{ CCI_REG8(0xaa4), 0x00 },
	{ CCI_REG8(0xaa8), 0x06 },
	{ CCI_REG8(0xab0), 0x2e },
	{ CCI_REG8(0xab6), 0x05 },
	{ CCI_REG8(0xab7), 0x08 },
	{ CCI_REG8(0xad0), 0x4b },
	{ CCI_REG8(0xad1), 0x4b },
	{ CCI_REG8(0xad2), 0x51 },
	{ CCI_REG8(0xad3), 0x53 },
	{ CCI_REG8(0xad4), 0x53 },
	{ CCI_REG8(0xad5), 0x4a },
	{ CCI_REG8(0xad6), 0x4a },
	{ CCI_REG8(0xad7), 0x4f },
	{ CCI_REG8(0xad8), 0x50 },
	{ CCI_REG8(0xad9), 0x50 },
	{ CCI_REG8(0xa8c), 0x50 },
	{ CCI_REG8(0xadf), 0x50 },
	{ CCI_REG8(0xae0), 0x50 },
	{ CCI_REG8(0xae1), 0x54 },
	{ CCI_REG8(0xae2), 0x54 },
	{ CCI_REG8(0xae3), 0x54 },
	{ CCI_REG8(0xaea), 0x10 },
	{ CCI_REG8(0xa1f), 0x0b },
	{ CCI_REG8(0xa20), 0x19 },
	{ CCI_REG8(0xa21), 0x33 },
	{ CCI_REG8(0xa25), 0x06 },
	{ CCI_REG8(0xa26), 0x0e },
	{ CCI_REG8(0xa27), 0x20 },
	{ CCI_REG8(0xa29), 0x9D },
	{ CCI_REG8(0xa88), 0x4c },
	{ CCI_REG8(0xa86), 0x4c },
	{ CCI_REG8(0xa40), 0x5d },
	{ CCI_REG8(0xa44), 0x9a },
	{ CCI_REG8(0xa2e), 0x00 },
	{ CCI_REG8(0xa19), 0x48 },
	{ CCI_REG8(0xa10), 0x84 },
	{ CCI_REG8(0xa12), 0x97 },
	{ CCI_REG8(0xa38), 0x2d },
	{ CCI_REG8(0xa57), 0xdb },
	{ CCI_REG8(0xa22), 0x9b },
	{ CCI_REG8(0xa23), 0xda },
	{ CCI_REG8(0xa24), 0xed },
	{ CCI_REG8(0xa28), 0x42 },
	{ CCI_REG8(0xa2A), 0x9b },
	{ CCI_REG8(0xa2B), 0xa9 },
	{ CCI_REG8(0xa2C), 0xb0 },
	{ CCI_REG8(0xa2D), 0x02 },
	{ CCI_REG8(0xb88), 0x47 },
	{ CCI_REG8(0xb1b), 0x00 },
	{ CCI_REG8(0xb1c), 0x7c },
	{ CCI_REG8(0xb80), 0x40 },
	{ CCI_REG8(0xb85), 0x08 },
	{ CCI_REG8(0xb84), 0x9A },
	{ CCI_REG8(0xb83), 0xcf },
	{ CCI_REG8(0xb87), 0x8a },
	{ CCI_REG8(0xb86), 0x0a },
	{ CCI_REG8(0xb88), 0x43 },
	{ CCI_REG8(0xb8b), 0x8f },
	{ CCI_REG8(0xb89), 0x7d },
	{ CCI_REG8(0xb8c), 0x9d },
	{ CCI_REG8(0x700), 0xff },
	{ CCI_REG8(0x701), 0x60 },
	{ CCI_REG8(0x70f), 0x8a },
	{ CCI_REG8(0x710), 0xf0 },
	{ CCI_REG8(0x716), 0x00 },
	{ CCI_REG8(0x717), 0x08 },
	{ CCI_REG8(0x742), 0x00 },
	{ CCI_REG8(0x743), 0x79 },
	{ CCI_REG8(0x744), 0x00 },
	{ CCI_REG8(0x745), 0x79 },
	{ CCI_REG8(0x746), 0x00 },
	{ CCI_REG8(0x747), 0x79 },
	{ CCI_REG8(0x748), 0x00 },
	{ CCI_REG8(0x749), 0x79 },
	{ CCI_REG8(0x74c), 0x30 },
	{ CCI_REG8(0x74d), 0x00 },
	{ CCI_REG8(0x74e), 0x10 },
	{ CCI_REG8(0x7bb), 0x00 },
	{ CCI_REG8(0x7b0), 0x01 },
	{ CCI_REG8(0x7f5), 0x01 },
	{ CCI_REG8(0x7f6), 0x6a },
	{ CCI_REG8(0x488), 0x80 },
	{ CCI_REG8(0x279), 0x1b },
	{ CCI_REG8(0x27d), 0x05 },
	{ CCI_REG8(0x26b), 0x00 },
	{ CCI_REG8(0x2a3), 0x01 },
	{ CCI_REG8(0x128), 0x00 },
	{ CCI_REG8(0x129), 0x00 },
	{ CCI_REG8(0x12a), 0x01 },
	{ CCI_REG8(0x12b), 0x46 },
	{ CCI_REG8(0x12c), 0x00 },
	{ CCI_REG8(0x12d), 0x00 },
	{ CCI_REG8(0x12e), 0x07 },
	{ CCI_REG8(0x12f), 0xA8 },
	{ CCI_REG8(0x131), 0x00 },
	{ CCI_REG8(0x132), 0x00 },
	{ CCI_REG8(0x139), 0x00 },
	{ CCI_REG8(0x105), 0x00 },
	{ CCI_REG8(0x106), 0xda },
	{ CCI_REG8(0x2e5), 0x0a },
	{ CCI_REG8(0x2e4), 0x30 },
	{ CCI_REG8(0x2e7), 0x07 },
	{ CCI_REG8(0x2e6), 0xa8 },
	{ CCI_REG8(0x2b0), 0x00 },
	{ CCI_REG8(0x2b1), 0x08 },
	{ CCI_REG8(0x2b2), 0x07 },
	{ CCI_REG8(0x2b3), 0x98 },
	{ CCI_REG8(0x2b4), 0x00 },
	{ CCI_REG8(0x2b5), 0x08 },
	{ CCI_REG8(0x2b6), 0x0a },
	{ CCI_REG8(0x2b7), 0x20 },
	{ CCI_REG8(0x299), 0x20 },
	{ CCI_REG8(0x29a), 0x0a },
	{ CCI_REG8(0x29b), 0x20 },
	{ CCI_REG8(0x29c), 0x0a },
	{ CCI_REG8(0x29d), 0x20 },
	{ CCI_REG8(0x29e), 0x0a },
	{ CCI_REG8(0x2a1), 0x98 },
	{ CCI_REG8(0x2a2), 0x07 },
	{ CCI_REG8(0x402), 0x00 },
	{ CCI_REG8(0x101), 0x31 },
	{ CCI_REG8(0x028), 0x74 },
};

static const struct os05e10_mode supported_modes[] = {
	{
		.width = 2592,
		.height = 1944,
		.vts = 2334,
		.exp = 1944,
		.bpp = 10,
	},
};

static const s64 link_freq_menu_items[] = {
	OS05E10_LINK_FREQ_720MHZ,
};

static inline struct os05e10 *to_os05e10(struct v4l2_subdev *sd)
{
	return container_of_const(sd, struct os05e10, sd);
}

static const u32 os05e10_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
};

static int os05e10_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct os05e10 *os05e10 = container_of_const(ctrl->handler,
						     struct os05e10, handler);
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *fmt;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(&os05e10->sd);
	fmt = v4l2_subdev_state_get_format(state, 0);

	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Honour the VBLANK limits when setting exposure */
		s64 max = fmt->height + ctrl->val - OS05E10_EXPOSURE_MARGIN;

		ret = __v4l2_ctrl_modify_range(os05e10->exposure,
					       os05e10->exposure->minimum, max,
					       os05e10->exposure->step,
					       os05e10->exposure->default_value);
		if (ret)
			return ret;
	}

	if (pm_runtime_get_if_active(os05e10->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(os05e10->cci, OS05E10_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(os05e10->cci, OS05E10_REG_ANALOG_GAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(os05e10->cci, OS05E10_REG_DIGITAL_GAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK: {
		/* P1:0x05/0x06 add dummy lines to the base VTS */
		cci_write(os05e10->cci, OS05E10_REG_VBLANK,
			  fmt->height + ctrl->val - OS05E10_VTS_BASE, &ret);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	/* Trigger the update by writing to P1:0x01 */
	cci_write(os05e10->cci, OS05E10_REG_FRAME_SYNC,
		  OS05E10_FRAME_SYNC_TRIGGER, &ret);

	pm_runtime_put(os05e10->dev);

	return ret;
}

static const struct v4l2_ctrl_ops os05e10_ctrl_ops = {
	.s_ctrl = os05e10_set_ctrl,
};

static u64 os05e10_pixel_rate(const struct os05e10_mode *mode)
{
	/* pixel_rate = link_freq * 2 * nr_of_lanes / bits_per_sample */
	return div_u64(OS05E10_LINK_FREQ_720MHZ * 2 * OS05E10_DATA_LANES,
		       mode->bpp);
}

static int os05e10_init_controls(struct os05e10 *os05e10)
{
	const struct os05e10_mode *mode = &supported_modes[0];
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	u64 vblank_def, exp_max, pixel_rate;
	struct v4l2_ctrl *link_freq;
	int ret;

	ret = v4l2_fwnode_device_parse(os05e10->dev, &props);
	if (ret)
		return ret;

	ctrl_hdlr = &os05e10->handler;
	v4l2_ctrl_handler_init(ctrl_hdlr, 12);

	pixel_rate = os05e10_pixel_rate(mode);
	v4l2_ctrl_new_std(ctrl_hdlr, &os05e10_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  pixel_rate, pixel_rate, 1, pixel_rate);

	link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &os05e10_ctrl_ops,
					   V4L2_CID_LINK_FREQ,
					   ARRAY_SIZE(link_freq_menu_items) - 1,
					   0, link_freq_menu_items);

	vblank_def = mode->vts - mode->height;
	os05e10->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &os05e10_ctrl_ops,
					    V4L2_CID_VBLANK,
					    OS05E10_VTS_BASE - mode->height,
					    OS05E10_VTS_BASE - mode->height +
					    OS05E10_VBLANK_MAX,
					    1, vblank_def);

	exp_max = mode->vts - OS05E10_EXPOSURE_MARGIN;
	os05e10->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &os05e10_ctrl_ops,
					      V4L2_CID_EXPOSURE,
					      OS05E10_EXPOSURE_MIN, exp_max,
					      OS05E10_EXPOSURE_STEP,
					      mode->exp);

	v4l2_ctrl_new_std(ctrl_hdlr, &os05e10_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN, OS05E10_ANALOG_GAIN_MIN,
			  OS05E10_ANALOG_GAIN_MAX, OS05E10_ANALOG_GAIN_STEP,
			  OS05E10_ANALOG_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &os05e10_ctrl_ops,
			  V4L2_CID_DIGITAL_GAIN, OS05E10_DIGITAL_GAIN_MIN,
			  OS05E10_DIGITAL_GAIN_MAX, OS05E10_DIGITAL_GAIN_STEP,
			  OS05E10_DIGITAL_GAIN_DEFAULT);

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr,
					      &os05e10_ctrl_ops, &props);
	if (ret)
		goto err_handler_free;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto err_handler_free;
	}

	link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	os05e10->sd.ctrl_handler = ctrl_hdlr;

	return 0;

err_handler_free:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int os05e10_enable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct os05e10 *os05e10 = to_os05e10(sd);
	int ret;

	ret = pm_runtime_resume_and_get(os05e10->dev);
	if (ret < 0)
		return ret;

	ret = cci_multi_reg_write(os05e10->cci, os05e10_common_regs,
				  ARRAY_SIZE(os05e10_common_regs), NULL);
	if (ret) {
		dev_err(os05e10->dev, "failed to write common registers\n");
		goto err_rpm_put;
	}

	ret = __v4l2_ctrl_handler_setup(os05e10->sd.ctrl_handler);
	if (ret)
		goto err_rpm_put;

	ret = cci_write(os05e10->cci, OS05E10_REG_STREAM_CTRL,
			OS05E10_STREAM_CTRL_ON, NULL);
	if (ret) {
		dev_err(os05e10->dev, "fail to start streaming\n");
		goto err_rpm_put;
	}

	return 0;

err_rpm_put:
	pm_runtime_put(os05e10->dev);

	return ret;
}

static int os05e10_disable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state, u32 pad,
				   u64 streams_mask)
{
	struct os05e10 *os05e10 = to_os05e10(sd);
	int ret;

	ret = cci_write(os05e10->cci, OS05E10_REG_STREAM_CTRL,
			OS05E10_STREAM_CTRL_OFF, NULL);
	if (ret)
		dev_err(os05e10->dev, "Failed to stop stream\n");

	pm_runtime_put(os05e10->dev);

	return ret;
}

static int os05e10_get_selection(struct v4l2_subdev *sd,
				 const struct v4l2_subdev_client_info *ci,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r = os05e10_native_area;
		return 0;
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = os05e10_active_area;
		return 0;
	default:
		return -EINVAL;
	}
}

static int os05e10_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > ARRAY_SIZE(os05e10_mbus_codes))
		return -EINVAL;

	code->code = os05e10_mbus_codes[code->index];

	return 0;
}

static int os05e10_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != os05e10_mbus_codes[0])
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int os05e10_set_pad_format(struct v4l2_subdev *sd,
				  const struct v4l2_subdev_client_info *ci,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_format *fmt)
{
	const struct os05e10_mode *mode = &supported_modes[0];
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(sd_state, 0);

	fmt->format.code = os05e10_mbus_codes[0];
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	*format = fmt->format;

	return 0;
}

static int os05e10_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.format = {
			.width = supported_modes[0].width,
			.height = supported_modes[0].height,
		},
	};

	return os05e10_set_pad_format(sd, NULL, state, &fmt);
}

static const struct v4l2_subdev_video_ops os05e10_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops os05e10_pad_ops = {
	.enum_mbus_code = os05e10_enum_mbus_code,
	.enum_frame_size = os05e10_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = os05e10_set_pad_format,
	.get_selection = os05e10_get_selection,
	.enable_streams = os05e10_enable_streams,
	.disable_streams = os05e10_disable_streams,
};

static const struct v4l2_subdev_ops os05e10_subdev_ops = {
	.video = &os05e10_video_ops,
	.pad = &os05e10_pad_ops,
};

static const struct v4l2_subdev_internal_ops os05e10_internal_ops = {
	.init_state = os05e10_init_state,
};

static int os05e10_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct os05e10 *os05e10 = to_os05e10(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(os05e10_supply_name),
				    os05e10->supplies);
	if (ret) {
		dev_err(os05e10->dev, "failed to enable regulators\n");
		return ret;
	}

	ret = clk_prepare_enable(os05e10->xclk);
	if (ret)
		goto err_regulator_off;

	/* T3/T4: Wait for supplies to stabilize before releasing reset */
	fsleep(5 * USEC_PER_MSEC);

	gpiod_set_value_cansleep(os05e10->reset_gpio, 0);

	/* T5: Wait before SCCB communication */
	fsleep(8 * USEC_PER_MSEC);

	return 0;

err_regulator_off:
	regulator_bulk_disable(ARRAY_SIZE(os05e10_supply_name), os05e10->supplies);

	return ret;
}

static int os05e10_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct os05e10 *os05e10 = to_os05e10(sd);

	clk_disable_unprepare(os05e10->xclk);
	gpiod_set_value_cansleep(os05e10->reset_gpio, 1);

	regulator_bulk_disable(ARRAY_SIZE(os05e10_supply_name), os05e10->supplies);

	return 0;
}

static int os05e10_identify_module(struct os05e10 *os05e10)
{
	int ret;
	u64 val;

	ret = cci_read(os05e10->cci, OS05E10_REG_CHIPID, &val, NULL);
	if (ret)
		return dev_err_probe(os05e10->dev, ret,
				     "failed to read chip id %x\n",
				     OS05E10_CHIPID);

	if (val != OS05E10_CHIPID)
		return dev_err_probe(os05e10->dev, -EIO,
				     "chip id mismatch: %x!=%llx\n",
				     OS05E10_CHIPID, val);

	return 0;
}

static int os05e10_parse_endpoint(struct os05e10 *os05e10)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long link_freq_bitmap;
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(os05e10->dev), NULL);
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	ret = v4l2_link_freq_to_bitmap(os05e10->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       link_freq_menu_items,
				       ARRAY_SIZE(link_freq_menu_items),
				       &link_freq_bitmap);

	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
};

static const struct regmap_range_cfg os05e10_ranges[] = {
	{
		.range_min      = 0x0000,
		.range_max      = 0x0fff,
		.selector_reg   = 0xfd,
		.selector_mask  = 0x0f,
		.selector_shift = 0,
		.window_start   = 0x00,
		.window_len     = 0x100,
	},
};

static const struct regmap_config os05e10_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = 0xfff,
	.ranges = os05e10_ranges,
	.num_ranges = ARRAY_SIZE(os05e10_ranges),
	.disable_locking = true,
};

static int os05e10_probe(struct i2c_client *client)
{
	struct os05e10 *os05e10;
	unsigned int xclk_freq;
	int ret;

	os05e10 = devm_kzalloc(&client->dev, sizeof(*os05e10), GFP_KERNEL);
	if (!os05e10)
		return -ENOMEM;

	os05e10->dev = &client->dev;

	v4l2_i2c_subdev_init(&os05e10->sd, client, &os05e10_subdev_ops);
	os05e10->sd.internal_ops = &os05e10_internal_ops;

	os05e10->cci = devm_regmap_init_i2c(client, &os05e10_regmap_config);
	if (IS_ERR(os05e10->cci))
		return dev_err_probe(os05e10->dev, PTR_ERR(os05e10->cci),
				     "failed to initialize CCI\n");

	ret = os05e10_parse_endpoint(os05e10);
	if (ret)
		return dev_err_probe(os05e10->dev, ret,
				     "failed to parse endpoint configuration\n");

	/* Get system clock (xvclk) */
	os05e10->xclk = devm_v4l2_sensor_clk_get(os05e10->dev, NULL);
	if (IS_ERR(os05e10->xclk))
		return dev_err_probe(os05e10->dev, PTR_ERR(os05e10->xclk),
				     "failed to get xclk\n");

	xclk_freq = clk_get_rate(os05e10->xclk);
	if (xclk_freq != OS05E10_XCLK_FREQ_24MHZ)
		return dev_err_probe(os05e10->dev, -EINVAL,
				     "xclk frequency not supported: %u Hz\n",
				     xclk_freq);

	for (unsigned int i = 0; i < ARRAY_SIZE(os05e10_supply_name); i++)
		os05e10->supplies[i].supply = os05e10_supply_name[i];

	ret = devm_regulator_bulk_get(os05e10->dev,
				      ARRAY_SIZE(os05e10_supply_name),
				      os05e10->supplies);
	if (ret)
		return dev_err_probe(os05e10->dev, ret,
				     "failed to get regulators\n");

	os05e10->reset_gpio = devm_gpiod_get_optional(os05e10->dev,
						      "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(os05e10->reset_gpio))
		return dev_err_probe(os05e10->dev, PTR_ERR(os05e10->reset_gpio),
				     "failed to get reset GPIO\n");

	ret = os05e10_power_on(os05e10->dev);
	if (ret)
		return ret;

	ret = os05e10_identify_module(os05e10);
	if (ret)
		goto error_power_off;

	ret = os05e10_init_controls(os05e10);
	if (ret)
		goto error_power_off;

	/* Initialize subdev */
	os05e10->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	os05e10->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	os05e10->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&os05e10->sd.entity, 1, &os05e10->pad);
	if (ret) {
		dev_err_probe(os05e10->dev, ret, "failed to init entity pads\n");
		goto error_handler_free;
	}

	os05e10->sd.state_lock = os05e10->handler.lock;
	ret = v4l2_subdev_init_finalize(&os05e10->sd);
	if (ret) {
		dev_err_probe(os05e10->dev, ret, "subdev init error\n");
		goto error_media_entity;
	}

	pm_runtime_set_active(os05e10->dev);
	pm_runtime_enable(os05e10->dev);

	ret = v4l2_async_register_subdev_sensor(&os05e10->sd);
	if (ret) {
		dev_err_probe(os05e10->dev, ret,
			      "failed to register os05e10 sub-device\n");
		goto error_subdev_cleanup;
	}

	pm_runtime_idle(os05e10->dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&os05e10->sd);
	pm_runtime_disable(os05e10->dev);
	pm_runtime_set_suspended(os05e10->dev);

error_media_entity:
	media_entity_cleanup(&os05e10->sd.entity);

error_handler_free:
	v4l2_ctrl_handler_free(os05e10->sd.ctrl_handler);

error_power_off:
	os05e10_power_off(os05e10->dev);

	return ret;
}

static void os05e10_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct os05e10 *os05e10 = to_os05e10(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(&os05e10->sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(os05e10->sd.ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev)) {
		os05e10_power_off(&client->dev);
		pm_runtime_set_suspended(&client->dev);
	}
}

static DEFINE_RUNTIME_DEV_PM_OPS(os05e10_pm_ops,
				 os05e10_power_off, os05e10_power_on, NULL);

static const struct of_device_id os05e10_id[] = {
	{ .compatible = "ovti,os05e10" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, os05e10_id);

static struct i2c_driver os05e10_driver = {
	.driver = {
		.name = "os05e10",
		.pm = pm_ptr(&os05e10_pm_ops),
		.of_match_table = os05e10_id,
	},
	.probe = os05e10_probe,
	.remove = os05e10_remove,
};
module_i2c_driver(os05e10_driver);

MODULE_DESCRIPTION("OS05E10 Camera Sensor Driver");
MODULE_AUTHOR("Bhavin Sharma <Bhavin.sharma@siliconsignals.io>");
MODULE_AUTHOR("Elgin Perumbilly <elgin.perumbilly@siliconsignals.io>");
MODULE_LICENSE("GPL");
