// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX681 CMOS Image Sensor Driver
 *
 * Front camera on Surface Pro 11 Business (Intel/Lunar Lake).
 * Register sequences reverse-engineered from Windows I2C traces.
 *
 * Copyright (C) 2025
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

/* Chip ID register and expected value */
#define IMX681_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX681_CHIP_ID			0x0681

/* Mode select */
#define IMX681_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX681_MODE_STANDBY		0x00
#define IMX681_MODE_STREAMING		0x01

/* Exposure (coarse integration time, 24-bit) */
#define IMX681_REG_EXPOSURE		CCI_REG24(0x0229)
/*
 * The external clock frequency, in MHz, as a Q8.8 fixed-point number: the
 * sensor takes 19.2 MHz as 19 in the high byte and 0.2 * 256 in the low one.
 */
#define IMX681_XCLK_FREQ		19200000
#define IMX681_XCLK_FREQ_ENC		((IMX681_XCLK_FREQ / 1000000) << 8 | \
					 (IMX681_XCLK_FREQ % 1000000) * 256 / 1000000)

#define IMX681_EXPOSURE_MIN		4
/* The exposure may not come within this many lines of the frame length. */
#define IMX681_EXPOSURE_MARGIN		4
#define IMX681_EXPOSURE_DEFAULT		IMX681_EXPOSURE_MIN

/* Analog gain */
#define IMX681_REG_ANALOG_GAIN		CCI_REG16(0x0204)
#define IMX681_ANA_GAIN_MIN		0
#define IMX681_ANA_GAIN_MAX		960	/* 16x, where the analogue stage ends */
#define IMX681_ANA_GAIN_DEFAULT		0

/* Digital gain */
#define IMX681_REG_DIGITAL_GAIN		CCI_REG16(0x020E)
#define IMX681_DIG_GAIN_MIN		0x0100	/* 1.0x */
#define IMX681_DIG_GAIN_MAX		0x0FFF
#define IMX681_DIG_GAIN_DEFAULT		0x0100

/* Test pattern, and the colour the solid-colour mode shows */
#define IMX681_REG_TEST_PATTERN		CCI_REG16(0x0600)
#define IMX681_REG_TESTP_RED		CCI_REG16(0x0602)
#define IMX681_REG_TESTP_GREENR		CCI_REG16(0x0604)
#define IMX681_REG_TESTP_BLUE		CCI_REG16(0x0606)
#define IMX681_REG_TESTP_GREENB		CCI_REG16(0x0608)
#define IMX681_TESTP_COLOUR_MIN		0
#define IMX681_TESTP_COLOUR_MAX		0x03ff
#define IMX681_TESTP_COLOUR_STEP	1

/*
 * Frame length and exposure are 24-bit and at vendor addresses; everything
 * else this driver touches is a CCS register at its CCS address, and is
 * named for it here.
 */
#define IMX681_REG_FRAME_LENGTH		CCI_REG24(0x033D)
#define IMX681_REG_LINE_LENGTH		CCI_REG16(0x0342)
#define IMX681_REG_ORIENTATION		CCI_REG8(0x0101)
#define IMX681_REG_CSI_SIGNALING_MODE	CCI_REG8(0x0111)
#define IMX681_REG_EXTCLK_FREQ_MHZ	CCI_REG16(0x0136)
#define IMX681_REG_PLL_MULTIPLIER	CCI_REG16(0x0306)
#define IMX681_REG_OP_PRE_PLL_CLK_DIV	CCI_REG16(0x030c)
#define IMX681_REG_OP_PLL_MULTIPLIER	CCI_REG16(0x030e)
#define IMX681_REG_X_ADDR_START		CCI_REG16(0x0344)
#define IMX681_REG_Y_ADDR_START		CCI_REG16(0x0346)
#define IMX681_REG_X_ADDR_END		CCI_REG16(0x0348)
#define IMX681_REG_Y_ADDR_END		CCI_REG16(0x034a)
#define IMX681_REG_X_OUTPUT_SIZE	CCI_REG16(0x034c)
#define IMX681_REG_Y_OUTPUT_SIZE	CCI_REG16(0x034e)
#define IMX681_REG_DIGITAL_CROP_WIDTH	CCI_REG16(0x040c)
#define IMX681_REG_DIGITAL_CROP_HEIGHT	CCI_REG16(0x040e)

