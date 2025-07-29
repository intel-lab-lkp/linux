// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2024-2025 Linaro Ltd

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define OV6211_LINK_FREQ_240MHZ		240000000ULL
#define OV6211_MCLK_FREQ_24MHZ		24000000

#define OV6211_REG_CHIP_ID		CCI_REG16(0x300a)
#define OV6211_CHIP_ID			0x6710

#define OV6211_REG_MODE_SELECT		CCI_REG8(0x0100)
#define OV6211_MODE_STANDBY		0x00
#define OV6211_MODE_STREAMING		0x01

/* Exposure controls from sensor */
#define OV6211_REG_EXPOSURE		CCI_REG24(0x3500)
#define OV6211_EXPOSURE_MIN		1
#define OV6211_EXPOSURE_MAX_MARGIN	4
#define OV6211_EXPOSURE_STEP		1
#define OV6211_EXPOSURE_DEFAULT		210

/* Analogue gain controls from sensor */
#define OV6211_REG_ANALOGUE_GAIN	CCI_REG16(0x350a)
#define OV6211_ANALOGUE_GAIN_MIN	1
#define OV6211_ANALOGUE_GAIN_MAX	0x3ff
#define OV6211_ANALOGUE_GAIN_STEP	1
#define OV6211_ANALOGUE_GAIN_DEFAULT	0xa0

#define to_ov6211(_sd)			container_of(_sd, struct ov6211, sd)

struct ov6211_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct ov6211_mode {
	u32 width;	/* Frame width in pixels */
	u32 height;	/* Frame height in pixels */
	u32 hts;	/* Horizontal timing size */
	u32 vts;	/* Default vertical timing size */
	u32 bpp;	/* Bits per pixel */

	const struct ov6211_reg_list reg_list;	/* Sensor register setting */
};

static const s64 link_freq_menu_items[] = {
	OV6211_LINK_FREQ_240MHZ,
};

