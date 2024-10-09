// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the AR0233 camera sensor from ON Semiconductor
 *
 * Written by Alexander Shiyan <eagle.alexander923@gmail.com>
 *
 * Some parts of code taken from ar0144.c by:
 * Copyright (C) 2024 Ideas on Board Oy
 * Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * Some parts of code taken from imx335.c by:
 * Copyright (C) 2021 Intel Corporation
 */

#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define AR0233_FRAME_STATUS					CCI_REG16(0x2008)
#	define AR0233_FRAME_STATUS_STANDBY			BIT(1)
#define AR0233_CHIP_VERSION_REG					CCI_REG16(0x3000)
#	define AR0233_CHIP_VERSION_AR0233			(0x0956)
#define AR0233_Y_ADDR_START					CCI_REG16(0x3002)
#define AR0233_X_ADDR_START					CCI_REG16(0x3004)
#define AR0233_Y_ADDR_END					CCI_REG16(0x3006)
#define AR0233_X_ADDR_END					CCI_REG16(0x3008)
#define AR0233_FRAME_LENGTH_LINES				CCI_REG16(0x300a)
#	define AR0233_MIN_FRAME_LENGTH_LINES			(40)
#	define AR0233_MAX_FRAME_LENGTH_LINES			(65535)
#define AR0233_LINE_LENGTH_PCK					CCI_REG16(0x300c)
#	define AR0233_MIN_LINE_LENGTH_PCK			(1840)
#	define AR0233_MAX_LINE_LENGTH_PCK			(65534)
#define AR0233_REVISION_NUMBER					CCI_REG16(0x300e)
#define AR0233_COARSE_INTEGRATION_TIME				CCI_REG16(0x3012)
#define AR0233_RESET_REGISTER					CCI_REG16(0x301a)
#	define AR0233_RESET_LPF_ENABLE				BIT(13)
#	define AR0233_RESET_REGISTER_SMIA_SERIALIZER_DIS	BIT(12)
#	define AR0233_RESET_REGISTER_MASK_BAD			BIT(9)
#	define AR0233_RESET_REGISTER_STANDBY_EOF		BIT(4)
#	define AR0233_RESET_REGISTER_LOCK_REG			BIT(3)
#	define AR0233_RESET_REGISTER_STREAM			BIT(2)
#define	AR0233_MODE_SELECT					CCI_REG8(0x301c)
#define AR0233_IMAGE_ORIENTATION				CCI_REG8(0x301d)
#	define AR0233_ORIENTATION_VERT_FLIP			BIT(1)
#	define AR0233_ORIENTATION_HORIZ_MIRROR			BIT(0)
#define AR0233_SOFTWARE_RESET					CCI_REG8(0x3021)
#	define AR0233_RESET					BIT(0)
#define AR0233_GROUPED_PARAMETER_HOLD				CCI_REG8(0x3022)
#	define AR0233_RESET_REGISTER_GROUPED_PARAMETER_HOLD	BIT(0)
#define AR0233_VT_PIX_CLK_DIV					CCI_REG16(0x302a)
#define AR0233_VT_SYS_CLK_DIV					CCI_REG16(0x302c)
#	define FASTER_CLK_DIV(x)				((x) << 8)
#define AR0233_PRE_PLL_CLK_DIV					CCI_REG16(0x302e)
#define AR0233_PLL_MULTIPLIER					CCI_REG16(0x3030)
#define AR0233_OP_PIX_CLK_DIV					CCI_REG16(0x3036)
#define AR0233_OP_SYS_CLK_DIV					CCI_REG16(0x3038)
#define AR0233_GLOBAL_GAIN					CCI_REG16(0x305e)
#define AR0233_TEST_PATTERN_MODE				CCI_REG16(0x3070)
#	define AR0233_TEST_PATTERN_NONE				(0)
#	define AR0233_TEST_PATTERN_SOLID			(1)
#	define AR0233_TEST_PATTERN_BARS				(2)
#	define AR0233_TEST_PATTERN_BARS_FADE			(3)
#	define AR0233_TEST_PATTERN_WALKING_1S			(256)
#define AR0233_TEST_DATA_RED					CCI_REG16(0x3072)
#define AR0233_TEST_DATA_GREENR					CCI_REG16(0x3074)
#define AR0233_TEST_DATA_BLUE					CCI_REG16(0x3076)
#define AR0233_TEST_DATA_GREENB					CCI_REG16(0x3078)
#define AR0233_DATA_FORMAT_BITS					CCI_REG16(0x31ac)
#	define AR0233_DATA_FORMAT_RAW(x)			((x) << 8)
#	define AR0233_DATA_FORMAT_OUTPUT(x)			((x) << 0)
#define AR0233_SERIAL_FORMAT					CCI_REG16(0x31ae)
#	define AR0233_SERIAL_FORMAT_TYPE_MIPI			(2 << 8)
#	define AR0233_SERIAL_FORMAT_LANES(x)			((x) << 0)
#define AR0233_CUSTOMER_REV					CCI_REG16(0x31fe)
#define AR0233_MIPI_F1_PDT_EDT					CCI_REG16(0x3342)
#	define AR0233_MIPI_Fx_EDT(x)				((x) << 8)
#	define AR0233_MIPI_Fx_PDT(x)				((x) << 0)
#define AR0233_MIPI_F1_VDT_VC					CCI_REG16(0x3344)
#	define AR0233_MIPI_Fx_VC(x)				((x) << 8)
#	define AR0233_MIPI_Fx_VDT(x)				((x) << 0)
#define AR0233_MIPI_F2_PDT_EDT					CCI_REG16(0x3346)
#define AR0233_MIPI_F2_VDT_VC					CCI_REG16(0x3348)
#define AR0233_MIPI_F3_PDT_EDT					CCI_REG16(0x334a)
#define AR0233_MIPI_F3_VDT_VC					CCI_REG16(0x334c)
#define AR0233_MIPI_F4_PDT_EDT					CCI_REG16(0x334e)
#define AR0233_MIPI_F4_VDT_VC					CCI_REG16(0x3350)
#define AR0233_ANALOG_GAIN					CCI_REG16(0x3366)

