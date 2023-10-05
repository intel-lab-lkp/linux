// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2023 Linaro Inc.
//   Author: AKASHI takahiro <takahiro.akashi@linaro.org>

#include <linux/gpio/driver.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include "gpiolib.h"

struct pin_control_gpio_priv {
	struct gpio_chip chip;
};

static int pin_control_gpio_get_direction(struct gpio_chip *chip,
				      unsigned int offset)
{
	unsigned long config;
	bool out_en, in_en;
	int ret;

	config = PIN_CONFIG_OUTPUT_ENABLE;
	ret = pinctrl_gpio_get_config(chip->gpiodev->base + offset, &config);
	if (!ret)
		out_en = !!config;
	else if (ret == -EINVAL)
		out_en = false;
	else
		return ret;

	config = PIN_CONFIG_INPUT_ENABLE;
	ret = pinctrl_gpio_get_config(chip->gpiodev->base + offset, &config);
	if (!ret)
		in_en = !!config;
	else if (ret == -EINVAL)
		in_en = false;
	else
		return ret;

	if (in_en && !out_en)
		return GPIO_LINE_DIRECTION_IN;

	if (!in_en && out_en)
		return GPIO_LINE_DIRECTION_OUT;

	if (in_en && out_en) {
	    /* This may be an emulation for output with open drain */
		config = PIN_CONFIG_DRIVE_OPEN_DRAIN;
		ret = pinctrl_gpio_get_config(chip->gpiodev->base + offset,
					      &config);
		if (!ret && config)
			return GPIO_LINE_DIRECTION_OUT;
	}

	return -EINVAL;
}

static int pin_control_gpio_direction_input(struct gpio_chip *chip,
					    unsigned int offset)
{
	return pinctrl_gpio_direction_input(chip->gpiodev->base + offset);
}

static int pin_control_gpio_direction_output(struct gpio_chip *chip,
					     unsigned int offset, int val)
{
	return pinctrl_gpio_direction_output(chip->gpiodev->base + offset);
}

static int pin_control_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	unsigned long config;
	int ret;

	config = PIN_CONFIG_INPUT;
	ret = pinctrl_gpio_get_config(chip->gpiodev->base + offset, &config);
	if (ret)
		return ret;

	if (config >> 8)
		return 1;

	return 0;
}

static void pin_control_gpio_set(struct gpio_chip *chip, unsigned int offset,
				 int val)
{
	unsigned long config;

	config = PIN_CONF_PACKED(PIN_CONFIG_OUTPUT, val);

	pinctrl_gpio_set_config(chip->gpiodev->base + offset, config);
}

static u16 sum_up_ngpios(struct gpio_chip *chip)
{
	struct gpio_pin_range *range;
	struct gpio_device *gdev = chip->gpiodev;
	u16 ngpios = 0;

	list_for_each_entry(range, &gdev->pin_ranges, node) {
		ngpios += range->range.npins;
	}

	return ngpios;
}

static int pin_control_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pin_control_gpio_priv *priv;
	struct gpio_chip *chip;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	chip = &priv->chip;
	chip->label = dev_name(dev);
	chip->parent = dev;
	chip->base = -1;

	chip->request = gpiochip_generic_request;
	chip->free = gpiochip_generic_free;
	chip->get_direction = pin_control_gpio_get_direction;
	chip->direction_input = pin_control_gpio_direction_input;
	chip->direction_output = pin_control_gpio_direction_output;
	chip->get = pin_control_gpio_get;
	chip->set = pin_control_gpio_set;
	chip->set_config = gpiochip_generic_config;

	ret = devm_gpiochip_add_data(dev, chip, priv);
	if (ret)
		return ret;

	chip->ngpio = sum_up_ngpios(chip);

	platform_set_drvdata(pdev, priv);

	return 0;
}

static const struct of_device_id pin_control_gpio_match[] = {
	{ .compatible = "pin-control-gpio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pin_control_gpio_match);

static struct platform_driver pin_control_gpio_driver = {
	.probe = pin_control_gpio_probe,
	.driver = {
		.name = "pin-control-gpio",
		.of_match_table = pin_control_gpio_match,
	},
};
module_platform_driver(pin_control_gpio_driver);

MODULE_AUTHOR("AKASHI Takahiro <takahiro.akashi@linaro.org>");
MODULE_DESCRIPTION("Pinctrl based GPIO driver");
MODULE_LICENSE("GPL");