static const struct cci_reg_sequence ov6211_400x400_mode[] = {
	{ CCI_REG8(0x0103), 0x01 },
	{ CCI_REG8(0x0100), 0x00 },
	{ CCI_REG8(0x3005), 0x08 },
	{ CCI_REG8(0x3013), 0x12 },
	{ CCI_REG8(0x3014), 0x04 },
	{ CCI_REG8(0x3016), 0x10 },
	{ CCI_REG8(0x3017), 0x00 },
	{ CCI_REG8(0x3018), 0x00 },
	{ CCI_REG8(0x301a), 0x00 },
	{ CCI_REG8(0x301b), 0x00 },
	{ CCI_REG8(0x301c), 0x00 },
	{ CCI_REG8(0x3037), 0xf0 },
	{ CCI_REG8(0x3080), 0x01 },
	{ CCI_REG8(0x3081), 0x00 },
	{ CCI_REG8(0x3082), 0x01 },
	{ CCI_REG8(0x3098), 0x04 },
	{ CCI_REG8(0x3099), 0x28 },
	{ CCI_REG8(0x309a), 0x06 },
	{ CCI_REG8(0x309b), 0x04 },
	{ CCI_REG8(0x309c), 0x00 },
	{ CCI_REG8(0x309d), 0x00 },
	{ CCI_REG8(0x309e), 0x01 },
	{ CCI_REG8(0x309f), 0x00 },
	{ CCI_REG8(0x30b0), 0x08 },
	{ CCI_REG8(0x30b1), 0x02 },
	{ CCI_REG8(0x30b2), 0x00 },
	{ CCI_REG8(0x30b3), 0x28 },
	{ CCI_REG8(0x30b4), 0x02 },
	{ CCI_REG8(0x30b5), 0x00 },
	{ CCI_REG8(0x3106), 0xd9 },
	{ CCI_REG8(0x3500), 0x00 },
	{ CCI_REG8(0x3501), 0x0d },
	{ CCI_REG8(0x3502), 0x20 },
	{ CCI_REG8(0x3503), 0x07 },
	{ CCI_REG8(0x3509), 0x10 },
	{ CCI_REG8(0x350b), 0xa0 },
	{ CCI_REG8(0x3600), 0xfc },
	{ CCI_REG8(0x3620), 0xb7 },
	{ CCI_REG8(0x3621), 0x05 },
	{ CCI_REG8(0x3626), 0x31 },
	{ CCI_REG8(0x3627), 0x40 },
	{ CCI_REG8(0x3632), 0xa3 },
	{ CCI_REG8(0x3633), 0x34 },
	{ CCI_REG8(0x3634), 0x40 },
	{ CCI_REG8(0x3636), 0x00 },
	{ CCI_REG8(0x3660), 0x80 },
	{ CCI_REG8(0x3662), 0x03 },
	{ CCI_REG8(0x3664), 0xf0 },
	{ CCI_REG8(0x366a), 0x10 },
	{ CCI_REG8(0x366b), 0x06 },
	{ CCI_REG8(0x3680), 0xf4 },
	{ CCI_REG8(0x3681), 0x50 },
	{ CCI_REG8(0x3682), 0x00 },
	{ CCI_REG8(0x3708), 0x20 },
	{ CCI_REG8(0x3709), 0x40 },
	{ CCI_REG8(0x370d), 0x03 },
	{ CCI_REG8(0x373b), 0x02 },
	{ CCI_REG8(0x373c), 0x08 },
	{ CCI_REG8(0x3742), 0x00 },
	{ CCI_REG8(0x3744), 0x16 },
	{ CCI_REG8(0x3745), 0x08 },
	{ CCI_REG8(0x3781), 0xfc },
	{ CCI_REG8(0x3788), 0x00 },
	{ CCI_REG8(0x3800), 0x00 },
	{ CCI_REG8(0x3801), 0x04 },
	{ CCI_REG8(0x3802), 0x00 },
	{ CCI_REG8(0x3803), 0x04 },
	{ CCI_REG8(0x3804), 0x01 },
	{ CCI_REG8(0x3805), 0x9b },
	{ CCI_REG8(0x3806), 0x01 },
	{ CCI_REG8(0x3807), 0x9b },
	{ CCI_REG8(0x3808), 0x01 },
	{ CCI_REG8(0x3809), 0x90 },
	{ CCI_REG8(0x380a), 0x01 },
	{ CCI_REG8(0x380b), 0x90 },
	{ CCI_REG8(0x380c), 0x05 },
	{ CCI_REG8(0x380d), 0xf2 },
	{ CCI_REG8(0x380e), 0x01 },
	{ CCI_REG8(0x380f), 0xb6 },
	{ CCI_REG8(0x3810), 0x00 },
	{ CCI_REG8(0x3811), 0x04 },
	{ CCI_REG8(0x3812), 0x00 },
	{ CCI_REG8(0x3813), 0x04 },
	{ CCI_REG8(0x3814), 0x11 },
	{ CCI_REG8(0x3815), 0x11 },
	{ CCI_REG8(0x3820), 0x00 },
	{ CCI_REG8(0x3821), 0x00 },
	{ CCI_REG8(0x382b), 0xfa },
	{ CCI_REG8(0x382f), 0x04 },
	{ CCI_REG8(0x3832), 0x00 },
	{ CCI_REG8(0x3833), 0x05 },
	{ CCI_REG8(0x3834), 0x00 },
	{ CCI_REG8(0x3835), 0x05 },
	{ CCI_REG8(0x3882), 0x04 },
	{ CCI_REG8(0x3883), 0x00 },
	{ CCI_REG8(0x38a4), 0x10 },
	{ CCI_REG8(0x38a5), 0x00 },
	{ CCI_REG8(0x38b1), 0x03 },
	{ CCI_REG8(0x3b80), 0x00 },
	{ CCI_REG8(0x3b81), 0xff },
	{ CCI_REG8(0x3b82), 0x10 },
	{ CCI_REG8(0x3b83), 0x00 },
	{ CCI_REG8(0x3b84), 0x08 },
	{ CCI_REG8(0x3b85), 0x00 },
	{ CCI_REG8(0x3b86), 0x01 },
	{ CCI_REG8(0x3b87), 0x00 },
	{ CCI_REG8(0x3b88), 0x00 },
	{ CCI_REG8(0x3b89), 0x00 },
	{ CCI_REG8(0x3b8a), 0x00 },
	{ CCI_REG8(0x3b8b), 0x05 },
	{ CCI_REG8(0x3b8c), 0x00 },
	{ CCI_REG8(0x3b8d), 0x00 },
	{ CCI_REG8(0x3b8e), 0x01 },
	{ CCI_REG8(0x3b8f), 0xb2 },
	{ CCI_REG8(0x3b94), 0x05 },
	{ CCI_REG8(0x3b95), 0xf2 },
	{ CCI_REG8(0x3b96), 0xc0 },
	{ CCI_REG8(0x4004), 0x04 },
	{ CCI_REG8(0x404e), 0x01 },
	{ CCI_REG8(0x4801), 0x0f },
	{ CCI_REG8(0x4806), 0x0f },
	{ CCI_REG8(0x4837), 0x43 },
	{ CCI_REG8(0x5a08), 0x00 },
	{ CCI_REG8(0x5a01), 0x00 },
	{ CCI_REG8(0x5a03), 0x00 },
	{ CCI_REG8(0x5a04), 0x10 },
	{ CCI_REG8(0x5a05), 0xa0 },
	{ CCI_REG8(0x5a06), 0x0c },
	{ CCI_REG8(0x5a07), 0x78 },
};

