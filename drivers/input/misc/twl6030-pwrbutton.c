// SPDX-License-Identifier: GPL-2.0-only
/**
 * TWL603x power button input driver
 *
 * Copyright (C) 2024 Andreas Kemnade <andreas@kemnade.info>
 *
 * based on older 6030 driver found in a v3.0 vendor kernel
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/twl.h>

#define PWR_PWRON_IRQ BIT(0)

#define STS_HW_CONDITIONS 0x2

static irqreturn_t powerbutton_irq(int irq, void *_pwr)
{
	struct input_dev *pwr = _pwr;
	int err;
	u8 value;

	err = twl_i2c_read_u8(TWL_MODULE_PM_MASTER, &value, STS_HW_CONDITIONS);
	if (err)
		return IRQ_HANDLED;

	pm_wakeup_event(pwr->dev.parent, 0);
	input_report_key(pwr, KEY_POWER, value & PWR_PWRON_IRQ);
	input_sync(pwr);

	return IRQ_HANDLED;
}

static int twl6030_pwrbutton_probe(struct platform_device *pdev)
{
	struct input_dev *pwr;
	int irq = platform_get_irq(pdev, 0);
	int err;

	pwr = devm_input_allocate_device(&pdev->dev);
	if (!pwr) {
		dev_err(&pdev->dev, "Can't allocate power button\n");
		return -ENOMEM;
	}

	input_set_capability(pwr, EV_KEY, KEY_POWER);
	pwr->name = "twl6030_pwrbutton";
	pwr->phys = "twl6030_pwrbutton/input0";
	pwr->dev.parent = &pdev->dev;

	err = devm_request_threaded_irq(&pdev->dev, irq, NULL, powerbutton_irq,
			IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING |
			IRQF_ONESHOT,
			"twl6030_pwrbutton", pwr);
	if (err < 0) {
		dev_err(&pdev->dev, "Can't get IRQ for pwrbutton: %d\n", err);
		return err;
	}

	err = input_register_device(pwr);
	if (err) {
		dev_err(&pdev->dev, "Can't register power button: %d\n", err);
		return err;
	}

	err = twl6030_interrupt_unmask(0x01, REG_INT_MSK_LINE_A);
	if (err < 0)
		return err;

	err = twl6030_interrupt_unmask(0x01, REG_INT_MSK_STS_A);
	if (err < 0)
		return err;

	device_init_wakeup(&pdev->dev, true);

	return 0;
}

static void twl6030_pwrbutton_remove(struct platform_device *pdev)
{
	twl6030_interrupt_mask(0x01, REG_INT_MSK_LINE_A);
	twl6030_interrupt_mask(0x01, REG_INT_MSK_STS_A);
}

static const struct of_device_id twl6030_pwrbutton_dt_match_table[] = {
	{ .compatible = "ti,twl6030-pwrbutton" },
	{},
};
MODULE_DEVICE_TABLE(of, twl6030_pwrbutton_dt_match_table);

static struct platform_driver twl6030_pwrbutton_driver = {
	.probe		= twl6030_pwrbutton_probe,
	.remove		= twl6030_pwrbutton_remove,
	.driver		= {
		.name	= "twl6030_pwrbutton",
		.of_match_table = of_match_ptr(twl6030_pwrbutton_dt_match_table),
	},
};
module_platform_driver(twl6030_pwrbutton_driver);

MODULE_ALIAS("platform:twl6030_pwrbutton");
MODULE_DESCRIPTION("Phoenix Power Button");
MODULE_LICENSE("GPL");
