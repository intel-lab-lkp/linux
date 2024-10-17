// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2019 - 2024 Intel Corporation.

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

/* Chip ID */
#define AR0234_REG_CHIP_ID		CCI_REG16(0x3000)
#define AR0234_CHIP_ID			0x0a56

#define AR0234_REG_MODE_SELECT		CCI_REG16(0x301a)
#define AR0234_REG_VTS			CCI_REG16(0x300a)
#define AR0234_REG_LINE_LENGTH_PCK	CCI_REG16(0x300c)
#define AR0234_REG_EXPOSURE		CCI_REG16(0x3012)
#define AR0234_REG_ANALOG_GAIN		CCI_REG16(0x3060)
#define AR0234_REG_GLOBAL_GAIN		CCI_REG16(0x305e)
#define AR0234_REG_ORIENTATION		CCI_REG16(0x3040)
#define AR0234_REG_TEST_PATTERN		CCI_REG16(0x3070)

/* Timing Settings */
#define AR0234_REG_Y_ADDR_START		CCI_REG16(0x3002)
#define AR0234_REG_X_ADDR_START		CCI_REG16(0x3004)
#define AR0234_REG_Y_ADDR_END		CCI_REG16(0x3006)
#define AR0234_REG_X_ADDR_END		CCI_REG16(0x3008)
#define AR0234_REG_SMIA_TEST		CCI_REG16(0x3064)
#define AR0234_REG_DATAPATH_SELECT	CCI_REG16(0x306e)
#define AR0234_REG_X_ODD_INC		CCI_REG16(0x30a2)
#define AR0234_REG_Y_ODD_INC		CCI_REG16(0x30a6)
#define AR0234_REG_DATA_FORMAT		CCI_REG16(0x31ac)
#define AR0234_REG_SERIAL_FORMAT	CCI_REG16(0x31ae)
#define AR0234_REG_COMPANDING		CCI_REG16(0x31d0)
#define AR0234_REG_DIGITAL_CTRL		CCI_REG16(0x3786)

/* PLL Settings */
#define AR0234_REG_VT_PIX_CLK_DIV	CCI_REG16(0x302a)
#define AR0234_REG_VT_SYS_CLK_DIV	CCI_REG16(0x302c)
#define AR0234_REG_PRE_PLL_CLK_DIV	CCI_REG16(0x302e)
#define AR0234_REG_PLL_MULTIPLIER	CCI_REG16(0x3030)
#define AR0234_REG_OP_PIX_CLK_DIV	CCI_REG16(0x3036)
#define AR0234_REG_OP_SYS_CLK_DIV	CCI_REG16(0x3038)
#define AR0234_REG_DIGITAL_TEST		CCI_REG16(0x30b0)
#define AR0234_REG_MIPI_CNTRL		CCI_REG16(0x3354)
#define AR0234_REG_FRAME_PREAMBLE	CCI_REG16(0x31b0)
#define AR0234_REG_LINE_PREAMBLE	CCI_REG16(0x31b2)
#define AR0234_REG_MIPI_TIMING_0	CCI_REG16(0x31b4)
#define AR0234_REG_MIPI_TIMING_1	CCI_REG16(0x31b6)
#define AR0234_REG_MIPI_TIMING_2	CCI_REG16(0x31b8)
#define AR0234_REG_MIPI_TIMING_3	CCI_REG16(0x31ba)
#define AR0234_REG_MIPI_TIMING_4	CCI_REG16(0x31bc)

#define AR0234_REG_OPERATION_MODE	CCI_REG16(0x3082)
#define AR0234_REG_SEQ_CTRL_PORT	CCI_REG16(0x3088)
#define AR0234_REG_SEQ_DATA_PORT	CCI_REG16(0x3086)
#define AR0234_REG_NOISE_PEDESTAL	CCI_REG16(0x30fe)
#define AR0234_REG_AE_LUMA_TARGET	CCI_REG16(0x3102)
#define AR0234_REG_DELTA_DK_CONTROL	CCI_REG16(0x3180)
#define AR0234_REG_PIX_DEF_ID		CCI_REG16(0x31e0)