static const struct ov6211_mode supported_modes[] = {
	{
		.width = 400,
		.height = 400,
		.hts = 1522,
		.vts = 438,
		.bpp = 8,
		.reg_list = {
			.regs = ov6211_400x400_mode,
			.num_regs = ARRAY_SIZE(ov6211_400x400_mode),
		},
	},
};

struct ov6211 {
	struct regmap *regmap;
	struct clk *xvclk;
	struct gpio_desc *reset_gpio;
	struct regulator *avdd;
	struct regulator *dovdd;
	struct regulator *dvdd;

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;

	const struct ov6211_mode *cur_mode;

	/* To serialize asynchronous callbacks */
	struct mutex mutex;
};

static int ov6211_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov6211 *ov6211 = container_of(ctrl->handler, struct ov6211,
					     ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&ov6211->sd);
	int ret;

	/* V4L2 controls values will be applied only when power is already up */
	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(ov6211->regmap, OV6211_REG_ANALOGUE_GAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(ov6211->regmap, OV6211_REG_EXPOSURE,
				ctrl->val << 4, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov6211_ctrl_ops = {
	.s_ctrl = ov6211_set_ctrl,
};

static int ov6211_init_controls(struct ov6211 *ov6211)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov6211->sd);
	struct v4l2_ctrl_handler *ctrl_hdlr = &ov6211->ctrl_handler;
	const struct ov6211_mode *cur_mode = ov6211->cur_mode;
	struct v4l2_fwnode_device_properties props;
	s64 exposure_max, pixel_rate, h_blank;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 8);
	if (ret)
		return ret;

	ctrl_hdlr->lock = &ov6211->mutex;

	ctrl = v4l2_ctrl_new_int_menu(ctrl_hdlr, &ov6211_ctrl_ops,
				      V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(link_freq_menu_items) - 1,
				      0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = link_freq_menu_items[0] * 2 / cur_mode->bpp;
	v4l2_ctrl_new_std(ctrl_hdlr, &ov6211_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  0, pixel_rate, 1, pixel_rate);

	h_blank = cur_mode->hts - cur_mode->width;
	ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &ov6211_ctrl_ops, V4L2_CID_HBLANK,
				 h_blank, h_blank, 1, h_blank);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &ov6211_ctrl_ops, V4L2_CID_VBLANK,
				 cur_mode->vts - cur_mode->height,
				 cur_mode->vts - cur_mode->height, 1,
				 cur_mode->vts - cur_mode->height);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(ctrl_hdlr, &ov6211_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  OV6211_ANALOGUE_GAIN_MIN, OV6211_ANALOGUE_GAIN_MAX,
			  OV6211_ANALOGUE_GAIN_STEP,
			  OV6211_ANALOGUE_GAIN_DEFAULT);

	exposure_max = (cur_mode->vts - OV6211_EXPOSURE_MAX_MARGIN);
	ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &ov6211_ctrl_ops,
				 V4L2_CID_EXPOSURE,
				 OV6211_EXPOSURE_MIN, exposure_max,
				 OV6211_EXPOSURE_STEP,
				 OV6211_EXPOSURE_DEFAULT);

	if (ctrl_hdlr->error)
		return ctrl_hdlr->error;

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &ov6211_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	ov6211->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static void ov6211_update_pad_format(const struct ov6211_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = MEDIA_BUS_FMT_Y8_1X8;
	fmt->field = V4L2_FIELD_NONE;
}

