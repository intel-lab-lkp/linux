// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Andes PWM, used in Andes AE350 platform and QiLai SoC
 *
 * Copyright (C) 2026 Andes Technology Corporation.
 *
 * Limitations:
 * - When disabling a channel, the current period will not be completed, and the
 *   output will be constant zero.
 * - The current period will be completed first if reconfiguring.
 * - Further, if the reconfiguration changes the clock source, the output will
 *   not be the old one nor the new one. And the output will be the new one
 *   until writing to the reload register.
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

#define ANDES_PWM_CH_ENABLE		0x1C
#define ANDES_PWM_CH_ENABLE_PWM(ch)	BIT(3 + (4 * (ch)))

#define ANDES_PWM_CH_CTRL(ch)		(0x20 + (0x10 * (ch)))
#define ANDES_PWM_CH_CTRL_MODE_PWM	BIT(2)
#define ANDES_PWM_CH_CTRL_CLK		BIT(3)
#define ANDES_PWM_CH_CTRL_PARK		BIT(4)
#define ANDES_PWM_CH_CTRL_MASK		GENMASK(4, 0)

#define ANDES_PWM_CH_RELOAD(ch)		(0x24 + (0x10 * (ch)))
#define ANDES_PWM_CH_RELOAD_HIGH	GENMASK(31, 16)
#define ANDES_PWM_CH_RELOAD_LOW		GENMASK(15, 0)

#define ANDES_PWM_CH_COUNTER(ch)	(0x28 + (0x10 * (ch)))

#define ANDES_PWM_CH_MAX		4
#define ANDES_PWM_CYCLE_MIN		1
#define ANDES_PWM_CYCLE_MAX		0x10000

struct andes_pwm {
	struct regmap *regmap;
	struct clk *pclk;
	struct clk *extclk;
	unsigned int pclk_rate;
	unsigned int extclk_rate;
};

static const struct regmap_config andes_pwm_regmap_config = {
	.name = "andes_pwm",
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.pad_bits = 0,
	.max_register = ANDES_PWM_CH_COUNTER(ANDES_PWM_CH_MAX - 1),
	.cache_type = REGCACHE_NONE,
};

static inline struct andes_pwm *to_andes_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int andes_pwm_enable(struct pwm_chip *chip, unsigned int channel,
			    bool enable)
{
	struct andes_pwm *ap = to_andes_pwm(chip);

	return regmap_assign_bits(ap->regmap, ANDES_PWM_CH_ENABLE,
				  ANDES_PWM_CH_ENABLE_PWM(channel), enable);
}

static int andes_pwm_config(struct pwm_chip *chip, unsigned int channel,
			    const struct pwm_state *state)
{
	struct andes_pwm *ap = to_andes_pwm(chip);
	unsigned int clk_rate = ap->extclk_rate;
	unsigned int try = 2;
	u64 high_ns = state->duty_cycle;
	u64 low_ns = state->period - high_ns;
	unsigned int ctrl = ANDES_PWM_CH_CTRL_MODE_PWM;
	u64 high_cycles;
	u64 low_cycles;
	u32 reload;

	/*
	 * Reload register for PWM mode:
	 *
	 *		31 : 16    15 : 0
	 *		PWM16_Hi | PWM16_Lo
	 *
	 * The high duration is (PWM16_Hi + 1) cycles and the low duration is
	 * (PWM16_Lo + 1) cycles. For a duty cycle of 10 cycles and a total
	 * period of 30 cycles in normal polarity, PWM16_Hi is set to
	 * 9 (10 - 1) and PWM16_Lo to 19 (30 - 10 - 1). Also, PWM16_Hi is set to
	 * 19 and PWM16_Lo is set to 9 in inversed polarity.
	 *
	 * Because the register stores "cycles - 1", the valid range for
	 * each phase is 1 to 65536 (0x10000) cycles. This implies the hardware
	 * cannot achieve a true 0% or 100% duty cycle.
	 *
	 * The controller supports two clock sources: the APB clock and an
	 * external clock. The driver first attempts to use the external clock
	 * to widest possible range of supported periods. If the requests
	 * exceeds the valid range of the register, it falls back to the APB
	 * clock. The request is rejected if the timing cannot be met by either
	 * source.
	 */
	if (state->polarity == PWM_POLARITY_INVERSED)
		swap(high_ns, low_ns);

	while (try) {
		high_cycles = mul_u64_u64_div_u64(clk_rate, high_ns,
						  NSEC_PER_SEC);
		low_cycles = mul_u64_u64_div_u64(clk_rate, low_ns,
						 NSEC_PER_SEC);
		if (high_cycles > ANDES_PWM_CYCLE_MAX)
			high_cycles = ANDES_PWM_CYCLE_MAX;

		if (low_cycles > ANDES_PWM_CYCLE_MAX)
			low_cycles = ANDES_PWM_CYCLE_MAX;

		if (high_cycles >= ANDES_PWM_CYCLE_MIN &&
		    low_cycles >= ANDES_PWM_CYCLE_MIN)
			break;

		try--;
		clk_rate = ap->pclk_rate;
	}

	/*
	 * try == 0 : no clock is valid
	 * try == 1 : use APB clock
	 * try == 2 : use external clock
	 */
	if (!try)
		return -EINVAL;

	/*
	 * If changing the clock source here, the output will not be the old one
	 * nor the new one. And the output will be the new one until writing to
	 * the reload register.
	 */
	ctrl |= (try == 1) ? ANDES_PWM_CH_CTRL_CLK : 0;
	ctrl |= (state->polarity == PWM_POLARITY_INVERSED) ?
		ANDES_PWM_CH_CTRL_PARK : 0;
	regmap_update_bits(ap->regmap, ANDES_PWM_CH_CTRL(channel),
			   ANDES_PWM_CH_CTRL_MASK, ctrl);
	reload = FIELD_PREP(ANDES_PWM_CH_RELOAD_HIGH, high_cycles - 1) |
		 FIELD_PREP(ANDES_PWM_CH_RELOAD_LOW, low_cycles - 1);

	return regmap_write(ap->regmap, ANDES_PWM_CH_RELOAD(channel), reload);
}

