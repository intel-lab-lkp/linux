// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for GalaxyCore GC5035 image sensor
 *
 * Copyright (c) 2020 Bitland Inc.
 * Copyright 2020 Google LLC.
 * Copyright (c) 2022 Intel Corporation.
 * Copyright (C) 2026 Nicola Fiorillo <nicfio@gmail.com>
 *
 * The register table comes from the GC5035 driver in intel/ipu6-drivers by
 * Liang Wang <liang1.wang@intel.com>, derived from the ChromeOS series by
 * Tomasz Figa <tfiga@chromium.org>.
 */
#include <linux/array_size.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/time64.h>
#include <linux/types.h>
#include <linux/units.h>

#include <media/mipi-csi2.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/*
 * The GC5035 uses 8 bit register addresses with banked pages: register
 * 0xfe selects the page. Every register below is on page 0 unless noted
 * otherwise. The vendor sequence also writes 0x10 to register 0xfe, which is
 * not a page number and is not documented, so the pages are not handled with
 * regmap ranges.
 */
#define GC5035_REG_PAGE_SELECT		CCI_REG8(0xfe)
#define GC5035_PAGE_0			0x00
#define GC5035_PAGE_1			0x01

/* readable on any page */
#define GC5035_REG_CHIP_ID		CCI_REG16(0xf0)
#define GC5035_CHIP_ID			0x5035

#define GC5035_REG_EXPOSURE		CCI_REG16(0x03)
#define GC5035_REG_LINE_LENGTH		CCI_REG16(0x05)
#define GC5035_REG_WIN_TOP		CCI_REG16(0x09)
#define GC5035_REG_WIN_LEFT		CCI_REG16(0x0b)
#define GC5035_REG_WIN_HEIGHT		CCI_REG16(0x0d)
#define GC5035_REG_WIN_WIDTH		CCI_REG16(0x0f)
#define GC5035_REG_STREAM		CCI_REG8(0x3e)
#define GC5035_STREAM_ON		0x91
#define GC5035_STREAM_OFF		0x01
#define GC5035_REG_FRAME_LENGTH		CCI_REG16(0x41)
#define GC5035_REG_DIGITAL_GAIN	CCI_REG16(0xb1)
#define GC5035_DIGITAL_GAIN_1X	0x100
#define GC5035_REG_ANALOGUE_GAIN	CCI_REG8(0xb6)
#define GC5035_REG_PLL_MULT		CCI_REG8(0xf8)

/* page 1 */
#define GC5035_REG_TEST_PATTERN		CCI_REG8(0x8c)
#define GC5035_TEST_PATTERN_ON		0x11
#define GC5035_TEST_PATTERN_OFF		0x10
#define GC5035_REG_CROP_TOP		CCI_REG16(0x91)
#define GC5035_REG_CROP_LEFT		CCI_REG16(0x93)
#define GC5035_REG_OUT_HEIGHT		CCI_REG16(0x95)
#define GC5035_REG_OUT_WIDTH		CCI_REG16(0x97)

/*
 * The sensor reads out a 2608x1960 window starting at (3, 4) and crops
 * 2592x1944 from it at (8, 8). The size of the full pixel array is not
 * documented, so the readout window is reported as the native size.
 */
#define GC5035_WIN_LEFT			3
#define GC5035_WIN_TOP			4
#define GC5035_NATIVE_WIDTH		2608
#define GC5035_NATIVE_HEIGHT		1960
#define GC5035_CROP_LEFT		8
#define GC5035_CROP_TOP			8
#define GC5035_WIDTH			2592
#define GC5035_HEIGHT			1944

/* The line length register counts units of 4 pixels. */
#define GC5035_HTS			2920
#define GC5035_VTS_DEF			2008

#define GC5035_EXP_MIN			4
#define GC5035_EXP_STEP			1
#define GC5035_EXP_MARGIN		16
#define GC5035_EXP_DEF			984
#define GC5035_VTS_MAX			0x3fff
/* The sensor rounds the frame length to a multiple of 4 lines. */
#define GC5035_VBLANK_STEP		4