static int ov6211_start_streaming(struct ov6211 *ov6211)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov6211->sd);
	const struct ov6211_reg_list *reg_list;
	int ret;

	reg_list = &ov6211->cur_mode->reg_list;
	ret = cci_multi_reg_write(ov6211->regmap, reg_list->regs,
				  reg_list->num_regs, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to set mode: %d\n", ret);
		return ret;
	}

	ret = __v4l2_ctrl_handler_setup(ov6211->sd.ctrl_handler);
	if (ret)
		return ret;

	ret = cci_write(ov6211->regmap, OV6211_REG_MODE_SELECT,
			OV6211_MODE_STREAMING, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to start streaming: %d\n", ret);
		return ret;
	}

	return 0;
}

static void ov6211_stop_streaming(struct ov6211 *ov6211)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov6211->sd);
	int ret;

	ret = cci_write(ov6211->regmap, OV6211_REG_MODE_SELECT,
			OV6211_MODE_STANDBY, NULL);
	if (ret)
		dev_err(&client->dev, "failed to stop streaming: %d\n", ret);
}

static int ov6211_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov6211 *ov6211 = to_ov6211(sd);
	int ret = 0;

	mutex_lock(&ov6211->mutex);

	if (enable) {
		ret = pm_runtime_resume_and_get(&client->dev);
		if (ret) {
			mutex_unlock(&ov6211->mutex);
			return ret;
		}

		ret = ov6211_start_streaming(ov6211);
		if (!ret) {
			mutex_unlock(&ov6211->mutex);
			return 0;
		}
	}

	ov6211_stop_streaming(ov6211);
	pm_runtime_put(&client->dev);

	mutex_unlock(&ov6211->mutex);

	return ret;
}

static int ov6211_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_format *fmt)
{
	struct ov6211 *ov6211 = to_ov6211(sd);
	const struct ov6211_mode *mode;

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width,
				      fmt->format.height);

	mutex_lock(&ov6211->mutex);

	ov6211_update_pad_format(mode, &fmt->format);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		*v4l2_subdev_state_get_format(sd_state, fmt->pad) = fmt->format;
	else
		ov6211->cur_mode = mode;

	mutex_unlock(&ov6211->mutex);

	return 0;
}

static int ov6211_get_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_format *fmt)
{
	struct ov6211 *ov6211 = to_ov6211(sd);

	mutex_lock(&ov6211->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt->format = *v4l2_subdev_state_get_format(sd_state, fmt->pad);
	else
		ov6211_update_pad_format(ov6211->cur_mode, &fmt->format);

	mutex_unlock(&ov6211->mutex);

	return 0;
}

static int ov6211_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_Y8_1X8;

	return 0;
}

static int ov6211_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_Y8_1X8)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int ov6211_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ov6211 *ov6211 = to_ov6211(sd);

	mutex_lock(&ov6211->mutex);

	ov6211_update_pad_format(&supported_modes[0],
				 v4l2_subdev_state_get_format(fh->state, 0));

	mutex_unlock(&ov6211->mutex);

	return 0;
}

static const struct v4l2_subdev_video_ops ov6211_video_ops = {
	.s_stream = ov6211_set_stream,
};

