// SPDX-License-Identifier: GPL-2.0
/*
 * RGB LED Driver for Samsung S2M series PMICs.
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 * Copyright (c) 2026 Kaustabh Chakraborty <kauschluss@disroot.org>
 */

#include <linux/container_of.h>
#include <linux/led-class-multicolor.h>
#include <linux/mfd/samsung/core.h>
#include <linux/mfd/samsung/s2mu005.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct s2m_rgb {
	struct device *dev;
	struct regmap *regmap;
	struct led_classdev_mc mc;
	enum sec_device_type device_type;
	/*
	 * The mutex object prevents race conditions when evaluation and
	 * application of LED pattern state.
	 */
	struct mutex lock;
	/*
	 * State variables representing the current LED pattern, these only to
	 * be accessed when lock is held.
	 */
	u8 ramp_up;
	u8 ramp_dn;
	u8 stay_hi;
	u8 stay_lo;
};

static struct led_classdev_mc *to_s2m_mc(struct led_classdev *cdev)
{
	return container_of(cdev, struct led_classdev_mc, led_cdev);
}

static struct s2m_rgb *to_s2m_rgb(struct led_classdev_mc *mc)
{
	return container_of(mc, struct s2m_rgb, mc);
}

static const u32 s2mu005_rgb_lut_ramp[] = {
	0,	100,	200,	300,	400,	500,	600,	700,
	800,	1000,	1200,	1400,	1600,	1800,	2000,	2200,
};

static const u32 s2mu005_rgb_lut_stay_hi[] = {
	100,	200,	300,	400,	500,	750,	1000,	1250,
	1500,	1750,	2000,	2250,	2500,	2750,	3000,	3250,
};

static const u32 s2mu005_rgb_lut_stay_lo[] = {
	0,	500,	1000,	1500,	2000,	2500,	3000,	3500,
	4000,	4500,	5000,	6000,	7000,	8000,	10000,	12000,
};

static int s2mu005_rgb_apply_params(struct s2m_rgb *rgb)
{
	struct regmap *regmap = rgb->regmap;
	unsigned int ramp_val = 0;
	unsigned int stay_val = 0;
	int ret;
	int i;

	ramp_val |= FIELD_PREP(S2MU005_RGB_CH_RAMP_UP, rgb->ramp_up);
	ramp_val |= FIELD_PREP(S2MU005_RGB_CH_RAMP_DN, rgb->ramp_dn);

	stay_val |= FIELD_PREP(S2MU005_RGB_CH_STAY_HI, rgb->stay_hi);
	stay_val |= FIELD_PREP(S2MU005_RGB_CH_STAY_LO, rgb->stay_lo);

	ret = regmap_write(regmap, S2MU005_REG_RGB_EN, S2MU005_RGB_RESET);
	if (ret < 0) {
		dev_err(rgb->dev, "failed to reset RGB LEDs\n");
		return ret;
	}

	for (i = 0; i < rgb->mc.num_colors; i++) {
		ret = regmap_write(regmap, S2MU005_REG_RGB_CH_CTRL(i),
				   rgb->mc.subled_info[i].brightness);
		if (ret < 0) {
			dev_err(rgb->dev, "failed to set LED brightness\n");
			return ret;
		}

		ret = regmap_write(regmap, S2MU005_REG_RGB_CH_RAMP(i), ramp_val);
		if (ret < 0) {
			dev_err(rgb->dev, "failed to set ramp timings\n");
			return ret;
		}

		ret = regmap_write(regmap, S2MU005_REG_RGB_CH_STAY(i), stay_val);
		if (ret < 0) {
			dev_err(rgb->dev, "failed to set stay timings\n");
			return ret;
		}
	}

	ret = regmap_update_bits(regmap, S2MU005_REG_RGB_EN, S2MU005_RGB_SLOPE,
				 S2MU005_RGB_SLOPE_SMOOTH);
	if (ret < 0) {
		dev_err(rgb->dev, "failed to set ramp slope\n");
		return ret;
	}

	return 0;
}

static int s2mu005_rgb_reset_params(struct s2m_rgb *rgb)
{
	struct regmap *regmap = rgb->regmap;
	int ret;

	ret = regmap_write(regmap, S2MU005_REG_RGB_EN, S2MU005_RGB_RESET);
	if (ret < 0) {
		dev_err(rgb->dev, "failed to reset RGB LEDs\n");
		return ret;
	}

	rgb->ramp_up = 0;
	rgb->ramp_dn = 0;
	rgb->stay_hi = 0;
	rgb->stay_lo = 0;

	return 0;
}