/*
 * The register table has only been tested with a 19.2 MHz external clock. The
 * link frequency is the external clock multiplied by the PLL setting and
 * divided by 4: 422.4 MHz.
 */
#define GC5035_XCLK_FREQ		(19200 * HZ_PER_KHZ)
#define GC5035_PLL_MULT			0x58
#define GC5035_LINK_FREQ		(GC5035_XCLK_FREQ * GC5035_PLL_MULT / 4)
#define GC5035_DATA_LANES		2
#define GC5035_RGB_DEPTH		10
#define GC5035_PIXEL_RATE		(GC5035_LINK_FREQ * 2 * \
					 GC5035_DATA_LANES / GC5035_RGB_DEPTH)

/*
 * No datasheet is available. From the GC5035 driver posted by Tomasz Figa in
 * 2020: IOVDD at least 50 us before AVDD and DVDD, at least 1200 MCLK cycles
 * before the first I2C transaction, and 2000 MCLK cycles after streaming stops
 * before the clock is switched off. From the intel/ipu6-drivers driver: the
 * clock is enabled before reset is released and disabled after reset is
 * asserted, and 5 ms elapse before the first I2C transaction, which also
 * covers the 1200 cycles.
 */
#define GC5035_IOVDD_DELAY_US		50
#define GC5035_RESET_DELAY_US		(5 * USEC_PER_MSEC)
#define GC5035_STOP_DELAY_US		DIV_ROUND_UP(2000 * USEC_PER_MSEC, \
					     GC5035_XCLK_FREQ / MSEC_PER_SEC)

#define GC5035_MBUS_CODE		MEDIA_BUS_FMT_SGRBG10_1X10

static const s64 gc5035_link_freq_menu[] = {
	GC5035_LINK_FREQ,
};

static const char * const gc5035_test_pattern_menu[] = {
	"No Pattern",
	"Test Chart",
};

/* DOVDD (IOVDD) comes first: it has to be enabled before the others. */
static const char * const gc5035_supply_name[] = {
	"dovdd",
	"avdd",
	"dvdd",
};

/*
 * V4L2_CID_ANALOGUE_GAIN is an index into this table of register 0xb6 values,
 * one per analogue gain step. The digital gain is left at the 1.0x the register
 * table programs.
 */
static const u8 gc5035_again_code[] = {
	0,	/*  1.000x */
	1,	/*  1.180x */
	2,	/*  1.398x */
	3,	/*  1.660x */
	8,	/*  1.961x */
	9,	/*  2.340x */
	10,	/*  2.801x */
	11,	/*  3.301x */
	12,	/*  3.898x */
	13,	/*  4.699x */
	14,	/*  5.602x */
	15,	/*  6.680x */
	16,	/*  7.801x */
	17,	/*  9.199x */
	18,	/* 11.000x */
	19,	/* 12.961x */
	20,	/* 15.602x */
};

struct gc5035 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct clk *xclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(gc5035_supply_name)];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *powerdown_gpio;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *exposure;

	struct regmap *regmap;
};

/*
 * The register sequence is the vendor one for the 2592x1944 mode, without the
 * registers written by the controls. No register documentation is available:
 * the names of the geometry registers are inferred from their values.
 */