#define AR0234_EXPOSURE_MIN		0
#define AR0234_EXPOSURE_MAX_MARGIN	80
#define AR0234_EXPOSURE_STEP		1

#define AR0234_ANALOG_GAIN_MIN		0
#define AR0234_ANALOG_GAIN_MAX		0x7f
#define AR0234_ANALOG_GAIN_STEP		1
#define AR0234_ANALOG_GAIN_DEFAULT	0xe

#define AR0234_GLOBAL_GAIN_MIN		0
#define AR0234_GLOBAL_GAIN_MAX		0x7ff
#define AR0234_GLOBAL_GAIN_STEP		1
#define AR0234_GLOBAL_GAIN_DEFAULT	0x80

#define AR0234_NATIVE_WIDTH		1920
#define AR0234_NATIVE_HEIGHT		1080
#define AR0234_COMMON_WIDTH		1280
#define AR0234_COMMON_HEIGHT		960
#define AR0234_PIXEL_ARRAY_LEFT		320
#define AR0234_PIXEL_ARRAY_TOP		60
#define AR0234_ORIENTATION_HFLIP	BIT(14)
#define AR0234_ORIENTATION_VFLIP	BIT(15)

#define AR0234_VTS_DEFAULT		0x04c4
#define AR0234_VTS_MAX			0xffff
#define AR0234_HTS_DEFAULT		0x04c4
#define AR0234_PPL_DEFAULT		3498

#define AR0234_MODE_RESET		0x00d9
#define AR0234_MODE_STANDBY		0x2058
#define AR0234_MODE_STREAMING		0x205c

#define AR0234_PIXEL_RATE		128000000ULL
#define AR0234_XCLK_FREQ		19200000ULL

#define AR0234_TEST_PATTERN_DISABLE	0
#define AR0234_TEST_PATTERN_SOLID_COLOR	1
#define AR0234_TEST_PATTERN_COLOR_BARS	2
#define AR0234_TEST_PATTERN_GREY_COLOR	3
#define AR0234_TEST_PATTERN_WALKING	256

#define to_ar0234(_sd)	container_of(_sd, struct ar0234, sd)

struct ar0234_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

struct ar0234_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts_def;
	u32 code;
	/* Sensor register settings for this mode */
	const struct ar0234_reg_list reg_list;
};

