// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Nuvoton MA35D1 PWM controller
 *
 * Copyright (C) 2026 Nuvoton Corporation
 *               Chi-Wen Weng <cwweng@nuvoton.com>
 *
 * Reference Manual:
 * https://www.nuvoton.com/resource-download.jsp?tp_GUID=DA05-MA35D16
 *
 * Limitations:
 * - The hardware supports 6 PWM channels.
 * - The hardware supports up, down and up-down counter types. This driver
 *   supports up-counting mode only.
 * - The hardware supports auto-reload and one-shot counter modes. This driver
 *   supports auto-reload mode only.
 * - The hardware supports independent and complementary output modes. This
 *   driver supports independent mode only. If a channel pair is already active
 *   in complementary mode, applying a new state to one channel fails with
 *   -EBUSY to avoid disturbing the paired output.
 * - The hardware supports period, immediate, window and center loading modes.
 *   This driver supports period loading mode only.
 * - The hardware supports programmable waveform actions at zero, period and
 *   compare points. This driver uses zero point high and compare-up point low
 *   actions for normal PWM output.
 * - In up-counting mode, the counter counts from 0 to PERIOD inclusive. With
 *   zero point high and compare-up point low actions, CMPDAT = 0 produces 0%
 *   duty and CMPDAT > PERIOD produces 100% duty.
 * - The driver limits the period to 0xffff counter cycles so that CMPDAT can
 *   be set greater than PERIOD to generate a 100% duty cycle.
 * - Period and duty cycle changes are buffered by hardware and take effect at
 *   the end of the current period.
 * - The clock prescaler is shared by each channel pair. The driver preserves
 *   the current prescaler and accounts for it in period and duty calculations.
 * - Polarity and waveform-control changes are applied directly and may cause
 *   a transient output change if the PWM output is running.
 * - When disabled, the output pin is put in tri-state by clearing POENn.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define MA35D1_REG_PWM_CTL0			0x00
#define MA35D1_REG_PWM_CTL1			0x04
#define MA35D1_REG_PWM_CLKPSC(ch)		(0x14 + 4 * ((ch) / 2))
#define MA35D1_REG_PWM_CNTEN			0x20
#define MA35D1_REG_PWM_PERIOD(ch)		(0x30 + 4 * (ch))
#define MA35D1_REG_PWM_CMPDAT(ch)		(0x50 + 4 * (ch))
#define MA35D1_REG_PWM_WGCTL0			0xb0
#define MA35D1_REG_PWM_WGCTL1			0xb4
#define MA35D1_REG_PWM_POLCTL			0xd4
#define MA35D1_REG_PWM_POEN			0xd8

#define MA35D1_PWM_CTL0_CTRLD(ch)		BIT(ch)
#define MA35D1_PWM_CTL0_WINLDEN(ch)		BIT(8 + (ch))
#define MA35D1_PWM_CTL0_IMMLDEN(ch)		BIT(16 + (ch))

#define MA35D1_PWM_CTL1_CNTTYPE_MASK(ch)	(0x3 << (2 * (ch)))
#define MA35D1_PWM_CTL1_CNTMODE_MASK(ch)	BIT(16 + (ch))
#define MA35D1_PWM_CTL1_OUTMODE_MASK(ch)	BIT(24 + ((ch) / 2))

#define MA35D1_PWM_WGCTL_ACTION_MASK		0x3
#define MA35D1_PWM_WGCTL_ACTION(ch)		\
	(MA35D1_PWM_WGCTL_ACTION_MASK << (2 * (ch)))
#define MA35D1_PWM_WGCTL_ACTION_VAL(ch, action)	((action) << (2 * (ch)))
#define MA35D1_PWM_WGCTL_ACTION_LOW		1
#define MA35D1_PWM_WGCTL_ACTION_HIGH		2

#define MA35D1_PWM_CLKPSC_MASK			GENMASK(11, 0)
#define MA35D1_PWM_DATA_MASK			GENMASK(15, 0)
#define MA35D1_PWM_MAX_CYCLES			0xffff

#define MA35D1_PWM_CNTEN_EN(ch)			BIT(ch)
#define MA35D1_PWM_POEN_EN(ch)			BIT(ch)
#define MA35D1_PWM_POLCTL_INV(ch)		BIT(ch)

#define MA35D1_PWM_NUM_CHANNELS			6

struct nuvoton_pwm {
	void __iomem *base;
	unsigned long clkrate;
};

static inline struct nuvoton_pwm *nuvoton_pwm_from_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static inline u32 nuvoton_pwm_readl(struct nuvoton_pwm *nvtpwm,
				    unsigned int offset)
{
	return readl(nvtpwm->base + offset);
}

static inline void nuvoton_pwm_writel(struct nuvoton_pwm *nvtpwm,
				      unsigned int offset, u32 value)
{
	writel(value, nvtpwm->base + offset);
}