#define AR0233_MIN_HBLANK			(400)
#define AR0233_MIN_VBLANK			(AR0233_MIN_FRAME_LENGTH_LINES)
#define AR0233_ACT_WIDTH			(2048)
#define AR0233_ACT_HEIGHT			(1280)
#define AR0233_MIN_WIDTH			(32)
#define AR0233_MIN_HEIGHT			(32)
#define AR0233_DEF_WIDTH			(1920)
#define AR0233_DEF_HEIGHT			(1080)

/* Offsets to sensor optical center */
#define AR0233_X_OFFSET				((2064 - AR0233_ACT_WIDTH) / 2)
#define AR0233_Y_OFFSET				((1288 - AR0233_ACT_HEIGHT) / 2)

struct ar0233_format_info {
	u32 format;
	u16 bpp_input;	/* Legal values are 12, 14, 16, 18, 20, 24 */
	u16 bpp_output;	/* Legal values are 10, 12, 14, 16, 20, 24 */
	u16 datatype;
};

static const struct ar0233_format_info ar0233_formats[] = {
	{
		.format = MEDIA_BUS_FMT_SGRBG12_1X12,
		.bpp_input = 14,
		.bpp_output = 12,
		.datatype = MIPI_CSI2_DT_RAW12,
	},
};

static const char * const ar0233_supplies[] = {
	"vaa",		/* Analog supply */
	"vaapix",	/* Pixel supply */
	"vddio",	/* I/O Digital supply */
	"vddphy",	/* PHY Digital supply */
	"vdd",		/* Core Digital supply */
};

struct ar0233 {
	struct device *dev;

	u16 customer_rev;

	struct regmap *cci;
	struct clk *clk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(ar0233_supplies)];

	u64 pixel_rate;

	u16 pll_div;
	u16 pll_mul;
	u16 vt_pix_clk_div;
	u16 vt_sys_clk_div;
	u16 op_pix_clk_div;
	u16 op_sys_clk_div;

	struct v4l2_fwnode_endpoint bus_cfg;

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *test_data[4];
	struct {
		struct v4l2_ctrl *hflip;
		struct v4l2_ctrl *vflip;
	};
};

static inline struct ar0233 *to_ar0233(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ar0233, sd);
}

static const struct ar0233_format_info *
	ar0233_format_info(struct ar0233 *sensor, u32 code, bool use_def)
{
	const struct ar0233_format_info *def = NULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ar0233_formats); ++i) {
		const struct ar0233_format_info *info = &ar0233_formats[i];

		if (info->format == code)
			return info;

		if (!def)
			def = info;
	}

	return use_def ? def : NULL;
}

static int ar0233_configure_pll(struct ar0233 *sensor)
{
	int ret = 0;

	if (sensor->pll_mul & 1)
		dev_warn_once(sensor->dev,
			"Odd PLL multiplier, Link frequency will not be exact\n");

	cci_write(sensor->cci, AR0233_PRE_PLL_CLK_DIV,
		  sensor->pll_div, &ret);
	cci_write(sensor->cci, AR0233_PLL_MULTIPLIER,
		  sensor->pll_mul, &ret);
	cci_write(sensor->cci, AR0233_VT_PIX_CLK_DIV,
		  sensor->vt_pix_clk_div, &ret);
	cci_write(sensor->cci, AR0233_VT_SYS_CLK_DIV,
		  FASTER_CLK_DIV(7) | sensor->vt_sys_clk_div, &ret);
	cci_write(sensor->cci, AR0233_OP_PIX_CLK_DIV,
		  sensor->op_pix_clk_div, &ret);
	cci_write(sensor->cci, AR0233_OP_SYS_CLK_DIV,
		  sensor->op_sys_clk_div, &ret);

	/* Wait 1ms for the PLL to lock */
	fsleep(1000);

	return ret;
}

