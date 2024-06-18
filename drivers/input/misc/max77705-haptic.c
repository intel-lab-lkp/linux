// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Based on max77693-haptic.c:
 *	Copyright (C) 2014,2015 Samsung Electronics
 *      Jaewon Kim <jaewon02.kim@samsung.com>
 *      Krzysztof Kozlowski <krzk@kernel.org>
 *
 * Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.com>
 *
 * This program is not provided / owned by Maxim Integrated Products.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/regulator/consumer.h>
#include <linux/mfd/max77705.h>
#include <linux/mfd/max77705-private.h>

#define MAX_MAGNITUDE_SHIFT	16

enum max77705_haptic_motor_type {
	MAX77705_HAPTIC_ERM = 0,
	MAX77705_HAPTIC_LRA,
};

enum max77705_haptic_pulse_mode {
	MAX77705_HAPTIC_EXTERNAL_MODE = 0,
	MAX77705_HAPTIC_INTERNAL_MODE,
};

enum max77705_haptic_pwm_divisor {
	MAX77705_HAPTIC_PWM_DIVISOR_32 = 0,
	MAX77705_HAPTIC_PWM_DIVISOR_64,
	MAX77705_HAPTIC_PWM_DIVISOR_128,
	MAX77705_HAPTIC_PWM_DIVISOR_256,
};

struct max77705_haptic {
	enum max77705_types dev_type;

	struct regmap *regmap_pmic;
	struct regmap *regmap_haptic;
	struct device *dev;
	struct input_dev *input_dev;
	struct pwm_device *pwm_dev;
	struct regulator *motor_reg;

	bool enabled;
	bool suspend_state;
	unsigned int magnitude;
	unsigned int pwm_duty;
	enum max77705_haptic_motor_type type;
	enum max77705_haptic_pulse_mode mode;

	struct work_struct work;
};

static int max77705_haptic_set_duty_cycle(struct max77705_haptic *haptic)
{
	struct pwm_args pargs;
	int delta;
	int error;

	pwm_get_args(haptic->pwm_dev, &pargs);
	delta = (pargs.period + haptic->pwm_duty) / 2;
	error = pwm_config(haptic->pwm_dev, delta, pargs.period);
	if (error) {
		dev_err(haptic->dev, "failed to configure pwm: %d\n", error);
		return error;
	}

	return 0;
}

static int max77705_haptic_bias(struct max77705_haptic *haptic, bool on)
{
	int error;

	error = regmap_update_bits(haptic->regmap_haptic,
							   MAX77705_PMIC_REG_MAINCTRL1,
							   MAX77705_MAINCTRL1_BIASEN_MASK,
							   on << MAX77705_MAINCTRL1_BIASEN_SHIFT);

	if (error) {
		dev_err(haptic->dev, "failed to %s bias: %d\n",
			on ? "enable" : "disable", error);
		return error;
	}

	return 0;
}

static int max77705_haptic_configure(struct max77705_haptic *haptic,
				     bool enable)
{
	unsigned int value, config_reg;
	int error;

	value = ((haptic->type << MAX77705_CONFIG2_MODE_SHIFT) |
		(enable << MAX77705_CONFIG2_MEN_SHIFT) |
		(haptic->mode << MAX77705_CONFIG2_HTYP_SHIFT) |
		MAX77705_HAPTIC_PWM_DIVISOR_128);
	config_reg = MAX77705_PMIC_REG_MCONFIG;

	error = regmap_write(haptic->regmap_haptic,
			     config_reg, value);
	if (error) {
		dev_err(haptic->dev,
			"failed to update haptic config: %d\n", error);
		return error;
	}

	return 0;
}

static void max77705_haptic_enable(struct max77705_haptic *haptic)
{
	int error;

	if (haptic->enabled)
		return;

	error = pwm_enable(haptic->pwm_dev);
	if (error) {
		dev_err(haptic->dev,
			"failed to enable haptic pwm device: %d\n", error);
		return;
	}

	error = max77705_haptic_configure(haptic, true);
	if (error)
		goto err_enable_config;

	haptic->enabled = true;

	return;

err_enable_config:
	pwm_disable(haptic->pwm_dev);
}

static void max77705_haptic_disable(struct max77705_haptic *haptic)
{
	int error;

	if (!haptic->enabled)
		return;

	error = max77705_haptic_configure(haptic, false);
	if (error)
		return;

	pwm_disable(haptic->pwm_dev);
	haptic->enabled = false;
}

static void max77705_haptic_play_work(struct work_struct *work)
{
	struct max77705_haptic *haptic =
			container_of(work, struct max77705_haptic, work);
	int error;

	error = max77705_haptic_set_duty_cycle(haptic);
	if (error) {
		dev_err(haptic->dev, "failed to set duty cycle: %d\n", error);
		return;
	}

	if (haptic->magnitude)
		max77705_haptic_enable(haptic);
	else
		max77705_haptic_disable(haptic);
}

static int max77705_haptic_play_effect(struct input_dev *dev, void *data,
				       struct ff_effect *effect)
{
	struct max77705_haptic *haptic = input_get_drvdata(dev);
	struct pwm_args pargs;
	u64 period_mag_multi;

	haptic->magnitude = effect->u.rumble.strong_magnitude;
	if (!haptic->magnitude)
		haptic->magnitude = effect->u.rumble.weak_magnitude;