static inline void nuvoton_pwm_rmw(struct nuvoton_pwm *nvtpwm,
				   unsigned int offset, u32 mask, u32 value)
{
	u32 reg;

	reg = nuvoton_pwm_readl(nvtpwm, offset);
	reg &= ~mask;
	reg |= value & mask;
	nuvoton_pwm_writel(nvtpwm, offset, reg);
}

static bool nuvoton_pwm_channel_active(struct nuvoton_pwm *nvtpwm,
				       unsigned int ch)
{
	u32 cnten, poen;
	u32 mask = BIT(ch);

	cnten = nuvoton_pwm_readl(nvtpwm, MA35D1_REG_PWM_CNTEN);
	poen = nuvoton_pwm_readl(nvtpwm, MA35D1_REG_PWM_POEN);

	return (cnten | poen) & mask;
}

static u32 nuvoton_pwm_get_prescale(struct nuvoton_pwm *nvtpwm,
				    unsigned int ch)
{
	return nuvoton_pwm_readl(nvtpwm, MA35D1_REG_PWM_CLKPSC(ch)) &
	       MA35D1_PWM_CLKPSC_MASK;
}

static int nuvoton_pwm_config_channel(struct nuvoton_pwm *nvtpwm,
				      unsigned int ch)
{
	u32 ctl0_mask;
	u32 ctl1, ctl1_mask, outmode_mask;
	u32 wgctl0_mask, wgctl0_val;
	u32 wgctl1_mask, wgctl1_val;

	outmode_mask = MA35D1_PWM_CTL1_OUTMODE_MASK(ch);
	ctl1 = nuvoton_pwm_readl(nvtpwm, MA35D1_REG_PWM_CTL1);

	if ((ctl1 & outmode_mask) &&
	    nuvoton_pwm_channel_active(nvtpwm, ch ^ 1))
		return -EBUSY;

	/*
	 * IMMLDENn = 0, WINLDENn = 0 and CTRLDn = 0 select period loading
	 * mode for up-counting operation.
	 */
	ctl0_mask = MA35D1_PWM_CTL0_IMMLDEN(ch);
	ctl0_mask |= MA35D1_PWM_CTL0_WINLDEN(ch);
	ctl0_mask |= MA35D1_PWM_CTL0_CTRLD(ch);

	/*
	 * CNTTYPEn = 00: up counter type
	 * CNTMODEn = 0: auto-reload mode
	 * OUTMODEn = 0: independent mode
	 */
	ctl1_mask = MA35D1_PWM_CTL1_CNTTYPE_MASK(ch);
	ctl1_mask |= MA35D1_PWM_CTL1_CNTMODE_MASK(ch);
	ctl1_mask |= outmode_mask;

	/*
	 * ZPCTLn = 10: output high at zero point
	 * PRDPCTLn = 00: do nothing at period point
	 */
	wgctl0_mask = MA35D1_PWM_WGCTL_ACTION(ch);
	wgctl0_mask |= MA35D1_PWM_WGCTL_ACTION(ch) << 16;
	wgctl0_val = MA35D1_PWM_WGCTL_ACTION_VAL(ch,
						 MA35D1_PWM_WGCTL_ACTION_HIGH);

	/*
	 * CMPUCTLn = 01: output low at compare up point
	 * CMPDCTLn = 00: do nothing at compare down point
	 */
	wgctl1_mask = MA35D1_PWM_WGCTL_ACTION(ch);
	wgctl1_mask |= MA35D1_PWM_WGCTL_ACTION(ch) << 16;
	wgctl1_val = MA35D1_PWM_WGCTL_ACTION_VAL(ch,
						 MA35D1_PWM_WGCTL_ACTION_LOW);

	nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_CTL0, ctl0_mask, 0);
	nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_CTL1, ctl1_mask, 0);
	nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_WGCTL0,
			wgctl0_mask, wgctl0_val);
	nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_WGCTL1,
			wgctl1_mask, wgctl1_val);

	return 0;
}