static const struct cci_reg_sequence mode_1280x960_10bit_2lane[] = {
	/* Defect Correction */
	{ CCI_REG16(0x3f4c), 0x121f },
	{ CCI_REG16(0x3f4e), 0x121f },
	{ CCI_REG16(0x3f50), 0x0b81 },
	{ AR0234_REG_PIX_DEF_ID, 0x0003 },
	{ AR0234_REG_DIGITAL_TEST, 0x0028 },
	/* PLL Settings */
	{ AR0234_REG_VT_PIX_CLK_DIV, 0x0005 },
	{ AR0234_REG_VT_SYS_CLK_DIV, 0x0001 },
	{ AR0234_REG_PRE_PLL_CLK_DIV, 0x0003 },
	{ AR0234_REG_PLL_MULTIPLIER, 0x0032 },
	{ AR0234_REG_OP_PIX_CLK_DIV, 0x000a },
	{ AR0234_REG_OP_SYS_CLK_DIV, 0x0001 },
	{ AR0234_REG_DIGITAL_TEST, 0x0028 },
	{ AR0234_REG_FRAME_PREAMBLE, 0x0082 },
	{ AR0234_REG_LINE_PREAMBLE, 0x005c },
	{ AR0234_REG_MIPI_TIMING_0, 0x5248 },
	{ AR0234_REG_MIPI_TIMING_1, 0x3257 },
	{ AR0234_REG_MIPI_TIMING_2, 0x904b },
	{ AR0234_REG_MIPI_TIMING_3, 0x030b },
	{ AR0234_REG_MIPI_TIMING_4, 0x8e09 },	/* Continuous Clock Mode */
	{ AR0234_REG_MIPI_CNTRL, 0x002b },
	{ AR0234_REG_COMPANDING, 0x0000 },
	{ AR0234_REG_SERIAL_FORMAT, 0x0202 },
	/* Timing Settings */
	{ AR0234_REG_Y_ADDR_START, 0x0080 },
	{ AR0234_REG_X_ADDR_START, 0x0148 },
	{ AR0234_REG_Y_ADDR_END, 0x043f },
	{ AR0234_REG_X_ADDR_END, 0x0647 },
	{ AR0234_REG_SMIA_TEST, 0x1802 },
	{ AR0234_REG_VTS, 0x04c4 },
	{ AR0234_REG_LINE_LENGTH_PCK, 0x04c4 },
	{ AR0234_REG_OPERATION_MODE, 0x0003 },
	{ AR0234_REG_ORIENTATION, 0x0000 },
	{ AR0234_REG_X_ODD_INC, 0x0001 },
	{ AR0234_REG_Y_ODD_INC, 0x0001 },
	{ AR0234_REG_EXPOSURE, 0x010c },
	{ AR0234_REG_DIGITAL_CTRL, 0x0006 },
	{ AR0234_REG_DATA_FORMAT, 0x0a0a },
	{ AR0234_REG_DATAPATH_SELECT, 0x9010 },
	/* Recommended Common 1 */
	{ AR0234_REG_SEQ_CTRL_PORT, 0x8050 },
	{ AR0234_REG_SEQ_DATA_PORT, 0x9237 },
	{ CCI_REG16(0x3044), 0x0410 },
	{ CCI_REG16(0x3094), 0x03d4 },
	{ CCI_REG16(0x3096), 0x0280 },
	/* [2:0] fixed to 6 as recommended setting */
	{ CCI_REG16(0x30ba), 0x7606 },
	{ AR0234_REG_DIGITAL_TEST, 0x0028 },
	{ CCI_REG16(0x30ba), 0x7600 },
	{ AR0234_REG_NOISE_PEDESTAL, 0x002a },
	{ CCI_REG16(0x31de), 0x0410 },
	{ CCI_REG16(0x3ed6), 0x1435 },
	{ CCI_REG16(0x3ed8), 0x9865 },
	{ CCI_REG16(0x3eda), 0x7698 },
	{ CCI_REG16(0x3edc), 0x99ff },
	{ CCI_REG16(0x3ee2), 0xbb88 },
	{ CCI_REG16(0x3ee4), 0x8836 },
	{ CCI_REG16(0x3ef0), 0x1cf0 },
	{ CCI_REG16(0x3ef2), 0x0000 },
	{ CCI_REG16(0x3ef8), 0x6166 },
	{ CCI_REG16(0x3efa), 0x3333 },
	{ CCI_REG16(0x3efc), 0x6634 },
	/* Recommended Common 2 */
	{ AR0234_REG_SEQ_CTRL_PORT, 0x81ba },
	{ AR0234_REG_SEQ_DATA_PORT, 0x3d02 },
	{ CCI_REG16(0x3276), 0x05dc },
	{ CCI_REG16(0x3f00), 0x9d05 },
	{ CCI_REG16(0x3ed2), 0xfa86 },
	{ CCI_REG16(0x3eee), 0xa4fe },
	{ CCI_REG16(0x3ecc), 0x6e42 },
	{ CCI_REG16(0x3ecc), 0x0e42 },
	{ CCI_REG16(0x3eec), 0x0c0c },
	{ CCI_REG16(0x3ee8), 0xaae4 },
	{ CCI_REG16(0x3ee6), 0x3363 },
	{ CCI_REG16(0x3ee6), 0x3363 },
	{ CCI_REG16(0x3ee8), 0xaae4 },
	{ CCI_REG16(0x3ee8), 0xaae4 },
	{ AR0234_REG_DELTA_DK_CONTROL, 0xc24f },
	{ AR0234_REG_AE_LUMA_TARGET, 0x5000 },
	{ AR0234_REG_ANALOG_GAIN, 0x000d },
	/* Maximum Analog Coarse Gain 16x */
	{ CCI_REG16(0x3ed0), 0xff44 },
	{ CCI_REG16(0x3ed2), 0xaa86 },
	{ CCI_REG16(0x3ed4), 0x031f },
	{ CCI_REG16(0x3eee), 0xa4aa },
};

static const char * const ar0234_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
	"Solid Color",
	"Grey Color Bars",
	"Walking 1s",
};

