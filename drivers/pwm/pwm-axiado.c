// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2021-2026 Axiado Corporation.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

/* Register offsets */
#define AX_PWM_CNTRL_REG     0x0000
#define AX_PWM_PERIOD_REG    0x0004
#define AX_PWM_HIGH_REG      0x0008

/* PWM channels */
#define AX_PWM_NUM 1

/* Period and duty cycle limits */
#define AX_PWM_PERIOD_MIN       2
#define AX_PWM_PERIOD_MAX       0xfffffffeU
#define AX_PWM_DUTY_MIN         1
#define AX_PWM_DUTY_MAX         0xfffffffdU

/* Control register bits */
#define AX_PWM_CTRL_ENABLE BIT(0)
#define AX_PWM_CTRL_DISABLE 0x0

struct axiado_pwm_chip {
	struct clk *clk;
	void __iomem *base;
};

static int axiado_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			     u64 duty_ns, u64 period_ns)
{
	struct axiado_pwm_chip *axpwm = pwmchip_get_drvdata(chip);
	unsigned long rate;
	u64 period_cycles, duty_cycles;

	/*
	 * The hardware does not support a zero period, 0% duty cycle, or
	 * 100% duty cycle. The caller should handle 0% duty cycle by
	 * disabling the PWM.
	 */
	if (!period_ns || !duty_ns || duty_ns >= period_ns)
		return -EINVAL;

	rate = clk_get_rate(axpwm->clk);
	if (!rate)
		return -EINVAL;

	period_cycles = mul_u64_u64_div_u64(period_ns, rate, NSEC_PER_SEC);
	if (period_cycles < AX_PWM_PERIOD_MIN ||
	    period_cycles > AX_PWM_PERIOD_MAX)
		return -EINVAL;

	duty_cycles = mul_u64_u64_div_u64(duty_ns, rate, NSEC_PER_SEC);
	if (duty_cycles < AX_PWM_DUTY_MIN ||
	    duty_cycles > AX_PWM_DUTY_MAX)
		return -EINVAL;

	if (duty_cycles >= period_cycles)
		return -EINVAL;

	writel((u32)period_cycles, axpwm->base + AX_PWM_PERIOD_REG);
	writel((u32)duty_cycles, axpwm->base + AX_PWM_HIGH_REG);

	return 0;
}

static int axiado_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	struct axiado_pwm_chip *axpwm = pwmchip_get_drvdata(chip);
	int ret;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled || !state->duty_cycle) {
		if (pwm->state.enabled)
			writel(AX_PWM_CTRL_DISABLE, axpwm->base + AX_PWM_CNTRL_REG);

		return 0;
	}

	ret = axiado_pwm_config(chip, pwm, state->duty_cycle, state->period);
	if (ret)
		return ret;

	if (!pwm->state.enabled)
		writel(AX_PWM_CTRL_ENABLE, axpwm->base + AX_PWM_CNTRL_REG);

	return 0;
}

static int axiado_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct axiado_pwm_chip *axpwm = pwmchip_get_drvdata(chip);
	unsigned long rate;
	u32 period_cycles;
	u32 duty_cycles;
	u32 ctrl;

	rate = clk_get_rate(axpwm->clk);
	if (!rate)
		return -EINVAL;

	ctrl = readl(axpwm->base + AX_PWM_CNTRL_REG);
	period_cycles = readl(axpwm->base + AX_PWM_PERIOD_REG);
	duty_cycles = readl(axpwm->base + AX_PWM_HIGH_REG);

	state->enabled = !!(ctrl & AX_PWM_CTRL_ENABLE);
	state->period = mul_u64_u64_div_u64(period_cycles, NSEC_PER_SEC, rate);
	state->duty_cycle = mul_u64_u64_div_u64(duty_cycles, NSEC_PER_SEC, rate);
	state->polarity = PWM_POLARITY_NORMAL;

	return 0;
}

static const struct pwm_ops axiado_pwm_ops = {
	.get_state = axiado_pwm_get_state,
	.apply = axiado_pwm_apply,
};

static void axiado_pwm_disable(void *data)
{
	struct axiado_pwm_chip *axpwm = data;

	writel(AX_PWM_CTRL_DISABLE, axpwm->base + AX_PWM_CNTRL_REG);
}

static int axiado_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct axiado_pwm_chip *axpwm;
	struct pwm_chip *chip;
	int ret;

	chip = devm_pwmchip_alloc(dev, AX_PWM_NUM, sizeof(*axpwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	axpwm = pwmchip_get_drvdata(chip);

	axpwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(axpwm->base))
		return dev_err_probe(dev, PTR_ERR(axpwm->base),
				     "failed to map registers\n");

	ret = devm_add_action_or_reset(dev, axiado_pwm_disable, axpwm);
	if (ret)
		return ret;


	axpwm->clk = devm_clk_get_enabled(dev, "pwm");
	if (IS_ERR(axpwm->clk))
		return dev_err_probe(dev, PTR_ERR(axpwm->clk),
				     "failed to get/enable clock\n");

	chip->ops = &axiado_pwm_ops;

	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add PWM chip\n");

	return 0;
}

static const struct of_device_id axiado_pwm_match[] = {
	{ .compatible = "axiado,ax3000-pwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, axiado_pwm_match);

static struct platform_driver axiado_pwm_driver = {
	.driver = {
		.name =  "axiado-pwm",
		.of_match_table = axiado_pwm_match,
	},
	.probe = axiado_pwm_probe,
};

module_platform_driver(axiado_pwm_driver);

MODULE_AUTHOR("Axiado Corporation");
MODULE_DESCRIPTION("Axiado PWM driver");
MODULE_LICENSE("GPL");
