// SPDX-License-Identifier: GPL-2.0
//
// Based on leds-max77650 driver
//
// LED driver for MAXIM 77705 PMIC.
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

	if (*delay_on < MAX77705_RGB_DELAY_100_STEP)
		on_value = 0;
	else if (*delay_on < MAX77705_RGB_DELAY_100_STEP_LIM)
		on_value = *delay_on / MAX77705_RGB_DELAY_100_STEP - 1;
	else if (*delay_on < MAX77705_RGB_DELAY_250_STEP_LIM)
		on_value = (*delay_on - MAX77705_RGB_DELAY_100_STEP_LIM) /
				MAX77705_RGB_DELAY_250_STEP +
				MAX77705_RGB_DELAY_100_STEP_COUNT;
	else
		on_value = 15;

	on_value <<= 4;

	if (*delay_off < 1)
		off_value = 0;
	else if (*delay_off < MAX77705_RGB_DELAY_500_STEP)
		off_value = 1;
	else if (*delay_off < MAX77705_RGB_DELAY_500_STEP_LIM)
		off_value = *delay_off / MAX77705_RGB_DELAY_500_STEP;
	else if (*delay_off < MAX77705_RGB_DELAY_1000_STEP_LIM)
		off_value = (*delay_off - MAX77705_RGB_DELAY_1000_STEP_LIM) /
				MAX77705_RGB_DELAY_1000_STEP +
				MAX77705_RGB_DELAY_500_STEP_COUNT;
	else if (*delay_off < MAX77705_RGB_DELAY_2000_STEP_LIM)
		off_value = (*delay_off - MAX77705_RGB_DELAY_2000_STEP_LIM) /
				MAX77705_RGB_DELAY_2000_STEP +
				MAX77705_RGB_DELAY_1000_STEP_COUNT;
	else
		off_value = 15;

	value = on_value | off_value;
	return regmap_write(led->regmap, MAX77705_RGBLED_REG_LEDBLNK, value);
}

static int max77705_led_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct max77705_led *led = max77705_to_led(cdev);
	unsigned long blink_default = 0;
	int ret;

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
	struct max77705_led *leds, *led;
	struct device *dev = &pdev->dev;
	struct regmap *regmap;
	int ret, num_leds;
	u32 reg;

	leds = devm_kcalloc(dev, sizeof(*leds),
				MAX77705_LED_NUM_LEDS, GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -ENODEV;

	num_leds = device_get_child_node_count(dev);
	if (num_leds < 0 || num_leds > MAX77705_LED_NUM_LEDS)
		return -ENODEV;

	device_for_each_child_node_scoped(dev, child) {
		struct led_init_data init_data = {};

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret || reg >= MAX77705_LED_NUM_LEDS)
			ret = -EINVAL;

		led = &leds[reg];
		led->regmap = regmap;
		led->reg_brightness = MAX77705_RGBLED_REG_LED0BRT + reg;
		led->en_shift = MAX77705_RGBLED_EN_WIDTH * reg;
		led->cdev.brightness_set_blocking = max77705_led_brightness_set;
		led->cdev.blink_set = max77705_rgb_blink;
		led->cdev.max_brightness = MAX77705_LED_MAX_BRIGHTNESS;

		init_data.fwnode = child;

		ret = devm_led_classdev_register_ext(dev, &led->cdev,
							&init_data);
		if (ret)
			return ret;

		ret = max77705_led_brightness_set(&led->cdev, LED_OFF);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct of_device_id max77705_led_of_match[] = {
	{ .compatible = "maxim,max77705-rgb" },
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

MODULE_DESCRIPTION("Maxim MAX77705 LED driver");
MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_LICENSE("GPL");