/* PLL, as the Windows traces configure it and the part reads back. */
#define IMX681_PLL_MULTIPLIER		225
#define IMX681_OP_PRE_PLL_CLK_DIV	3
#define IMX681_OP_PLL_MULTIPLIER	303

/*
 * Where the read-out window sits in the pixel array, and how much array
 * there is. The bounds are the part's own answer: X_ADDR_MIN, X_ADDR_MAX,
 * Y_ADDR_MIN and Y_ADDR_MAX at the CCS addresses read 8, 4039, 64 and 3087,
 * so 4032x3024 of readable pixels at (8, 64), and the window below sits
 * inside that. What lies beyond the readable area is not documented, so the
 * native size is the smallest rectangle that contains it.
 */
#define IMX681_CROP_X_START		100
#define IMX681_CROP_Y_START		256
#define IMX681_PIXEL_ARRAY_LEFT		8
#define IMX681_PIXEL_ARRAY_TOP		64
#define IMX681_PIXEL_ARRAY_WIDTH	4032
#define IMX681_PIXEL_ARRAY_HEIGHT	3024
#define IMX681_NATIVE_WIDTH		4040
#define IMX681_NATIVE_HEIGHT		3088

/* Image dimensions — native sensor output */
#define IMX681_WIDTH			3844
#define IMX681_HEIGHT			2640
#define IMX681_LINE_LENGTH_PCK		7552	/* 0x1D80 */
#define IMX681_FRAME_LENGTH_LINES	3177	/* 0x0C69 */
#define IMX681_FRAME_LENGTH_MAX		0xFFFF	/* 24-bit reg, limit to 16-bit */

/* MIPI lanes */
#define IMX681_NUM_LANES		2

/*
 * Link frequency derived from PLL settings in Windows trace:
 * EXCK=19.2MHz, PLL2_MUL=303, PLL2_PRE_DIV=3
 * OP output = 19.2 * 303 / 3 = 1939.2 MHz (MIPI bit rate)
 * Link freq = 1939.2 / 2 (DDR) = 969.6 MHz
 */
#define IMX681_LINK_FREQ		969600000LL

/*
 * The pixel rate cannot be derived from the CSI-2 link: this part runs two
 * PLLs, and the pixel array is clocked from the video-timing one while the
 * link is clocked from the other. It cannot be derived from the video-timing
 * PLL either, because these registers are at the CCS addresses without
 * having the CCS meaning. Read back from the part at its operating point
 * they are PRE_PLL_CLK_DIV 2 and PLL_MULTIPLIER 225, so a 2160 MHz VCO, then
 * VT_SYS_CLK_DIV 2 and VT_PIX_CLK_DIV 6 - by the CCS formula 180 MHz, which
 * would be 7.5 fps. The sensor runs at 30.
 *
 * So it is measured: LINE_LENGTH_PCK x FRAME_LENGTH_LINES x the frame rate
 * the part achieves, which is 29.95 fps over 300 frames and 720 MHz.
 */
#define IMX681_PIXEL_RATE		720000000LL

/* Power-on delay after reset deassert */
#define IMX681_RESET_DELAY_US		1000
#define IMX681_RESET_DELAY_RANGE_US	1000

/* Post-standby-cancel stabilisation delays */
#define IMX681_INIT_DELAY_US		10000

#define IMAGE_PAD			0

static const s64 imx681_link_frequencies[] = {
	IMX681_LINK_FREQ,
};

/*
 * Sensor init register sequence, captured from Windows I2C traces.
 * This configures the sensor for 3844x2640 RAW10 output at ~30fps
 * with 2-lane MIPI CSI-2, 19.2MHz input clock.
 */
