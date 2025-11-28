// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EHRPWM PWM driver
 *
 * Copyright (C) 2012 Texas Instruments, Inc. - https://www.ti.com/
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/bitfield.h>

/* EHRPWM registers and bits definitions */

/* Time base module registers */
#define TIEHRPWM_TBCTL				0x00
#define TIEHRPWM_TBPRD				0x0A

#define TIEHRPWM_TBCTL_PRDLD			BIT(3)
#define TIEHRPWM_TBCTL_PRDLD_SHDW		FIELD_PREP(TIEHRPWM_TBCTL_PRDLD, 0)
#define TIEHRPWM_TBCTL_PRDLD_IMDT		FIELD_PREP(TIEHRPWM_TBCTL_PRDLD, 1)

#define TIEHRPWM_TBCTL_CLKDIV_MASK		GENMASK(12, 10)
#define TIEHRPWM_TBCTL_HSPCLKDIV_MASK		GENMASK(9, 7)
#define TIEHRPWM_TBCTL_PRESCALE_MASK		(TIEHRPWM_TBCTL_CLKDIV_MASK | \
						TIEHRPWM_TBCTL_HSPCLKDIV_MASK)

#define TIEHRPWM_TBCTL_CTRMODE_MASK		GENMASK(1, 0)
#define TIEHRPWM_TBCTL_CTRMODE_UP		FIELD_PREP(TIEHRPWM_TBCTL_CTRMODE_MASK, 0)
#define TIEHRPWM_TBCTL_CTRMODE_DOWN		FIELD_PREP(TIEHRPWM_TBCTL_CTRMODE_MASK, 1)
#define TIEHRPWM_TBCTL_CTRMODE_UPDOWN		FIELD_PREP(TIEHRPWM_TBCTL_CTRMODE_MASK, 2)
#define TIEHRPWM_TBCTL_CTRMODE_FREEZE		FIELD_PREP(TIEHRPWM_TBCTL_CTRMODE_MASK, 3)

#define TIEHRPWM_CLKDIV_MAX			7
#define TIEHRPWM_HSPCLKDIV_MAX			7

#define TIEHRPWM_TBPRD_TBPRD			GENMASK(15, 0)
#define TIEHRPWM_PERIOD_MAX			(FIELD_MAX(TIEHRPWM_TBPRD_TBPRD) + 1)

/* compare module registers */
#define TIEHRPWM_CMPA				0x12
#define TIEHRPWM_CMPB				0x14

/* Action qualifier module registers */
#define TIEHRPWM_AQCTLA				0x16
#define TIEHRPWM_AQCTLB				0x18
#define TIEHRPWM_AQSFRC				0x1A
#define TIEHRPWM_AQCSFRC			0x1C

#define TIEHRPWM_AQCTL_CBU_MASK			GENMASK(9, 8)
#define TIEHRPWM_AQCTL_CBU_FRCLOW		FIELD_PREP(TIEHRPWM_AQCTL_CBU_MASK, 1)
#define TIEHRPWM_AQCTL_CBU_FRCHIGH		FIELD_PREP(TIEHRPWM_AQCTL_CBU_MASK, 2)
#define TIEHRPWM_AQCTL_CBU_FRCTOGGLE		FIELD_PREP(TIEHRPWM_AQCTL_CBU_MASK, 3)

#define TIEHRPWM_AQCTL_CAU_MASK			GENMASK(5, 4)
#define TIEHRPWM_AQCTL_CAU_FRCLOW		FIELD_PREP(TIEHRPWM_AQCTL_CAU_MASK, 1)
#define TIEHRPWM_AQCTL_CAU_FRCHIGH		FIELD_PREP(TIEHRPWM_AQCTL_CAU_MASK, 2)
#define TIEHRPWM_AQCTL_CAU_FRCTOGGLE		FIELD_PREP(TIEHRPWM_AQCTL_CAU_MASK, 3)

#define TIEHRPWM_AQCTL_PRD_MASK			GENMASK(3, 2)
#define TIEHRPWM_AQCTL_PRD_FRCLOW		FIELD_PREP(TIEHRPWM_AQCTL_PRD_MASK, 1)
#define TIEHRPWM_AQCTL_PRD_FRCHIGH		FIELD_PREP(TIEHRPWM_AQCTL_PRD_MASK, 2)
#define TIEHRPWM_AQCTL_PRD_FRCTOGGLE		FIELD_PREP(TIEHRPWM_AQCTL_PRD_MASK, 3)

