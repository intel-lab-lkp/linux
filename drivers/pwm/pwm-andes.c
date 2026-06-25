// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Andes PWM, used in Andes AE350 platform and QiLai SoC
 *
 * Copyright (C) 2026 Andes Technology Corporation.
 *
 * Limitations:
 * - When disabling a channel, the current period is not completed and the
 *   output is driven to the PARK level (low when ANDES_PWM_CH_CTRL_PARK is
 *   clear, high when it is set).
 * - The current period will be completed first if reconfiguring.
 * - Further, if the reconfiguration changes the clock source, the output will
 *   not be the old one nor the new one. And the output will be the new one
 *   after writing to the reload register.
 * - The hardware cannot run a 0% or 100% relative duty cycle; the driver
 *   emulates these by disabling the channel and parking the output at the
 *   constant level.
 * - A period or duty cycle larger than the selected clock can represent is
 *   rounded down to the largest achievable value rather than rejected.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/minmax.h>
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

static inline struct andes_pwm *andes_pwm_from_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int andes_pwm_enable(struct pwm_chip *chip, unsigned int channel,
			    bool enable)
{
	struct andes_pwm *ap = andes_pwm_from_chip(chip);

	return regmap_assign_bits(ap->regmap, ANDES_PWM_CH_ENABLE,
				  ANDES_PWM_CH_ENABLE_PWM(channel), enable);
}

/*
 * Hold the output at a constant level by parking the disabled channel. A
 * disabled channel drives its output to the PARK level (low when @high is
 * false, high when @high is true), which is used to emulate a 0% or 100%
 * relative duty cycle.
 */
static int andes_pwm_park(struct pwm_chip *chip, unsigned int channel,
			  bool high)
{
	struct andes_pwm *ap = andes_pwm_from_chip(chip);

	regmap_assign_bits(ap->regmap, ANDES_PWM_CH_CTRL(channel),
			   ANDES_PWM_CH_CTRL_PARK, high);

	return andes_pwm_enable(chip, channel, false);
}

static int andes_pwm_config(struct pwm_chip *chip, unsigned int channel,
			    const struct pwm_state *state)
{
	struct andes_pwm *ap = andes_pwm_from_chip(chip);
	unsigned int clk_rate = ap->extclk_rate;
	unsigned int ctrl = ANDES_PWM_CH_CTRL_MODE_PWM;
	bool use_pclk = false;
	u64 high_cycles;
	u64 low_cycles;
	u64 period_cycles;
	u64 duty_cycles;
	u32 reload;

	/*
	 * Reload register for PWM mode:
	 *
	 *		31 : 16    15 : 0
	 *		PWM16_Hi | PWM16_Lo
	 *
	 * The high duration is (PWM16_Hi + 1) cycles and the low duration is
	 * (PWM16_Lo + 1) cycles, so each phase spans ANDES_PWM_CYCLE_MIN to
	 * ANDES_PWM_CYCLE_MAX cycles. The hardware period (their sum) can reach
	 * 2 * ANDES_PWM_CYCLE_MAX cycles, but the PWM core requires the period
	 * to be chosen from the requested period alone, independent of the duty
	 * cycle. That holds only while both phases stay within
	 * ANDES_PWM_CYCLE_MAX for every duty split, so the usable period is
	 * capped at ANDES_PWM_CYCLE_MAX + ANDES_PWM_CYCLE_MIN cycles.
	 *
	 * The controller has two clock sources, the APB clock and an external
	 * clock. Since the external clock frequency must be slower than the APB
	 * clock, it is tried first for its wider period range; the APB clock is
	 * used only when the external clock is too fast to represent the period
	 * (it resolves fewer than two cycles) or is absent.
	 */
	period_cycles = mul_u64_u64_div_u64(clk_rate, state->period,
					    NSEC_PER_SEC);
	if (period_cycles < 2 * ANDES_PWM_CYCLE_MIN) {
		use_pclk = true;
		clk_rate = ap->pclk_rate;
		period_cycles = mul_u64_u64_div_u64(clk_rate, state->period,
						    NSEC_PER_SEC);
		if (period_cycles < 2 * ANDES_PWM_CYCLE_MIN)
			return -EINVAL;
	}

	/*
	 * Round the period down to the largest value representable for every
	 * duty cycle, so the chosen period depends on the requested period
	 * alone. With both phases capped at ANDES_PWM_CYCLE_MAX, that bound is
	 * ANDES_PWM_CYCLE_MAX + ANDES_PWM_CYCLE_MIN cycles.
	 */
	period_cycles = min_t(u64, period_cycles,
			      ANDES_PWM_CYCLE_MAX + ANDES_PWM_CYCLE_MIN);

	/* The duty cycle cannot exceed the (possibly clamped) period. */
	duty_cycles = mul_u64_u64_div_u64(clk_rate, state->duty_cycle,
					  NSEC_PER_SEC);
	duty_cycles = min_t(u64, duty_cycles, period_cycles);
	if (state->polarity == PWM_POLARITY_INVERSED) {
		low_cycles = duty_cycles;
		high_cycles = period_cycles - low_cycles;
	} else {
		high_cycles = duty_cycles;
		low_cycles = period_cycles - high_cycles;
	}

	/*
	 * A zero-length phase means a 0% or 100% relative duty cycle, which the
	 * hardware cannot run. Emit the matching constant level by parking the
	 * channel: high_cycles == 0 stays low, low_cycles == 0 stays high.
	 */
	if (!high_cycles)
		return andes_pwm_park(chip, channel, false);
	if (!low_cycles)
		return andes_pwm_park(chip, channel, true);

	/*
	 * If changing the clock source here, the output will not be the old one
	 * nor the new one. And the output will be the new one after writing to
	 * the reload register.
	 */
	ctrl |= use_pclk ? ANDES_PWM_CH_CTRL_CLK : 0;
	ctrl |= (state->polarity == PWM_POLARITY_INVERSED) ?
		ANDES_PWM_CH_CTRL_PARK : 0;
	regmap_update_bits(ap->regmap, ANDES_PWM_CH_CTRL(channel),
			   ANDES_PWM_CH_CTRL_MASK, ctrl);
	reload = FIELD_PREP(ANDES_PWM_CH_RELOAD_HIGH, high_cycles - 1) |
		 FIELD_PREP(ANDES_PWM_CH_RELOAD_LOW, low_cycles - 1);
	regmap_write(ap->regmap, ANDES_PWM_CH_RELOAD(channel), reload);
	return andes_pwm_enable(chip, channel, true);
}