static int ar0233_configure_mipi(struct ar0233 *sensor,
				 const struct ar0233_format_info *info)
{
	const u32 edt = AR0233_MIPI_Fx_EDT(MIPI_CSI2_DT_EMBEDDED_8B);
	const u32 pdt = AR0233_MIPI_Fx_PDT(info->datatype);
	const u32 vdt = AR0233_MIPI_Fx_VDT(MIPI_CSI2_DT_BLANKING);
	unsigned int num_lanes = sensor->bus_cfg.bus.mipi_csi2.num_data_lanes;
	int ret = 0;

	cci_write(sensor->cci, AR0233_SERIAL_FORMAT,
		  AR0233_SERIAL_FORMAT_TYPE_MIPI |
		  AR0233_SERIAL_FORMAT_LANES(num_lanes), &ret);

	cci_write(sensor->cci, AR0233_MIPI_F1_PDT_EDT, edt | pdt, &ret);
	cci_write(sensor->cci, AR0233_MIPI_F1_VDT_VC,
		  AR0233_MIPI_Fx_VC(0) | vdt, &ret);
	cci_write(sensor->cci, AR0233_MIPI_F2_PDT_EDT, edt | pdt, &ret);
	cci_write(sensor->cci, AR0233_MIPI_F2_VDT_VC,
		  AR0233_MIPI_Fx_VC(0) | vdt, &ret);
	cci_write(sensor->cci, AR0233_MIPI_F3_PDT_EDT, edt | pdt, &ret);
	cci_write(sensor->cci, AR0233_MIPI_F3_VDT_VC,
		  AR0233_MIPI_Fx_VC(0) | vdt, &ret);
	cci_write(sensor->cci, AR0233_MIPI_F4_PDT_EDT, edt | pdt, &ret);
	cci_write(sensor->cci, AR0233_MIPI_F4_VDT_VC,
		  AR0233_MIPI_Fx_VC(0) | vdt, &ret);

	return ret;
}

static int ar0233_start_streaming(struct ar0233 *sensor,
				  const struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *format;
	const struct ar0233_format_info *info;
	const struct v4l2_rect *crop;
	int ret;
	u16 val;

	format = v4l2_subdev_state_get_format(state, 0);
	crop = v4l2_subdev_state_get_crop(state, 0);
	info = ar0233_format_info(sensor, format->code, true);

	/* For REV1, 24 bit is not operational (OUTPUT) */
	if ((sensor->customer_rev < 2) && (info->bpp_output == 24)) {
		dev_err(sensor->dev, "The sensor does not support 24bpp.\n");
		return -EINVAL;
	}

	ret = ar0233_configure_pll(sensor);
	if (ret)
		return ret;

	cci_write(sensor->cci, AR0233_DATA_FORMAT_BITS,
		  AR0233_DATA_FORMAT_RAW(info->bpp_input) |
		  AR0233_DATA_FORMAT_OUTPUT(info->bpp_output), &ret);

	cci_write(sensor->cci, AR0233_X_ADDR_START,
		  crop->left + AR0233_X_OFFSET, &ret);
	cci_write(sensor->cci, AR0233_Y_ADDR_START,
		  crop->top + AR0233_Y_OFFSET, &ret);
	cci_write(sensor->cci, AR0233_X_ADDR_END,
		  crop->left + crop->width + AR0233_X_OFFSET - 1, &ret);
	cci_write(sensor->cci, AR0233_Y_ADDR_END,
		  crop->top + crop->height + AR0233_Y_OFFSET - 1, &ret);

	if (ret)
		return ret;

	ret = __v4l2_ctrl_handler_setup(&sensor->ctrls);
	if (ret)
		return ret;

	ret = ar0233_configure_mipi(sensor, info);

	val = AR0233_RESET_LPF_ENABLE;
	val |= AR0233_RESET_REGISTER_MASK_BAD;
	val |= AR0233_RESET_REGISTER_STANDBY_EOF;
	val |= AR0233_RESET_REGISTER_LOCK_REG;
	val |= AR0233_RESET_REGISTER_STREAM;
	cci_write(sensor->cci, AR0233_RESET_REGISTER, val, &ret);

	return ret;
}

#define ar0233_read_poll_timeout(sensor, addr, bit)			\
({									\
	int __ret, __err = 0;						\
	u64 __val = 0;							\
									\
	__ret = read_poll_timeout(cci_read, __err,			\
				  __err || (__val & (bit)), 200,	\
				  2000000, false, (sensor)->cci,	\
				  addr,	&__val, NULL);			\
									\
	__ret ? : __err;						\
})

static int ar0233_stop_streaming(struct ar0233 *sensor)
{
	int ret = 0;

	/* Initiate the transition to standby by clearing the STREAM bit */
	if (cci_write(sensor->cci, AR0233_MODE_SELECT, 0, &ret))
		return ret;

	ret = ar0233_read_poll_timeout(sensor, AR0233_FRAME_STATUS,
				       AR0233_FRAME_STATUS_STANDBY);
	if (ret)
		dev_warn(sensor->dev, "%s while trying to enter standby (%d)\n",
			 (ret == -ETIMEDOUT) ? "timeout" : "error", ret);

	/* Standby state reached, disable the output interface */
	return cci_write(sensor->cci, AR0233_RESET_REGISTER,
			 AR0233_RESET_REGISTER_SMIA_SERIALIZER_DIS, NULL);
}

