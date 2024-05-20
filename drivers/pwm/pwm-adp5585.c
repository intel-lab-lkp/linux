// SPDX-License-Identifier: GPL-2.0-only
/*
 * Analog Devices ADP5585 PWM driver
 *
 * Copyright 2022 NXP
 * Copyright 2024 Ideas on Board Oy
 */

#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/mfd/adp5585.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/time.h>

#define ADP5585_PWM_CHAN_NUM		1

#define ADP5585_PWM_OSC_FREQ_HZ		1000000U
#define ADP5585_PWM_MIN_PERIOD_NS	(2ULL * NSEC_PER_SEC / ADP5585_PWM_OSC_FREQ_HZ)
#define ADP5585_PWM_MAX_PERIOD_NS	(2ULL * 0xffff * NSEC_PER_SEC / ADP5585_PWM_OSC_FREQ_HZ)

struct adp5585_pwm_chip {
	struct pwm_chip chip;
	struct regmap *regmap;
	struct mutex lock;
	u8 pin_config_val;
};

static inline struct adp5585_pwm_chip *
to_adp5585_pwm_chip(struct pwm_chip *chip)
{
	return container_of(chip, struct adp5585_pwm_chip, chip);
}

static int pwm_adp5585_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct adp5585_pwm_chip *adp5585_pwm = to_adp5585_pwm_chip(chip);
	unsigned int val;
	int ret;

	guard(mutex)(&adp5585_pwm->lock);

	ret = regmap_read(adp5585_pwm->regmap, ADP5585_PIN_CONFIG_C, &val);
	if (ret)
		return ret;

	adp5585_pwm->pin_config_val = val;

	ret = regmap_update_bits(adp5585_pwm->regmap, ADP5585_PIN_CONFIG_C,
				 ADP5585_R3_EXTEND_CFG_MASK,
				 ADP5585_R3_EXTEND_CFG_PWM_OUT);
	if (ret)
		return ret;

	ret = regmap_update_bits(adp5585_pwm->regmap, ADP5585_GENERAL_CFG,
				 ADP5585_OSC_EN, ADP5585_OSC_EN);
	if (ret)
		return ret;

	return 0;
}

static void pwm_adp5585_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct adp5585_pwm_chip *adp5585_pwm = to_adp5585_pwm_chip(chip);

	guard(mutex)(&adp5585_pwm->lock);

	regmap_update_bits(adp5585_pwm->regmap, ADP5585_PIN_CONFIG_C,
			   ADP5585_R3_EXTEND_CFG_MASK,
			   adp5585_pwm->pin_config_val);
	regmap_update_bits(adp5585_pwm->regmap, ADP5585_GENERAL_CFG,
			   ADP5585_OSC_EN, 0);
}

static int pwm_adp5585_apply(struct pwm_chip *chip,
			     struct pwm_device *pwm,
			     const struct pwm_state *state)
{
	struct adp5585_pwm_chip *adp5585_pwm = to_adp5585_pwm_chip(chip);
	u32 on, off;
	int ret;

	if (!state->enabled) {
		guard(mutex)(&adp5585_pwm->lock);

		return regmap_update_bits(adp5585_pwm->regmap, ADP5585_PWM_CFG,
					  ADP5585_PWM_EN, 0);
	}

	if (state->period < ADP5585_PWM_MIN_PERIOD_NS ||
	    state->period > ADP5585_PWM_MAX_PERIOD_NS)
		return -EINVAL;

	/*
	 * Compute the on and off time. As the internal oscillator frequency is
	 * 1MHz, the calculation can be simplified without loss of precision.
	 */
	on = DIV_ROUND_CLOSEST_ULL(state->duty_cycle,
				   NSEC_PER_SEC / ADP5585_PWM_OSC_FREQ_HZ);
	off = DIV_ROUND_CLOSEST_ULL(state->period - state->duty_cycle,
				    NSEC_PER_SEC / ADP5585_PWM_OSC_FREQ_HZ);

	if (state->polarity == PWM_POLARITY_INVERSED)
		swap(on, off);

	guard(mutex)(&adp5585_pwm->lock);

	ret = regmap_write(adp5585_pwm->regmap, ADP5585_PWM_OFFT_LOW,
			   off & 0xff);
	if (ret)
		return ret;
	ret = regmap_write(adp5585_pwm->regmap, ADP5585_PWM_OFFT_HIGH,
			   (off >> 8) & 0xff);
	if (ret)
		return ret;
	ret = regmap_write(adp5585_pwm->regmap, ADP5585_PWM_ONT_LOW,
			   on & 0xff);
	if (ret)
		return ret;
	ret = regmap_write(adp5585_pwm->regmap, ADP5585_PWM_ONT_HIGH,
			   (on >> 8) & 0xff);
	if (ret)
		return ret;

	/* Enable PWM in continuous mode and no external AND'ing. */
	ret = regmap_update_bits(adp5585_pwm->regmap, ADP5585_PWM_CFG,
				 ADP5585_PWM_IN_AND | ADP5585_PWM_MODE |
				 ADP5585_PWM_EN, ADP5585_PWM_EN);
	if (ret)
		return ret;

	return 0;
}