static const struct cci_reg_sequence gc5035_regs[] = {
	/* System */
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ GC5035_REG_STREAM, GC5035_STREAM_OFF },
	{ CCI_REG8(0xfc), 0x01 },
	{ CCI_REG8(0xf4), 0x40 },
	{ CCI_REG8(0xf5), 0xe9 },
	{ CCI_REG8(0xf6), 0x14 },
	{ GC5035_REG_PLL_MULT, GC5035_PLL_MULT },
	{ CCI_REG8(0xf9), 0x82 },
	{ CCI_REG8(0xfa), 0x00 },
	{ CCI_REG8(0xfc), 0x81 },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ CCI_REG8(0x36), 0x01 },
	{ CCI_REG8(0xd3), 0x87 },
	{ CCI_REG8(0x36), 0x00 },
	{ CCI_REG8(0x33), 0x00 },
	{ GC5035_REG_PAGE_SELECT, 0x03 },
	{ CCI_REG8(0x01), 0xe7 },
	{ CCI_REG8(0xf7), 0x01 },
	{ CCI_REG8(0xfc), 0x8f },
	{ CCI_REG8(0xfc), 0x8f },
	{ CCI_REG8(0xfc), 0x8e },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ CCI_REG8(0xee), 0x30 },
	{ CCI_REG8(0x87), 0x18 },
	{ GC5035_REG_PAGE_SELECT, 0x01 },
	{ CCI_REG8(0x8c), 0x90 },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	/* Analog & CISCTL */
	{ GC5035_REG_LINE_LENGTH, GC5035_HTS / 4 },
	{ CCI_REG8(0x9d), 0x18 },
	{ GC5035_REG_WIN_TOP, GC5035_WIN_TOP },
	{ GC5035_REG_WIN_LEFT, GC5035_WIN_LEFT },
	{ GC5035_REG_WIN_HEIGHT, GC5035_NATIVE_HEIGHT },
	{ GC5035_REG_WIN_WIDTH, GC5035_NATIVE_WIDTH },
	{ CCI_REG8(0x11), 0x02 },
	{ CCI_REG8(0x17), 0x80 },
	{ CCI_REG8(0x19), 0x05 },
	{ GC5035_REG_PAGE_SELECT, 0x02 },
	{ CCI_REG8(0x30), 0x03 },
	{ CCI_REG8(0x31), 0x03 },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ CCI_REG8(0xd9), 0xc0 },
	{ CCI_REG8(0x1b), 0x20 },
	{ CCI_REG8(0x21), 0x40 },
	{ CCI_REG8(0x28), 0x22 },
	{ CCI_REG8(0x29), 0x56 },
	{ CCI_REG8(0x44), 0x20 },
	{ CCI_REG8(0x4b), 0x10 },
	{ CCI_REG8(0x4e), 0x1a },
	{ CCI_REG8(0x50), 0x11 },
	{ CCI_REG8(0x52), 0x33 },
	{ CCI_REG8(0x53), 0x44 },
	{ CCI_REG8(0x55), 0x10 },
	{ CCI_REG8(0x5b), 0x11 },
	{ CCI_REG8(0xc5), 0x02 },
	{ CCI_REG8(0x8c), 0x1a },
	{ GC5035_REG_PAGE_SELECT, 0x02 },
	{ CCI_REG8(0x33), 0x05 },
	{ CCI_REG8(0x32), 0x38 },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ CCI_REG8(0x91), 0x80 },
	{ CCI_REG8(0x92), 0x28 },
	{ CCI_REG8(0x93), 0x20 },
	{ CCI_REG8(0x95), 0xa0 },
	{ CCI_REG8(0x96), 0xe0 },
	{ CCI_REG8(0xd5), 0xfc },
	{ CCI_REG8(0x97), 0x28 },
	{ CCI_REG8(0x16), 0x0c },
	{ CCI_REG8(0x1a), 0x1a },
	{ CCI_REG8(0x1f), 0x11 },
	{ CCI_REG8(0x20), 0x10 },
	{ CCI_REG8(0x46), 0x83 },
	{ CCI_REG8(0x4a), 0x04 },
	{ CCI_REG8(0x54), 0x02 },
	{ CCI_REG8(0x62), 0x00 },
	{ CCI_REG8(0x72), 0x8f },
	{ CCI_REG8(0x73), 0x89 },
	{ CCI_REG8(0x7a), 0x05 },
	{ CCI_REG8(0x7d), 0xcc },
	{ CCI_REG8(0x90), 0x00 },
	{ CCI_REG8(0xce), 0x18 },
	{ CCI_REG8(0xd0), 0xb2 },
	{ CCI_REG8(0xd2), 0x40 },
	{ CCI_REG8(0xe6), 0xe0 },
	{ GC5035_REG_PAGE_SELECT, 0x02 },
	{ CCI_REG8(0x12), 0x01 },
	{ CCI_REG8(0x13), 0x01 },
	{ CCI_REG8(0x14), 0x01 },
	{ CCI_REG8(0x15), 0x02 },
	{ CCI_REG8(0x22), 0x7c },
	{ CCI_REG8(0x91), 0x00 },
	{ CCI_REG8(0x92), 0x00 },
	{ CCI_REG8(0x93), 0x00 },
	{ CCI_REG8(0x94), 0x00 },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ CCI_REG8(0xfc), 0x88 },
	{ GC5035_REG_PAGE_SELECT, 0x10 },	/* not a page */
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ CCI_REG8(0xfc), 0x8e },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ CCI_REG8(0xfc), 0x88 },
	{ GC5035_REG_PAGE_SELECT, 0x10 },	/* not a page */
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ CCI_REG8(0xfc), 0x8e },
	/* GAIN */
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ CCI_REG8(0xb0), 0x6e },
	{ GC5035_REG_DIGITAL_GAIN, GC5035_DIGITAL_GAIN_1X },
	{ CCI_REG8(0xb3), 0x00 },
	{ CCI_REG8(0xb4), 0x00 },
	/* ISP */
	{ GC5035_REG_PAGE_SELECT, 0x01 },
	{ CCI_REG8(0x53), 0x00 },
	{ CCI_REG8(0x89), 0x03 },
	{ CCI_REG8(0x60), 0x40 },
	/* BLK */
	{ GC5035_REG_PAGE_SELECT, 0x01 },
	{ CCI_REG8(0x42), 0x21 },
	{ CCI_REG8(0x49), 0x03 },
	{ CCI_REG8(0x4a), 0xff },
	{ CCI_REG8(0x4b), 0xc0 },
	{ CCI_REG8(0x55), 0x00 },
	/* anti_blooming */
	{ GC5035_REG_PAGE_SELECT, 0x01 },
	{ CCI_REG8(0x41), 0x28 },
	{ CCI_REG8(0x4c), 0x00 },
	{ CCI_REG8(0x4d), 0x00 },
	{ CCI_REG8(0x4e), 0x3c },
	{ CCI_REG8(0x44), 0x08 },
	{ CCI_REG8(0x48), 0x02 },
	/* CROP */
	{ GC5035_REG_PAGE_SELECT, 0x01 },
	{ GC5035_REG_CROP_TOP, GC5035_CROP_TOP },
	{ GC5035_REG_CROP_LEFT, GC5035_CROP_LEFT },
	{ GC5035_REG_OUT_HEIGHT, GC5035_HEIGHT },
	{ GC5035_REG_OUT_WIDTH, GC5035_WIDTH },
	{ CCI_REG8(0x99), 0x00 },
	/* MIPI */
	{ GC5035_REG_PAGE_SELECT, 0x03 },
	{ CCI_REG8(0x02), 0x57 },
	{ CCI_REG8(0x03), 0xb7 },
	{ CCI_REG8(0x15), 0x14 },
	{ CCI_REG8(0x18), 0x0f },
	{ CCI_REG8(0x21), 0x22 },
	{ CCI_REG8(0x22), 0x06 },
	{ CCI_REG8(0x23), 0x48 },
	{ CCI_REG8(0x24), 0x12 },
	{ CCI_REG8(0x25), 0x28 },
	{ CCI_REG8(0x26), 0x08 },
	{ CCI_REG8(0x29), 0x06 },
	{ CCI_REG8(0x2a), 0x58 },
	{ CCI_REG8(0x2b), 0x08 },
	{ GC5035_REG_PAGE_SELECT, 0x00 },
	{ GC5035_REG_STREAM, GC5035_STREAM_OFF },
};

