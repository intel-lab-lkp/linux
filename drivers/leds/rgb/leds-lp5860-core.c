// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Pengutronix
 *
 * Author: Steffen Trumtrar <kernel@pengutronix.de>
 */

#include <linux/gpio.h>
#include <linux/led-class-multicolor.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>

#include <linux/platform_data/leds-lp5860.h>

static struct lp5860_led *mcled_cdev_to_led(struct led_classdev_mc *mc_cdev)
{
	return container_of(mc_cdev, struct lp5860_led, mc_cdev);
}

static int lp5860_set_dot_onoff(struct lp5860_led *led, unsigned int dot, bool enable)
{
	unsigned int offset = dot / LP5860_MAX_DOT_ONOFF_GROUP_NUM;
	unsigned int mask = BIT(dot % LP5860_MAX_DOT_ONOFF_GROUP_NUM);

	if (dot > LP5860_REG_DOT_ONOFF_MAX)
		return -EINVAL;

	return regmap_update_bits(led->chip->regmap,
				  LP5860_REG_DOT_ONOFF_START + offset, mask,
				  enable ? LP5860_DOT_ALL_ON : LP5860_DOT_ALL_OFF);
}

static int lp5860_set_mc_brightness(struct led_classdev *cdev,
				    enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	struct lp5860_led *led = mcled_cdev_to_led(mc_cdev);
	int i;

	led_mc_calc_color_components(mc_cdev, brightness);

	for (i = 0; i < led->mc_cdev.num_colors; i++) {
		unsigned int channel = mc_cdev->subled_info[i].channel;
		unsigned int led_brightness = mc_cdev->subled_info[i].brightness;
		int ret;

		ret = lp5860_set_dot_onoff(led, channel, led_brightness);
		if (ret)
			return ret;

		ret = regmap_write(led->chip->regmap,
				   LP5860_REG_PWM_BRI_START + channel, led_brightness);
		if (ret)
			return ret;
	}

	return 0;
}

static int lp5860_chip_enable_toggle(struct lp5860 *led, int enable)
{
	return regmap_write(led->regmap, LP5860_REG_CHIP_EN, enable);
}

static int lp5860_led_init(struct lp5860_led *led, struct fwnode_handle *fwnode,
			   unsigned int channel)
{
	enum led_default_state default_state;
	unsigned int brightness;
	int ret;

	ret = regmap_read(led->chip->regmap, LP5860_REG_PWM_BRI_START + channel, &brightness);

	default_state = led_init_default_state_get(fwnode);

	switch (default_state) {
	case LEDS_DEFSTATE_ON:
		led->brightness = LP5860_MAX_BRIGHTNESS;
		break;
	case LEDS_DEFSTATE_KEEP:
		led->brightness = min(brightness, LP5860_MAX_BRIGHTNESS);
		break;
	default:
		led->brightness = 0;
		break;
	}

	return lp5860_set_mc_brightness(&led->mc_cdev.led_cdev, led->brightness);
}

static int lp5860_init_dt(struct lp5860 *lp)
{
	struct fwnode_handle *led_node = NULL;
	struct led_init_data init_data = {};
	struct led_classdev *led_cdev;
	struct mc_subled *mc_led_info;
	struct lp5860_led *led;
	int led_index = 0;
	int chan;
	int ret;

	device_for_each_child_node_scoped(lp->dev, multi_led) {
		led = &lp->leds[led_index];

		init_data.fwnode = multi_led;

		/* Count the number of channels in this multi_led */
		chan = fwnode_get_child_node_count(multi_led);
		if (!chan || chan > LP5860_MAX_LED_CHANNELS)
			return -EINVAL;

		led->mc_cdev.num_colors = chan;

		mc_led_info = devm_kcalloc(lp->dev, chan, sizeof(*mc_led_info), GFP_KERNEL);
		if (!mc_led_info)
			return -ENOMEM;

		led->chip = lp;
		led->mc_cdev.subled_info = mc_led_info;
		led_cdev = &led->mc_cdev.led_cdev;
		led_cdev->max_brightness = LP5860_MAX_BRIGHTNESS;
		led_cdev->brightness_set_blocking = lp5860_set_mc_brightness;

		chan = 0;
		/* Initialize all channels of this multi_led */
		fwnode_for_each_child_node(multi_led, led_node) {
			u32 channel;
			u32 color_index;

			ret = fwnode_property_read_u32(led_node, "color", &color_index);
			if (ret) {
				dev_err(lp->dev, "%pfwP: Cannot read 'color' property. Skipping.\n",
					led_node);
				fwnode_handle_put(led_node);
				continue;
			}

			ret = fwnode_property_read_u32(led_node, "reg", &channel);
			if (ret < 0) {
				dev_err(lp->dev, "%pfwP: 'reg' property is missing. Skipping.\n",
					led_node);
				fwnode_handle_put(led_node);
				continue;
			}

			mc_led_info[chan].color_index = color_index;
			mc_led_info[chan].channel = channel;
			lp5860_led_init(led, init_data.fwnode, chan);

			chan++;
		}

		ret = devm_led_classdev_multicolor_register_ext(lp->dev, &led->mc_cdev, &init_data);
		if (ret) {
			dev_err(lp->dev, "%pfwP: Failed to register Multi-Color LEDs\n", multi_led);
			return ret;
		}
		led_index++;
	}

	return 0;
}

int lp5860_device_init(struct device *dev)
{
	struct lp5860 *lp = dev_get_drvdata(dev);
	int ret;

	ret = lp5860_chip_enable_toggle(lp, LP5860_CHIP_ENABLE);
	if (ret)
		return ret;

	/*
	 * Set to 8-bit PWM data without VSYNC.
	 * Data is sent out for display instantly after received.
	 */
	ret = regmap_update_bits(lp->regmap, LP5860_REG_DEV_INITIAL, LP5860_MODE_MASK,
				 LP5860_MODE_1 << LP5860_MODE_OFFSET);
	if (ret < 0)
		return ret;

	return lp5860_init_dt(lp);
}

void lp5860_device_remove(struct device *dev)
{
	struct lp5860 *lp = dev_get_drvdata(dev);

	lp5860_chip_enable_toggle(lp, LP5860_CHIP_DISABLE);
}