static int pwm_adp5585_get_state(struct pwm_chip *chip,
				 struct pwm_device *pwm,
				 struct pwm_state *state)
{
	struct adp5585_pwm_chip *adp5585_pwm = to_adp5585_pwm_chip(chip);
	unsigned int on, off;
	unsigned int val;

	regmap_read(adp5585_pwm->regmap, ADP5585_PWM_OFFT_LOW, &off);
	regmap_read(adp5585_pwm->regmap, ADP5585_PWM_OFFT_HIGH, &val);
	off |= val << 8;

	regmap_read(adp5585_pwm->regmap, ADP5585_PWM_ONT_LOW, &on);
	regmap_read(adp5585_pwm->regmap, ADP5585_PWM_ONT_HIGH, &val);
	on |= val << 8;

	state->duty_cycle = on * (NSEC_PER_SEC / ADP5585_PWM_OSC_FREQ_HZ);
	state->period = (on + off) * (NSEC_PER_SEC / ADP5585_PWM_OSC_FREQ_HZ);

	state->polarity = PWM_POLARITY_NORMAL;

	regmap_read(adp5585_pwm->regmap, ADP5585_PWM_CFG, &val);
	state->enabled = !!(val & ADP5585_PWM_EN);

	return 0;
}

static const struct pwm_ops adp5585_pwm_ops = {
	.request = pwm_adp5585_request,
	.free = pwm_adp5585_free,
	.apply = pwm_adp5585_apply,
	.get_state = pwm_adp5585_get_state,
};

static int adp5585_pwm_probe(struct platform_device *pdev)
{
	struct adp5585_dev *adp5585 = dev_get_drvdata(pdev->dev.parent);
	struct adp5585_pwm_chip *adp5585_pwm;
	int ret;

	adp5585_pwm = devm_kzalloc(&pdev->dev, sizeof(*adp5585_pwm), GFP_KERNEL);
	if (!adp5585_pwm)
		return -ENOMEM;

	platform_set_drvdata(pdev, adp5585_pwm);

	adp5585_pwm->regmap = adp5585->regmap;

	mutex_init(&adp5585_pwm->lock);

	adp5585_pwm->chip.dev = &pdev->dev;
	adp5585_pwm->chip.ops = &adp5585_pwm_ops;
	adp5585_pwm->chip.npwm = ADP5585_PWM_CHAN_NUM;

	ret = devm_pwmchip_add(&pdev->dev, &adp5585_pwm->chip);
	if (ret) {
		mutex_destroy(&adp5585_pwm->lock);
		return dev_err_probe(&pdev->dev, ret, "failed to add PWM chip\n");
	}

	return 0;
}

static void adp5585_pwm_remove(struct platform_device *pdev)
{
	struct adp5585_pwm_chip *adp5585_pwm = platform_get_drvdata(pdev);

	mutex_destroy(&adp5585_pwm->lock);
}

static const struct of_device_id adp5585_pwm_of_match[] = {
	{ .compatible = "adi,adp5585-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, adp5585_pwm_of_match);

static struct platform_driver adp5585_pwm_driver = {
	.driver	= {
		.name = "adp5585-pwm",
		.of_match_table = adp5585_pwm_of_match,
	},
	.probe = adp5585_pwm_probe,
	.remove_new = adp5585_pwm_remove,
};
module_platform_driver(adp5585_pwm_driver);

MODULE_AUTHOR("Xiaoning Wang <xiaoning.wang@nxp.com>");
MODULE_DESCRIPTION("ADP5585 PWM Driver");
MODULE_LICENSE("GPL");