static const struct cci_reg_sequence imx681_init_regs[] = {
	{ IMX681_REG_EXTCLK_FREQ_MHZ, IMX681_XCLK_FREQ_ENC },
	/* Vendor specific configuration */
	{ CCI_REG16(0x002C), 0x0505 },
	/* 0x02 is D-PHY, which is the only mode this part offers. */
	{ IMX681_REG_CSI_SIGNALING_MODE, 0x02 },
	/* Vendor access unlock sequence */
	{ CCI_REG8(0x30EB),  0x05 },
	{ CCI_REG8(0x30EB),  0x0C },
	/* Vendor specific */
	{ CCI_REG16(0x300A), 0xFFFF },
	{ CCI_REG16(0x3532), 0xFFFF },
	/*
	 * The analogue crop, its digital counterpart and the output size, at
	 * the CCS addresses. The traces wrote these as overlapping partial
	 * writes - 0x0345 and 0x0349 carry only the low byte of a 16-bit
	 * register, so the window was right only while the high bytes held
	 * their reset values. Written whole here, and the values read back
	 * from the part at the operating point agree: the window is
	 * 3943 - 100 + 1 by 2895 - 256 + 1, which is the output size.
	 */
	{ IMX681_REG_X_ADDR_START, IMX681_CROP_X_START },
	{ IMX681_REG_Y_ADDR_START, IMX681_CROP_Y_START },
	{ IMX681_REG_X_ADDR_END, IMX681_CROP_X_START + IMX681_WIDTH - 1 },
	{ IMX681_REG_Y_ADDR_END, IMX681_CROP_Y_START + IMX681_HEIGHT - 1 },
	{ IMX681_REG_DIGITAL_CROP_WIDTH, IMX681_WIDTH },
	{ IMX681_REG_DIGITAL_CROP_HEIGHT, IMX681_HEIGHT },
	{ IMX681_REG_X_OUTPUT_SIZE, IMX681_WIDTH },
	{ IMX681_REG_Y_OUTPUT_SIZE, IMX681_HEIGHT },
	/*
	 * Both of these were partial writes in the traces too: 0x0307 is the
	 * low byte of PLL_MULTIPLIER and 0x030d the low byte of
	 * OP_PRE_PLL_CLK_DIV, and each was right only while its high byte
	 * held the reset value. Written whole, and the part reads back
	 * 225, 3 and 303 at the operating point either way.
	 */
	{ IMX681_REG_PLL_MULTIPLIER, IMX681_PLL_MULTIPLIER },
	{ IMX681_REG_OP_PRE_PLL_CLK_DIV, IMX681_OP_PRE_PLL_CLK_DIV },
	{ IMX681_REG_OP_PLL_MULTIPLIER, IMX681_OP_PLL_MULTIPLIER },
	/* Vendor specific registers */
	{ CCI_REG8(0x7E9B),  0x02 },
	{ CCI_REG8(0x0368),  0x00 },
	{ CCI_REG8(0xD383),  0x01 },
};

/*
 * The CCS numbering, which this part follows: 1 solid colour, 2 colour bars,
 * 3 bars fading to grey. Mode 4, PN9, is not offered because it produces a
 * black frame on this sensor where 2 and 3 produce their patterns, and
 * TEST_PATTERN_CAPABILITY reads zero here so it cannot be asked.
 */
static const char * const imx681_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Eight Vertical Colour Bars",
	"Colour Bars With Fade to Grey",
};

/*
 * The pixel array reads out RGGB. Mirroring moves the Bayer phase by one
 * column and flipping by one row, so the code the sensor delivers follows
 * the two controls: index = vflip << 1 | hflip.
 */
static const u32 imx681_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
};

/* Regulator supplies */
static const char * const imx681_supply_names[] = {
	"avdd",		/* Analog 2.8V */
	"dvdd",		/* Digital 1.05V */
	"dovdd",	/* I/O 1.8V */
};

struct imx681 {
	struct device *dev;
	struct regmap *cci;

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx681_supply_names)];

	/* V4L2 Controls */
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	unsigned long link_freq_bitmap;
};

static inline struct imx681 *to_imx681(struct v4l2_subdev *sd)
{
	return container_of_const(sd, struct imx681, sd);
}