static int ar0233_pll_calculate(struct ar0233 *sensor, unsigned int bpp)
{
	u64 link_freq = sensor->bus_cfg.link_frequencies[0] * 2;
	u16 num_lanes = sensor->bus_cfg.bus.mipi_csi2.num_data_lanes;
	u16 bpp_div = bpp / 2;

	sensor->pixel_rate = DIV_ROUND_CLOSEST_ULL(link_freq, bpp_div);

	if ((sensor->pixel_rate < 4000000ULL) ||
	    (sensor->pixel_rate > 125000000ULL)) {
		dev_err(sensor->dev, "Link frequency out of bounds\n");
		return -EINVAL;
	}

	sensor->vt_pix_clk_div = bpp_div;
	sensor->vt_sys_clk_div = 1 << (2 - __fls(num_lanes));
	sensor->op_pix_clk_div = bpp_div;
	sensor->op_sys_clk_div = 2;

	for (sensor->pll_div = 1; sensor->pll_div < 64; sensor->pll_div++) {
		u64 rate = DIV_ROUND_CLOSEST_ULL(clk_get_rate(sensor->clk),
						 sensor->pll_div);
		sensor->pll_mul = DIV_ROUND_DOWN_ULL(link_freq, rate);
		if ((rate * sensor->pll_mul) == link_freq)
			return 0;
	}

	return -EINVAL;
}

static int ar0233_pll_update(struct ar0233 *sensor,
			     const struct ar0233_format_info *info)
{
	int ret;

	ret = ar0233_pll_calculate(sensor, info->bpp_output);
	if (ret)
		dev_err(sensor->dev, "PLL calculations failed: %d\n", ret);
	else
		__v4l2_ctrl_s_ctrl_int64(sensor->rate, sensor->pixel_rate);

	return ret;
}

static const char * const ar0233_test_pattern_menu[] = {
	"Disabled",
	"Solid color",
	"Full Color Bars",
	"Fade to Gray Color Bars",
	"Walking 1",
};

static const u32 ar0233_test_pattern_values[] = {
	AR0233_TEST_PATTERN_NONE,
	AR0233_TEST_PATTERN_SOLID,
	AR0233_TEST_PATTERN_BARS,
	AR0233_TEST_PATTERN_BARS_FADE,
	AR0233_TEST_PATTERN_WALKING_1S,
};

static int ar0233_update_exposure(struct ar0233 *sensor,
				  const struct v4l2_rect *crop)
{
	const unsigned int max = crop->height + sensor->vblank->val - 1;

	return __v4l2_ctrl_modify_range(sensor->exposure, 1, max, 1, max);
}

static void ar0233_update_blanking(struct ar0233 *sensor,
				   const struct v4l2_subdev_state *state)
{
	const struct v4l2_rect *crop;
	unsigned int min, max;

	crop = v4l2_subdev_state_get_crop(state, 0);

	min = AR0233_MIN_HBLANK;
	max = AR0233_MAX_LINE_LENGTH_PCK - crop->width;
	__v4l2_ctrl_modify_range(sensor->hblank, min, max, 2, min);

	min = AR0233_MIN_VBLANK;
	max = AR0233_MAX_FRAME_LENGTH_LINES - crop->height;
	__v4l2_ctrl_modify_range(sensor->vblank, min, max, 1, min);

	ar0233_update_exposure(sensor, crop);
}