#define TIEHRPWM_AQCTL_ZRO_MASK			GENMASK(1, 0)
#define TIEHRPWM_AQCTL_ZRO_FRCLOW		FIELD_PREP(TIEHRPWM_AQCTL_ZRO_MASK, 1)
#define TIEHRPWM_AQCTL_ZRO_FRCHIGH		FIELD_PREP(TIEHRPWM_AQCTL_ZRO_MASK, 2)
#define TIEHRPWM_AQCTL_ZRO_FRCTOGGLE		FIELD_PREP(TIEHRPWM_AQCTL_ZRO_MASK, 3)

#define TIEHRPWM_AQCTL_CHANA_POLNORMAL		(TIEHRPWM_AQCTL_CAU_FRCLOW | \
						TIEHRPWM_AQCTL_ZRO_FRCHIGH)
#define TIEHRPWM_AQCTL_CHANA_POLINVERSED	(TIEHRPWM_AQCTL_CAU_FRCHIGH | \
						TIEHRPWM_AQCTL_ZRO_FRCLOW)
#define TIEHRPWM_AQCTL_CHANB_POLNORMAL		(TIEHRPWM_AQCTL_CBU_FRCLOW | \
						TIEHRPWM_AQCTL_ZRO_FRCHIGH)
#define TIEHRPWM_AQCTL_CHANB_POLINVERSED	(TIEHRPWM_AQCTL_CBU_FRCHIGH | \
						TIEHRPWM_AQCTL_ZRO_FRCLOW)

#define TIEHRPWM_AQSFRC_RLDCSF_MASK		GENMASK(7, 6)
#define TIEHRPWM_AQSFRC_RLDCSF_ZRO		FIELD_PREP(TIEHRPWM_AQSFRC_RLDCSF_MASK, 0)
#define TIEHRPWM_AQSFRC_RLDCSF_PRD		FIELD_PREP(TIEHRPWM_AQSFRC_RLDCSF_MASK, 1)
#define TIEHRPWM_AQSFRC_RLDCSF_ZROPRD		FIELD_PREP(TIEHRPWM_AQSFRC_RLDCSF_MASK, 2)
#define TIEHRPWM_AQSFRC_RLDCSF_IMDT		FIELD_PREP(TIEHRPWM_AQSFRC_RLDCSF_MASK, 3)

#define TIEHRPWM_AQCSFRC_CSFB_MASK		GENMASK(3, 2)
#define TIEHRPWM_AQCSFRC_CSFB_FRCDIS		FIELD_PREP(TIEHRPWM_AQCSFRC_CSFB_MASK, 0)
#define TIEHRPWM_AQCSFRC_CSFB_FRCLOW		FIELD_PREP(TIEHRPWM_AQCSFRC_CSFB_MASK, 1)
#define TIEHRPWM_AQCSFRC_CSFB_FRCHIGH		FIELD_PREP(TIEHRPWM_AQCSFRC_CSFB_MASK, 2)
#define TIEHRPWM_AQCSFRC_CSFB_DISSWFRC		FIELD_PREP(TIEHRPWM_AQCSFRC_CSFB_MASK, 3)

#define TIEHRPWM_AQCSFRC_CSFA_MASK		GENMASK(1, 0)
#define TIEHRPWM_AQCSFRC_CSFA_FRCDIS		FIELD_PREP(TIEHRPWM_AQCSFRC_CSFA_MASK, 0)
#define TIEHRPWM_AQCSFRC_CSFA_FRCLOW		FIELD_PREP(TIEHRPWM_AQCSFRC_CSFA_MASK, 1)
#define TIEHRPWM_AQCSFRC_CSFA_FRCHIGH		FIELD_PREP(TIEHRPWM_AQCSFRC_CSFA_MASK, 2)
#define TIEHRPWM_AQCSFRC_CSFA_DISSWFRC		FIELD_PREP(TIEHRPWM_AQCSFRC_CSFA_MASK, 3)

#define TIEHRPWM_NUM_PWM_CHANNEL		2	/* EHRPWM channels */

struct ehrpwm_context {
	u16 tbctl;
	u16 tbprd;
	u16 cmpa;
	u16 cmpb;
	u16 aqctla;
	u16 aqctlb;
	u16 aqsfrc;
	u16 aqcsfrc;
};