static inline struct gc5035 *to_gc5035(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc5035, sd);
}

static int gc5035_set_page(struct gc5035 *gc5035, u8 page)
{
	return cci_write(gc5035->regmap, GC5035_REG_PAGE_SELECT, page, NULL);
}

static int gc5035_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gc5035 *gc5035 = to_gc5035(sd);
	int ret;

	ret = regulator_enable(gc5035->supplies[0].consumer);
	if (ret) {
		dev_err(dev, "failed to enable dovdd: %d\n", ret);
		return ret;
	}

	fsleep(GC5035_IOVDD_DELAY_US);

	ret = regulator_bulk_enable(ARRAY_SIZE(gc5035_supply_name) - 1,
				    &gc5035->supplies[1]);
	if (ret) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		goto err_dovdd;
	}

	ret = clk_prepare_enable(gc5035->xclk);
	if (ret) {
		dev_err(dev, "failed to enable clock: %d\n", ret);
		goto err_regulators;
	}

	gpiod_set_value_cansleep(gc5035->powerdown_gpio, 0);
	gpiod_set_value_cansleep(gc5035->reset_gpio, 0);

	fsleep(GC5035_RESET_DELAY_US);

	return 0;

err_regulators:
	regulator_bulk_disable(ARRAY_SIZE(gc5035_supply_name) - 1,
			       &gc5035->supplies[1]);