static const struct v4l2_subdev_pad_ops ov6211_pad_ops = {
	.set_fmt = ov6211_set_format,
	.get_fmt = ov6211_get_format,
	.enum_mbus_code = ov6211_enum_mbus_code,
	.enum_frame_size = ov6211_enum_frame_size,
};

static const struct v4l2_subdev_ops ov6211_subdev_ops = {
	.video = &ov6211_video_ops,
	.pad = &ov6211_pad_ops,
};

static const struct media_entity_operations ov6211_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops ov6211_internal_ops = {
	.open = ov6211_open,
};

static int ov6211_identify_module(struct ov6211 *ov6211)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov6211->sd);
	u64 val;
	int ret;

	ret = cci_read(ov6211->regmap, OV6211_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(&client->dev, "failed to read chip id: %d\n", ret);
		return ret;
	}

	if (val != OV6211_CHIP_ID) {
		dev_err(&client->dev, "chip id mismatch: %x!=%llx\n",
			OV6211_CHIP_ID, val);
		return -ENODEV;
	}

	return 0;
}

static int ov6211_check_hwcfg(struct ov6211 *ov6211)
{
	struct i2c_client *client = v4l2_get_subdevdata(&ov6211->sd);
	struct device *dev = &client->dev;
	struct fwnode_handle *fwnode = dev_fwnode(dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(dev, "link-frequency property is not found\n");
		ret = -EINVAL;
		goto error;
	}

	if (bus_cfg.nr_of_link_frequencies != 1 ||
	    bus_cfg.link_frequencies[0] != link_freq_menu_items[0]) {
		dev_err(dev, "Unsupported MIPI CSI2 link frequency\n");
		ret = -EINVAL;
		goto error;
	}

error:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int ov6211_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov6211 *ov6211 = to_ov6211(sd);
	int ret;

	if (ov6211->avdd) {
		ret = regulator_enable(ov6211->avdd);
		if (ret)
			return ret;
	}

	if (ov6211->dovdd) {
		ret = regulator_enable(ov6211->dovdd);
		if (ret)
			goto avdd_disable;
	}

	if (ov6211->dvdd) {
		ret = regulator_enable(ov6211->dvdd);
		if (ret)
			goto dovdd_disable;
	}

	gpiod_set_value_cansleep(ov6211->reset_gpio, 0);
	usleep_range(10 * USEC_PER_MSEC, 15 * USEC_PER_MSEC);

	ret = clk_prepare_enable(ov6211->xvclk);
	if (ret)
		goto reset_gpio;

	return 0;

reset_gpio:
	gpiod_set_value_cansleep(ov6211->reset_gpio, 1);

	if (ov6211->dvdd)
		regulator_disable(ov6211->dvdd);
dovdd_disable:
	if (ov6211->dovdd)
		regulator_disable(ov6211->dovdd);
avdd_disable:
	if (ov6211->avdd)
		regulator_disable(ov6211->avdd);

	return ret;
}

static int ov6211_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov6211 *ov6211 = to_ov6211(sd);

	clk_disable_unprepare(ov6211->xvclk);

	gpiod_set_value_cansleep(ov6211->reset_gpio, 1);

	if (ov6211->dvdd)
		regulator_disable(ov6211->dvdd);

	if (ov6211->dovdd)
		regulator_disable(ov6211->dovdd);

	if (ov6211->avdd)
		regulator_disable(ov6211->avdd);

	return 0;
}