static int nuvoton_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			     const struct pwm_state *state)
{
	struct nuvoton_pwm *nvtpwm = nuvoton_pwm_from_chip(chip);
	unsigned int ch = pwm->hwpwm;
	u64 duty_cycles, period_cycles;
	u64 divisor;
	u32 cmpdat, period, prescale;
	int ret;

	if (!state->enabled) {
		nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_POEN,
				MA35D1_PWM_POEN_EN(ch), 0);
		nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_CNTEN,
				MA35D1_PWM_CNTEN_EN(ch), 0);

		return 0;
	}

	prescale = nuvoton_pwm_get_prescale(nvtpwm, ch);
	divisor = (u64)NSEC_PER_SEC * (prescale + 1);

	period_cycles = mul_u64_u64_div_u64(nvtpwm->clkrate,
					    state->period, divisor);
	if (!period_cycles)
		return -EINVAL;

	if (period_cycles > MA35D1_PWM_MAX_CYCLES)
		period_cycles = MA35D1_PWM_MAX_CYCLES;

	duty_cycles = mul_u64_u64_div_u64(nvtpwm->clkrate,
					  state->duty_cycle, divisor);
	if (duty_cycles > period_cycles)
		duty_cycles = period_cycles;

	ret = nuvoton_pwm_config_channel(nvtpwm, ch);
	if (ret)
		return ret;

	if (state->polarity == PWM_POLARITY_NORMAL)
		nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_POLCTL,
				MA35D1_PWM_POLCTL_INV(ch), 0);
	else
		nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_POLCTL,
				MA35D1_PWM_POLCTL_INV(ch),
				MA35D1_PWM_POLCTL_INV(ch));

	/*
	 * In up-counting mode the counter counts from 0 to PERIOD inclusive.
	 * With zero point high and compare-up point low actions:
	 * - CMPDAT = 0 produces 0% duty.
	 * - CMPDAT > PERIOD produces 100% duty.
	 *
	 * period_cycles is limited to 0xffff, so PERIOD is at most 0xfffe
	 * and a 100% duty cycle can be represented by CMPDAT = 0xffff.
	 */
	period = period_cycles - 1;
	cmpdat = duty_cycles;

	nuvoton_pwm_writel(nvtpwm, MA35D1_REG_PWM_PERIOD(ch), period);
	nuvoton_pwm_writel(nvtpwm, MA35D1_REG_PWM_CMPDAT(ch), cmpdat);

	nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_POEN,
			MA35D1_PWM_POEN_EN(ch), MA35D1_PWM_POEN_EN(ch));
	nuvoton_pwm_rmw(nvtpwm, MA35D1_REG_PWM_CNTEN,
			MA35D1_PWM_CNTEN_EN(ch), MA35D1_PWM_CNTEN_EN(ch));

	return 0;
}

static int nuvoton_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				 struct pwm_state *state)
{
	struct nuvoton_pwm *nvtpwm = nuvoton_pwm_from_chip(chip);
	unsigned int ch = pwm->hwpwm;
	u32 cmpdat, cnten, period, poen, polctl, prescale;
	u64 duty_cycles, period_cycles;
	u64 prescaled_cycles;

	cnten = nuvoton_pwm_readl(nvtpwm, MA35D1_REG_PWM_CNTEN);
	poen = nuvoton_pwm_readl(nvtpwm, MA35D1_REG_PWM_POEN);
	polctl = nuvoton_pwm_readl(nvtpwm, MA35D1_REG_PWM_POLCTL);
	prescale = nuvoton_pwm_get_prescale(nvtpwm, ch);
	period = nuvoton_pwm_readl(nvtpwm, MA35D1_REG_PWM_PERIOD(ch)) &
		 MA35D1_PWM_DATA_MASK;
	cmpdat = nuvoton_pwm_readl(nvtpwm, MA35D1_REG_PWM_CMPDAT(ch)) &
		 MA35D1_PWM_DATA_MASK;

	period_cycles = period + 1;
	if (cmpdat > period)
		duty_cycles = period_cycles;
	else
		duty_cycles = cmpdat;

	state->enabled = (cnten & MA35D1_PWM_CNTEN_EN(ch)) &&
			 (poen & MA35D1_PWM_POEN_EN(ch));
	state->polarity = (polctl & MA35D1_PWM_POLCTL_INV(ch)) ?
			  PWM_POLARITY_INVERSED : PWM_POLARITY_NORMAL;

	prescaled_cycles = period_cycles * (prescale + 1);
	state->period = DIV64_U64_ROUND_UP(prescaled_cycles * NSEC_PER_SEC,
					   nvtpwm->clkrate);

	prescaled_cycles = duty_cycles * (prescale + 1);
	state->duty_cycle =
		DIV64_U64_ROUND_UP(prescaled_cycles * NSEC_PER_SEC,
				   nvtpwm->clkrate);

	return 0;
}

static const struct pwm_ops nuvoton_pwm_ops = {
	.apply = nuvoton_pwm_apply,
	.get_state = nuvoton_pwm_get_state,
};

static int nuvoton_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct nuvoton_pwm *nvtpwm;
	struct clk *clk;
	int ret;

	chip = devm_pwmchip_alloc(dev, MA35D1_PWM_NUM_CHANNELS,
				  sizeof(*nvtpwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	nvtpwm = nuvoton_pwm_from_chip(chip);

	nvtpwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(nvtpwm->base))
		return PTR_ERR(nvtpwm->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Unable to get the clock\n");

	ret = devm_clk_rate_exclusive_get(dev, clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Unable to get exclusive clock rate\n");

	nvtpwm->clkrate = clk_get_rate(clk);
	if (!nvtpwm->clkrate)
		return dev_err_probe(dev, -EINVAL,
				     "PWM clock rate is zero\n");

	if (nvtpwm->clkrate > NSEC_PER_SEC)
		return dev_err_probe(dev, -EINVAL,
				     "PWM clock out of range (%lu)\n",
				     nvtpwm->clkrate);

	chip->ops = &nuvoton_pwm_ops;
	chip->atomic = true;

	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "Unable to add PWM chip\n");

	return 0;
}

static const struct of_device_id nuvoton_pwm_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-pwm" },
	{ }
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