struct ehrpwm_pwm_chip {
	unsigned long clk_rate;
	void __iomem *mmio_base;
	unsigned long period_cycles[TIEHRPWM_NUM_PWM_CHANNEL];
	struct clk *tbclk;
	struct ehrpwm_context ctx;
};

static inline struct ehrpwm_pwm_chip *to_ehrpwm_pwm_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static inline u16 ehrpwm_read(void __iomem *base, unsigned int offset)
{
	return readw(base + offset);
}

static inline void ehrpwm_write(void __iomem *base, unsigned int offset,
				u16 value)
{
	writew(value, base + offset);
}

static void ehrpwm_modify(void __iomem *base, unsigned int offset, u16 mask,
			  u16 value)
{
	unsigned short val;

	val = readw(base + offset);
	val &= ~mask;
	val |= value & mask;
	writew(val, base + offset);
}

/**
 * set_prescale_div -	Set up the prescaler divider function
 * @rqst_prescaler:	prescaler value min
 * @prescale_div:	prescaler value set
 * @tb_clk_div:		Time Base Control prescaler bits
 */
static int set_prescale_div(unsigned long rqst_prescaler, u16 *prescale_div,
			    u16 *tb_clk_div)
{
	unsigned int clkdiv, hspclkdiv;

	for (clkdiv = 0; clkdiv <= TIEHRPWM_CLKDIV_MAX; clkdiv++) {
		for (hspclkdiv = 0; hspclkdiv <= TIEHRPWM_HSPCLKDIV_MAX; hspclkdiv++) {
			/*
			 * calculations for prescaler value :
			 * prescale_div = HSPCLKDIVIDER * CLKDIVIDER.
			 * HSPCLKDIVIDER =  2 ** hspclkdiv
			 * CLKDIVIDER = (1),		if clkdiv == 0 *OR*
			 *		(2 * clkdiv),	if clkdiv != 0
			 *
			 * Configure prescale_div value such that period
			 * register value is less than 65535.
			 */

			*prescale_div = (1 << clkdiv) *
					(hspclkdiv ? (hspclkdiv * 2) : 1);
			if (*prescale_div >= rqst_prescaler) {
				*tb_clk_div = FIELD_PREP(TIEHRPWM_TBCTL_CLKDIV_MASK, clkdiv) |
					FIELD_PREP(TIEHRPWM_TBCTL_HSPCLKDIV_MASK, hspclkdiv);
				return 0;
			}
		}
	}

	return 1;
}

/*
 * period_ns = 10^9 * (ps_divval * period_cycles) / PWM_CLK_RATE
 * duty_ns   = 10^9 * (ps_divval * duty_cycles) / PWM_CLK_RATE
 */