static int imx681_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx681 *imx681 = container_of(ctrl->handler, struct imx681,
					     ctrl_handler);
	s64 exposure_max;
	int pm_status;
	int ret = 0;

	/* Update exposure max when VBLANK changes (even when not streaming) */
	if (ctrl->id == V4L2_CID_VBLANK) {
		exposure_max = IMX681_HEIGHT + ctrl->val - IMX681_EXPOSURE_MARGIN;
		ret = __v4l2_ctrl_modify_range(imx681->exposure,
					       IMX681_EXPOSURE_MIN, exposure_max,
					       1, IMX681_EXPOSURE_DEFAULT);
		if (ret)
			return ret;
	}

	/*
	 * 1 with a reference taken, 0 if the device is not active, or -EINVAL
	 * if runtime PM is unavailable. Only the 0 means there is nothing to
	 * do: without runtime PM the sensor is powered from probe and never
	 * suspended, so the write still has to go out - but no reference was
	 * taken then, and none may be dropped.
	 */
	pm_status = pm_runtime_get_if_active(imx681->dev);
	if (!pm_status)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		ret = cci_write(imx681->cci, IMX681_REG_FRAME_LENGTH,
				IMX681_HEIGHT + ctrl->val, NULL);
		break;

	case V4L2_CID_EXPOSURE:
		ret = cci_write(imx681->cci, IMX681_REG_EXPOSURE, ctrl->val,
				NULL);
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		/* Gain formula: gain = 1024/(1024-code); code 960 is 16x. */
		ret = cci_write(imx681->cci, IMX681_REG_ANALOG_GAIN, ctrl->val,
				NULL);
		break;

	case V4L2_CID_DIGITAL_GAIN:
		ret = cci_write(imx681->cci, IMX681_REG_DIGITAL_GAIN, ctrl->val,
				NULL);
		break;

	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		/*
		 * Read from imx681_start_streaming(). These are grabbed while
		 * streaming, so this runs only when the part is idle and the
		 * value is applied at the next stream start.
		 */
		break;

	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(imx681->cci, IMX681_REG_TEST_PATTERN,
				ctrl->val, NULL);
		break;

	case V4L2_CID_TEST_PATTERN_RED:
		ret = cci_write(imx681->cci, IMX681_REG_TESTP_RED, ctrl->val,
				NULL);
		break;

	case V4L2_CID_TEST_PATTERN_GREENR:
		ret = cci_write(imx681->cci, IMX681_REG_TESTP_GREENR,
				ctrl->val, NULL);
		break;

	case V4L2_CID_TEST_PATTERN_BLUE:
		ret = cci_write(imx681->cci, IMX681_REG_TESTP_BLUE, ctrl->val,
				NULL);
		break;

	case V4L2_CID_TEST_PATTERN_GREENB:
		ret = cci_write(imx681->cci, IMX681_REG_TESTP_GREENB,
				ctrl->val, NULL);
		break;

	default:
		break;
	}

	if (pm_status > 0)
		pm_runtime_put(imx681->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx681_ctrl_ops = {
	.s_ctrl = imx681_set_ctrl,
};

static u32 imx681_get_format_code(struct imx681 *imx681)
{
	return imx681_mbus_codes[(imx681->vflip->val ? 2 : 0) |
				 (imx681->hflip->val ? 1 : 0)];
}

static int imx681_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct imx681 *imx681 = to_imx681(sd);

	if (code->index > 0)
		return -EINVAL;

	code->code = imx681_get_format_code(imx681);

	return 0;
}

static bool imx681_is_valid_mbus_code(struct imx681 *imx681, u32 code)
{
	return code == imx681_get_format_code(imx681);
}

static int imx681_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index > 0)
		return -EINVAL;

	if (!imx681_is_valid_mbus_code(to_imx681(sd), fse->code))
		return -EINVAL;

	fse->min_width = IMX681_WIDTH;
	fse->max_width = IMX681_WIDTH;
	fse->min_height = IMX681_HEIGHT;
	fse->max_height = IMX681_HEIGHT;

	return 0;
}

static int imx681_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_state_get_format(state, IMAGE_PAD);
	format->width = IMX681_WIDTH;
	format->height = IMX681_HEIGHT;
	format->code = imx681_get_format_code(to_imx681(sd));
	format->field = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_RAW;
	format->ycbcr_enc = V4L2_YCBCR_ENC_601;
	format->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	format->xfer_func = V4L2_XFER_FUNC_NONE;

	return 0;
}

/*
 * There is nothing to configure on the part here - the size is fixed and the
 * code follows the flips - but the stored format still has to be told which
 * Bayer order the flips have produced, or the pipeline validates the state
 * against a code the sensor is no longer delivering.
 */