static const int ar0234_test_pattern_val[] = {
	AR0234_TEST_PATTERN_DISABLE,
	AR0234_TEST_PATTERN_COLOR_BARS,
	AR0234_TEST_PATTERN_SOLID_COLOR,
	AR0234_TEST_PATTERN_GREY_COLOR,
	AR0234_TEST_PATTERN_WALKING,
};

static const s64 link_freq_menu_items[] = {
	360000000ULL,
};

static const struct ar0234_mode supported_modes[] = {
	{
		.width = AR0234_COMMON_WIDTH,
		.height = AR0234_COMMON_HEIGHT,
		.hts = AR0234_HTS_DEFAULT,
		.vts_def = AR0234_VTS_DEFAULT,
		.code = MEDIA_BUS_FMT_SGRBG10_1X10,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_1280x960_10bit_2lane),
			.regs = mode_1280x960_10bit_2lane,
		},
	},
};

struct ar0234 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrl_handler;

	/* V4L2 Controls */
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;
	struct regmap *regmap;
	unsigned long link_freq_bitmap;
	const struct ar0234_mode *cur_mode;
};

static int ar0234_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ar0234 *ar0234 =
		container_of(ctrl->handler, struct ar0234, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&ar0234->sd);
	s64 exposure_max, exposure_def;
	struct v4l2_subdev_state *state;
	const struct v4l2_mbus_framefmt *format;
	int ret;

	state = v4l2_subdev_get_locked_active_state(&ar0234->sd);
	format = v4l2_subdev_state_get_format(state, 0);

	/* Propagate change of current control to all related controls */
	if (ctrl->id == V4L2_CID_VBLANK) {
		/* Update max exposure while meeting expected vblanking */
		exposure_max = format->height + ctrl->val -
			       AR0234_EXPOSURE_MAX_MARGIN;
		exposure_def = format->height - AR0234_EXPOSURE_MAX_MARGIN;
		__v4l2_ctrl_modify_range(ar0234->exposure,
					 ar0234->exposure->minimum,
					 exposure_max, ar0234->exposure->step,
					 exposure_def);
	}

	/* V4L2 controls values will be applied only when power is already up */
	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(ar0234->regmap, AR0234_REG_ANALOG_GAIN,
				ctrl->val, NULL);
		break;

	case V4L2_CID_DIGITAL_GAIN:
		ret = cci_write(ar0234->regmap, AR0234_REG_GLOBAL_GAIN,
				ctrl->val, NULL);
		break;

	case V4L2_CID_EXPOSURE:
		ret = cci_write(ar0234->regmap, AR0234_REG_EXPOSURE,
				ctrl->val, NULL);
		break;

	case V4L2_CID_VBLANK:
		ret = cci_write(ar0234->regmap, AR0234_REG_VTS,
				ar0234->cur_mode->height + ctrl->val, NULL);
		break;

	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
	{
		u64 reg;

		ret = cci_read(ar0234->regmap, AR0234_REG_ORIENTATION,
			       &reg, NULL);
		if (ret)
			break;

		reg &= ~(AR0234_ORIENTATION_HFLIP |
			 AR0234_ORIENTATION_VFLIP);
		if (ar0234->hflip->val)
			reg |= AR0234_ORIENTATION_HFLIP;
		if (ar0234->vflip->val)
			reg |= AR0234_ORIENTATION_VFLIP;

		ret = cci_write(ar0234->regmap, AR0234_REG_ORIENTATION,
				reg, NULL);
		break;
	}
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(ar0234->regmap, AR0234_REG_TEST_PATTERN,
				ar0234_test_pattern_val[ctrl->val], NULL);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ar0234_ctrl_ops = {
	.s_ctrl = ar0234_set_ctrl,
};