static int ar0233_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ar0233 *sensor = container_of(ctrl->handler, struct ar0233, ctrls);
	const struct v4l2_subdev_state *state;
	const struct v4l2_mbus_framefmt *format;
	const struct v4l2_rect *crop;
	int ret = 0;
	u16 reg;

	/*
	 * Return immediately for controls that don't need to be applied to the
	 * device.
	 */
	if (ctrl->flags & (V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_GRABBED))
		return 0;

	state = v4l2_subdev_get_locked_active_state(&sensor->sd);
	format = v4l2_subdev_state_get_format(state, 0);
	crop = v4l2_subdev_state_get_crop(state, 0);

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		ret = ar0233_update_exposure(sensor, crop);
		break;
	default:
		break;
	}

	if (ret)
		return ret;

	if (!pm_runtime_get_if_in_use(sensor->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		reg = ctrl->val << 12;
		reg |= ctrl->val << 8;
		reg |= ctrl->val << 4;
		reg |= ctrl->val << 0;
		cci_write(sensor->cci, AR0233_ANALOG_GAIN, reg, &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(sensor->cci, AR0233_GLOBAL_GAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_write(sensor->cci, AR0233_COARSE_INTEGRATION_TIME,
			  ctrl->val, &ret);
		break;
	case V4L2_CID_HBLANK:
		cci_write(sensor->cci, AR0233_LINE_LENGTH_PCK,
			  crop->width + ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(sensor->cci, AR0233_FRAME_LENGTH_LINES,
			  crop->height + ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(sensor->cci, AR0233_TEST_PATTERN_MODE,
			  ar0233_test_pattern_values[ctrl->val], &ret);
		break;
	case V4L2_CID_TEST_PATTERN_RED:
	case V4L2_CID_TEST_PATTERN_GREENR:
	case V4L2_CID_TEST_PATTERN_BLUE:
	case V4L2_CID_TEST_PATTERN_GREENB:
		cci_write(sensor->cci, AR0233_GROUPED_PARAMETER_HOLD,
			  AR0233_RESET_REGISTER_GROUPED_PARAMETER_HOLD, &ret);

		cci_write(sensor->cci, AR0233_TEST_DATA_RED,
			  sensor->test_data[0]->val, &ret);
		cci_write(sensor->cci, AR0233_TEST_DATA_GREENR,
			  sensor->test_data[1]->val, &ret);
		cci_write(sensor->cci, AR0233_TEST_DATA_BLUE,
			  sensor->test_data[2]->val, &ret);
		cci_write(sensor->cci, AR0233_TEST_DATA_GREENB,
			  sensor->test_data[3]->val, &ret);

		cci_write(sensor->cci, AR0233_GROUPED_PARAMETER_HOLD,
			  0, &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		reg = sensor->hflip->val ? AR0233_ORIENTATION_HORIZ_MIRROR : 0;
		reg |= sensor->vflip->val ? AR0233_ORIENTATION_VERT_FLIP : 0;

		cci_write(sensor->cci, AR0233_IMAGE_ORIENTATION, reg, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(sensor->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ar0233_ctrl_ops = {
	.s_ctrl = ar0233_s_ctrl,
};

static const u32 test_pattern_ctrls[] = {
	V4L2_CID_TEST_PATTERN_RED,
	V4L2_CID_TEST_PATTERN_GREENR,
	V4L2_CID_TEST_PATTERN_BLUE,
	V4L2_CID_TEST_PATTERN_GREENB,
};

static int ar0233_init_ctrls(struct ar0233 *sensor)
{
	struct v4l2_fwnode_device_properties props;
	unsigned int i;
	int ret;

	ret = v4l2_fwnode_device_parse(sensor->dev, &props);
	if (ret < 0)
		return ret;

	v4l2_ctrl_handler_init(&sensor->ctrls, 13);

	v4l2_ctrl_new_fwnode_properties(&sensor->ctrls, &ar0233_ctrl_ops,
					&props);

	sensor->rate = v4l2_ctrl_new_std(&sensor->ctrls, &ar0233_ctrl_ops,
					 V4L2_CID_PIXEL_RATE, 1, INT_MAX, 1, 1);

	sensor->hblank =
		v4l2_ctrl_new_std(&sensor->ctrls, &ar0233_ctrl_ops,
				  V4L2_CID_HBLANK, AR0233_MIN_HBLANK,
				  AR0233_MIN_HBLANK, 2, AR0233_MIN_HBLANK);

	sensor->vblank =
		v4l2_ctrl_new_std(&sensor->ctrls, &ar0233_ctrl_ops,
				  V4L2_CID_VBLANK, AR0233_MIN_VBLANK,
				  AR0233_MIN_VBLANK, 1, AR0233_MIN_VBLANK);

	sensor->exposure =
		v4l2_ctrl_new_std(&sensor->ctrls, &ar0233_ctrl_ops,
				  V4L2_CID_EXPOSURE, 1,
				  AR0233_MAX_FRAME_LENGTH_LINES - 1, 1,
				  AR0233_DEF_HEIGHT + AR0233_MIN_VBLANK - 1);

	v4l2_ctrl_new_std(&sensor->ctrls, &ar0233_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN, 0, 5, 1, 2);

	v4l2_ctrl_new_std(&sensor->ctrls, &ar0233_ctrl_ops,
			  V4L2_CID_DIGITAL_GAIN, 0, 0x7ff, 1, 0x080);

	v4l2_ctrl_new_std_menu_items(&sensor->ctrls, &ar0233_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ar0233_test_pattern_menu) - 1,
				     0, 0, ar0233_test_pattern_menu);

	for (i = 0; i < ARRAY_SIZE(test_pattern_ctrls); ++i)
		sensor->test_data[i] =
			v4l2_ctrl_new_std(&sensor->ctrls, &ar0233_ctrl_ops,
					  test_pattern_ctrls[i], 0, 4095, 1, 0);

	sensor->hflip = v4l2_ctrl_new_std(&sensor->ctrls, &ar0233_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);

	sensor->vflip = v4l2_ctrl_new_std(&sensor->ctrls, &ar0233_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);

	if (sensor->ctrls.error) {
		ret = sensor->ctrls.error;
		v4l2_ctrl_handler_free(&sensor->ctrls);
		return ret;
	}

	v4l2_ctrl_cluster(4, sensor->test_data);
	v4l2_ctrl_cluster(2, &sensor->hflip);

	sensor->sd.ctrl_handler = &sensor->ctrls;

	return 0;
}

static int ar0233_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(ar0233_formats))
		return -EINVAL;

	code->code = ar0233_formats[code->index].format;

	return 0;
}

static int ar0233_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct ar0233 *sensor = to_ar0233(sd);
	const struct ar0233_format_info *info;

	info = ar0233_format_info(sensor, fse->code, false);
	if (!info)
		return -EINVAL;

	if (fse->index >= ARRAY_SIZE(ar0233_formats))
		return -EINVAL;

	fse->min_width = AR0233_MIN_WIDTH;
	fse->max_width = AR0233_ACT_WIDTH;
	fse->min_height = AR0233_MIN_HEIGHT;
	fse->max_height = AR0233_ACT_HEIGHT;

	return 0;
}

static int ar0233_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct ar0233 *sensor = to_ar0233(sd);
	const struct ar0233_format_info *info;
	struct v4l2_mbus_framefmt *fmt;
	const struct v4l2_rect *crop;

	if (v4l2_subdev_is_streaming(sd) &&
	    (format->which == V4L2_SUBDEV_FORMAT_ACTIVE))
		return -EBUSY;

	fmt = v4l2_subdev_state_get_format(state, format->pad);
	crop = v4l2_subdev_state_get_crop(state, format->pad);
	info = ar0233_format_info(sensor, format->format.code, true);
	fmt->code = info->format;

	fmt->width = clamp(format->format.width, 1U, crop->width);
	fmt->height = clamp(format->format.height, 1U, crop->height);

	format->format = *fmt;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		ar0233_pll_update(sensor, info);

	return 0;
}

static int ar0233_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, sel->pad);
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = (AR0233_ACT_WIDTH - AR0233_DEF_WIDTH) / 2;
		sel->r.top = (AR0233_ACT_HEIGHT - AR0233_DEF_HEIGHT) / 2;
		sel->r.width = AR0233_DEF_WIDTH;
		sel->r.height = AR0233_DEF_HEIGHT;
		break;
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = AR0233_ACT_WIDTH;
		sel->r.height = AR0233_ACT_HEIGHT;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int ar0233_set_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_rect *crop;

	if (v4l2_subdev_is_streaming(sd) &&
	    (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE))
		return -EBUSY;

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	crop = v4l2_subdev_state_get_crop(state, 0);
	fmt = v4l2_subdev_state_get_format(state, 0);

	crop->left = min_t(unsigned int, ALIGN(sel->r.left, 2),
			   AR0233_ACT_WIDTH - AR0233_MIN_WIDTH);
	crop->top = min_t(unsigned int, ALIGN(sel->r.top, 2),
			  AR0233_ACT_HEIGHT - AR0233_MIN_HEIGHT);
	crop->width = clamp(sel->r.width, AR0233_MIN_WIDTH,
			    AR0233_ACT_WIDTH - crop->left);
	crop->height = clamp(sel->r.height, AR0233_MIN_HEIGHT,
			     AR0233_ACT_HEIGHT - crop->top);

	sel->r = *crop;

	fmt->width = crop->width;
	fmt->height = crop->height;

	return 0;
}

static int ar0233_get_frame_desc(struct v4l2_subdev *sd, unsigned int pad,
				 struct v4l2_mbus_frame_desc *fd)
{
	struct ar0233 *sensor = to_ar0233(sd);
	const struct ar0233_format_info *info;
	const struct v4l2_mbus_framefmt *fmt;
	struct v4l2_subdev_state *state;
	u32 code;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	fmt = v4l2_subdev_state_get_format(state, 0);
	code = fmt->code;
	v4l2_subdev_unlock_state(state);

	info = ar0233_format_info(sensor, code, true);

	fd->type = V4L2_MBUS_FRAME_DESC_TYPE_CSI2;
	fd->num_entries = 1;

	fd->entry[0].pixelcode = code;
	fd->entry[0].stream = 0;
	fd->entry[0].bus.csi2.vc = 0;
	fd->entry[0].bus.csi2.dt = info->datatype;

	return 0;
}

static int ar0233_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct ar0233 *sensor = to_ar0233(sd);
	int ret;

	ret = pm_runtime_resume_and_get(sensor->dev);
	if (ret)
		return ret;

	ret = ar0233_start_streaming(sensor, state);
	if (ret) {
		dev_err(sensor->dev, "Failed to start streaming: %d\n", ret);
		pm_runtime_put_sync(sensor->dev);
	}

	return ret;
}

static int ar0233_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct ar0233 *sensor = to_ar0233(sd);

	ar0233_stop_streaming(sensor);
	pm_runtime_put(sensor->dev);

	return 0;
}