err_dovdd:
	regulator_disable(gc5035->supplies[0].consumer);

	return ret;
}

static int gc5035_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gc5035 *gc5035 = to_gc5035(sd);

	fsleep(GC5035_STOP_DELAY_US);

	gpiod_set_value_cansleep(gc5035->powerdown_gpio, 1);
	gpiod_set_value_cansleep(gc5035->reset_gpio, 1);
	clk_disable_unprepare(gc5035->xclk);
	regulator_bulk_disable(ARRAY_SIZE(gc5035_supply_name) - 1,
			       &gc5035->supplies[1]);
	regulator_disable(gc5035->supplies[0].consumer);

	return 0;
}

static int gc5035_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = GC5035_MBUS_CODE;

	return 0;
}

static int gc5035_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->code != GC5035_MBUS_CODE)
		return -EINVAL;

	if (fse->index > 0)
		return -EINVAL;

	fse->min_width = GC5035_WIDTH;
	fse->max_width = GC5035_WIDTH;
	fse->min_height = GC5035_HEIGHT;
	fse->max_height = GC5035_HEIGHT;

	return 0;
}

static void gc5035_fill_state(struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_rect *crop;

	crop = v4l2_subdev_state_get_crop(state, 0);
	crop->left = GC5035_CROP_LEFT;
	crop->top = GC5035_CROP_TOP;
	crop->width = GC5035_WIDTH;
	crop->height = GC5035_HEIGHT;

	fmt = v4l2_subdev_state_get_format(state, 0);
	fmt->width = GC5035_WIDTH;
	fmt->height = GC5035_HEIGHT;
	fmt->code = GC5035_MBUS_CODE;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int gc5035_get_selection(struct v4l2_subdev *sd,
				const struct v4l2_subdev_client_info *ci,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.top = GC5035_CROP_TOP;
		sel->r.left = GC5035_CROP_LEFT;
		sel->r.width = GC5035_WIDTH;
		sel->r.height = GC5035_HEIGHT;
		break;
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = GC5035_NATIVE_WIDTH;
		sel->r.height = GC5035_NATIVE_HEIGHT;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int gc5035_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	gc5035_fill_state(state);

	return 0;
}

static int gc5035_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_mbus_frame_desc *fd)
{
	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;
	fd->entry[0].pixelcode = GC5035_MBUS_CODE;
	fd->entry[0].stream = 0;
	fd->entry[0].bus.csi2.vc = 0;
	fd->entry[0].bus.csi2.dt = MIPI_CSI2_DT_RAW10;

	return 0;
}