static int imx681_set_pad_format(struct v4l2_subdev *sd,
				 const struct v4l2_subdev_client_info *ci,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx681 *imx681 = to_imx681(sd);
	struct v4l2_mbus_framefmt *format;

	fmt->format.width = IMX681_WIDTH;
	fmt->format.height = IMX681_HEIGHT;
	fmt->format.code = imx681_get_format_code(imx681);
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	format = v4l2_subdev_state_get_format(state, fmt->pad);
	*format = fmt->format;

	return 0;
}

static int imx681_get_selection(struct v4l2_subdev *sd,
				const struct v4l2_subdev_client_info *ci,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.left = IMX681_CROP_X_START;
		sel->r.top = IMX681_CROP_Y_START;
		sel->r.width = IMX681_WIDTH;
		sel->r.height = IMX681_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = IMX681_PIXEL_ARRAY_LEFT;
		sel->r.top = IMX681_PIXEL_ARRAY_TOP;
		sel->r.width = IMX681_PIXEL_ARRAY_WIDTH;
		sel->r.height = IMX681_PIXEL_ARRAY_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX681_NATIVE_WIDTH;
		sel->r.height = IMX681_NATIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx681_start_streaming(struct imx681 *imx681)
{
	int ret;

	/*
	 * The orientation goes in before the register list, because the crop
	 * window in it is interpreted in the orientation then in force. The
	 * downstream driver had this write inside the list, ahead of the crop
	 * registers, for the same reason.
	 */
	ret = cci_write(imx681->cci, IMX681_REG_ORIENTATION,
			imx681->hflip->val | imx681->vflip->val << 1, NULL);
	if (ret)
		return ret;

	ret = cci_multi_reg_write(imx681->cci, imx681_init_regs,
				  ARRAY_SIZE(imx681_init_regs), NULL);
	if (ret) {
		dev_err(imx681->dev, "failed to write init regs: %d\n", ret);
		return ret;
	}

	/* Wait for sensor to stabilise after configuration */
	usleep_range(IMX681_INIT_DELAY_US, IMX681_INIT_DELAY_US + 1000);

	/*
	 * The blanking is the controls' to set, not the register list's:
	 * HBLANK gives the line length, and VBLANK the frame length through
	 * imx681_set_ctrl() below, along with the exposure and the gains.
	 */
	ret = cci_write(imx681->cci, IMX681_REG_LINE_LENGTH,
			IMX681_WIDTH + imx681->hblank->val, NULL);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_handler_setup(imx681->sd.ctrl_handler);
	if (ret) {
		dev_err(imx681->dev, "failed to apply controls: %d\n", ret);
		return ret;
	}

	/* Start streaming */
	ret = cci_write(imx681->cci, IMX681_REG_MODE_SELECT,
			IMX681_MODE_STREAMING, NULL);
	if (ret) {
		dev_err(imx681->dev, "failed to start streaming: %d\n", ret);
		return ret;
	}

	return 0;
}

static int imx681_stop_streaming(struct imx681 *imx681)
{
	int ret;

	ret = cci_write(imx681->cci, IMX681_REG_MODE_SELECT,
			IMX681_MODE_STANDBY, NULL);
	if (ret)
		dev_err(imx681->dev, "failed to stop streaming: %d\n", ret);

	return ret;
}

static int imx681_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct imx681 *imx681 = to_imx681(sd);
	int ret;

	ret = pm_runtime_resume_and_get(imx681->dev);
	if (ret)
		return ret;

	ret = imx681_start_streaming(imx681);
	if (ret) {
		pm_runtime_put_autosuspend(imx681->dev);
		return ret;
	}

	/*
	 * The flips are read once, in imx681_start_streaming(), and they
	 * choose the media bus code. Accepting a change now would report
	 * success, leave the part as it is and leave the stored format
	 * naming a code the sensor is no longer delivering.
	 */
	__v4l2_ctrl_grab(imx681->hflip, true);
	__v4l2_ctrl_grab(imx681->vflip, true);

	return 0;
}

static int imx681_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct imx681 *imx681 = to_imx681(sd);
	int ret;

	__v4l2_ctrl_grab(imx681->hflip, false);
	__v4l2_ctrl_grab(imx681->vflip, false);

	ret = imx681_stop_streaming(imx681);

	/* The reference goes back whether or not the part acknowledged. */
	pm_runtime_put_autosuspend(imx681->dev);

	return ret;
}