static int ar0233_entity_init_state(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state)
{
	const struct ar0233_format_info *info;
	struct ar0233 *sensor = to_ar0233(sd);
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_rect *crop;

	info = ar0233_format_info(sensor, 0, true);

	fmt = v4l2_subdev_state_get_format(state, 0);
	fmt->width = AR0233_DEF_WIDTH;
	fmt->height = AR0233_DEF_HEIGHT;
	fmt->code = info->format;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;

	crop = v4l2_subdev_state_get_crop(state, 0);
	crop->left = 0;
	crop->top = 0;
	crop->width = AR0233_DEF_WIDTH;
	crop->height = AR0233_DEF_HEIGHT;

	return 0;
}

static const struct v4l2_subdev_pad_ops ar0233_subdev_pad_ops = {
	.enum_mbus_code = ar0233_enum_mbus_code,
	.enum_frame_size = ar0233_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ar0233_set_fmt,
	.get_selection = ar0233_get_selection,
	.set_selection = ar0233_set_selection,
	.get_frame_desc = ar0233_get_frame_desc,
	.enable_streams = ar0233_enable_streams,
	.disable_streams = ar0233_disable_streams,
};

static const struct v4l2_subdev_ops ar0233_subdev_ops = {
	.pad = &ar0233_subdev_pad_ops,
};