static int gc5035_test_pattern(struct gc5035 *gc5035, u32 pattern)
{
	int ret;

	ret = gc5035_set_page(gc5035, GC5035_PAGE_1);
	if (ret)
		return ret;

	ret = cci_write(gc5035->regmap, GC5035_REG_TEST_PATTERN,
			pattern ? GC5035_TEST_PATTERN_ON :
				  GC5035_TEST_PATTERN_OFF, NULL);

	/* Back to page 0 even on error, the other controls expect it. */
	return gc5035_set_page(gc5035, GC5035_PAGE_0) ?: ret;
}

static int gc5035_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc5035 *gc5035 =
		container_of(ctrl->handler, struct gc5035, ctrls);
	const struct v4l2_mbus_framefmt *format;
	struct v4l2_subdev_state *state;
	s64 exposure_max;
	int ret;

	state = v4l2_subdev_get_locked_active_state(&gc5035->sd);
	format = v4l2_subdev_state_get_format(state, 0);

	if (ctrl->id == V4L2_CID_VBLANK) {
		exposure_max = format->height + ctrl->val -
			       GC5035_EXP_MARGIN;
		ret = __v4l2_ctrl_modify_range(gc5035->exposure,
					       GC5035_EXP_MIN, exposure_max,
					       GC5035_EXP_STEP, GC5035_EXP_DEF);
		if (ret)
			return ret;
	}

	if (!pm_runtime_get_if_active(gc5035->dev))
		return 0;

	ret = gc5035_set_page(gc5035, GC5035_PAGE_0);
	if (ret)
		goto out;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = cci_write(gc5035->regmap, GC5035_REG_EXPOSURE,
				ctrl->val, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(gc5035->regmap, GC5035_REG_ANALOGUE_GAIN,
				gc5035_again_code[ctrl->val], NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(gc5035->regmap, GC5035_REG_FRAME_LENGTH,
				format->height + ctrl->val, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = gc5035_test_pattern(gc5035, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

out:
	pm_runtime_put_autosuspend(gc5035->dev);

	return ret;
}

static const struct v4l2_ctrl_ops gc5035_ctrl_ops = {
	.s_ctrl = gc5035_set_ctrl,
};

/* Only ever called from probe, with the sensor powered up. */
static int gc5035_identify_module(struct gc5035 *gc5035)
{
	u64 val;
	int ret;

	ret = cci_read(gc5035->regmap, GC5035_REG_CHIP_ID, &val, NULL);
	if (ret)
		return ret;

	if (val != GC5035_CHIP_ID)
		return dev_err_probe(gc5035->dev, -ENXIO,
				     "chip id mismatch: %x != %llx\n",
				     GC5035_CHIP_ID, val);

	return 0;
}

static int gc5035_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct gc5035 *gc5035 = to_gc5035(sd);
	int ret;

	ret = pm_runtime_resume_and_get(gc5035->dev);
	if (ret < 0)
		return ret;

	ret = cci_multi_reg_write(gc5035->regmap, gc5035_regs,
				  ARRAY_SIZE(gc5035_regs), NULL);
	if (ret)
		goto err_rpm_put;

	ret = __v4l2_ctrl_handler_setup(&gc5035->ctrls);
	if (ret)
		goto err_rpm_put;

	ret = cci_write(gc5035->regmap, GC5035_REG_STREAM,
			GC5035_STREAM_ON, NULL);
	if (ret)
		goto err_rpm_put;

	return 0;

err_rpm_put:
	pm_runtime_put_autosuspend(gc5035->dev);

	return ret;
}

static int gc5035_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct gc5035 *gc5035 = to_gc5035(sd);
	int ret = 0;

	/* cci_write() logs errors, nothing else can be done about them. */
	cci_write(gc5035->regmap, GC5035_REG_PAGE_SELECT, GC5035_PAGE_0, &ret);
	cci_write(gc5035->regmap, GC5035_REG_STREAM, GC5035_STREAM_OFF, &ret);

	pm_runtime_put_autosuspend(gc5035->dev);

	return 0;
}

static const struct v4l2_subdev_video_ops gc5035_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops gc5035_pad_ops = {
	.enum_mbus_code = gc5035_enum_mbus_code,
	.enum_frame_size = gc5035_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = gc5035_get_selection,
	.get_frame_desc = gc5035_get_frame_desc,
	.enable_streams = gc5035_enable_streams,
	.disable_streams = gc5035_disable_streams,
};

static const struct v4l2_subdev_ops gc5035_subdev_ops = {
	.video = &gc5035_video_ops,
	.pad = &gc5035_pad_ops,
};

static const struct v4l2_subdev_internal_ops gc5035_internal_ops = {
	.init_state = gc5035_init_state,
};

static int gc5035_parse_fwnode(struct gc5035 *gc5035)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct device *dev = gc5035->dev;
	struct fwnode_handle *endpoint;
	unsigned long link_freq_bitmap;
	int ret;

	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
						   FWNODE_GRAPH_ENDPOINT_NEXT);
	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &bus_cfg);
	fwnode_handle_put(endpoint);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse endpoint\n");

	/* The register table configures the CSI-2 transmitter for 2 lanes. */
	if (bus_cfg.bus.mipi_csi2.num_data_lanes != GC5035_DATA_LANES) {
		ret = dev_err_probe(dev, -EINVAL,
				    "unsupported number of data lanes %u\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
		goto done;
	}

	ret = v4l2_link_freq_to_bitmap(dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       gc5035_link_freq_menu,
				       ARRAY_SIZE(gc5035_link_freq_menu),
				       &link_freq_bitmap);

done:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int gc5035_init_controls(struct gc5035 *gc5035)
{
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct v4l2_ctrl *link_freq, *hblank;
	s64 exposure_max;
	int ret;

	ret = v4l2_fwnode_device_parse(gc5035->dev, &props);
	if (ret)
		return ret;

	ctrl_hdlr = &gc5035->ctrls;
	v4l2_ctrl_handler_init(ctrl_hdlr, 9);

	link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, NULL, V4L2_CID_LINK_FREQ,
					   ARRAY_SIZE(gc5035_link_freq_menu) -
					   1, 0, gc5035_link_freq_menu);

	v4l2_ctrl_new_std(ctrl_hdlr, NULL, V4L2_CID_PIXEL_RATE,
			  GC5035_PIXEL_RATE, GC5035_PIXEL_RATE, 1,
			  GC5035_PIXEL_RATE);

	v4l2_ctrl_new_std(ctrl_hdlr, &gc5035_ctrl_ops, V4L2_CID_VBLANK,
			  GC5035_VTS_DEF - GC5035_HEIGHT,
			  GC5035_VTS_DEF - GC5035_HEIGHT +
			  round_down(GC5035_VTS_MAX - GC5035_VTS_DEF,
				     GC5035_VBLANK_STEP),
			  GC5035_VBLANK_STEP, GC5035_VTS_DEF - GC5035_HEIGHT);

	hblank = v4l2_ctrl_new_std(ctrl_hdlr, NULL, V4L2_CID_HBLANK,
				   GC5035_HTS - GC5035_WIDTH,
				   GC5035_HTS - GC5035_WIDTH, 1,
				   GC5035_HTS - GC5035_WIDTH);

	exposure_max = GC5035_VTS_DEF - GC5035_EXP_MARGIN;
	gc5035->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &gc5035_ctrl_ops,
					     V4L2_CID_EXPOSURE, GC5035_EXP_MIN,
					     exposure_max, GC5035_EXP_STEP,
					     GC5035_EXP_DEF);

	v4l2_ctrl_new_std(ctrl_hdlr, &gc5035_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  0, ARRAY_SIZE(gc5035_again_code) - 1, 1, 0);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &gc5035_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(gc5035_test_pattern_menu) - 1,
				     0, 0, gc5035_test_pattern_menu);

	v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &gc5035_ctrl_ops, &props);

	if (ctrl_hdlr->error)
		return v4l2_ctrl_handler_free(ctrl_hdlr);

	link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gc5035->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