	/*
	 * The magnitude comes from force-feedback interface.
	 * The formula to convert magnitude to pwm_duty as follows:
	 * - pwm_duty = (magnitude * pwm_period) / MAX_MAGNITUDE(0xFFFF)
	 */
	pr_info("magnitude: %d(%x)", haptic->magnitude, haptic->magnitude);
	pwm_get_args(haptic->pwm_dev, &pargs);
	period_mag_multi = (u64)pargs.period * haptic->magnitude;
	haptic->pwm_duty = (unsigned int)(period_mag_multi >>
						MAX_MAGNITUDE_SHIFT);

	schedule_work(&haptic->work);

	return 0;
}

static int max77705_haptic_open(struct input_dev *dev)
{
	struct max77705_haptic *haptic = input_get_drvdata(dev);
	int error;

	error = max77705_haptic_bias(haptic, true);
	if (error)
		return error;

	error = regulator_enable(haptic->motor_reg);
	if (error) {
		dev_err(haptic->dev,
			"failed to enable regulator: %d\n", error);
		return error;
	}

	return 0;
}

static void max77705_haptic_close(struct input_dev *dev)
{
	struct max77705_haptic *haptic = input_get_drvdata(dev);
	int error;

	cancel_work_sync(&haptic->work);
	max77705_haptic_disable(haptic);

	error = regulator_disable(haptic->motor_reg);
	if (error)
		dev_err(haptic->dev,
			"failed to disable regulator: %d\n", error);

	max77705_haptic_bias(haptic, false);
}

static int max77705_haptic_probe(struct platform_device *pdev)
{
	struct max77705_dev *max77705 = dev_get_drvdata(pdev->dev.parent);
	struct max77705_haptic *haptic;
	int error;

	haptic = devm_kzalloc(&pdev->dev, sizeof(*haptic), GFP_KERNEL);
	if (!haptic)
		return -ENOMEM;

	haptic->regmap_pmic = max77705->regmap;
	haptic->dev = &pdev->dev;
	haptic->type = MAX77705_HAPTIC_LRA;
	haptic->mode = MAX77705_HAPTIC_EXTERNAL_MODE;
	haptic->suspend_state = false;

	/* Variant-specific init */
	haptic->dev_type = max77705->type;
	haptic->regmap_haptic = max77705->regmap;

	INIT_WORK(&haptic->work, max77705_haptic_play_work);

	/* Get pwm and regulatot for haptic device */
	haptic->pwm_dev = devm_pwm_get(&pdev->dev, NULL);
	if (IS_ERR(haptic->pwm_dev)) {
		dev_err(&pdev->dev, "failed to get pwm device\n");
		return PTR_ERR(haptic->pwm_dev);
	}

	/*
	 * FIXME: pwm_apply_args() should be removed when switching to the
	 * atomic PWM API.
	 */
	pwm_apply_args(haptic->pwm_dev);

	haptic->motor_reg = devm_regulator_get(&pdev->dev, "haptic");
	if (IS_ERR(haptic->motor_reg)) {
		dev_err(&pdev->dev, "failed to get regulator\n");
		return PTR_ERR(haptic->motor_reg);
	}

	/* Initialize input device for haptic device */
	haptic->input_dev = devm_input_allocate_device(&pdev->dev);
	if (!haptic->input_dev) {
		dev_err(&pdev->dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	haptic->input_dev->name = "max77705-haptic";
	haptic->input_dev->id.version = 1;
	haptic->input_dev->dev.parent = &pdev->dev;
	haptic->input_dev->open = max77705_haptic_open;
	haptic->input_dev->close = max77705_haptic_close;
	input_set_drvdata(haptic->input_dev, haptic);
	input_set_capability(haptic->input_dev, EV_FF, FF_RUMBLE);

	error = input_ff_create_memless(haptic->input_dev, NULL,
				max77705_haptic_play_effect);
	if (error) {
		dev_err(&pdev->dev, "failed to create force-feedback\n");
		return error;
	}

	error = input_register_device(haptic->input_dev);
	if (error) {
		dev_err(&pdev->dev, "failed to register input device\n");
		return error;
	}

	platform_set_drvdata(pdev, haptic);

	return 0;
}

static void max77705_haptic_remove(struct platform_device *pdev)
{
	struct max77705_haptic *haptic = platform_get_drvdata(pdev);

	if (haptic->enabled)
		max77705_haptic_disable(haptic);
}

static int max77705_haptic_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct max77705_haptic *haptic = platform_get_drvdata(pdev);

	if (haptic->enabled) {
		max77705_haptic_disable(haptic);
		haptic->suspend_state = true;
	}

	return 0;
}

static int max77705_haptic_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct max77705_haptic *haptic = platform_get_drvdata(pdev);

	if (haptic->suspend_state) {
		max77705_haptic_enable(haptic);
		haptic->suspend_state = false;
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(max77705_haptic_pm_ops,
				max77705_haptic_suspend,
				max77705_haptic_resume);

static const struct of_device_id of_max77705_haptic_dt_match[] = {
	{ .compatible = "maxim,max77705-haptic", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, of_max77705_haptic_dt_match);

static struct platform_driver max77705_haptic_driver = {
	.driver		= {
		.name	= "max77705-haptic",
		.pm	= pm_sleep_ptr(&max77705_haptic_pm_ops),
		.of_match_table = of_max77705_haptic_dt_match,
	},
	.probe		= max77705_haptic_probe,
	.remove_new	= max77705_haptic_remove,
};
module_platform_driver(max77705_haptic_driver);

MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_AUTHOR("Jaewon Kim <jaewon02.kim@samsung.com>");
MODULE_AUTHOR("Krzysztof Kozlowski <krzk@kernel.org>");
MODULE_DESCRIPTION("MAXIM 77705/77705 Haptic driver");
MODULE_LICENSE("GPL");