static int s2m_rgb_lut_calc_timing(const u32 *lut, const size_t len,
				   const u32 req_time, u8 *idx)
{
	int lo = 0;
	int hi = len - 2;

	/* Bounds checking */
	if (req_time < lut[0] || req_time > lut[len - 1])
		return -EINVAL;

	/*
	 * Perform a binary search to pick the best timing from the LUT.
	 *
	 * The search algorithm picks two consecutive elements of the
	 * LUT and tries to search the pair between which the requested
	 * time lies.
	 */
	while (lo <= hi) {
		*idx = (lo + hi) / 2;

		if ((lut[*idx] <= req_time) && (req_time <= lut[*idx + 1]))
			break;

		if ((req_time < lut[*idx]) && (req_time < lut[*idx + 1]))
			hi = *idx - 1;
		else
			lo = *idx + 1;
	}

	/*
	 * The searched timing is always less than the requested time. At
	 * times, the succeeding timing in the LUT is closer thus more
	 * accurate. Adjust the resulting value if that's the case.
	 */
	if (abs(req_time - lut[*idx]) > abs(lut[*idx + 1] - req_time))
		(*idx)++;

	return 0;
}

static int s2m_rgb_pattern_set(struct led_classdev *cdev, struct led_pattern *pattern,
			       u32 len, int repeat)
{
	struct s2m_rgb *rgb = to_s2m_rgb(to_s2m_mc(cdev));
	const u32 *lut_ramp_up, *lut_ramp_dn, *lut_stay_hi, *lut_stay_lo;
	size_t lut_ramp_up_len, lut_ramp_dn_len, lut_stay_hi_len, lut_stay_lo_len;
	int brightness_peak = 0;
	u32 time_hi = 0, time_lo = 0;
	bool ramp_up_en, ramp_dn_en;
	int ret;
	int i;

	/*
	 * The typical pattern supported by this device can be
	 * represented with the following graph:
	 *
	 *  255 T ''''''-.                         .-'''''''-.
	 *      |         '.                     .'           '.
	 *      |           \                   /               \
	 *      |            '.               .'                 '.
	 *      |              '-...........-'                     '-
	 *    0 +----------------------------------------------------> time (s)
	 *
	 *       <---- HIGH ----><-- LOW --><-------- HIGH --------->
	 *       <-----><-------><---------><-------><-----><------->
	 *       stay_hi ramp_dn   stay_lo   ramp_up stay_hi ramp_dn
	 *
	 * There are two states, named HIGH and LOW. HIGH has a non-zero
	 * brightness level, while LOW is of zero brightness. The
	 * pattern provided should mention only one zero and non-zero
	 * brightness level. The hardware always starts the pattern from
	 * the HIGH state, as shown in the graph.
	 *
	 * The HIGH state can be divided in three somewhat equal timings:
	 * ramp_up, stay_hi, and ramp_dn. The LOW state has only one
	 * timing: stay_lo.
	 */

	/* Only indefinitely looping patterns are supported. */
	if (repeat != -1)
		return -EINVAL;

	/* Pattern should consist of at least two tuples. */
	if (len < 2)
		return -EINVAL;

	for (i = 0; i < len; i++) {
		int brightness = pattern[i].brightness;
		u32 delta_t = pattern[i].delta_t;

		if (brightness) {
			/*
			 * The pattern shold define only one non-zero
			 * brightness in the HIGH state. The device
			 * doesn't have any provisions to handle
			 * multiple peak brightness levels.
			 */
			if (brightness_peak && brightness_peak != brightness)
				return -EINVAL;

			brightness_peak = brightness;
			time_hi += delta_t;
			ramp_dn_en = !!delta_t;
		} else {
			time_lo += delta_t;
			ramp_up_en = !!delta_t;
		}
	}

	switch (rgb->device_type) {
	case S2MU005:
		lut_ramp_up = s2mu005_rgb_lut_ramp;
		lut_ramp_up_len = ARRAY_SIZE(s2mu005_rgb_lut_ramp);
		lut_ramp_dn = s2mu005_rgb_lut_ramp;
		lut_ramp_dn_len = ARRAY_SIZE(s2mu005_rgb_lut_ramp);
		lut_stay_hi = s2mu005_rgb_lut_stay_hi;
		lut_stay_hi_len = ARRAY_SIZE(s2mu005_rgb_lut_stay_hi);
		lut_stay_lo = s2mu005_rgb_lut_stay_lo;
		lut_stay_lo_len = ARRAY_SIZE(s2mu005_rgb_lut_stay_lo);
		break;
	default:
		/* execution shouldn't reach here */
		break;
	}

	mutex_lock(&rgb->lock);

	/*
	 * The timings ramp_up, stay_hi, and ramp_dn of the HIGH state
	 * are roughly equal. Firstly, calculate and set timings for
	 * ramp_up and ramp_dn (making sure they're exactly equal).
	 */
	rgb->ramp_up = 0;
	rgb->ramp_dn = 0;

	if (ramp_up_en) {
		ret = s2m_rgb_lut_calc_timing(lut_ramp_up, lut_ramp_up_len, time_hi / 3,
					      &rgb->ramp_up);
		if (ret < 0)
			goto param_fail;
	}

	if (ramp_dn_en) {
		ret = s2m_rgb_lut_calc_timing(lut_ramp_dn, lut_ramp_dn_len, time_hi / 3,
					      &rgb->ramp_dn);
		if (ret < 0)
			goto param_fail;
	}

	/*
	 * Subtract the allocated ramp timings from time_hi (and also
	 * making sure it doesn't underflow!). The remaining time is
	 * allocated to stay_hi.
	 */
	time_hi -= min(time_hi, lut_ramp_up[rgb->ramp_up]);
	time_hi -= min(time_hi, lut_ramp_dn[rgb->ramp_dn]);

	ret = s2m_rgb_lut_calc_timing(lut_stay_hi, lut_stay_hi_len, time_hi, &rgb->stay_hi);
	if (ret < 0)
		goto param_fail;

	ret = s2m_rgb_lut_calc_timing(lut_stay_lo, lut_stay_lo_len, time_lo, &rgb->stay_lo);
	if (ret < 0)
		goto param_fail;

	led_mc_calc_color_components(&rgb->mc, brightness_peak);
	switch (rgb->device_type) {
	case S2MU005:
		ret = s2mu005_rgb_apply_params(rgb);
		break;
	default:
		/* execution shouldn't reach here */
		break;
	}
	if (ret < 0)
		goto param_fail;

	mutex_unlock(&rgb->lock);

	return 0;

param_fail:
	rgb->ramp_up = 0;
	rgb->ramp_dn = 0;
	rgb->stay_hi = 0;
	rgb->stay_lo = 0;

	mutex_unlock(&rgb->lock);

	return ret;
}