static int ar0234_init_controls(struct ar0234 *ar0234)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ar0234->sd);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	s64 exposure_max, vblank_max, vblank_def, hblank;
	u32 link_freq_size;
	int ret;

	ctrl_hdlr = &ar0234->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 10);
	if (ret)
		return ret;

	link_freq_size = ARRAY_SIZE(link_freq_menu_items) - 1;
	ar0234->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr,
						   &ar0234_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   link_freq_size, 0,
						   link_freq_menu_items);
	if (ar0234->link_freq)
		ar0234->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(ctrl_hdlr, &ar0234_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  AR0234_ANALOG_GAIN_MIN, AR0234_ANALOG_GAIN_MAX,
			  AR0234_ANALOG_GAIN_STEP, AR0234_ANALOG_GAIN_DEFAULT);
	v4l2_ctrl_new_std(ctrl_hdlr, &ar0234_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  AR0234_GLOBAL_GAIN_MIN, AR0234_GLOBAL_GAIN_MAX,
			  AR0234_GLOBAL_GAIN_STEP, AR0234_GLOBAL_GAIN_DEFAULT);

	exposure_max = ar0234->cur_mode->vts_def - AR0234_EXPOSURE_MAX_MARGIN;
	ar0234->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &ar0234_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     AR0234_EXPOSURE_MIN, exposure_max,
					     AR0234_EXPOSURE_STEP,
					     exposure_max);

	v4l2_ctrl_new_std(ctrl_hdlr, &ar0234_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  AR0234_PIXEL_RATE, AR0234_PIXEL_RATE, 1,
			  AR0234_PIXEL_RATE);

	vblank_max = AR0234_VTS_MAX - ar0234->cur_mode->height;
	vblank_def = ar0234->cur_mode->vts_def - ar0234->cur_mode->height;
	ar0234->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &ar0234_ctrl_ops,
					   V4L2_CID_VBLANK, 0, vblank_max, 1,
					   vblank_def);
	hblank = AR0234_PPL_DEFAULT - ar0234->cur_mode->width;
	ar0234->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &ar0234_ctrl_ops,
					   V4L2_CID_HBLANK, hblank, hblank, 1,
					   hblank);
	if (ar0234->hblank)
		ar0234->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ar0234->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &ar0234_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	ar0234->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &ar0234_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &ar0234_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ar0234_test_pattern_menu) - 1,
				     0, 0, ar0234_test_pattern_menu);

	if (ctrl_hdlr->error)
		return ctrl_hdlr->error;

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		return ret;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &ar0234_ctrl_ops,
					      &props);
	if (ret)
		return ret;

	ar0234->sd.ctrl_handler = ctrl_hdlr;

	return 0;
}

static void ar0234_update_pad_format(const struct ar0234_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = mode->code;
	fmt->field = V4L2_FIELD_NONE;
}

static int ar0234_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct ar0234 *ar0234 = to_ar0234(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&ar0234->sd);
	const struct ar0234_reg_list *reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret)
		return ret;

	/*
	 * Setting 0x301A.bit[0] will initiate a reset sequence:
	 * the frame being generated will be truncated.
	 */
	ret = cci_write(ar0234->regmap, AR0234_REG_MODE_SELECT,
			AR0234_MODE_RESET, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to reset: %d\n", ret);
		goto err_rpm_put;
	}

	usleep_range(1000, 1500);

	reg_list = &ar0234->cur_mode->reg_list;
	ret = cci_multi_reg_write(ar0234->regmap, reg_list->regs,
				  reg_list->num_of_regs, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to set mode: %d\n", ret);
		goto err_rpm_put;
	}

	ret = __v4l2_ctrl_handler_setup(ar0234->sd.ctrl_handler);
	if (ret)
		goto err_rpm_put;

	ret = cci_write(ar0234->regmap, AR0234_REG_MODE_SELECT,
			AR0234_MODE_STREAMING, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to start stream: %d\n", ret);
		goto err_rpm_put;
	}

	return 0;

err_rpm_put:
	pm_runtime_put(&client->dev);
	return ret;
}

static int ar0234_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct ar0234 *ar0234 = to_ar0234(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&ar0234->sd);
	int ret;

	ret = cci_write(ar0234->regmap, AR0234_REG_MODE_SELECT,
			AR0234_MODE_STANDBY, NULL);
	if (ret < 0)
		dev_err(&client->dev, "failed to stop stream: %d\n", ret);

	pm_runtime_put(&client->dev);

	return 0;
}