static int gc5035_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc5035 *gc5035;
	unsigned long freq;
	int ret;

	gc5035 = devm_kzalloc(dev, sizeof(*gc5035), GFP_KERNEL);
	if (!gc5035)
		return -ENOMEM;

	gc5035->dev = dev;

	v4l2_i2c_subdev_init(&gc5035->sd, client, &gc5035_subdev_ops);
	gc5035->sd.internal_ops = &gc5035_internal_ops;

	ret = gc5035_parse_fwnode(gc5035);
	if (ret)
		return ret;

	gc5035->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(gc5035->xclk))
		return dev_err_probe(dev, PTR_ERR(gc5035->xclk),
				     "failed to get clock\n");

	freq = clk_get_rate(gc5035->xclk);
	if (freq != GC5035_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "external clock %lu Hz not supported\n",
				     freq);

	gc5035->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(gc5035->regmap))
		return dev_err_probe(dev, PTR_ERR(gc5035->regmap),
				     "failed to init CCI\n");

	gc5035->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(gc5035->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(gc5035->reset_gpio),
				     "failed to get reset GPIO\n");

	gc5035->powerdown_gpio = devm_gpiod_get_optional(dev, "powerdown",
							 GPIOD_OUT_HIGH);
	if (IS_ERR(gc5035->powerdown_gpio))
		return dev_err_probe(dev, PTR_ERR(gc5035->powerdown_gpio),
				     "failed to get powerdown GPIO\n");

	for (unsigned int i = 0; i < ARRAY_SIZE(gc5035_supply_name); i++)
		gc5035->supplies[i].supply = gc5035_supply_name[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(gc5035_supply_name),
				      gc5035->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ret = gc5035_power_on(dev);
	if (ret)
		return ret;

	ret = gc5035_identify_module(gc5035);
	if (ret)
		goto err_power_off;

	ret = gc5035_init_controls(gc5035);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init controls\n");
		goto err_power_off;
	}

	gc5035->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	gc5035->pad.flags = MEDIA_PAD_FL_SOURCE;
	gc5035->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&gc5035->sd.entity, 1, &gc5035->pad);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to init media entity\n");
		goto err_ctrl_handler_free;
	}

	gc5035->sd.state_lock = gc5035->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&gc5035->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to finalize subdev\n");
		goto err_media_entity_cleanup;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	ret = v4l2_async_register_subdev_sensor(&gc5035->sd);
	if (ret < 0) {
		dev_err_probe(dev, ret, "failed to register subdev\n");
		goto err_rpm;
	}

	pm_runtime_idle(dev);

	return 0;

