// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Bootlin
 *
 * Author: Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>
 */

#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mfd/max7360.h>
#include <linux/property.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/regmap.h>
#include <linux/slab.h>

struct max7360_rotary {
	u32 axis;
	struct input_dev *input;
	unsigned int debounce_ms;
	struct regmap *regmap;
};

static irqreturn_t max7360_rotary_irq(int irq, void *data)
{
	struct max7360_rotary *max7360_rotary = data;
	int val;
	int ret;

	ret = regmap_read(max7360_rotary->regmap, MAX7360_REG_RTR_CNT, &val);
	if (ret < 0) {
		dev_err(&max7360_rotary->input->dev,
			"Failed to read rotary counter\n");
		return IRQ_NONE;
	}

	if (val == 0) {
		dev_dbg(&max7360_rotary->input->dev,
			"Got a spurious interrupt\n");

		return IRQ_NONE;
	}

	input_report_rel(max7360_rotary->input, max7360_rotary->axis,
			 (int8_t)val);
	input_sync(max7360_rotary->input);

	return IRQ_HANDLED;
}

static int max7360_rotary_hw_init(struct max7360_rotary *max7360_rotary)
{
	int val;
	int ret;

	val = FIELD_PREP(MAX7360_ROT_DEBOUNCE, max7360_rotary->debounce_ms) |
		FIELD_PREP(MAX7360_ROT_INTCNT, 1) | MAX7360_ROT_INTCNT_DLY;
	ret = regmap_write(max7360_rotary->regmap, MAX7360_REG_RTRCFG, val);
	if (ret) {
		dev_err(&max7360_rotary->input->dev,
			"Failed to set max7360 rotary encoder configuration\n");
		return ret;
	}

	return 0;
}

static int max7360_rotary_probe(struct platform_device *pdev)
{
	struct max7360_rotary *max7360_rotary;
	struct input_dev *input;
	int irq;
	int ret;

	if (!pdev->dev.parent)
		return dev_err_probe(&pdev->dev, -ENODEV, "No parent device\n");

	irq = platform_get_irq_byname(to_platform_device(pdev->dev.parent),
				      "inti");
	if (irq < 0)
		return irq;

	max7360_rotary = devm_kzalloc(&pdev->dev, sizeof(*max7360_rotary),
				      GFP_KERNEL);
	if (!max7360_rotary)
		return -ENOMEM;

	max7360_rotary->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!max7360_rotary->regmap)
		dev_err_probe(&pdev->dev, -ENODEV,
			      "Could not get parent regmap\n");

	device_property_read_u32(pdev->dev.parent, "linux,axis",
				 &max7360_rotary->axis);
	device_property_read_u32(pdev->dev.parent, "rotary-debounce-delay-ms",
				 &max7360_rotary->debounce_ms);
	if (max7360_rotary->debounce_ms > MAX7360_ROT_DEBOUNCE_MAX)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "Invalid debounce timing: %u\n",
				     max7360_rotary->debounce_ms);

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;

	max7360_rotary->input = input;

	input->id.bustype = BUS_I2C;
	input->name = pdev->name;
	input->dev.parent = &pdev->dev;

	input_set_capability(input, EV_REL, max7360_rotary->axis);
	input_set_drvdata(input, max7360_rotary);

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					max7360_rotary_irq,
					IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_SHARED,
					"max7360-rotary", max7360_rotary);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register interrupt\n");

	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Could not register input device\n");

	platform_set_drvdata(pdev, max7360_rotary);

	device_init_wakeup(&pdev->dev, true);
	ret = dev_pm_set_wake_irq(&pdev->dev, irq);
	if (ret)
		dev_warn(&pdev->dev, "Failed to set up wakeup irq: %d\n", ret);

	ret = max7360_rotary_hw_init(max7360_rotary);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to initialize max7360 rotary\n");

	return 0;
}

static void max7360_rotary_remove(struct platform_device *pdev)
{
	dev_pm_clear_wake_irq(&pdev->dev);
}

static struct platform_driver max7360_rotary_driver = {
	.driver = {
		.name	= "max7360-rotary",
	},
	.probe		= max7360_rotary_probe,
	.remove		= max7360_rotary_remove,
};
module_platform_driver(max7360_rotary_driver);

MODULE_DESCRIPTION("MAX7360 Rotary driver");
MODULE_AUTHOR("Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>");
MODULE_LICENSE("GPL");
