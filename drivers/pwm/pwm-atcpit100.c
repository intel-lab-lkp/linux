// SPDX-License-Identifier: GPL-2.0-only
/*
 * PWM driver for ATCPIT100 on Andes AE350 platform
 *
 * Copyright (C) 2024 Andes Technology Corporation.
 *
 * Limitations:
 * - When disabling a channel, the current period will not be completed, and the
 *   output will be constant zero.
 * - The current period will be completed first while reconfiguring.
 * - Further, if the reconfiguration changes the clock source, the output will
 *   not be the old one nor the new one. And the output will be the new one
 *   once writing to the reload register.
 * - The hardware can neither do a 0% nor a 100% relative duty cycle.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/time.h>
#include <linux/types.h>

#define ATCPIT100_CHANNEL_MAX			4
#define ATCPIT100_CHANNEL_ENABLE		0x1C
#define ATCPIT100_CHANNEL_ENABLE_PWM(ch)	BIT(3 + (4 * ch))
#define ATCPIT100_CHANNEL_CTRL(ch)		(0x20 + (0x10 * ch))
#define ATCPIT100_CHANNEL_CTRL_MODE_PWM		0x04
#define ATCPIT100_CHANNEL_CTRL_CLK		BIT(3)
#define ATCPIT100_CHANNEL_CTRL_MASK		GENMASK(4, 0)
#define ATCPIT100_CHANNEL_RELOAD(ch)		(0x24 + (0x10 * ch))
#define ATCPIT100_CHANNEL_RELOAD_HIGH		GENMASK(31, 16)
#define ATCPIT100_CHANNEL_RELOAD_LOW		GENMASK(15, 0)
#define ATCPIT100_CYCLE_MIN			1
#define ATCPIT100_CYCLE_MAX			0x010000
#define ATCPIT100_IS_VALID_PERIOD(p)		\
		in_range(p, min_period, max_period - min_period + 1)

enum atcpit100_clk {
	ATCPIT100_CLK_EXT = 0,
	ATCPIT100_CLK_APB,
	NUM_ATCPIT100_CLK
};

struct atcpit100_pwm {
	struct regmap *regmap;
	struct clk *ext_clk;
	struct clk *apb_clk;
};

static const struct regmap_config atcpit100_pwm_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
};

static inline struct atcpit100_pwm *to_atcpit100_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int atcpit100_pwm_enable(struct pwm_chip *chip, unsigned int channel,
				bool enable)
{
	unsigned int enable_bit = ATCPIT100_CHANNEL_ENABLE_PWM(channel);
	struct atcpit100_pwm *ap = to_atcpit100_pwm(chip);

	return regmap_update_bits(ap->regmap, ATCPIT100_CHANNEL_ENABLE,
				  enable_bit, enable ? enable_bit : 0);
}

static int atcpit100_pwm_config(struct pwm_chip *chip, unsigned int channel,
				const struct pwm_state *state)
{
	int clk;
	int ret;
	unsigned int reload_val;
	unsigned long rate[NUM_ATCPIT100_CLK];
	u64 max_period;
	u64 min_period;
	u64 high_cycle;
	u64 low_cycle;
	struct atcpit100_pwm *ap = to_atcpit100_pwm(chip);
	unsigned int ctrl_val = ATCPIT100_CHANNEL_CTRL_MODE_PWM;
	u64 high_period = state->duty_cycle;
	u64 low_period = state->period - high_period;

	rate[ATCPIT100_CLK_EXT] = clk_get_rate(ap->ext_clk);
	rate[ATCPIT100_CLK_APB] = clk_get_rate(ap->apb_clk);

	/*
	 * Reload register for PWM mode:
	 *
	 *		31 : 16    15 : 0
	 *		PWM16_Hi | PWM16_Lo
	 *
	 * In the PWM mode, the high period is (PWM16_Hi + 1) cycles, and the
	 * low period is (PWM16_Lo + 1) cycles. Since we need to write
	 * "numcycles - 1" to the register, the valid range of numcycles will
	 * be between 1 to 0x10000. Calculate the possible periods that satisfy
	 * the above restriction:
	 *
	 *	Let m = 1, M = 0x10000,
	 *	m <= floor(cycle) <= M
	 * <=>	m <= floor(rate * period / NSEC_PER_SEC) <= M
	 * <=>	m <= rate * period / NSEC_PER_SEC < M + 1
	 * <=>	m * NSEC_PER_SEC / rate <= period < (M + 1) * NSEC_PER_SEC / rate
	 * <=>	ceil(m * NSEC_PER_SEC / rate) <= period <= ceil((M + 1) * NSEC_PER_SEC / rate) - 1
	 *
	 * Since there are two clock sources for ATCPIT100, if the period is not
	 * valid for the first clock source, then the second clock source will
	 * be checked. Reject the request when both clock sources are not valid
	 * for the settings.
	 */
	for (clk = ATCPIT100_CLK_EXT; clk < NUM_ATCPIT100_CLK; clk++) {
		max_period =
			DIV64_U64_ROUND_UP(
				(ATCPIT100_CYCLE_MAX + 1) * NSEC_PER_SEC,
				rate[clk]) - 1;
		min_period =
			DIV64_U64_ROUND_UP(ATCPIT100_CYCLE_MIN * NSEC_PER_SEC,
					   rate[clk]);

		if (ATCPIT100_IS_VALID_PERIOD(high_period) &&
		    ATCPIT100_IS_VALID_PERIOD(low_period))
			break;
	}

	if (clk == NUM_ATCPIT100_CLK)
		return -EINVAL;

	/*
	 * Once changing the clock source here, the output will be neither the
	 * old one nor the new one until writing to the reload register.
	 */
	ctrl_val |= clk ? ATCPIT100_CHANNEL_CTRL_CLK : 0;
	ret = regmap_update_bits(ap->regmap, ATCPIT100_CHANNEL_CTRL(channel),
				 ATCPIT100_CHANNEL_CTRL_MASK, ctrl_val);
	if (ret)
		return ret;

	high_cycle = mul_u64_u64_div_u64(rate[clk], high_period, NSEC_PER_SEC);
	low_cycle = mul_u64_u64_div_u64(rate[clk], low_period, NSEC_PER_SEC);
	reload_val = FIELD_PREP(ATCPIT100_CHANNEL_RELOAD_HIGH, high_cycle - 1) |
		     FIELD_PREP(ATCPIT100_CHANNEL_RELOAD_LOW, low_cycle - 1);

	return regmap_write(ap->regmap, ATCPIT100_CHANNEL_RELOAD(channel),
			    reload_val);
}