static const struct v4l2_subdev_video_ops imx681_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx681_pad_ops = {
	.enum_mbus_code = imx681_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx681_set_pad_format,
	.get_selection = imx681_get_selection,
	.enum_frame_size = imx681_enum_frame_size,
	.enable_streams = imx681_enable_streams,
	.disable_streams = imx681_disable_streams,
};

static const struct v4l2_subdev_ops imx681_subdev_ops = {
	.video = &imx681_video_ops,
	.pad = &imx681_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx681_internal_ops = {
	.init_state = imx681_init_state,
};

/* Power management */
static int imx681_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx681 *imx681 = to_imx681(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx681_supply_names), imx681->supplies);
	if (ret) {
		dev_err(imx681->dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(imx681->xclk);
	if (ret) {
		dev_err(imx681->dev, "failed to enable clock: %d\n", ret);
		goto err_reg_disable;
	}

	/* Deassert reset (active low) */
	gpiod_set_value_cansleep(imx681->reset_gpio, 0);

	usleep_range(IMX681_RESET_DELAY_US,
		     IMX681_RESET_DELAY_US + IMX681_RESET_DELAY_RANGE_US);

	return 0;

err_reg_disable:
	regulator_bulk_disable(ARRAY_SIZE(imx681_supply_names), imx681->supplies);

	return ret;
}

static int imx681_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx681 *imx681 = to_imx681(sd);

	/* Assert reset */
	gpiod_set_value_cansleep(imx681->reset_gpio, 1);
	clk_disable_unprepare(imx681->xclk);
	regulator_bulk_disable(ARRAY_SIZE(imx681_supply_names), imx681->supplies);

	return 0;
}