static int andes_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			   const struct pwm_state *state)
{
	unsigned int channel = pwm->hwpwm;
	int ret;

	if (!state->enabled) {
		if (pwm->state.enabled)
			andes_pwm_enable(chip, channel, false);

		return 0;
	}

	ret = andes_pwm_config(chip, channel, state);
	if (ret)
		return ret;

	return andes_pwm_enable(chip, channel, true);
}

static int andes_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			       struct pwm_state *state)
{
	struct andes_pwm *ap = to_andes_pwm(chip);
	unsigned int channel = pwm->hwpwm;
	unsigned int ctrl;
	unsigned int clk_rate;
	unsigned int reload;
	u64 high_cycles;
	u64 low_cycles;

	regmap_read(ap->regmap, ANDES_PWM_CH_CTRL(channel), &ctrl);
	clk_rate = FIELD_GET(ANDES_PWM_CH_CTRL_CLK, ctrl) ? ap->pclk_rate
							  : ap->extclk_rate;
	state->enabled = regmap_test_bits(ap->regmap, ANDES_PWM_CH_ENABLE,
					  ANDES_PWM_CH_ENABLE_PWM(channel));
	state->polarity = regmap_test_bits(ap->regmap,
					   ANDES_PWM_CH_CTRL(channel),
					   ANDES_PWM_CH_CTRL_PARK);
	regmap_read(ap->regmap, ANDES_PWM_CH_RELOAD(channel), &reload);
	high_cycles = FIELD_GET(ANDES_PWM_CH_RELOAD_HIGH, reload) + 1;
	low_cycles = FIELD_GET(ANDES_PWM_CH_RELOAD_LOW, reload) + 1;

	/*
	 * high_cycles and low_cycles are both 16 bits, and NSEC_PER_SEC is 30
	 * bits. Thus, the multiplication is safe from overflow
	 */
	if (state->polarity == PWM_POLARITY_NORMAL) {
		state->duty_cycle = DIV_ROUND_UP_ULL(high_cycles * NSEC_PER_SEC,
						     clk_rate);
		state->period = state->duty_cycle +
				DIV_ROUND_UP_ULL(low_cycles * NSEC_PER_SEC,
						 clk_rate);
	} else {
		state->duty_cycle = DIV_ROUND_UP_ULL(low_cycles * NSEC_PER_SEC,
						     clk_rate);
		state->period = state->duty_cycle +
				DIV_ROUND_UP_ULL(high_cycles * NSEC_PER_SEC,
						 clk_rate);
	}

	return 0;
}

static const struct pwm_ops andes_pwm_ops = {
	.apply = andes_pwm_apply,
	.get_state = andes_pwm_get_state,
};

static int andes_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct andes_pwm *ap;
	void __iomem *reg_base;
	int ret;

	chip = devm_pwmchip_alloc(dev, ANDES_PWM_CH_MAX, sizeof(*ap));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	ap = to_andes_pwm(chip);
	reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg_base))
		return dev_err_probe(dev, PTR_ERR(reg_base),
				     "failed to map I/O space\n");

	ap->pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(ap->pclk))
		return dev_err_probe(dev, PTR_ERR(ap->pclk),
				     "failed to get APB clock\n");

	ap->extclk = devm_clk_get_optional_enabled(dev, "extclk");
	if (IS_ERR(ap->extclk))
		return dev_err_probe(dev, PTR_ERR(ap->extclk),
				     "failed to get external clock\n");

	/*
	 * If the clock rate is greater than 10^9, there may be an overflow when
	 * calculating the cycles in andes_pwm_config()
	 */
	ap->pclk_rate = clk_get_rate(ap->pclk);
	if (ap->pclk_rate > NSEC_PER_SEC)
		ap->pclk = NULL;

	ap->extclk_rate = ap->extclk ? clk_get_rate(ap->extclk) : 0;
	if (ap->extclk_rate > NSEC_PER_SEC)
		ap->extclk = NULL;

	if (!ap->pclk && !ap->extclk)
		return dev_err_probe(dev, -EINVAL, "clocks are out of range\n");

	ap->regmap = devm_regmap_init_mmio(dev, reg_base,
					   &andes_pwm_regmap_config);
	if (IS_ERR(ap->regmap)) {
		return dev_err_probe(dev, PTR_ERR(ap->regmap),
				     "failed to initialize regmap\n");
	}

	chip->ops = &andes_pwm_ops;
	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add pwm chip\n");

	return 0;
}

static const struct of_device_id andes_pwm_of_match[] = {
	{ .compatible = "andestech,ae350-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, andes_pwm_of_match);

static struct platform_driver andes_pwm_driver = {
	.driver = {
		.name = "andes_pwm",
		.of_match_table = andes_pwm_of_match,
	},
	.probe = andes_pwm_probe,
};
module_platform_driver(andes_pwm_driver);

MODULE_AUTHOR("Ben Zong-You Xie <ben717@andestech.com>");
MODULE_DESCRIPTION("Andes PWM driver");
MODULE_LICENSE("GPL");