static int ehrpwm_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			     u64 duty_ns, u64 period_ns, enum pwm_polarity polarity)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);
	u32 period_cycles, duty_cycles;
	u16 ps_divval, tb_divval;
	unsigned int i, cmp_reg;
	unsigned long long c;
	u16 aqctl_val, aqctl_mask;
	unsigned int aqctl_reg;

	if (period_ns > NSEC_PER_SEC)
		return -ERANGE;

	c = pc->clk_rate;
	c = c * period_ns;
	do_div(c, NSEC_PER_SEC);
	period_cycles = (unsigned long)c;

	c = pc->clk_rate;
	c = c * duty_ns;
	do_div(c, NSEC_PER_SEC);
	duty_cycles = (unsigned long)c;

	/*
	 * Period values should be same for multiple PWM channels as IP uses
	 * same period register for multiple channels.
	 */
	for (i = 0; i < TIEHRPWM_NUM_PWM_CHANNEL; i++) {
		if (pc->period_cycles[i] &&
				(pc->period_cycles[i] != period_cycles)) {
			/*
			 * Allow channel to reconfigure period if no other
			 * channels being configured.
			 */
			if (i == pwm->hwpwm)
				continue;

			dev_err(pwmchip_parent(chip),
				"period value conflicts with channel %u\n",
				i);
			return -EINVAL;
		}
	}

	pc->period_cycles[pwm->hwpwm] = period_cycles;

	/* Configure clock prescaler to support Low frequency PWM wave */
	if (set_prescale_div(DIV_ROUND_UP(period_cycles, TIEHRPWM_PERIOD_MAX), &ps_divval,
			     &tb_divval)) {
		dev_err(pwmchip_parent(chip), "Unsupported values\n");
		return -EINVAL;
	}

	/* Update period & duty cycle with presacler division */
	period_cycles = period_cycles / ps_divval;
	duty_cycles = duty_cycles / ps_divval;

	if (period_cycles < 1)
		period_cycles = 1;

	pm_runtime_get_sync(pwmchip_parent(chip));

	/* Update clock prescaler values */
	ehrpwm_modify(pc->mmio_base, TIEHRPWM_TBCTL,  TIEHRPWM_TBCTL_PRESCALE_MASK, tb_divval);

	if (pwm->hwpwm == 1) {
		/* Channel 1 configured with compare B register */
		cmp_reg = TIEHRPWM_CMPB;

		aqctl_reg = TIEHRPWM_AQCTLB;
		aqctl_mask = TIEHRPWM_AQCTL_CBU_MASK;

		if (polarity == PWM_POLARITY_INVERSED)
			aqctl_val = TIEHRPWM_AQCTL_CHANB_POLINVERSED;
		else
			aqctl_val = TIEHRPWM_AQCTL_CHANB_POLNORMAL;

		/* if duty_cycle is big, don't toggle on CBU */
		if (duty_cycles > period_cycles)
			aqctl_val &= ~TIEHRPWM_AQCTL_CBU_MASK;

	} else {
		/* Channel 0 configured with compare A register */
		cmp_reg = TIEHRPWM_CMPA;

		aqctl_reg = TIEHRPWM_AQCTLA;
		aqctl_mask = TIEHRPWM_AQCTL_CAU_MASK;

		if (polarity == PWM_POLARITY_INVERSED)
			aqctl_val = TIEHRPWM_AQCTL_CHANA_POLINVERSED;
		else
			aqctl_val = TIEHRPWM_AQCTL_CHANA_POLNORMAL;

		/* if duty_cycle is big, don't toggle on CAU */
		if (duty_cycles > period_cycles)
			aqctl_val &= ~TIEHRPWM_AQCTL_CAU_MASK;
	}

	aqctl_mask |= TIEHRPWM_AQCTL_PRD_MASK | TIEHRPWM_AQCTL_ZRO_MASK;
	ehrpwm_modify(pc->mmio_base, aqctl_reg, aqctl_mask, aqctl_val);

	/* Configure shadow loading on Period register */
	ehrpwm_modify(pc->mmio_base, TIEHRPWM_TBCTL, TIEHRPWM_TBCTL_PRDLD,
		      TIEHRPWM_TBCTL_PRDLD_SHDW);

	ehrpwm_write(pc->mmio_base, TIEHRPWM_TBPRD, period_cycles - 1);

	/* Configure ehrpwm counter for up-count mode */
	ehrpwm_modify(pc->mmio_base, TIEHRPWM_TBCTL, TIEHRPWM_TBCTL_CTRMODE_MASK,
		      TIEHRPWM_TBCTL_CTRMODE_UP);

	if (!(duty_cycles > period_cycles))
		ehrpwm_write(pc->mmio_base, cmp_reg, duty_cycles);

	pm_runtime_put_sync(pwmchip_parent(chip));

	return 0;
}

static int ehrpwm_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);
	u16 aqcsfrc_val, aqcsfrc_mask;
	int ret;

	/* Leave clock enabled on enabling PWM */
	pm_runtime_get_sync(pwmchip_parent(chip));

	/* Disabling Action Qualifier on PWM output */
	if (pwm->hwpwm) {
		aqcsfrc_val = TIEHRPWM_AQCSFRC_CSFB_FRCDIS;
		aqcsfrc_mask = TIEHRPWM_AQCSFRC_CSFB_MASK;
	} else {
		aqcsfrc_val = TIEHRPWM_AQCSFRC_CSFA_FRCDIS;
		aqcsfrc_mask = TIEHRPWM_AQCSFRC_CSFA_MASK;
	}

	/* Changes to shadow mode */
	ehrpwm_modify(pc->mmio_base, TIEHRPWM_AQSFRC, TIEHRPWM_AQSFRC_RLDCSF_MASK,
		      TIEHRPWM_AQSFRC_RLDCSF_ZRO);

	ehrpwm_modify(pc->mmio_base, TIEHRPWM_AQCSFRC, aqcsfrc_mask, aqcsfrc_val);

	/* Enable TBCLK */
	ret = clk_enable(pc->tbclk);
	if (ret) {
		dev_err(pwmchip_parent(chip), "Failed to enable TBCLK for %s: %d\n",
			dev_name(pwmchip_parent(chip)), ret);
		return ret;
	}

	return 0;
}