static int ar0234_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_format *fmt)
{
	struct ar0234 *ar0234 = to_ar0234(sd);
	struct i2c_client *client = v4l2_get_subdevdata(&ar0234->sd);
	struct v4l2_rect *crop;
	const struct ar0234_mode *mode;
	s64 hblank;
	int ret;

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width,
				      fmt->format.height);

	crop = v4l2_subdev_state_get_crop(sd_state, fmt->pad);
	crop->width = mode->width;
	crop->height = mode->height;

	ar0234_update_pad_format(mode, &fmt->format);
	*v4l2_subdev_state_get_format(sd_state, fmt->pad) = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	ar0234->cur_mode = mode;

	hblank = AR0234_PPL_DEFAULT - mode->width;
	ret = __v4l2_ctrl_modify_range(ar0234->hblank, hblank, hblank,
				       1, hblank);
	if (ret) {
		dev_err(&client->dev, "HB ctrl range update failed");
		return ret;
	}

	/* Update limits and set FPS to default */
	ret = __v4l2_ctrl_modify_range(ar0234->vblank, 0,
				       AR0234_VTS_MAX - mode->height, 1,
				       mode->vts_def - mode->height);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "VB ctrl range update failed");

	ret = __v4l2_ctrl_s_ctrl(ar0234->vblank, mode->vts_def - mode->height);
	if (ret)
		return dev_err_probe(&client->dev, ret, "VB ctrl set failed");

	return 0;
}

static int ar0234_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SGRBG10_1X10;

	return 0;
}

static int ar0234_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SGRBG10_1X10)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int ar0234_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = AR0234_PIXEL_ARRAY_TOP;
		sel->r.left = AR0234_PIXEL_ARRAY_LEFT;
		sel->r.width = AR0234_COMMON_WIDTH;
		sel->r.height = AR0234_COMMON_HEIGHT;
		break;

	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		break;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = AR0234_NATIVE_WIDTH;
		sel->r.height = AR0234_NATIVE_HEIGHT;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int ar0234_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = MEDIA_BUS_FMT_SGRBG10_1X10,
			.width = AR0234_COMMON_WIDTH,
			.height = AR0234_COMMON_HEIGHT,
		},
	};

	return ar0234_set_format(sd, sd_state, &fmt);
}

static const struct v4l2_subdev_video_ops ar0234_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops ar0234_pad_ops = {
	.set_fmt = ar0234_set_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.enable_streams = ar0234_enable_streams,
	.disable_streams = ar0234_disable_streams,
	.enum_mbus_code = ar0234_enum_mbus_code,
	.enum_frame_size = ar0234_enum_frame_size,
	.get_selection = ar0234_get_selection,
};

static const struct v4l2_subdev_core_ops ar0234_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops ar0234_subdev_ops = {
	.core = &ar0234_core_ops,
	.video = &ar0234_video_ops,
	.pad = &ar0234_pad_ops,
};

static const struct media_entity_operations ar0234_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops ar0234_internal_ops = {
	.init_state = ar0234_init_state,
};

static int ar0234_parse_fwnode(struct ar0234 *ar0234, struct device *dev)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	int ret;

	endpoint =
		fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
						FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found");
		return -EPROBE_DEFER;
	}

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &bus_cfg);
	if (ret) {
		dev_err(dev, "parsing endpoint node failed");
		goto out_err;
	}

	/* Check the number of MIPI CSI2 data lanes */
	if (bus_cfg.bus.mipi_csi2.num_data_lanes != 2 &&
	    bus_cfg.bus.mipi_csi2.num_data_lanes != 4) {
		dev_err(dev, "only 2 or 4 data lanes are currently supported");
		goto out_err;
	}

	ret = v4l2_link_freq_to_bitmap(dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       link_freq_menu_items,
				       ARRAY_SIZE(link_freq_menu_items),
				       &ar0234->link_freq_bitmap);
	if (ret)
		goto out_err;

out_err:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	fwnode_handle_put(endpoint);
	return ret;
}

static int ar0234_identify_module(struct ar0234 *ar0234)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ar0234->sd);
	int ret;
	u64 val;

	ret = cci_read(ar0234->regmap, AR0234_REG_CHIP_ID, &val, NULL);
	if (ret)
		return ret;

	if (val != AR0234_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%llx",
			AR0234_CHIP_ID, val);
		return -ENXIO;
	}

	return 0;
}