static const struct v4l2_subdev_internal_ops ar0233_subdev_internal_ops = {
	.init_state = ar0233_entity_init_state,
};

static const struct media_entity_operations ar0233_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int ar0233_init_subdev(struct ar0233 *sensor)
{
	struct i2c_client *client = to_i2c_client(sensor->dev);
	struct v4l2_subdev *sd = &sensor->sd;
	const struct v4l2_mbus_framefmt *format;
	const struct ar0233_format_info *info;
	struct v4l2_subdev_state *state;
	int ret;

	v4l2_i2c_subdev_init(sd, client, &ar0233_subdev_ops);

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->internal_ops = &ar0233_subdev_internal_ops;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sd->entity.ops = &ar0233_entity_ops;

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, 1, &sensor->pad);
	if (ret)
		return ret;

	ret = ar0233_init_ctrls(sensor);
	if (ret)
		goto err_entity;

	sensor->sd.state_lock = sensor->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto err_ctrls;

	state = v4l2_subdev_lock_and_get_active_state(sd);
	format = v4l2_subdev_state_get_format(state, 0);
	info = ar0233_format_info(sensor, format->code, true);

	ar0233_pll_update(sensor, info);
	ar0233_update_blanking(sensor, state);

	v4l2_subdev_unlock_state(state);

	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls);

err_entity:
	media_entity_cleanup(&sd->entity);

	return ret;
}

static void ar0233_cleanup_subdev(struct ar0233 *sensor)
{
	v4l2_subdev_cleanup(&sensor->sd);
	v4l2_ctrl_handler_free(&sensor->ctrls);
	media_entity_cleanup(&sensor->sd.entity);
}

static int ar0233_detect(struct ar0233 *sensor)
{
	int ret = 0;
	u64 val;

	if (cci_read(sensor->cci, AR0233_CHIP_VERSION_REG, &val, &ret)) {
		dev_err(sensor->dev, "Failed to read Chip ID: %d\n", ret);
		return ret;
	}

	if (val != AR0233_CHIP_VERSION_AR0233) {
		dev_err(sensor->dev, "Wrong Chip ID: 0x%04x\n", (u16)val);
		return -ENXIO;
	}

	if (!cci_read(sensor->cci, AR0233_CUSTOMER_REV, &val, &ret))
		sensor->customer_rev = val & 0xf;

	if (!cci_read(sensor->cci, AR0233_REVISION_NUMBER, &val, &ret))
		dev_info(sensor->dev,
			 "Found AR0233 chip, revision %d, customer rev %d\n",
			 (u16)val, sensor->customer_rev);

	return ret;
}

static int ar0233_power_on(struct ar0233 *sensor)
{
	unsigned long clk_rate;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ar0233_supplies),
				    sensor->supplies);
	if (ret) {
		dev_err(sensor->dev, "Failed to enable regulators\n");
		return ret;
	}

	ret = clk_prepare_enable(sensor->clk);
	if (ret) {
		dev_err(sensor->dev, "Failed to enable clock\n");
		regulator_bulk_disable(ARRAY_SIZE(ar0233_supplies),
				       sensor->supplies);
		return ret;
	}

	clk_rate = clk_get_rate(sensor->clk);
	if ((clk_rate < 6000000) || (clk_rate > 64000000)) {
		dev_err(sensor->dev, "Clock frequency out of bounds\n");
		clk_disable_unprepare(sensor->clk);
		regulator_bulk_disable(ARRAY_SIZE(ar0233_supplies),
				       sensor->supplies);
		return -EINVAL;
	}

	fsleep(1000);

	if (sensor->reset_gpio)
		gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	else
		cci_write(sensor->cci, AR0233_SOFTWARE_RESET, AR0233_RESET,
			  &ret);

	/* The typical internal initialization time is 236K Ext clk cycles */
	fsleep(DIV_ROUND_UP_ULL(236000ULL * USEC_PER_SEC, clk_rate));

	return ret;
}

static void ar0233_power_off(struct ar0233 *sensor)
{
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	clk_disable_unprepare(sensor->clk);
	regulator_bulk_disable(ARRAY_SIZE(ar0233_supplies), sensor->supplies);
}