static void ehrpwm_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);
	u16 aqcsfrc_val, aqcsfrc_mask;

	/* Action Qualifier puts PWM output low forcefully */
	if (pwm->hwpwm) {
		aqcsfrc_val = TIEHRPWM_AQCSFRC_CSFB_FRCLOW;
		aqcsfrc_mask = TIEHRPWM_AQCSFRC_CSFB_MASK;
	} else {
		aqcsfrc_val = TIEHRPWM_AQCSFRC_CSFA_FRCLOW;
		aqcsfrc_mask = TIEHRPWM_AQCSFRC_CSFA_MASK;
	}

	/* Update shadow register first before modifying active register */
	ehrpwm_modify(pc->mmio_base, TIEHRPWM_AQSFRC, TIEHRPWM_AQSFRC_RLDCSF_MASK,
		      TIEHRPWM_AQSFRC_RLDCSF_ZRO);
	ehrpwm_modify(pc->mmio_base, TIEHRPWM_AQCSFRC, aqcsfrc_mask, aqcsfrc_val);
	/*
	 * Changes to immediate action on Action Qualifier. This puts
	 * Action Qualifier control on PWM output from next TBCLK
	 */
	ehrpwm_modify(pc->mmio_base, TIEHRPWM_AQSFRC, TIEHRPWM_AQSFRC_RLDCSF_MASK,
		      TIEHRPWM_AQSFRC_RLDCSF_IMDT);

	ehrpwm_modify(pc->mmio_base, TIEHRPWM_AQCSFRC, aqcsfrc_mask, aqcsfrc_val);

	/* Disabling TBCLK on PWM disable */
	clk_disable(pc->tbclk);

	/* Disable clock on PWM disable */
	pm_runtime_put_sync(pwmchip_parent(chip));
}

static void ehrpwm_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	/* Don't let a pwm without consumer block requests to the other channel */
	pc->period_cycles[pwm->hwpwm] = 0;
}

static int ehrpwm_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	int err;
	bool enabled = pwm->state.enabled;

	if (state->polarity != pwm->state.polarity) {
		if (enabled) {
			ehrpwm_pwm_disable(chip, pwm);
			enabled = false;
		}
	}

	if (!state->enabled) {
		if (enabled)
			ehrpwm_pwm_disable(chip, pwm);
		return 0;
	}

	err = ehrpwm_pwm_config(chip, pwm, state->duty_cycle, state->period, state->polarity);
	if (err)
		return err;

	if (!enabled)
		err = ehrpwm_pwm_enable(chip, pwm);

	return err;
}

static const struct pwm_ops ehrpwm_pwm_ops = {
	.free = ehrpwm_pwm_free,
	.apply = ehrpwm_pwm_apply,
};

static const struct of_device_id ehrpwm_of_match[] = {
	{ .compatible = "ti,am3352-ehrpwm" },
	{ .compatible = "ti,am33xx-ehrpwm" },
	{},
};
MODULE_DEVICE_TABLE(of, ehrpwm_of_match);

static int ehrpwm_pwm_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct ehrpwm_pwm_chip *pc;
	struct pwm_chip *chip;
	struct clk *clk;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, TIEHRPWM_NUM_PWM_CHANNEL, sizeof(*pc));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	pc = to_ehrpwm_pwm_chip(chip);

	clk = devm_clk_get(&pdev->dev, "fck");
	if (IS_ERR(clk)) {
		if (of_device_is_compatible(np, "ti,am33xx-ecap")) {
			dev_warn(&pdev->dev, "Binding is obsolete.\n");
			clk = devm_clk_get(pdev->dev.parent, "fck");
		}
	}

	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "Failed to get fck\n");

	pc->clk_rate = clk_get_rate(clk);
	if (!pc->clk_rate) {
		dev_err(&pdev->dev, "failed to get clock rate\n");
		return -EINVAL;
	}

	chip->ops = &ehrpwm_pwm_ops;

	pc->mmio_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pc->mmio_base))
		return PTR_ERR(pc->mmio_base);

	/* Acquire tbclk for Time Base EHRPWM submodule */
	pc->tbclk = devm_clk_get(&pdev->dev, "tbclk");
	if (IS_ERR(pc->tbclk))
		return dev_err_probe(&pdev->dev, PTR_ERR(pc->tbclk), "Failed to get tbclk\n");

	ret = clk_prepare(pc->tbclk);
	if (ret < 0) {
		dev_err(&pdev->dev, "clk_prepare() failed: %d\n", ret);
		return ret;
	}

	ret = pwmchip_add(chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "pwmchip_add() failed: %d\n", ret);
		goto err_clk_unprepare;
	}

	platform_set_drvdata(pdev, chip);
	pm_runtime_enable(&pdev->dev);

	return 0;

