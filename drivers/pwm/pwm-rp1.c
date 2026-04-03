// SPDX-License-Identifier: GPL-2.0
/*
 * pwm-rp1.c
 *
 * Raspberry Pi RP1 PWM.
 *
 * Copyright © 2026 Raspberry Pi Ltd.
 *
 * Author: Naushir Patuck (naush@raspberrypi.com)
 *
 * Based on the pwm-bcm2835 driver by:
 * Bart Tanghe <bart.tanghe@thomasmore.be>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define PWM_GLOBAL_CTRL		0x000
#define PWM_CHANNEL_CTRL(x)	(0x014 + ((x) * 0x10))
#define PWM_RANGE(x)		(0x018 + ((x) * 0x10))
#define PWM_PHASE(x)		(0x01C + ((x) * 0x10))
#define PWM_DUTY(x)		(0x020 + ((x) * 0x10))

/* 8:FIFO_POP_MASK + 0:Trailing edge M/S modulation */
#define PWM_CHANNEL_DEFAULT	(BIT(8) + BIT(0))
#define PWM_CHANNEL_ENABLE(x)	BIT(x)
#define PWM_POLARITY		BIT(3)
#define SET_UPDATE		BIT(31)
#define PWM_MODE_MASK		GENMASK(1, 0)

#define NUM_PWMS		4

struct rp1_pwm {
	void __iomem	*base;
	struct clk	*clk;
};

static const struct hwmon_channel_info * const rp1_fan_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	NULL
};

static umode_t rp1_fan_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	umode_t mode = 0;

	if (type == hwmon_fan && attr == hwmon_fan_input)
		mode = 0444;

	return mode;
}

static int rp1_fan_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct rp1_pwm *rp1 = dev_get_drvdata(dev);

	if (type != hwmon_fan || attr != hwmon_fan_input)
		return -EOPNOTSUPP;

	*val = readl(rp1->base + PWM_PHASE(2));

	return 0;
}

static const struct hwmon_ops rp1_fan_hwmon_ops = {
	.is_visible = rp1_fan_hwmon_is_visible,
	.read = rp1_fan_hwmon_read,
};

static const struct hwmon_chip_info rp1_fan_hwmon_chip_info = {
	.ops = &rp1_fan_hwmon_ops,
	.info = rp1_fan_hwmon_info,
};

static void rp1_pwm_apply_config(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	u32 value;

	value = readl(rp1->base + PWM_GLOBAL_CTRL);
	value |= SET_UPDATE;
	writel(value, rp1->base + PWM_GLOBAL_CTRL);
}

static int rp1_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);

	writel(PWM_CHANNEL_DEFAULT, rp1->base + PWM_CHANNEL_CTRL(pwm->hwpwm));
	return 0;
}

static void rp1_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	u32 value;

	value = readl(rp1->base + PWM_CHANNEL_CTRL(pwm->hwpwm));
	value &= ~PWM_MODE_MASK;
	writel(value, rp1->base + PWM_CHANNEL_CTRL(pwm->hwpwm));

	rp1_pwm_apply_config(chip, pwm);
}

static int rp1_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			 const struct pwm_state *state)
{
	struct rp1_pwm *rp1 = pwmchip_get_drvdata(chip);
	unsigned long clk_rate = clk_get_rate(rp1->clk);
	unsigned long clk_period;
	u32 value;

	if (!clk_rate) {
		dev_err(&chip->dev, "failed to get clock rate\n");
		return -EINVAL;
	}

	/* set period and duty cycle */
	clk_period = DIV_ROUND_CLOSEST(NSEC_PER_SEC, clk_rate);

	writel(DIV_ROUND_CLOSEST(state->duty_cycle, clk_period),
	       rp1->base + PWM_DUTY(pwm->hwpwm));

	writel(DIV_ROUND_CLOSEST(state->period, clk_period),
	       rp1->base + PWM_RANGE(pwm->hwpwm));

	/* set polarity */
	value = readl(rp1->base + PWM_CHANNEL_CTRL(pwm->hwpwm));
	if (state->polarity == PWM_POLARITY_NORMAL)
		value &= ~PWM_POLARITY;
	else
		value |= PWM_POLARITY;
	writel(value, rp1->base + PWM_CHANNEL_CTRL(pwm->hwpwm));

	/* enable/disable */
	value = readl(rp1->base + PWM_GLOBAL_CTRL);
	if (state->enabled)
		value |= PWM_CHANNEL_ENABLE(pwm->hwpwm);
	else
		value &= ~PWM_CHANNEL_ENABLE(pwm->hwpwm);
	writel(value, rp1->base + PWM_GLOBAL_CTRL);

	rp1_pwm_apply_config(chip, pwm);

	return 0;
}

static const struct pwm_ops rp1_pwm_ops = {
	.request = rp1_pwm_request,
	.free = rp1_pwm_free,
	.apply = rp1_pwm_apply,
};

static int rp1_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *hwmon_dev;
	struct pwm_chip *chip;
	struct rp1_pwm *rp1;
	int ret;

	chip = devm_pwmchip_alloc(dev, NUM_PWMS, sizeof(*rp1));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	rp1 = pwmchip_get_drvdata(chip);

	rp1->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rp1->base))
		return PTR_ERR(rp1->base);

	rp1->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(rp1->clk))
		return dev_err_probe(dev, PTR_ERR(rp1->clk), "clock not found\n");

	ret = devm_clk_rate_exclusive_get(dev, rp1->clk);
	if (ret)
		return dev_err_probe(dev, ret, "fail to get exclusive rate\n");

	chip->ops = &rp1_pwm_ops;

	platform_set_drvdata(pdev, chip);

	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register PWM chip\n");

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "rp1_fan_tach", rp1,
							 &rp1_fan_hwmon_chip_info,
							 NULL);

	if (IS_ERR(hwmon_dev))
		return dev_err_probe(dev, PTR_ERR(hwmon_dev),
				     "failed to register hwmon fan device\n");

	return 0;
}

static int rp1_pwm_suspend(struct device *dev)
{
	struct rp1_pwm *rp1 = dev_get_drvdata(dev);

	clk_disable_unprepare(rp1->clk);

	return 0;
}

static int rp1_pwm_resume(struct device *dev)
{
	struct rp1_pwm *rp1 = dev_get_drvdata(dev);

	return clk_prepare_enable(rp1->clk);
}

static DEFINE_SIMPLE_DEV_PM_OPS(rp1_pwm_pm_ops, rp1_pwm_suspend, rp1_pwm_resume);

static const struct of_device_id rp1_pwm_of_match[] = {
	{ .compatible = "raspberrypi,rp1-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rp1_pwm_of_match);

static struct platform_driver rp1_pwm_driver = {
	.probe = rp1_pwm_probe,
	.driver = {
		.name = "rp1-pwm",
		.of_match_table = rp1_pwm_of_match,
		.pm = pm_ptr(&rp1_pwm_pm_ops),
	},
};
module_platform_driver(rp1_pwm_driver);

MODULE_DESCRIPTION("RP1 PWM driver");
MODULE_AUTHOR("Naushir Patuck <naush@raspberrypi.com>");
MODULE_LICENSE("GPL");