static int atcpit100_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			       const struct pwm_state *state)
{
	int ret;
	unsigned int channel = pwm->hwpwm;

	/* ATCPIT100 PWM driver now only supports normal polarity. */
	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled) {
		if (pwm->state.enabled)
			return atcpit100_pwm_enable(chip, channel, false);

		return 0;
	}

	if (ret)
		return ret;

	ret = atcpit100_pwm_config(chip, channel, state);
	if (ret)
		return ret;

	return atcpit100_pwm_enable(chip, channel, true);
}

static int atcpit100_pwm_get_state(struct pwm_chip *chip,
				   struct pwm_device *pwm,
				   struct pwm_state *state)
{
	int ret;
	unsigned int ctrl_val;
	unsigned int reload_val;
	unsigned long rate;
	u16 pwm_high;
	u16 pwm_low;
	unsigned int channel = pwm->hwpwm;
	struct atcpit100_pwm *ap = to_atcpit100_pwm(chip);

	ret = regmap_read(ap->regmap, ATCPIT100_CHANNEL_CTRL(channel),
			  &ctrl_val);
	if (ret)
		return ret;

	rate = (ctrl_val & ATCPIT100_CHANNEL_CTRL_CLK) ?
	       clk_get_rate(ap->apb_clk) : clk_get_rate(ap->ext_clk);
	state->enabled =
		regmap_test_bits(ap->regmap, ATCPIT100_CHANNEL_ENABLE,
				 ATCPIT100_CHANNEL_ENABLE_PWM(channel));
	state->polarity = PWM_POLARITY_NORMAL;
	ret = regmap_read(ap->regmap, ATCPIT100_CHANNEL_RELOAD(channel),
			  &reload_val);
	if (ret)
		return ret;

	pwm_high = FIELD_GET(ATCPIT100_CHANNEL_RELOAD_HIGH, reload_val);
	pwm_low = FIELD_GET(ATCPIT100_CHANNEL_RELOAD_LOW, reload_val);
	state->duty_cycle =
		DIV64_U64_ROUND_UP((pwm_high + 1) * NSEC_PER_SEC, rate);
	state->period =
		state->duty_cycle +
		DIV64_U64_ROUND_UP((pwm_low + 1) * NSEC_PER_SEC, rate);

	return 0;
}

static const struct pwm_ops atcpit_pwm_ops = {
	.apply = atcpit100_pwm_apply,
	.get_state = atcpit100_pwm_get_state,
};

static int atcpit100_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct atcpit100_pwm *ap;
	struct pwm_chip *chip;
	void __iomem *base;
	int ret;

	chip = devm_pwmchip_alloc(dev, ATCPIT100_CHANNEL_MAX, sizeof(*ap));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	ap = to_atcpit100_pwm(chip);
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ap->ext_clk = devm_clk_get(dev, "ext");
	if (IS_ERR(ap->ext_clk)) {
		dev_err_probe(dev, PTR_ERR(ap->ext_clk),
			      "failed to obtain external clock\n");
	}

	ap->apb_clk = devm_clk_get_enabled(dev, "apb");
	if (IS_ERR(ap->apb_clk)) {
		dev_err_probe(dev, PTR_ERR(ap->apb_clk),
			      "failed to obtain APB clock\n");
	}

	ap->regmap = devm_regmap_init_mmio(dev, base,
					   &atcpit100_pwm_regmap_config);
	if (IS_ERR(ap->regmap)) {
		return dev_err_probe(dev, PTR_ERR(ap->regmap),
				     "failed to init register map\n");
	}

	chip->ops = &atcpit_pwm_ops;
	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add PWM chip\n");

	return 0;
}

static const struct of_device_id atcpit100_pwm_dt[] = {
	{ .compatible = "andestech,atcpit100-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, atcpit100_pwm_dt);

static struct platform_driver atcpit100_pwm_driver = {
	.driver = {
		.name = "atcpit100-pwm",
		.of_match_table = atcpit100_pwm_dt,
	},
	.probe = atcpit100_pwm_probe,
};
module_platform_driver(atcpit100_pwm_driver);

MODULE_AUTHOR("Ben Zong-You Xie <ben717@andestech.com>");
MODULE_DESCRIPTION("Andes ATCPIT100 PWM driver");
MODULE_LICENSE("GPL");
