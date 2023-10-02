// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2023 Linaro Inc.
//   Author: AKASHI takahiro <takahiro.akashi@linaro.org>

#include <linux/gpio/driver.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include "gpiolib.h"

struct scmi_gpio_priv {
	struct gpio_chip chip;
};

static int scmi_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	unsigned long config;

	config = PIN_CONFIG_OUTPUT_ENABLE;
	if (pinctrl_gpio_get_config(chip->gpiodev->base + offset, &config))
		return -1;
	if (config)
		return GPIO_LINE_DIRECTION_OUT;

	config = PIN_CONFIG_INPUT_ENABLE;
	if (pinctrl_gpio_get_config(chip->gpiodev->base + offset, &config))
		return -1;
	if (config)
		return GPIO_LINE_DIRECTION_IN;

	return -1;
}

static int scmi_gpio_direction_input(struct gpio_chip *chip,
				     unsigned int offset)
{
	return pinctrl_gpio_direction_input(chip->gpiodev->base + offset);
}

static int scmi_gpio_direction_output(struct gpio_chip *chip,
				      unsigned int offset, int val)
{
	return pinctrl_gpio_direction_output(chip->gpiodev->base + offset);
}

static int scmi_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	unsigned long config;

	/* FIXME: currently, PIN_CONFIG_INPUT not defined */
	config = PIN_CONFIG_INPUT;
	if (pinctrl_gpio_get_config(chip->gpiodev->base + offset, &config))
		return -1;

	/* FIXME: the packed format not defined */
	if (config >> 8)
		return 1;

	return 0;
}

static void scmi_gpio_set(struct gpio_chip *chip, unsigned int offset, int val)
{
	unsigned long config;

	config = PIN_CONF_PACKED(PIN_CONFIG_OUTPUT, val & 0x1);
;
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

static int scmi_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *parent_np;
	struct scmi_gpio_priv *priv;
	struct gpio_chip *chip;
	int ret;

	/* FIXME: who should be the parent */
	parent_np = NULL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	chip = &priv->chip;
	chip->label = dev_name(dev);
	chip->parent = dev;
	chip->base = -1;

	chip->request = gpiochip_generic_request;
	chip->free = gpiochip_generic_free;
	chip->get_direction = scmi_gpio_get_direction;
	chip->direction_input = scmi_gpio_direction_input;
	chip->direction_output = scmi_gpio_direction_output;
	chip->get = scmi_gpio_get;
	chip->set = scmi_gpio_set;

	ret = devm_gpiochip_add_data(dev, chip, priv);
	if (ret)
		return ret;

	chip->ngpio = sum_up_ngpios(chip);

	platform_set_drvdata(pdev, priv);

	return 0;
}

static int scmi_gpio_remove(struct platform_device *pdev)
{
	struct scmi_gpio_priv *priv = platform_get_drvdata(pdev);

	gpiochip_remove(&priv->chip);

	return 0;
}

static const struct of_device_id scmi_gpio_match[] = {
	{ .compatible = "arm,scmi-gpio-generic" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, scmi_gpio_match);

static struct platform_driver scmi_gpio_driver = {
	.probe = scmi_gpio_probe,
	.remove = scmi_gpio_remove,
	.driver = {
		.name = "scmi-gpio",
		.of_match_table = scmi_gpio_match,
	},
};
module_platform_driver(scmi_gpio_driver);

MODULE_AUTHOR("AKASHI Takahiro <takahiro.akashi@linaro.org>");
MODULE_DESCRIPTION("SCMI Pinctrl based GPIO driver");
MODULE_LICENSE("GPL");
