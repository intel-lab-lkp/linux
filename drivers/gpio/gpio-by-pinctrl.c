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

static int pin_control_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	unsigned long config;
	bool in, out;
	int ret;

	config = PIN_CONFIG_INPUT_ENABLE;
	ret = pinctrl_gpio_get_config(gc, offset, &config);
	if (ret)
		return ret;
	in = config;

	config = PIN_CONFIG_OUTPUT_ENABLE;
	ret = pinctrl_gpio_get_config(gc, offset, &config);
	if (ret)
		return ret;
	out = config;

	/* Consistency check - in theory both can be enabled! */
	if (in && !out)
		return GPIO_LINE_DIRECTION_IN;
	if (!in && out)
		return GPIO_LINE_DIRECTION_OUT;

	return -EINVAL;
}

static int pin_control_gpio_direction_output(struct gpio_chip *chip,
					     unsigned int offset, int val)
{
	return pinctrl_gpio_direction_output(chip, offset);
}

static int pin_control_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	unsigned long config;
	int ret;

	config = PIN_CONFIG_LEVEL;
	ret = pinctrl_gpio_get_config(chip, offset, &config);
	if (ret)
		return ret;

	return !!config;
}

static int pin_control_gpio_set(struct gpio_chip *chip, unsigned int offset,
				 int val)
{
	unsigned long config;

	config = PIN_CONF_PACKED(PIN_CONFIG_LEVEL, val);
	return pinctrl_gpio_set_config(chip, offset, config);
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
	chip->direction_input = pinctrl_gpio_direction_input;
	chip->direction_output = pin_control_gpio_direction_output;
	chip->get = pin_control_gpio_get;
	chip->set = pin_control_gpio_set;
	chip->set_config = gpiochip_generic_config;

	ret = devm_gpiochip_add_data(dev, chip, priv);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	return 0;
}

static const struct of_device_id pin_control_gpio_match[] = {
	{ .compatible = "scmi-pinctrl-gpio" },
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
