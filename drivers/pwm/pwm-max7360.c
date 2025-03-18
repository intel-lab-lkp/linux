// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Bootlin
 *
 * Author: Kamel BOUHARA <kamel.bouhara@bootlin.com>
 * Author: Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>
 *
 * Limitations:
 * - Only supports normal polarity.
 * - The period is fixed to 2 ms.
 * - Only the duty cycle can be changed, new values are applied at the beginning
 *   of the next cycle.
 * - When disabled, the output is put in Hi-Z.
 */
#include <linux/bits.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/mfd/max7360.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/time.h>
#include <linux/types.h>

#define MAX7360_NUM_PWMS			8
#define MAX7360_PWM_MAX_RES			255
#define MAX7360_PWM_PERIOD_NS			(2 * NSEC_PER_MSEC)

struct max7360_pwm_waveform {
	u8 duty_steps;
	bool enabled;
};

static int max7360_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct regmap *regmap;
	struct device *dev;
	int ret;

	regmap = pwmchip_get_drvdata(chip);
	dev = regmap_get_device(regmap);

	ret = regmap_write_bits(regmap, MAX7360_REG_PWMCFG(pwm->hwpwm),
				MAX7360_PORT_CFG_COMMON_PWM, 0);
	if (ret)
		return ret;

	return regmap_write_bits(regmap, MAX7360_REG_PORTS, BIT(pwm->hwpwm), BIT(pwm->hwpwm));
}

static void max7360_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct regmap *regmap;
	struct device *dev;

	regmap = pwmchip_get_drvdata(chip);
	dev = regmap_get_device(regmap);
}

static int max7360_pwm_round_waveform_tohw(struct pwm_chip *chip,
					   struct pwm_device *pwm,
					   const struct pwm_waveform *wf,
					   void *_wfhw)
{
	struct max7360_pwm_waveform *wfhw = _wfhw;
	u64 duty_steps;

	/*
	 * Ignore user provided values for period_length_ns and duty_offset_ns:
	 * we only support fixed period of MAX7360_PWM_PERIOD_NS and offset of 0.
	 */
	duty_steps = mul_u64_u64_div_u64(wf->duty_length_ns, MAX7360_PWM_MAX_RES,
					 MAX7360_PWM_PERIOD_NS);

	wfhw->duty_steps = min(MAX7360_PWM_MAX_RES, duty_steps);
	wfhw->enabled = (wf->duty_length_ns != 0);

	return 0;
}

static int max7360_pwm_round_waveform_fromhw(struct pwm_chip *chip, struct pwm_device *pwm,
					     const void *_wfhw, struct pwm_waveform *wf)
{
	const struct max7360_pwm_waveform *wfhw = _wfhw;

	wf->period_length_ns = wfhw->enabled ? MAX7360_PWM_PERIOD_NS : 0;
	wf->duty_offset_ns = 0;
	wf->duty_length_ns = DIV64_U64_ROUND_UP(wfhw->duty_steps * MAX7360_PWM_PERIOD_NS,
						MAX7360_PWM_MAX_RES);

	return 0;
}

static int max7360_pwm_write_waveform(struct pwm_chip *chip,
				      struct pwm_device *pwm,
				      const void *_wfhw)
{
	const struct max7360_pwm_waveform *wfhw = _wfhw;
	struct regmap *regmap;
	unsigned int val;
	int ret;

	regmap = pwmchip_get_drvdata(chip);
	val = (wfhw->enabled) ? BIT(pwm->hwpwm) : 0;
	ret = regmap_write_bits(regmap, MAX7360_REG_GPIOCTRL, BIT(pwm->hwpwm), val);
	if (ret)
		return ret;

	if (wfhw->duty_steps)
		return regmap_write(regmap, MAX7360_REG_PWM(pwm->hwpwm), wfhw->duty_steps);

	return 0;
}

static int max7360_pwm_read_waveform(struct pwm_chip *chip,
				     struct pwm_device *pwm,
				     void *_wfhw)
{
	struct max7360_pwm_waveform *wfhw = _wfhw;
	struct regmap *regmap;
	unsigned int val;
	int ret;

	regmap = pwmchip_get_drvdata(chip);

	ret = regmap_read(regmap, MAX7360_REG_GPIOCTRL, &val);
	if (ret)
		return ret;

	if (val & BIT(pwm->hwpwm)) {
		wfhw->enabled = true;
		ret = regmap_read(regmap, MAX7360_REG_PWM(pwm->hwpwm), &val);
		wfhw->duty_steps = val;
	} else {
		wfhw->enabled = false;
		wfhw->duty_steps = 0;
	}

	return ret;
}

static const struct pwm_ops max7360_pwm_ops = {
	.request = max7360_pwm_request,
	.free = max7360_pwm_free,
	.round_waveform_tohw = max7360_pwm_round_waveform_tohw,
	.round_waveform_fromhw = max7360_pwm_round_waveform_fromhw,
	.read_waveform = max7360_pwm_read_waveform,
	.write_waveform = max7360_pwm_write_waveform,
};

static int max7360_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct regmap *regmap;
	int ret;

	if (!dev->parent)
		return dev_err_probe(dev, -ENODEV, "no parent device\n");

	chip = devm_pwmchip_alloc(dev->parent, MAX7360_NUM_PWMS, 0);
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	chip->ops = &max7360_pwm_ops;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return dev_err_probe(dev, -ENODEV, "could not get parent regmap\n");

	pwmchip_set_drvdata(chip, regmap);

	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add PWM chip\n");

	return 0;
}

static struct platform_driver max7360_pwm_driver = {
	.driver = {
		.name = "max7360-pwm",
	},
	.probe = max7360_pwm_probe,
};
module_platform_driver(max7360_pwm_driver);

MODULE_DESCRIPTION("MAX7360 PWM driver");
MODULE_AUTHOR("Kamel BOUHARA <kamel.bouhara@bootlin.com>");
MODULE_AUTHOR("Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>");
MODULE_LICENSE("GPL");
