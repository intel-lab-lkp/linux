// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Nuvoton MA35D1 PWM controller
 *
 * Copyright (C) 2024 Nuvoton Corporation
 *               Chi-Wen Weng <cwweng@nuvoton.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/math64.h>

/* The following are registers for PWM controller */
#define REG_PWM_CTL0            (0x00)
#define REG_PWM_CNTEN           (0x20)
#define REG_PWM_PERIOD0         (0x30)
#define REG_PWM_CMPDAT0         (0x50)
#define REG_PWM_WGCTL0          (0xB0)
#define REG_PWM_POLCTL          (0xD4)
#define REG_PWM_POEN            (0xD8)

#define PWM_TOTAL_CHANNELS      6
#define PWM_CH_REG_SIZE         4

struct nuvoton_pwm {
	void __iomem *base;
	u64 clkrate;
};

static inline struct nuvoton_pwm *to_nuvoton_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int nuvoton_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			     const struct pwm_state *state)
{
	struct nuvoton_pwm *nvtpwm;
	unsigned int ch = pwm->hwpwm;

	nvtpwm = to_nuvoton_pwm(chip);
	if (state->enabled) {
		u64 duty_cycles, period_cycles;

		/* Calculate the duty and period cycles */
		duty_cycles = mul_u64_u64_div_u64(nvtpwm->clkrate,
						  state->duty_cycle, NSEC_PER_SEC);
		if (duty_cycles > 0xFFFF)
			duty_cycles = 0xFFFF;

		period_cycles = mul_u64_u64_div_u64(nvtpwm->clkrate,
						    state->period, NSEC_PER_SEC);
		if (period_cycles > 0xFFFF)
			period_cycles = 0xFFFF;

		/* Write the duty and period cycles to registers */
		writel(duty_cycles, nvtpwm->base + REG_PWM_CMPDAT0 + (ch * PWM_CH_REG_SIZE));
		writel(period_cycles, nvtpwm->base + REG_PWM_PERIOD0 + (ch * PWM_CH_REG_SIZE));
		/* Enable counter */
		writel(readl(nvtpwm->base + REG_PWM_CNTEN) | BIT(ch),
		       nvtpwm->base + REG_PWM_CNTEN);
		/* Enable output */
		writel(readl(nvtpwm->base + REG_PWM_POEN) | BIT(ch),
		       nvtpwm->base + REG_PWM_POEN);
	} else {
		/* Disable counter */
		writel(readl(nvtpwm->base + REG_PWM_CNTEN) & ~BIT(ch),
		       nvtpwm->base + REG_PWM_CNTEN);
		/* Disable output */
		writel(readl(nvtpwm->base + REG_PWM_POEN) & ~BIT(ch),
		       nvtpwm->base + REG_PWM_POEN);
	}

	/* Set polarity state to register */
	if (state->polarity == PWM_POLARITY_NORMAL)
		writel(readl(nvtpwm->base + REG_PWM_POLCTL) & ~BIT(ch),
		       nvtpwm->base + REG_PWM_POLCTL);
	else
		writel(readl(nvtpwm->base + REG_PWM_POLCTL) | BIT(ch),
		       nvtpwm->base + REG_PWM_POLCTL);

	return 0;
}

static int nuvoton_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				 struct pwm_state *state)
{
	struct nuvoton_pwm *nvtpwm;
	unsigned int duty_cycles, period_cycles, cnten, outen, polarity;
	unsigned int ch = pwm->hwpwm;

	nvtpwm = to_nuvoton_pwm(chip);

	cnten = readl(nvtpwm->base + REG_PWM_CNTEN);
	outen = readl(nvtpwm->base + REG_PWM_POEN);
	duty_cycles = readl(nvtpwm->base + REG_PWM_CMPDAT0 + (ch * PWM_CH_REG_SIZE));
	period_cycles = readl(nvtpwm->base + REG_PWM_PERIOD0 + (ch * PWM_CH_REG_SIZE));
	polarity = readl(nvtpwm->base + REG_PWM_POLCTL) & BIT(ch);

	state->enabled = (cnten & BIT(ch)) && (outen & BIT(ch));
	state->polarity = polarity ? PWM_POLARITY_INVERSED : PWM_POLARITY_NORMAL;
	state->duty_cycle = DIV64_U64_ROUND_UP((u64)duty_cycles * NSEC_PER_SEC, nvtpwm->clkrate);
	state->period = DIV64_U64_ROUND_UP((u64)period_cycles * NSEC_PER_SEC, nvtpwm->clkrate);

	return 0;
}

static const struct pwm_ops nuvoton_pwm_ops = {
	.apply = nuvoton_pwm_apply,
	.get_state = nuvoton_pwm_get_state,
};

static int nuvoton_pwm_probe(struct platform_device *pdev)
{
	struct pwm_chip *chip;
	struct nuvoton_pwm *nvtpwm;
	struct clk *clk;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, PWM_TOTAL_CHANNELS, sizeof(*nvtpwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	nvtpwm = to_nuvoton_pwm(chip);

	nvtpwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(nvtpwm->base))
		return PTR_ERR(nvtpwm->base);

	clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "unable to get the clock");

	nvtpwm->clkrate = clk_get_rate(clk);
	if (nvtpwm->clkrate > NSEC_PER_SEC)
		return dev_err_probe(&pdev->dev, -EINVAL, "pwm clock out of range");

	chip->ops = &nuvoton_pwm_ops;
	chip->atomic = true;

	ret = devm_pwmchip_add(&pdev->dev, chip);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "unable to add pwm chip");

	return 0;
}

static const struct of_device_id nuvoton_pwm_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-pwm" },
	{}
};
MODULE_DEVICE_TABLE(of, nuvoton_pwm_of_match);

static struct platform_driver nuvoton_pwm_driver = {
	.probe = nuvoton_pwm_probe,
	.driver = {
		.name = "nuvoton-pwm",
		.of_match_table = nuvoton_pwm_of_match,
	},
};
module_platform_driver(nuvoton_pwm_driver);

MODULE_AUTHOR("Chi-Wen Weng <cwweng@nuvoton.com>");
MODULE_DESCRIPTION("Nuvoton MA35D1 PWM driver");
MODULE_LICENSE("GPL");