err_rpm:
	pm_runtime_disable(dev);
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&gc5035->sd);

err_media_entity_cleanup:
	media_entity_cleanup(&gc5035->sd.entity);

err_ctrl_handler_free:
	v4l2_ctrl_handler_free(&gc5035->ctrls);

err_power_off:
	gc5035_power_off(dev);

	return ret;
}

static void gc5035_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc5035 *gc5035 = to_gc5035(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&gc5035->ctrls);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev)) {
		gc5035_power_off(&client->dev);
		pm_runtime_set_suspended(&client->dev);
	}
	pm_runtime_dont_use_autosuspend(&client->dev);
}

static const struct dev_pm_ops gc5035_pm_ops = {
	RUNTIME_PM_OPS(gc5035_power_off, gc5035_power_on, NULL)
};

static const struct acpi_device_id gc5035_acpi_ids[] = {
	{ "GCTI5035" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, gc5035_acpi_ids);

static struct i2c_driver gc5035_i2c_driver = {
	.driver = {
		.name = "gc5035",
		.acpi_match_table = gc5035_acpi_ids,
		.pm = pm_ptr(&gc5035_pm_ops),
	},
	.probe = gc5035_probe,
	.remove = gc5035_remove,
};
module_i2c_driver(gc5035_i2c_driver);

MODULE_AUTHOR("Nicola Fiorillo <nicfio@gmail.com>");
MODULE_DESCRIPTION("GalaxyCore GC5035 sensor driver");
MODULE_LICENSE("GPL");