static int andes_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			   const struct pwm_state *state)
{
	unsigned int channel = pwm->hwpwm;

	if (!state->enabled) {
		if (pwm->state.enabled)
			andes_pwm_enable(chip, channel, false);

		return 0;
	}

	return andes_pwm_config(chip, channel, state);
}

static int andes_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			       struct pwm_state *state)
{
	struct andes_pwm *ap = andes_pwm_from_chip(chip);
	unsigned int channel = pwm->hwpwm;
	unsigned int ctrl;
	unsigned int clk_rate;
	unsigned int reload;
	u64 high_cycles;
	u64 low_cycles;

	regmap_read(ap->regmap, ANDES_PWM_CH_CTRL(channel), &ctrl);
	clk_rate = FIELD_GET(ANDES_PWM_CH_CTRL_CLK, ctrl) ? ap->pclk_rate
							  : ap->extclk_rate;
	if (!clk_rate) {
		/*
		 * The selected clock source is unavailable, so the channel
		 * cannot be running; report it as disabled and avoid the
		 * division by zero below.
		 */
		state->enabled = false;
		state->period = 0;
		state->duty_cycle = 0;
		return 0;
	}

	state->enabled = regmap_test_bits(ap->regmap, ANDES_PWM_CH_ENABLE,
					  ANDES_PWM_CH_ENABLE_PWM(channel)) > 0;
	state->polarity = FIELD_GET(ANDES_PWM_CH_CTRL_PARK, ctrl) ?
			  PWM_POLARITY_INVERSED : PWM_POLARITY_NORMAL;
	regmap_read(ap->regmap, ANDES_PWM_CH_RELOAD(channel), &reload);
	high_cycles = FIELD_GET(ANDES_PWM_CH_RELOAD_HIGH, reload) + 1;
	low_cycles = FIELD_GET(ANDES_PWM_CH_RELOAD_LOW, reload) + 1;

	/*
	 * high_cycles and low_cycles are each at most ANDES_PWM_CYCLE_MAX
	 * (0x10000, 17 bits) and NSEC_PER_SEC is below 2^30, so the products
	 * below are safe from 64-bit overflow.
	 */
	if (state->polarity == PWM_POLARITY_INVERSED)
		state->duty_cycle = DIV_ROUND_UP_ULL(low_cycles * NSEC_PER_SEC,
						     clk_rate);
	else
		state->duty_cycle = DIV_ROUND_UP_ULL(high_cycles * NSEC_PER_SEC,
						     clk_rate);

	state->period = DIV_ROUND_UP_ULL((high_cycles + low_cycles) *
					 NSEC_PER_SEC, clk_rate);

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
	unsigned long pclk_rate;
	unsigned long extclk_rate;
	int ret;

	chip = devm_pwmchip_alloc(dev, ANDES_PWM_CH_MAX, sizeof(*ap));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	ap = andes_pwm_from_chip(chip);
	reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg_base))
		return dev_err_probe(dev, PTR_ERR(reg_base),
				     "Failed to map I/O space\n");

	ap->pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(ap->pclk))
		return dev_err_probe(dev, PTR_ERR(ap->pclk),
				     "Failed to get APB clock\n");

	ap->extclk = devm_clk_get_optional_enabled(dev, "extclk");
	if (IS_ERR(ap->extclk))
		return dev_err_probe(dev, PTR_ERR(ap->extclk),
				     "Failed to get external clock\n");

	/*
	 * If the clock rate is greater than 10^9, there may be an overflow when
	 * calculating the cycles in andes_pwm_config()
	 */
	pclk_rate = clk_get_rate(ap->pclk);
	extclk_rate = clk_get_rate(ap->extclk);

	ap->pclk_rate = pclk_rate > NSEC_PER_SEC ? 0 : pclk_rate;
	ap->extclk_rate = extclk_rate > NSEC_PER_SEC ? 0 : extclk_rate;

	if (!ap->pclk_rate && !ap->extclk_rate)
		return dev_err_probe(dev, -EINVAL,
				     "No usable clock: pclk %lu Hz, extclk %lu Hz\n",
				     pclk_rate, extclk_rate);

	ap->regmap = devm_regmap_init_mmio(dev, reg_base,
					   &andes_pwm_regmap_config);
	if (IS_ERR(ap->regmap))
		return dev_err_probe(dev, PTR_ERR(ap->regmap),
				     "Failed to initialize regmap\n");

	chip->ops = &andes_pwm_ops;
	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add PWM chip\n");

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