static int s2m_rgb_pattern_clear(struct led_classdev *cdev)
{
	struct s2m_rgb *rgb = to_s2m_rgb(to_s2m_mc(cdev));
	int ret = 0;

	mutex_lock(&rgb->lock);

	switch (rgb->device_type) {
	case S2MU005:
		ret = s2mu005_rgb_reset_params(rgb);
		break;
	default:
		/* execution shouldn't reach here */
		break;
	}

	mutex_unlock(&rgb->lock);

	return ret;
}

static int s2m_rgb_brightness_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct s2m_rgb *rgb = to_s2m_rgb(to_s2m_mc(cdev));
	int ret = 0;

	if (!value)
		return s2m_rgb_pattern_clear(cdev);

	mutex_lock(&rgb->lock);

	led_mc_calc_color_components(&rgb->mc, value);
	switch (rgb->device_type) {
	case S2MU005:
		ret = s2mu005_rgb_apply_params(rgb);
		break;
	default:
		/* execution shouldn't reach here */
		break;
	}

	mutex_unlock(&rgb->lock);

	return ret;
}

static struct mc_subled s2mu005_rgb_subled_info[] = {
	{ .channel = 0, .color_index = LED_COLOR_ID_BLUE },
	{ .channel = 1, .color_index = LED_COLOR_ID_GREEN },
	{ .channel = 2, .color_index = LED_COLOR_ID_RED },
};

static int s2m_rgb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sec_pmic_dev *pmic_drvdata = dev_get_drvdata(dev->parent);
	struct s2m_rgb *rgb;
	struct led_init_data init_data = {};
	int ret;

	rgb = devm_kzalloc(dev, sizeof(*rgb), GFP_KERNEL);
	if (!rgb)
		return -ENOMEM;

	platform_set_drvdata(pdev, rgb);
	rgb->dev = dev;
	rgb->regmap = pmic_drvdata->regmap_pmic;
	rgb->device_type = platform_get_device_id(pdev)->driver_data;

	switch (rgb->device_type) {
	case S2MU005:
		rgb->mc.subled_info = s2mu005_rgb_subled_info;
		rgb->mc.num_colors = ARRAY_SIZE(s2mu005_rgb_subled_info);
		break;
	default:
		return dev_err_probe(dev, -ENODEV, "device type %d is not supported by driver\n",
				     pmic_drvdata->device_type);
	}

	rgb->mc.led_cdev.max_brightness = 255;
	rgb->mc.led_cdev.brightness_set_blocking = s2m_rgb_brightness_set;
	rgb->mc.led_cdev.pattern_set = s2m_rgb_pattern_set;
	rgb->mc.led_cdev.pattern_clear = s2m_rgb_pattern_clear;

	ret = devm_mutex_init(dev, &rgb->lock);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create mutex lock\n");

	init_data.fwnode = of_fwnode_handle(dev->of_node);
	ret = devm_led_classdev_multicolor_register_ext(dev, &rgb->mc, &init_data);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to create LED device\n");

	return 0;
}

static const struct platform_device_id s2m_rgb_id_table[] = {
	{ "s2mu005-rgb", S2MU005 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, s2m_rgb_id_table);

static const struct of_device_id s2m_rgb_of_match_table[] = {
	{ .compatible = "samsung,s2mu005-rgb", .data = (void *)S2MU005 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, s2m_rgb_of_match_table);

static struct platform_driver s2m_rgb_driver = {
	.driver = {
		.name = "s2m-rgb",
	},
	.probe = s2m_rgb_probe,
	.id_table = s2m_rgb_id_table,
};
module_platform_driver(s2m_rgb_driver);

MODULE_DESCRIPTION("RGB LED Driver For Samsung S2M Series PMICs");
MODULE_AUTHOR("Kaustabh Chakraborty <kauschluss@disroot.org>");
MODULE_LICENSE("GPL");
