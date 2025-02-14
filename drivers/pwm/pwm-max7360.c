// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Bootlin
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
#include <linux/err.h>
#include <linux/math.h>
#include <linux/mfd/max7360.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regmap.h>

#define MAX7360_NUM_PWMS			8
#define MAX7360_PWM_MAX_RES			255
#define MAX7360_PWM_PERIOD_NS			2000000 /* 500 Hz */
#define MAX7360_PWM_COMMON_PWN			BIT(5)
#define MAX7360_PWM_CTRL_ENABLE(n)		BIT(n)
#define MAX7360_PWM_PORT(n)			BIT(n)

struct max7360_pwm {
	struct device *parent;
	struct regmap *regmap;
};

struct max7360_pwm_waveform {
	u8 duty_steps;
};

static inline struct max7360_pwm *max7360_pwm_from_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int max7360_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct max7360_pwm *max7360_pwm;
	int ret;

	max7360_pwm = max7360_pwm_from_chip(chip);
	ret = max7360_port_pin_request(max7360_pwm->parent, pwm->hwpwm, true);
	if (ret) {
		dev_warn(&chip->dev, "failed to request pwm-%d\n", pwm->hwpwm);
		return ret;
	}

	ret = regmap_write_bits(max7360_pwm->regmap,
				MAX7360_REG_PWMCFG(pwm->hwpwm),
				MAX7360_PWM_COMMON_PWN,
				0);
	if (ret)
		return ret;

	return regmap_write_bits(max7360_pwm->regmap, MAX7360_REG_PORTS,
				 MAX7360_PWM_PORT(pwm->hwpwm),
				 MAX7360_PWM_PORT(pwm->hwpwm));
}

static void max7360_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct max7360_pwm *max7360_pwm;

	max7360_pwm = max7360_pwm_from_chip(chip);
	max7360_port_pin_request(max7360_pwm->parent, pwm->hwpwm, false);
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
	 * we only support fixed period of MAX7360_PWM_PERIOD_NS and offset of
	 * 0.
	 */

	duty_steps = mul_u64_u64_div_u64(wf->duty_length_ns, MAX7360_PWM_MAX_RES,
					 MAX7360_PWM_PERIOD_NS);

	wfhw->duty_steps = min(MAX7360_PWM_MAX_RES, duty_steps);

	return 0;
}

static int max7360_pwm_round_waveform_fromhw(struct pwm_chip *chip, struct pwm_device *pwm,
					     const void *_wfhw, struct pwm_waveform *wf)
{
	const struct max7360_pwm_waveform *wfhw = _wfhw;

	wf->period_length_ns = MAX7360_PWM_PERIOD_NS;
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
	struct max7360_pwm *max7360_pwm;
	unsigned int val;
	int ret;

	max7360_pwm = max7360_pwm_from_chip(chip);

	val = (wfhw->duty_steps == 0) ? 0 : MAX7360_PWM_CTRL_ENABLE(pwm->hwpwm);
	ret = regmap_write_bits(max7360_pwm->regmap, MAX7360_REG_GPIOCTRL,
				MAX7360_PWM_CTRL_ENABLE(pwm->hwpwm), val);

	if (!ret && wfhw->duty_steps != 0) {
		ret = regmap_write(max7360_pwm->regmap, MAX7360_REG_PWM(pwm->hwpwm),
				   wfhw->duty_steps);
	}

	return ret;
}

static int max7360_pwm_read_waveform(struct pwm_chip *chip,
				     struct pwm_device *pwm,
				     void *_wfhw)
{
	struct max7360_pwm_waveform *wfhw = _wfhw;
	struct max7360_pwm *max7360_pwm;
	unsigned int val;
	int ret;

	max7360_pwm = max7360_pwm_from_chip(chip);

	ret = regmap_read(max7360_pwm->regmap, MAX7360_REG_GPIOCTRL, &val);
	if (ret)
		return ret;

	if (val & MAX7360_PWM_CTRL_ENABLE(pwm->hwpwm)) {
		ret = regmap_read(max7360_pwm->regmap, MAX7360_REG_PWM(pwm->hwpwm),
				  &val);
		val = wfhw->duty_steps;
	} else {
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
	struct max7360_pwm *max7360_pwm;
	struct pwm_chip *chip;
	int ret;

	if (!pdev->dev.parent)
		return dev_err_probe(&pdev->dev, -ENODEV, "no parent device\n");

	chip = devm_pwmchip_alloc(pdev->dev.parent, MAX7360_NUM_PWMS,
				  sizeof(*max7360_pwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	chip->ops = &max7360_pwm_ops;

	max7360_pwm = max7360_pwm_from_chip(chip);
	max7360_pwm->parent = pdev->dev.parent;

	max7360_pwm->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!max7360_pwm->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "could not get parent regmap\n");

	ret = devm_pwmchip_add(&pdev->dev, chip);
	if (ret != 0)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to add PWM chip\n");

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