static int imx681_identify_module(struct imx681 *imx681)
{
	u64 val;
	int ret;

	ret = cci_read(imx681->cci, IMX681_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(imx681->dev, ret,
				     "failed to read chip ID register 0x0016\n");

	if (val != IMX681_CHIP_ID) {
		return dev_err_probe(imx681->dev, -EIO,
				     "chip ID mismatch: 0x%04llx != 0x%04x\n",
				     val, IMX681_CHIP_ID);
	}

	return 0;
}

static int imx681_init_controls(struct imx681 *imx681)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx681->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *link_freq;
	s64 hblank, vblank;
	int ret;

	ret = v4l2_fwnode_device_parse(imx681->dev, &props);
	if (ret)
		return ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 16);
	if (ret)
		return ret;

	/* Pixel rate (read-only) */
	v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
			  V4L2_CID_PIXEL_RATE, IMX681_PIXEL_RATE,
			  IMX681_PIXEL_RATE, 1, IMX681_PIXEL_RATE);

	/* Link frequency (read-only) */
	link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &imx681_ctrl_ops,
					   V4L2_CID_LINK_FREQ,
					   __fls(imx681->link_freq_bitmap),
					   __ffs(imx681->link_freq_bitmap),
					   imx681_link_frequencies);
	if (link_freq)
		link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* Horizontal blanking (read-only, fixed) */
	hblank = IMX681_LINE_LENGTH_PCK - IMX681_WIDTH;
	imx681->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank,
					   1, hblank);
	if (imx681->hblank)
		imx681->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* Vertical blanking (writable to allow longer exposures) */
	vblank = IMX681_FRAME_LENGTH_LINES - IMX681_HEIGHT;
	imx681->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   IMX681_FRAME_LENGTH_MAX - IMX681_HEIGHT,
					   1, vblank);

	/* Exposure */
	imx681->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX681_EXPOSURE_MIN,
					     IMX681_FRAME_LENGTH_LINES -
					     IMX681_EXPOSURE_MARGIN, 1,
					     IMX681_EXPOSURE_DEFAULT);

	/*
	 * Both flips move the Bayer phase, so the media bus code changes with
	 * them and the layout flag has to say so.
	 */
	imx681->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	imx681->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (imx681->hflip)
		imx681->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	if (imx681->vflip)
		imx681->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	/* Analog gain */
	v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX681_ANA_GAIN_MIN, IMX681_ANA_GAIN_MAX, 1,
			  IMX681_ANA_GAIN_DEFAULT);

	/* Digital gain */
	v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX681_DIG_GAIN_MIN, IMX681_DIG_GAIN_MAX, 1,
			  IMX681_DIG_GAIN_DEFAULT);

	/* Test pattern */
	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx681_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx681_test_pattern_menu) - 1,
				     0, 0, imx681_test_pattern_menu);

	/*
	 * The solid-colour mode shows whatever is in these, and they reset to
	 * zero - so without them the mode this driver advertises can only
	 * ever produce a black frame. Default to white, as imx219 does.
	 */
	v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
			  V4L2_CID_TEST_PATTERN_RED, IMX681_TESTP_COLOUR_MIN,
			  IMX681_TESTP_COLOUR_MAX, IMX681_TESTP_COLOUR_STEP,
			  IMX681_TESTP_COLOUR_MAX);
	v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
			  V4L2_CID_TEST_PATTERN_GREENR, IMX681_TESTP_COLOUR_MIN,
			  IMX681_TESTP_COLOUR_MAX, IMX681_TESTP_COLOUR_STEP,
			  IMX681_TESTP_COLOUR_MAX);
	v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
			  V4L2_CID_TEST_PATTERN_BLUE, IMX681_TESTP_COLOUR_MIN,
			  IMX681_TESTP_COLOUR_MAX, IMX681_TESTP_COLOUR_STEP,
			  IMX681_TESTP_COLOUR_MAX);
	v4l2_ctrl_new_std(ctrl_hdlr, &imx681_ctrl_ops,
			  V4L2_CID_TEST_PATTERN_GREENB, IMX681_TESTP_COLOUR_MIN,
			  IMX681_TESTP_COLOUR_MAX, IMX681_TESTP_COLOUR_STEP,
			  IMX681_TESTP_COLOUR_MAX);

	if (ctrl_hdlr->error) {
		ret = dev_err_probe(imx681->dev, ctrl_hdlr->error,
				    "control init failed\n");
		goto error;
	}

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx681_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	imx681->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int imx681_parse_endpoint(struct imx681 *imx681)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx681->dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(fwnode, 0, 0, 0);

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(imx681->dev, ret,
				     "failed to parse endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX681_NUM_LANES) {
		ret = dev_err_probe(imx681->dev, -EINVAL,
				    "expected %d data lanes, got %d\n",
				    IMX681_NUM_LANES,
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
		goto done;
	}

	ret = v4l2_link_freq_to_bitmap(imx681->dev,
				       bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx681_link_frequencies,
				       ARRAY_SIZE(imx681_link_frequencies),
				       &imx681->link_freq_bitmap);
	if (ret)
		ret = dev_err_probe(imx681->dev, ret,
				    "link frequency mismatch\n");

done:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int imx681_probe(struct i2c_client *client)
{
	struct imx681 *imx681;
	unsigned long xclk_freq;
	int ret;

	imx681 = devm_kzalloc(&client->dev, sizeof(*imx681), GFP_KERNEL);
	if (!imx681)
		return -ENOMEM;

	imx681->dev = &client->dev;

	/* Initialise V4L2 subdev */
	v4l2_i2c_subdev_init(&imx681->sd, client, &imx681_subdev_ops);

	/* Initialise CCI regmap for 16-bit register addresses */
	imx681->cci = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx681->cci))
		return dev_err_probe(imx681->dev, PTR_ERR(imx681->cci),
				     "failed to init CCI\n");

	imx681->xclk = devm_v4l2_sensor_clk_get(imx681->dev, NULL);
	if (IS_ERR(imx681->xclk))
		return dev_err_probe(imx681->dev, PTR_ERR(imx681->xclk),
				     "failed to get clock\n");

	/*
	 * Both PLLs in imx681_init_regs[] and the value written to
	 * EXTCLK_FREQUENCY_MHZ assume this rate.
	 */
	xclk_freq = clk_get_rate(imx681->xclk);
	if (xclk_freq != IMX681_XCLK_FREQ)
		return dev_err_probe(imx681->dev, -EINVAL,
				     "external clock %lu Hz not supported, expected %u Hz\n",
				     xclk_freq, IMX681_XCLK_FREQ);

	/* Get regulators */
	for (unsigned int i = 0; i < ARRAY_SIZE(imx681_supply_names); i++)
		imx681->supplies[i].supply = imx681_supply_names[i];

	ret = devm_regulator_bulk_get(imx681->dev, ARRAY_SIZE(imx681_supply_names),
				      imx681->supplies);
	if (ret)
		return dev_err_probe(imx681->dev, ret,
				     "failed to get regulators\n");

	/* Get reset GPIO (optional) */
	imx681->reset_gpio = devm_gpiod_get_optional(imx681->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx681->reset_gpio))
		return dev_err_probe(imx681->dev,
				     PTR_ERR(imx681->reset_gpio),
				     "failed to get reset GPIO\n");

	/* Parse CSI-2 endpoint */
	ret = imx681_parse_endpoint(imx681);
	if (ret)
		return dev_err_probe(imx681->dev, ret,
				     "endpoint parse failed\n");

	/* Power on and verify chip ID */
	ret = imx681_power_on(imx681->dev);
	if (ret)
		return dev_err_probe(imx681->dev, ret, "power on failed\n");

	ret = imx681_identify_module(imx681);
	if (ret)
		goto error_power_off;

	/* Enable runtime PM */
	pm_runtime_set_active(imx681->dev);
	pm_runtime_enable(imx681->dev);
	pm_runtime_set_autosuspend_delay(imx681->dev, 1000);
	pm_runtime_use_autosuspend(imx681->dev);

	/* Init V4L2 controls */
	ret = imx681_init_controls(imx681);
	if (ret)
		goto error_pm;

	/* Setup subdev */
	imx681->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx681->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx681->sd.internal_ops = &imx681_internal_ops;

	/* Init media entity */
	imx681->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx681->sd.entity, 1, &imx681->pad);
	if (ret) {
		ret = dev_err_probe(imx681->dev, ret,
				    "media entity init failed\n");
		goto error_handler_free;
	}

	imx681->sd.state_lock = imx681->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&imx681->sd);
	if (ret < 0) {
		ret = dev_err_probe(imx681->dev, ret,
				    "subdev init finalize failed\n");
		goto error_media_entity;
	}

	ret = v4l2_async_register_subdev_sensor(&imx681->sd);
	if (ret < 0) {
		ret = dev_err_probe(imx681->dev, ret,
				    "async register subdev failed\n");
		goto error_subdev_cleanup;
	}

	pm_runtime_idle(imx681->dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&imx681->sd);
