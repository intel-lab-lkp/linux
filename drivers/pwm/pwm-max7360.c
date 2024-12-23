// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Bootlin
 *
 * Author: Kamel BOUHARA <kamel.bouhara@bootlin.com>
 *
 * Limitations:
 * - Only supports normal polarity.
 * - The period is fixed to 2 ms.
 * - Only the duty cycle can be changed, new values are applied at the beginning
 *   of the next cycle.
 * - When disabled, the output is put in Hi-Z.
 */
#include <linux/math.h>
#include <linux/mfd/max7360.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regmap.h>

#define MAX7360_NUM_PWMS			8
#define MAX7360_PWM_MAX_RES			256
#define MAX7360_PWM_PERIOD_NS			2000000 /* 500 Hz */
#define MAX7360_PWM_COMMON_PWN			BIT(5)
#define MAX7360_PWM_CTRL_ENABLE(n)		BIT(n)
#define MAX7360_PWM_PORT(n)			BIT(n)

struct max7360_pwm {
	struct device *parent;
	struct regmap *regmap;
};

static inline struct max7360_pwm *to_max7360_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int max7360_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct max7360_pwm *max7360_pwm;
	int ret;

	max7360_pwm = to_max7360_pwm(chip);
	ret = max7360_port_pin_request(max7360_pwm->parent, pwm->hwpwm,
				       true);
	if (ret) {
		dev_warn(&chip->dev, "failed to request pwm-%d\n", pwm->hwpwm);
		return ret;
	}

	ret = regmap_write_bits(max7360_pwm->regmap,
				MAX7360_REG_PWMCFG + pwm->hwpwm,
				MAX7360_PWM_COMMON_PWN,
				0);
	if (ret) {
		dev_warn(&chip->dev,
			 "failed to write pwm-%d cfg register, error %d\n",
			 pwm->hwpwm, ret);
		return ret;
	}

	ret = regmap_write_bits(max7360_pwm->regmap, MAX7360_REG_PORTS,
				MAX7360_PWM_PORT(pwm->hwpwm),
				MAX7360_PWM_PORT(pwm->hwpwm));
	if (ret) {
		dev_warn(&chip->dev,
			 "failed to write pwm-%d ports register, error %d\n",
			 pwm->hwpwm, ret);
		return ret;
	}

	return 0;
}

static void max7360_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct max7360_pwm *max7360_pwm;
	int ret;

	max7360_pwm = to_max7360_pwm(chip);
	ret = regmap_write_bits(max7360_pwm->regmap, MAX7360_REG_GPIOCTRL,
				MAX7360_PWM_CTRL_ENABLE(pwm->hwpwm),
				0);
	if (ret)
		dev_warn(&chip->dev, "failed to disable pwm-%d , error %d\n",
			 pwm->hwpwm, ret);

	max7360_port_pin_request(max7360_pwm->parent, pwm->hwpwm,
				 false);
}

static int max7360_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			     const struct pwm_state *state)
{
	struct max7360_pwm *max7360_pwm;
	u64 duty_steps;
	int ret;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (state->period != MAX7360_PWM_PERIOD_NS) {
		dev_warn(&chip->dev,
			 "unsupported pwm period: %llu, should be %u\n",
			 state->period, MAX7360_PWM_PERIOD_NS);
		return -EINVAL;
	}

	duty_steps = mul_u64_u64_div_u64(state->duty_cycle, MAX7360_PWM_MAX_RES,
					 MAX7360_PWM_PERIOD_NS);

	max7360_pwm = to_max7360_pwm(chip);
	ret = regmap_write_bits(max7360_pwm->regmap, MAX7360_REG_GPIOCTRL,
				MAX7360_PWM_CTRL_ENABLE(pwm->hwpwm),
				MAX7360_PWM_CTRL_ENABLE(pwm->hwpwm));
	if (ret) {
		dev_warn(&chip->dev, "failed to enable pwm-%d , error %d\n",
			 pwm->hwpwm, ret);
		return ret;
	}

	ret = regmap_write(max7360_pwm->regmap, MAX7360_REG_PWMBASE + pwm->hwpwm,
			   duty_steps >= 255 ? 255 : duty_steps);
	if (ret) {
		dev_warn(&chip->dev,
			 "failed to apply pwm duty_cycle %llu on pwm-%d, error %d\n",
			 duty_steps, pwm->hwpwm, ret);
		return ret;
	}

	return 0;
}

static int max7360_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				 struct pwm_state *state)
{
	struct max7360_pwm *max7360_pwm;
	unsigned int val;
	int ret;

	max7360_pwm = to_max7360_pwm(chip);

	state->period = MAX7360_PWM_PERIOD_NS;
	state->polarity = PWM_POLARITY_NORMAL;

	ret = regmap_read(max7360_pwm->regmap, MAX7360_REG_GPIOCTRL, &val);
	if (ret) {
		dev_warn(&chip->dev,
			 "failed to read pwm configuration on pwm-%d, error %d\n",
			 pwm->hwpwm, ret);
		return ret;
	}
	state->enabled = !!(val & MAX7360_PWM_CTRL_ENABLE(pwm->hwpwm));

	ret = regmap_read(max7360_pwm->regmap, MAX7360_REG_PWMBASE + pwm->hwpwm,
			  &val);
	if (ret) {
		dev_warn(&chip->dev,
			 "failed to read pwm duty_cycle on pwm-%d, error %d\n",
			 pwm->hwpwm, ret);
		return ret;
	}
	state->duty_cycle = mul_u64_u64_div_u64(val, MAX7360_PWM_PERIOD_NS,
						MAX7360_PWM_MAX_RES);

	return 0;
}

static const struct pwm_ops max7360_pwm_ops = {
	.request = max7360_pwm_request,
	.free = max7360_pwm_free,
	.apply = max7360_pwm_apply,
	.get_state = max7360_pwm_get_state,
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

	max7360_pwm = to_max7360_pwm(chip);
	max7360_pwm->parent = pdev->dev.parent;

	max7360_pwm->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!max7360_pwm->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "could not get parent regmap\n");

	ret = devm_pwmchip_add(&pdev->dev, chip);
	if (ret != 0)
		dev_err_probe(&pdev->dev, ret, "failed to add PWM chip");

	return 0;
}

static struct platform_driver max7360_pwm_driver = {
	.driver = {
		.name = MAX7360_DRVNAME_PWM,
	},
	.probe = max7360_pwm_probe,
};
module_platform_driver(max7360_pwm_driver);

MODULE_DESCRIPTION("MAX7360 PWM driver");
MODULE_AUTHOR("Kamel BOUHARA <kamel.bouhara@bootlin.com>");
MODULE_ALIAS("platform:" MAX7360_DRVNAME_PWM);
MODULE_LICENSE("GPL");
