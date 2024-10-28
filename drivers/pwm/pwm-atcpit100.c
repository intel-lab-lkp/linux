// SPDX-License-Identifier: GPL-2.0-only
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
#define CLK_EXTERNAL				32768
#define CLK_APB					60000000
#define CYCLE_MIN				0x01
#define CYCLE_MAX				0x010000

struct atcpit100_pwm {
	struct regmap *regmap;
	u32 clk_src[ATCPIT100_CHANNEL_MAX];
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

static int of_atcpit100_pwm_set_clk_src(struct atcpit100_pwm *ap,
					struct device_node *np)
{
	int ret;

	for (int i = 0; i < ATCPIT100_CHANNEL_MAX; i++) {
		ret = of_property_read_u32_index(np, "andestech,clock-source",
						 i, &(ap->clk_src[i]));
		if (ret)
			return ret;
	}

	return 0;
}

static int atcpit100_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm,
				bool enable)
{
	unsigned int channel = pwm->hwpwm;
	unsigned int enable_bit = ATCPIT100_CHANNEL_ENABLE_PWM(channel);
	struct atcpit100_pwm *ap = to_atcpit100_pwm(chip);

	pwm->state.enabled = enable;
	return regmap_update_bits(ap->regmap, ATCPIT100_CHANNEL_ENABLE,
				  enable_bit, enable ? enable_bit : 0);
}

static int atcpit100_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
				const struct pwm_state *state)
{
	int ret;
	u64 period_cycle;
	u64 duty_cycle;
	u16 pwm_high;
	u16 pwm_low;
	struct atcpit100_pwm *ap = to_atcpit100_pwm(chip);
	unsigned int ctrl_val = 0;
	unsigned int channel = pwm->hwpwm;
	u64 rate = ap->clk_src[channel] ? CLK_APB : CLK_EXTERNAL;

	/* cycle count = clock rate * time */
	period_cycle = mul_u64_u64_div_u64(rate, state->period, NSEC_PER_SEC);
	duty_cycle = mul_u64_u64_div_u64(rate, state->duty_cycle,
					 NSEC_PER_SEC);
	if (period_cycle < CYCLE_MIN || period_cycle > CYCLE_MAX ||
	    duty_cycle < CYCLE_MIN || duty_cycle > CYCLE_MAX) {
		dev_err(pwmchip_parent(chip),
			"channel%d: period cycles = 0x%llx, duty cycles = 0x%llx\n",
			channel, period_cycle, duty_cycle);
		return -EINVAL;
	}

	/*
	 * In the PWM mode, the high period is (PWM16_Hi + 1) cycles, and the
	 * low period is (PWM16_Lo + 1) cycles.
	 * For example, if period is 30 cycles and duty_cycle is 10 cycles,
	 * PWM16_Hi = 10 - 1 = 9, PWM16_Lo = 30 - 10 - 1 = 19.
	 */
	pwm_high = duty_cycle - 1;
	pwm_low = period_cycle - duty_cycle - 1;

	/* Set control register. */
	ctrl_val |= ATCPIT100_CHANNEL_CTRL_MODE_PWM;
	ctrl_val |= ap->clk_src[channel] ? ATCPIT100_CHANNEL_CTRL_CLK : 0;
	ret = regmap_update_bits(ap->regmap, ATCPIT100_CHANNEL_CTRL(channel),
				 ATCPIT100_CHANNEL_CTRL_MASK, ctrl_val);
	if (ret)
		return ret;

	/* Set reload register. */
	ret = regmap_write(ap->regmap, ATCPIT100_CHANNEL_RELOAD(channel),
			   (pwm_high << 16) | pwm_low);
	if (ret)
		return ret;

	return 0;
}

static int atcpit100_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			       const struct pwm_state *state)
{
	int ret;

	/* ATCPIT100 PWM driver now only supports normal polarity. */
	if (state->polarity != PWM_POLARITY_NORMAL) {
		dev_err(pwmchip_parent(chip),
			"only supports normal polarity now\n");
		return -EINVAL;
	}

	if (!state->enabled) {
		if (pwm->state.enabled)
			return atcpit100_pwm_enable(chip, pwm, 0);

		return 0;
	}

	ret = atcpit100_pwm_config(chip, pwm, state);
	if (ret)
		return ret;

	return atcpit100_pwm_enable(chip, pwm, 1);
}

static int atcpit100_pwm_get_state(struct pwm_chip *chip,
				   struct pwm_device *pwm,
				   struct pwm_state *state)
{
	int ret;
	unsigned int reload_val;
	u16 pwm_high;
	u16 pwm_low;
	unsigned int channel = pwm->hwpwm;
	struct atcpit100_pwm *ap = to_atcpit100_pwm(chip);
	u64 rate = ap->clk_src[channel] ? CLK_APB : CLK_EXTERNAL;

	state->enabled =
		regmap_test_bits(ap->regmap, ATCPIT100_CHANNEL_ENABLE,
				 ATCPIT100_CHANNEL_ENABLE_PWM(channel));
	state->polarity = PWM_POLARITY_NORMAL;
	ret = regmap_read(ap->regmap, ATCPIT100_CHANNEL_RELOAD(channel),
			  &reload_val);
	if (ret)
		return ret;

	pwm_high = reload_val >> 16;
	pwm_low = reload_val & 0xFFFF;
	state->duty_cycle = mul_u64_u64_div_u64(pwm_high + 1, NSEC_PER_SEC,
						rate);
	state->period = mul_u64_u64_div_u64(pwm_low + pwm_high + 1,
					    NSEC_PER_SEC, rate);

	return 0;
}

static const struct pwm_ops atcpit_pwm_ops = {
	.apply = atcpit100_pwm_apply,
	.get_state = atcpit100_pwm_get_state,
};

static int atcpit100_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct atcpit100_pwm *ap;
	struct pwm_chip *chip;
	void __iomem *base;
	int ret;

	chip = devm_pwmchip_alloc(dev, ATCPIT100_CHANNEL_MAX, sizeof(*ap));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	ap = to_atcpit100_pwm(chip);

	/*
	 * Each channel can select two different clock sources by toggling the
	 * third bit in its control register. 0 means using an external clock,
	 * and 1 means using APB clock from APB bus. Select the clock source for
	 * each channel by DTS.
	 */
	ret = of_atcpit100_pwm_set_clk_src(ap, np);
	if (ret)
		return ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ap->regmap = devm_regmap_init_mmio(dev, base,
					   &atcpit100_pwm_regmap_config);
	if (IS_ERR(ap->regmap))
		return dev_err_probe(dev, PTR_ERR(ap->regmap),
				     "failed to init register map\n");

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
MODULE_AUTHOR("Andes Technology Corporation <ben717@andestech.com>");
MODULE_DESCRIPTION("Andes ATCPIT100 PWM driver");
MODULE_LICENSE("GPL");