static int ov6211_probe(struct i2c_client *client)
{
	struct ov6211 *ov6211;
	unsigned long freq;
	int ret;

	ov6211 = devm_kzalloc(&client->dev, sizeof(*ov6211), GFP_KERNEL);
	if (!ov6211)
		return -ENOMEM;

	ov6211->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(ov6211->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(ov6211->regmap),
				     "failed to init CCI\n");

	v4l2_i2c_subdev_init(&ov6211->sd, client, &ov6211_subdev_ops);

	ov6211->xvclk = devm_v4l2_sensor_clk_get(&client->dev, NULL);
	if (IS_ERR(ov6211->xvclk))
		return dev_err_probe(&client->dev, PTR_ERR(ov6211->xvclk),
				     "failed to get XVCLK clock\n");

	freq = clk_get_rate(ov6211->xvclk);
	if (freq && freq != OV6211_MCLK_FREQ_24MHZ)
		return dev_err_probe(&client->dev, -EINVAL,
				"XVCLK clock frequency %lu is not supported\n",
				freq);

	ret = ov6211_check_hwcfg(ov6211);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to check HW configuration\n");

	ov6211->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(ov6211->reset_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(ov6211->reset_gpio),
				     "cannot get reset GPIO\n");

	ov6211->avdd = devm_regulator_get_optional(&client->dev, "avdd");
	if (IS_ERR(ov6211->avdd)) {
		ret = PTR_ERR(ov6211->avdd);
		if (ret != -ENODEV)
			return dev_err_probe(&client->dev, ret,
					     "Failed to get avdd regulator\n");

		ov6211->avdd = NULL;
	}

	ov6211->dovdd = devm_regulator_get_optional(&client->dev, "dovdd");
	if (IS_ERR(ov6211->dovdd)) {
		ret = PTR_ERR(ov6211->dovdd);
		if (ret != -ENODEV)
			return dev_err_probe(&client->dev, ret,
					     "Failed to get dovdd regulator\n");

		ov6211->dovdd = NULL;
	}

	ov6211->dvdd = devm_regulator_get_optional(&client->dev, "dvdd");
	if (IS_ERR(ov6211->dvdd)) {
		ret = PTR_ERR(ov6211->dvdd);
		if (ret != -ENODEV)
			return dev_err_probe(&client->dev, ret,
					     "Failed to get dvdd regulator\n");

		ov6211->dvdd = NULL;
	}

	/* The sensor must be powered on to read the CHIP_ID register */
	ret = ov6211_power_on(&client->dev);
	if (ret)
		return ret;

	ret = ov6211_identify_module(ov6211);
	if (ret) {
		dev_err_probe(&client->dev, ret, "failed to find sensor\n");
		goto power_off;
	}

	mutex_init(&ov6211->mutex);
	ov6211->cur_mode = &supported_modes[0];

	ret = ov6211_init_controls(ov6211);
	if (ret) {
		dev_err_probe(&client->dev, ret, "failed to init controls\n");
		goto mutex_destroy;
	}

	ov6211->sd.internal_ops = &ov6211_internal_ops;
	ov6211->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov6211->sd.entity.ops = &ov6211_subdev_entity_ops;
	ov6211->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ov6211->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&ov6211->sd.entity, 1, &ov6211->pad);
	if (ret) {
		dev_err_probe(&client->dev, ret,
			      "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&ov6211->sd);
	if (ret < 0) {
		dev_err_probe(&client->dev, ret,
			      "failed to register V4L2 subdev\n");
		goto media_entity_cleanup;
	}

	/* Enable runtime PM and turn off the device */
	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	return 0;

media_entity_cleanup:
	media_entity_cleanup(&ov6211->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(ov6211->sd.ctrl_handler);

mutex_destroy:
	mutex_destroy(&ov6211->mutex);

power_off:
	ov6211_power_off(&client->dev);

	return ret;
}

static void ov6211_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov6211 *ov6211 = to_ov6211(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(&client->dev);
	mutex_destroy(&ov6211->mutex);
}

static const struct dev_pm_ops ov6211_pm_ops = {
	SET_RUNTIME_PM_OPS(ov6211_power_off, ov6211_power_on, NULL)
};

static const struct of_device_id ov6211_of_match[] = {
	{ .compatible = "ovti,ov6211" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov6211_of_match);

static struct i2c_driver ov6211_i2c_driver = {
	.driver = {
		.name = "ov6211",
		.pm = &ov6211_pm_ops,
		.of_match_table = ov6211_of_match,
	},
	.probe = ov6211_probe,
	.remove = ov6211_remove,
};

module_i2c_driver(ov6211_i2c_driver);

MODULE_AUTHOR("Vladimir Zapolskiy <vladimir.zapolskiy@linaro.org>");
MODULE_DESCRIPTION("OmniVision OV6211 sensor driver");
MODULE_LICENSE("GPL");
