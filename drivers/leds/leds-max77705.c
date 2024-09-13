// SPDX-License-Identifier: GPL-2.0
//
// Based on leds-max77650 driver:
//		Copyright (C) 2018 BayLibre SAS
//		Author: Bartosz Golaszewski <bgolaszewski@baylibre.com>
//
// LED driver for MAXIM 77705 MFD.
// Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.org>

#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/mfd/max77705-private.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define MAX77705_LED_NUM_LEDS		4
#define MAX77705_LED_EN_MASK		GENMASK(1, 0)
#define MAX77705_LED_MAX_BRIGHTNESS	0xff

struct max77705_led {
	struct led_classdev cdev;
	struct regmap *regmap;
	unsigned int en_shift;
	unsigned int reg_brightness;
};

static struct max77705_led *max77705_to_led(struct led_classdev *cdev)
{
	return container_of(cdev, struct max77705_led, cdev);
}

static int max77705_rgb_blink(struct led_classdev *cdev,
				unsigned long *delay_on,
				unsigned long *delay_off)
{
	struct max77705_led *led = max77705_to_led(cdev);
	int value, on_value, off_value;

	on_value = (((*delay_on < 100) ? 0 :
			(*delay_on < 500) ? *delay_on/100 - 1 :
			(*delay_on < 3250) ? (*delay_on - 500) / 250 + 4 : 15) << 4);
	off_value = ((*delay_off < 1) ? 0x00 :
			(*delay_off < 500) ? 0x01 :
			(*delay_off < 5000) ? *delay_off / 500 :
			(*delay_off < 8000) ? (*delay_off - 5000) / 1000 + 10 :
			(*delay_off < 12000) ? (*delay_off - 8000) / 2000 + 13 : 15);
	value = on_value | off_value;
	return regmap_write(led->regmap, MAX77705_RGBLED_REG_LEDBLNK, value);
}

static int max77705_led_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct max77705_led *led = max77705_to_led(cdev);
	int ret;
	unsigned long blink_default = 0;

	if (brightness == LED_OFF) {
		// Flash OFF
		ret = regmap_update_bits(led->regmap,
					MAX77705_RGBLED_REG_LEDEN,
					MAX77705_LED_EN_MASK << led->en_shift, 0);
		max77705_rgb_blink(cdev, &blink_default, &blink_default);
	} else {
		// Set current
		ret = regmap_write(led->regmap,
				   led->reg_brightness, brightness);
		if (ret < 0)
			return ret;

		ret = regmap_update_bits(led->regmap,
					MAX77705_RGBLED_REG_LEDEN, LED_ON << led->en_shift,
					MAX77705_LED_EN_MASK << led->en_shift);
	}

	return ret;
}

static int max77705_led_probe(struct platform_device *pdev)
{
	struct fwnode_handle *child;
	struct max77705_led *leds, *led;
	struct device *dev;
	struct regmap *map;
	int rv, num_leds;
	u32 reg;

	dev = &pdev->dev;

	leds = devm_kcalloc(dev, sizeof(*leds),
				MAX77705_LED_NUM_LEDS, GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	map = dev_get_regmap(dev->parent, NULL);
	if (!map)
		return -ENODEV;

	num_leds = device_get_child_node_count(dev);
	if (!num_leds || num_leds > MAX77705_LED_NUM_LEDS)
		return -ENODEV;

	device_for_each_child_node(dev, child) {
		struct led_init_data init_data = {};

		rv = fwnode_property_read_u32(child, "reg", &reg);
		if (rv || reg >= MAX77705_LED_NUM_LEDS) {
			rv = -EINVAL;
			goto err_node_put;
		}

		led = &leds[reg];
		led->regmap = map;
		led->reg_brightness = MAX77705_RGBLED_REG_LED0BRT + reg;
		led->en_shift = 2 * reg;
		led->cdev.brightness_set_blocking = max77705_led_brightness_set;
		led->cdev.blink_set = max77705_rgb_blink;
		led->cdev.max_brightness = MAX77705_LED_MAX_BRIGHTNESS;

		init_data.fwnode = child;
		init_data.devicename = "max77705";

		rv = devm_led_classdev_register_ext(dev, &led->cdev,
							&init_data);
		if (rv)
			goto err_node_put;

		rv = max77705_led_brightness_set(&led->cdev, LED_OFF);
		if (rv)
			goto err_node_put;
	}

	return 0;
err_node_put:
	fwnode_handle_put(child);
	return rv;
}

static const struct of_device_id max77705_led_of_match[] = {
	{ .compatible = "maxim,max77705-led" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77705_led_of_match);

static struct platform_driver max77705_led_driver = {
	.driver = {
		.name = "max77705-led",
		.of_match_table = max77705_led_of_match,
	},
	.probe = max77705_led_probe,
};
module_platform_driver(max77705_led_driver);

MODULE_DESCRIPTION("MAXIM 77705 LED driver");
MODULE_AUTHOR("Bartosz Golaszewski <bgolaszewski@baylibre.com>");
MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_LICENSE("GPL");