err_clk_unprepare:
	clk_unprepare(pc->tbclk);

	return ret;
}

static void ehrpwm_pwm_remove(struct platform_device *pdev)
{
	struct pwm_chip *chip = platform_get_drvdata(pdev);
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	pwmchip_remove(chip);

	clk_unprepare(pc->tbclk);

	pm_runtime_disable(&pdev->dev);
}

static void ehrpwm_pwm_save_context(struct pwm_chip *chip)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	pm_runtime_get_sync(pwmchip_parent(chip));

	pc->ctx.tbctl = ehrpwm_read(pc->mmio_base, TIEHRPWM_TBCTL);
	pc->ctx.tbprd = ehrpwm_read(pc->mmio_base, TIEHRPWM_TBPRD);
	pc->ctx.cmpa = ehrpwm_read(pc->mmio_base, TIEHRPWM_CMPA);
	pc->ctx.cmpb = ehrpwm_read(pc->mmio_base, TIEHRPWM_CMPB);
	pc->ctx.aqctla = ehrpwm_read(pc->mmio_base, TIEHRPWM_AQCTLA);
	pc->ctx.aqctlb = ehrpwm_read(pc->mmio_base, TIEHRPWM_AQCTLB);
	pc->ctx.aqsfrc = ehrpwm_read(pc->mmio_base, TIEHRPWM_AQSFRC);
	pc->ctx.aqcsfrc = ehrpwm_read(pc->mmio_base, TIEHRPWM_AQCSFRC);

	pm_runtime_put_sync(pwmchip_parent(chip));
}

static void ehrpwm_pwm_restore_context(struct pwm_chip *chip)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	ehrpwm_write(pc->mmio_base, TIEHRPWM_TBPRD, pc->ctx.tbprd);
	ehrpwm_write(pc->mmio_base, TIEHRPWM_CMPA, pc->ctx.cmpa);
	ehrpwm_write(pc->mmio_base, TIEHRPWM_CMPB, pc->ctx.cmpb);
	ehrpwm_write(pc->mmio_base, TIEHRPWM_AQCTLA, pc->ctx.aqctla);
	ehrpwm_write(pc->mmio_base, TIEHRPWM_AQCTLB, pc->ctx.aqctlb);
	ehrpwm_write(pc->mmio_base, TIEHRPWM_AQSFRC, pc->ctx.aqsfrc);
	ehrpwm_write(pc->mmio_base, TIEHRPWM_AQCSFRC, pc->ctx.aqcsfrc);
	ehrpwm_write(pc->mmio_base, TIEHRPWM_TBCTL, pc->ctx.tbctl);
}

static int ehrpwm_pwm_suspend(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	unsigned int i;

	ehrpwm_pwm_save_context(chip);

	for (i = 0; i < chip->npwm; i++) {
		struct pwm_device *pwm = &chip->pwms[i];

		if (!pwm_is_enabled(pwm))
			continue;

		/* Disable explicitly if PWM is running */
		pm_runtime_put_sync(dev);
	}

	return 0;
}

static int ehrpwm_pwm_resume(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	unsigned int i;

	for (i = 0; i < chip->npwm; i++) {
		struct pwm_device *pwm = &chip->pwms[i];

		if (!pwm_is_enabled(pwm))
			continue;

		/* Enable explicitly if PWM was running */
		pm_runtime_get_sync(dev);
	}

	ehrpwm_pwm_restore_context(chip);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ehrpwm_pwm_pm_ops, ehrpwm_pwm_suspend,
				ehrpwm_pwm_resume);

static struct platform_driver ehrpwm_pwm_driver = {
	.driver = {
		.name = "ehrpwm",
		.of_match_table = ehrpwm_of_match,
		.pm = pm_ptr(&ehrpwm_pwm_pm_ops),
	},
	.probe = ehrpwm_pwm_probe,
	.remove = ehrpwm_pwm_remove,
};
module_platform_driver(ehrpwm_pwm_driver);

MODULE_DESCRIPTION("EHRPWM PWM driver");
MODULE_AUTHOR("Texas Instruments");
MODULE_LICENSE("GPL");