error_media_entity:
	media_entity_cleanup(&imx681->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(imx681->sd.ctrl_handler);
error_pm:
	pm_runtime_dont_use_autosuspend(imx681->dev);
	pm_runtime_disable(imx681->dev);
	pm_runtime_set_suspended(imx681->dev);
error_power_off:
	imx681_power_off(imx681->dev);

	return ret;
}

static void imx681_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx681 *imx681 = to_imx681(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(&imx681->sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(imx681->sd.ctrl_handler);

	pm_runtime_disable(imx681->dev);
	if (!pm_runtime_status_suspended(imx681->dev)) {
		imx681_power_off(imx681->dev);
		pm_runtime_set_suspended(imx681->dev);
	}
	pm_runtime_dont_use_autosuspend(imx681->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(imx681_pm_ops, imx681_power_off,
				 imx681_power_on, NULL);

#ifdef CONFIG_ACPI
static const struct acpi_device_id imx681_acpi_ids[] = {
	{ "SONY0681" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(acpi, imx681_acpi_ids);
#endif

static const struct of_device_id imx681_dt_ids[] = {
	{ .compatible = "sony,imx681" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx681_dt_ids);

static struct i2c_driver imx681_i2c_driver = {
	.driver = {
		.name = "imx681",
		.pm = pm_ptr(&imx681_pm_ops),
		.acpi_match_table = ACPI_PTR(imx681_acpi_ids),
		.of_match_table = imx681_dt_ids,
	},
	.probe = imx681_probe,
	.remove = imx681_remove,
};
module_i2c_driver(imx681_i2c_driver);

MODULE_DESCRIPTION("Sony IMX681 CMOS Image Sensor Driver");
MODULE_AUTHOR("Andre Gilerson <andre.gilerson@gmail.com>");
MODULE_LICENSE("GPL");