static void ar0234_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ar0234 *ar0234 = to_ar0234(sd);

	v4l2_async_unregister_subdev(&ar0234->sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&ar0234->sd.entity);
	v4l2_ctrl_handler_free(&ar0234->ctrl_handler);
	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static int ar0234_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ar0234 *ar0234;
	struct clk *xclk;
	u32 xclk_freq;
	int ret;

	ar0234 = devm_kzalloc(&client->dev, sizeof(*ar0234), GFP_KERNEL);
	if (!ar0234)
		return -ENOMEM;

	ret = ar0234_parse_fwnode(ar0234, dev);
	if (ret)
		return ret;

	ar0234->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(ar0234->regmap))
		return dev_err_probe(dev, PTR_ERR(ar0234->regmap),
				     "failed to init CCI");

	v4l2_i2c_subdev_init(&ar0234->sd, client, &ar0234_subdev_ops);

	xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(xclk)) {
		if (PTR_ERR(xclk) != -EPROBE_DEFER)
			dev_err(dev, "failed to get xclk %ld", PTR_ERR(xclk));
		return PTR_ERR(xclk);
	}

	xclk_freq = clk_get_rate(xclk);
	if (xclk_freq != AR0234_XCLK_FREQ) {
		dev_err(dev, "xclk frequency not supported: %d Hz", xclk_freq);
		return -EINVAL;
	}

	/* Check module identity */
	ret = ar0234_identify_module(ar0234);
	if (ret) {
		dev_err(dev, "failed to find sensor: %d", ret);
		return ret;
	}

	ar0234->cur_mode = &supported_modes[0];
	ret = ar0234_init_controls(ar0234);
	if (ret) {
		dev_err(&client->dev, "failed to init controls: %d", ret);
		goto probe_error_v4l2_ctrl_handler_free;
	}

	ar0234->sd.internal_ops = &ar0234_internal_ops;
	ar0234->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	ar0234->sd.entity.ops = &ar0234_subdev_entity_ops;
	ar0234->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ar0234->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&ar0234->sd.entity, 1, &ar0234->pad);
	if (ret) {
		dev_err(&client->dev, "failed to init entity pads: %d", ret);
		goto probe_error_v4l2_ctrl_handler_free;
	}

	ar0234->sd.state_lock = ar0234->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&ar0234->sd);
	if (ret < 0) {
		dev_err(dev, "v4l2 subdev init error: %d", ret);
		goto probe_error_media_entity_cleanup;
	}

	/*
	 * Device is already turned on by i2c-core with ACPI domain PM.
	 * Enable runtime PM and turn off the device.
	 */
	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	ret = v4l2_async_register_subdev_sensor(&ar0234->sd);
	if (ret < 0) {
		dev_err(&client->dev, "failed to register V4L2 subdev: %d",
			ret);
		goto probe_error_rpm;
	}

	return 0;
probe_error_rpm:
	pm_runtime_disable(&client->dev);
	v4l2_subdev_cleanup(&ar0234->sd);

probe_error_media_entity_cleanup:
	media_entity_cleanup(&ar0234->sd.entity);

probe_error_v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(ar0234->sd.ctrl_handler);

	return ret;
}

static const struct acpi_device_id ar0234_acpi_ids[] = {
	{ "INTC10C0" },
	{}
};
MODULE_DEVICE_TABLE(acpi, ar0234_acpi_ids);

static struct i2c_driver ar0234_i2c_driver = {
	.driver = {
		.name = "ar0234",
		.acpi_match_table = ACPI_PTR(ar0234_acpi_ids),
	},
	.probe = ar0234_probe,
	.remove = ar0234_remove,
};

module_i2c_driver(ar0234_i2c_driver);

MODULE_DESCRIPTION("ON Semiconductor ar0234 sensor driver");
MODULE_AUTHOR("Dongcheng Yan <dongcheng.yan@intel.com>");
MODULE_AUTHOR("Hao Yao <hao.yao@intel.com>");
MODULE_LICENSE("GPL");