static int ar0233_parse_dt(struct ar0233 *sensor)
{
	struct v4l2_fwnode_endpoint *ep = &sensor->bus_cfg;
	struct fwnode_handle *endpoint;
	unsigned int i;
	int ret;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(sensor->dev), NULL);
	if (!endpoint) {
		dev_err(sensor->dev, "Endpoint node not found\n");
		return -EINVAL;
	}

	ep->bus_type = V4L2_MBUS_UNKNOWN;
	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(sensor->dev, "Parsing endpoint node failed\n");
		goto ep_free;
	}

	switch (ep->bus_type) {
	case V4L2_MBUS_CSI2_DPHY:
		switch (ep->bus.mipi_csi2.num_data_lanes) {
		case 1:
		case 2:
		case 4:
			break;
		default:
			dev_err(sensor->dev, "Invalid data lanes count: %d\n",
				ep->bus.mipi_csi2.num_data_lanes);
			ret = -EINVAL;
			goto ep_free;
		}
		break;

	default:
		dev_err(sensor->dev, "Unsupported bus type %u\n", ep->bus_type);
		ret = -EINVAL;
		goto ep_free;
	}

	if (!ep->nr_of_link_frequencies) {
		dev_err(sensor->dev, "link-frequency property not found\n");
		ret = -EINVAL;
		goto ep_free;
	}

	for (i = 0; i < ARRAY_SIZE(ar0233_formats); i++) {
		const struct ar0233_format_info *info = &ar0233_formats[i];

		ret = ar0233_pll_calculate(sensor, info->bpp_output);
		if (ret) {
			dev_err(sensor->dev,
				"Link frequency %llu not valid for %u bpp\n",
				ep->link_frequencies[0], info->bpp_output);
			goto ep_free;
		}
	}

	return 0;

ep_free:
	v4l2_fwnode_endpoint_free(&sensor->bus_cfg);

	return ret;
}

static int ar0233_probe(struct i2c_client *client)
{
	struct ar0233 *sensor;
	unsigned int i;
	int ret;

	sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->dev = &client->dev;

	sensor->cci = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->cci))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->cci),
				     "Unable to initialize I2C\n");

	sensor->clk = devm_clk_get(sensor->dev, NULL);
	if (IS_ERR(sensor->clk))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->clk),
				     "Cannot get clock\n");

	sensor->reset_gpio = devm_gpiod_get_optional(sensor->dev, "reset",
						     GPIOD_OUT_LOW);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(sensor->dev, PTR_ERR(sensor->reset_gpio),
				     "Cannot get reset gpio\n");

	for (i = 0; i < ARRAY_SIZE(ar0233_supplies); i++)
		sensor->supplies[i].supply = ar0233_supplies[i];

	ret = devm_regulator_bulk_get(sensor->dev, ARRAY_SIZE(ar0233_supplies),
				      sensor->supplies);
	if (ret)
		return dev_err_probe(sensor->dev, ret, "Cannot get supplies\n");

	ret = ar0233_parse_dt(sensor);
	if (ret)
		return ret;

	ret = ar0233_power_on(sensor);
	if (ret) {
		dev_err_probe(sensor->dev, ret,
			      "Could not power on the device\n");
		goto err_dt;
	}

	ret = ar0233_detect(sensor);
	if (ret)
		goto err_power;

	ret = ar0233_init_subdev(sensor);
	if (ret) {
		dev_err(sensor->dev, "Subdev initialization error %d\n", ret);
		goto err_power;
	}

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret) {
		dev_err(sensor->dev, "Could not register V4L2 subdevice\n");
		goto err_subdev;
	}

	pm_runtime_set_active(sensor->dev);
	pm_runtime_enable(sensor->dev);
	pm_runtime_idle(sensor->dev);

	return 0;

err_subdev:
	ar0233_cleanup_subdev(sensor);

err_power:
	ar0233_power_off(sensor);

err_dt:
	v4l2_fwnode_endpoint_free(&sensor->bus_cfg);

	return ret;
}

static void ar0233_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ar0233 *sensor = to_ar0233(sd);

	v4l2_async_unregister_subdev(&sensor->sd);
	ar0233_cleanup_subdev(sensor);
	v4l2_fwnode_endpoint_free(&sensor->bus_cfg);

	pm_runtime_disable(sensor->dev);
	if (!pm_runtime_status_suspended(sensor->dev))
		ar0233_power_off(sensor);
	pm_runtime_set_suspended(sensor->dev);
}

static int __maybe_unused ar0233_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ar0233 *sensor = to_ar0233(sd);

	return ar0233_power_on(sensor);
}

static int __maybe_unused ar0233_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ar0233 *sensor = to_ar0233(sd);

	ar0233_power_off(sensor);

	return 0;
}

static const struct dev_pm_ops __maybe_unused ar0233_pm_ops = {
	RUNTIME_PM_OPS(ar0233_runtime_suspend, ar0233_runtime_resume, NULL)
};

static const struct of_device_id ar0233_of_match[] = {
	{ .compatible = "onnn,ar0233" },
	{ }
};
MODULE_DEVICE_TABLE(of, ar0233_of_match);

static struct i2c_driver ar0233_i2c_driver = {
	.driver = {
		.name = "ar0233",
		.of_match_table = of_match_ptr(ar0233_of_match),
		.pm = pm_ptr(&ar0233_pm_ops),
	},
	.probe = ar0233_probe,
	.remove = ar0233_remove,
};
module_i2c_driver(ar0233_i2c_driver);

MODULE_DESCRIPTION("Onsemi AR0233 Camera Sensor Driver");
MODULE_AUTHOR("Alexander Shiyan <eagle.alexander923@gmail.com>");
MODULE_LICENSE("GPL");
