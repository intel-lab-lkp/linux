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
#define TBCTL			0x00
#define TBPRD			0x0A

#define TBCTL_PRDLD_MASK	BIT(3)
#define TBCTL_PRDLD_SHDW	0
#define TBCTL_PRDLD_IMDT	BIT(3)
#define TBCTL_CLKDIV_MASK	(BIT(12) | BIT(11) | BIT(10) | BIT(9) | \
				BIT(8) | BIT(7))
#define TBCTL_CTRMODE_MASK	(BIT(1) | BIT(0))
#define TBCTL_CTRMODE_UP	0
#define TBCTL_CTRMODE_DOWN	BIT(0)
#define TBCTL_CTRMODE_UPDOWN	BIT(1)
#define TBCTL_CTRMODE_FREEZE	(BIT(1) | BIT(0))

#define TBCTL_HSPCLKDIV_SHIFT	7
#define TBCTL_CLKDIV_SHIFT	10

#define CLKDIV_MAX		7
#define HSPCLKDIV_MAX		7
#define PERIOD_MAX		0xFFFF

/* compare module registers */
#define CMPA			0x12
#define CMPB			0x14

/* Action qualifier module registers */
#define AQCTLA			0x16
#define AQCTLB			0x18
#define AQSFRC			0x1A
#define AQCSFRC			0x1C

#define AQCTL_CBU_MASK		(BIT(9) | BIT(8))
#define AQCTL_CBU_FRCLOW	BIT(8)
#define AQCTL_CBU_FRCHIGH	BIT(9)
#define AQCTL_CBU_FRCTOGGLE	(BIT(9) | BIT(8))
#define AQCTL_CAU_MASK		(BIT(5) | BIT(4))
#define AQCTL_CAU_FRCLOW	BIT(4)
#define AQCTL_CAU_FRCHIGH	BIT(5)
#define AQCTL_CAU_FRCTOGGLE	(BIT(5) | BIT(4))
#define AQCTL_PRD_MASK		(BIT(3) | BIT(2))
#define AQCTL_PRD_FRCLOW	BIT(2)
#define AQCTL_PRD_FRCHIGH	BIT(3)
#define AQCTL_PRD_FRCTOGGLE	(BIT(3) | BIT(2))
#define AQCTL_ZRO_MASK		(BIT(1) | BIT(0))
#define AQCTL_ZRO_FRCLOW	BIT(0)
#define AQCTL_ZRO_FRCHIGH	BIT(1)
#define AQCTL_ZRO_FRCTOGGLE	(BIT(1) | BIT(0))

#define AQCTL_CHANA_POLNORMAL	(AQCTL_CAU_FRCLOW | AQCTL_PRD_FRCHIGH | \
				AQCTL_ZRO_FRCHIGH)
#define AQCTL_CHANA_POLINVERSED	(AQCTL_CAU_FRCHIGH | AQCTL_PRD_FRCLOW | \
				AQCTL_ZRO_FRCLOW)
#define AQCTL_CHANB_POLNORMAL	(AQCTL_CBU_FRCLOW | AQCTL_PRD_FRCHIGH | \
				AQCTL_ZRO_FRCHIGH)
#define AQCTL_CHANB_POLINVERSED	(AQCTL_CBU_FRCHIGH | AQCTL_PRD_FRCLOW | \
				AQCTL_ZRO_FRCLOW)

#define AQSFRC_RLDCSF_MASK	(BIT(7) | BIT(6))
#define AQSFRC_RLDCSF_ZRO	0
#define AQSFRC_RLDCSF_PRD	BIT(6)
#define AQSFRC_RLDCSF_ZROPRD	BIT(7)
#define AQSFRC_RLDCSF_IMDT	(BIT(7) | BIT(6))

#define AQCSFRC_CSFB_MASK	(BIT(3) | BIT(2))
#define AQCSFRC_CSFB_FRCDIS	0
#define AQCSFRC_CSFB_FRCLOW	BIT(2)
#define AQCSFRC_CSFB_FRCHIGH	BIT(3)
#define AQCSFRC_CSFB_DISSWFRC	(BIT(3) | BIT(2))
#define AQCSFRC_CSFA_MASK	(BIT(1) | BIT(0))
#define AQCSFRC_CSFA_FRCDIS	0
#define AQCSFRC_CSFA_FRCLOW	BIT(0)
#define AQCSFRC_CSFA_FRCHIGH	BIT(1)
#define AQCSFRC_CSFA_DISSWFRC	(BIT(1) | BIT(0))

#define AQCTLA_CAU      GENMASK(5, 4)
#define AQCTLA_CAD      GENMASK(9, 8)

/**
 * The ePWM hardware encodes compare actions with two bits each:
 *   00 = Do nothing
 *   01 = Clear
 *   10 = Set
 *   11 = Toggle
 */
#define AQ_CLEAR  1
#define AQ_SET    2

#define NUM_PWM_CHANNEL		2	/* EHRPWM channels */

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
	unsigned long period_cycles[NUM_PWM_CHANNEL];
	enum pwm_polarity polarity[NUM_PWM_CHANNEL];
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

	for (clkdiv = 0; clkdiv <= CLKDIV_MAX; clkdiv++) {
		for (hspclkdiv = 0; hspclkdiv <= HSPCLKDIV_MAX; hspclkdiv++) {
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
			if (*prescale_div > rqst_prescaler) {
				*tb_clk_div = (clkdiv << TBCTL_CLKDIV_SHIFT) |
					(hspclkdiv << TBCTL_HSPCLKDIV_SHIFT);
				return 0;
			}
		}
	}

	return 1;
}

static void configure_polarity(struct ehrpwm_pwm_chip *pc, int chan)
{
	u16 aqctl_val, aqctl_mask;
	unsigned int aqctl_reg;

	/*
	 * Configure PWM output to HIGH/LOW level on counter
	 * reaches compare register value and LOW/HIGH level
	 * on counter value reaches period register value and
	 * zero value on counter
	 */
	if (chan == 1) {
		aqctl_reg = AQCTLB;
		aqctl_mask = AQCTL_CBU_MASK;

		if (pc->polarity[chan] == PWM_POLARITY_INVERSED)
			aqctl_val = AQCTL_CHANB_POLINVERSED;
		else
			aqctl_val = AQCTL_CHANB_POLNORMAL;
	} else {
		aqctl_reg = AQCTLA;
		aqctl_mask = AQCTL_CAU_MASK;

		if (pc->polarity[chan] == PWM_POLARITY_INVERSED)
			aqctl_val = AQCTL_CHANA_POLINVERSED;
		else
			aqctl_val = AQCTL_CHANA_POLNORMAL;
	}

	aqctl_mask |= AQCTL_PRD_MASK | AQCTL_ZRO_MASK;
	ehrpwm_modify(pc->mmio_base, aqctl_reg, aqctl_mask, aqctl_val);
}

/*
 * period_ns = 10^9 * (ps_divval * period_cycles) / PWM_CLK_RATE
 * duty_ns   = 10^9 * (ps_divval * duty_cycles) / PWM_CLK_RATE
 */
static int ehrpwm_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			     u64 duty_ns, u64 period_ns)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);
	u32 period_cycles, duty_cycles;
	u16 ps_divval, tb_divval;
	unsigned int i, cmp_reg;
	unsigned long long c;

	if (period_ns > NSEC_PER_SEC)
		return -ERANGE;

	c = pc->clk_rate;
	c = c * period_ns;
	do_div(c, NSEC_PER_SEC);
	period_cycles = (unsigned long)c;

	if (period_cycles < 1) {
		period_cycles = 1;
		duty_cycles = 1;
	} else {
		c = pc->clk_rate;
		c = c * duty_ns;
		do_div(c, NSEC_PER_SEC);
		duty_cycles = (unsigned long)c;
	}

	/*
	 * Period values should be same for multiple PWM channels as IP uses
	 * same period register for multiple channels.
	 */
	for (i = 0; i < NUM_PWM_CHANNEL; i++) {
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
	if (set_prescale_div(period_cycles/PERIOD_MAX, &ps_divval,
			     &tb_divval)) {
		dev_err(pwmchip_parent(chip), "Unsupported values\n");
		return -EINVAL;
	}

	pm_runtime_get_sync(pwmchip_parent(chip));

	/* Update clock prescaler values */
	ehrpwm_modify(pc->mmio_base, TBCTL, TBCTL_CLKDIV_MASK, tb_divval);

	/* Update period & duty cycle with presacler division */
	period_cycles = period_cycles / ps_divval;
	duty_cycles = duty_cycles / ps_divval;

	/* Configure shadow loading on Period register */
	ehrpwm_modify(pc->mmio_base, TBCTL, TBCTL_PRDLD_MASK, TBCTL_PRDLD_SHDW);

	ehrpwm_write(pc->mmio_base, TBPRD, period_cycles);

	/* Configure ehrpwm counter for up-count mode */
	ehrpwm_modify(pc->mmio_base, TBCTL, TBCTL_CTRMODE_MASK,
		      TBCTL_CTRMODE_UP);

	if (pwm->hwpwm == 1)
		/* Channel 1 configured with compare B register */
		cmp_reg = CMPB;
	else
		/* Channel 0 configured with compare A register */
		cmp_reg = CMPA;

	ehrpwm_write(pc->mmio_base, cmp_reg, duty_cycles);

	pm_runtime_put_sync(pwmchip_parent(chip));

	return 0;
}

static int ehrpwm_pwm_set_polarity(struct pwm_chip *chip,
				   struct pwm_device *pwm,
				   enum pwm_polarity polarity)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	/* Configuration of polarity in hardware delayed, do at enable */
	pc->polarity[pwm->hwpwm] = polarity;

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
		aqcsfrc_val = AQCSFRC_CSFB_FRCDIS;
		aqcsfrc_mask = AQCSFRC_CSFB_MASK;
	} else {
		aqcsfrc_val = AQCSFRC_CSFA_FRCDIS;
		aqcsfrc_mask = AQCSFRC_CSFA_MASK;
	}

	/* Changes to shadow mode */
	ehrpwm_modify(pc->mmio_base, AQSFRC, AQSFRC_RLDCSF_MASK,
		      AQSFRC_RLDCSF_ZRO);

	ehrpwm_modify(pc->mmio_base, AQCSFRC, aqcsfrc_mask, aqcsfrc_val);

	/* Channels polarity can be configured from action qualifier module */
	configure_polarity(pc, pwm->hwpwm);

	/* Enable TBCLK */
	ret = clk_enable(pc->tbclk);
	if (ret) {
		dev_err(pwmchip_parent(chip), "Failed to enable TBCLK for %s: %d\n",
			dev_name(pwmchip_parent(chip)), ret);
		return ret;
	}

	return 0;
}

/**
 * ehrpwm_is_enabled - Checks if the eHRPWM channel is enabled
 * @chip:	pointer to the PWM chip structure
 *
 * @return:	true if the channel is enabled, false otherwise
 */
static bool ehrpwm_is_enabled(struct pwm_chip *chip)
{
	bool ret = false;

	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	u16 aqcsfrc_reg = 0u;
	u8 csfa_bits = 0u;
	u16 aqctla_reg = 0u;

	aqcsfrc_reg	= readw(pc->mmio_base + AQCSFRC);
	csfa_bits = (u8)(aqcsfrc_reg & AQCSFRC_CSFA_MASK);
	aqctla_reg = readw(pc->mmio_base + AQCTLA);
	/*
	 * If the CSFA value is non-zero,
	 * it means channel A is being forced via software
	 * (override), so we consider the PWM "disabled".
	 */
	if (csfa_bits)
		ret = false;

	/*
	 * If the control register (AQCTLA) is configured
	 * (non-zero), it means channel A has qualified actions
	 * and is therefore enabled to generate PWM.
	 */
	if (aqctla_reg)
		ret = true;

	return ret;
}

/**
 * ehrpwm_read_period - Reads the period of the eHRPWM channel (in ns)
 * @chip:		pointer to the PWM chip structure
 * @tbclk_rate:	time-base clock rate in Hz
 *
 * @return:		period in nanoseconds
 */
static u64 ehrpwm_read_period(struct pwm_chip *chip, unsigned long tbclk_rate)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	u16 tbprd_reg = 0u;
	u64 period_cycles = 0u;
	u64 period_ns = 0u;

	tbprd_reg = readw(pc->mmio_base + TBPRD);
	period_cycles = tbprd_reg + 1u;

	/*
	 * period_ns = (period_cycles * 1e9) / tbclk_rate
	 * Using DIV_ROUND_UP_ULL to avoid floating-point operations.
	 */
	period_ns = DIV_ROUND_UP_ULL(period_cycles * NSEC_PER_SEC, tbclk_rate);

	return period_ns;
}

/**
 * ehrpwm_read_duty_cycle - Reads the duty cycle of the eHRPWM channel (in ns)
 * @chip:		pointer to the PWM chip structure
 * @tbclk_rate:	time-base clock rate in Hz
 *
 * @return:		duty cycle in nanoseconds
 */
static u64 ehrpwm_read_duty_cycle(struct pwm_chip *chip, unsigned long tbclk_rate)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	u16 cmpa_reg = 0u;
	u64 duty_cycles = 0u;
	u64 duty_ns = 0u;

	cmpa_reg = readw(pc->mmio_base + CMPA);

	duty_cycles = cmpa_reg;

	/*
	 * duty_ns = (duty_cycles * 1e9) / tbclk_rate
	 * Using DIV_ROUND_UP_ULL to avoid floating-point operations.
	 */
	duty_ns = DIV_ROUND_UP_ULL(duty_cycles * NSEC_PER_SEC, tbclk_rate);

	return duty_ns;
}

/**
 * ehrpwm_read_polarity - Reads the polarity of the eHRPWM channel
 * @chip:	pointer to the PWM chip structure
 *
 * @return:	the polarity of the PWM (PWM_POLARITY_NORMAL or PWM_POLARITY_INVERSED)
 */
static enum pwm_polarity ehrpwm_read_polarity(struct pwm_chip *chip)
{
	enum pwm_polarity ret = PWM_POLARITY_NORMAL;

	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	u16 aqctla_reg = 0u;
	u8 cau_action = 0u;
	u8 cad_action = 0u;

	aqctla_reg = readw(pc->mmio_base + AQCTLA);
	cau_action = FIELD_GET(AQCTLA_CAU, aqctla_reg);
	cad_action = FIELD_GET(AQCTLA_CAD, aqctla_reg);

	/*
	 * Evaluate the actions to determine the PWM polarity:
	 *  - If an up-count event sets the output (AQ_SET) and a down-count
	 *    event clears it (AQ_CLEAR), then polarity is NORMAL.
	 *  - If an up-count event clears the output (AQ_CLEAR) and a down-count
	 *    event sets it (AQ_SET), then polarity is INVERSED.
	 */
	if (cau_action == AQ_SET && cad_action == AQ_CLEAR)
		ret = PWM_POLARITY_NORMAL;

	else if (cau_action == AQ_CLEAR && cad_action == AQ_SET)
		ret = PWM_POLARITY_INVERSED;

	return ret;
}

/**
 * ehrpwm_get_state - Retrieves the current state of the eHRPWM channel
 * @chip:	pointer to the PWM chip structure
 * @pwm:	pointer to the PWM device structure
 * @state:	pointer to the pwm_state structure to be filled
 *
 * @return:	0 on success or a negative error code on failure
 */
static int ehrpwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
							struct pwm_state *state)
{
	int ret = 0u;
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);
	unsigned long tbclk_rate = 0u;

	if (chip == NULL || pwm == NULL || state == NULL)
		return -EINVAL;

	tbclk_rate = clk_get_rate(pc->tbclk);
	if (tbclk_rate <= 0)
		return -EINVAL;

	state->enabled = ehrpwm_is_enabled(chip);
	state->period = ehrpwm_read_period(chip, tbclk_rate);
	state->duty_cycle = ehrpwm_read_duty_cycle(chip, tbclk_rate);
	state->polarity = ehrpwm_read_polarity(chip);

	return ret;
}

static void ehrpwm_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);
	u16 aqcsfrc_val, aqcsfrc_mask;

	/* Action Qualifier puts PWM output low forcefully */
	if (pwm->hwpwm) {
		aqcsfrc_val = AQCSFRC_CSFB_FRCLOW;
		aqcsfrc_mask = AQCSFRC_CSFB_MASK;
	} else {
		aqcsfrc_val = AQCSFRC_CSFA_FRCLOW;
		aqcsfrc_mask = AQCSFRC_CSFA_MASK;
	}

	/* Update shadow register first before modifying active register */
	ehrpwm_modify(pc->mmio_base, AQSFRC, AQSFRC_RLDCSF_MASK,
		      AQSFRC_RLDCSF_ZRO);
	ehrpwm_modify(pc->mmio_base, AQCSFRC, aqcsfrc_mask, aqcsfrc_val);
	/*
	 * Changes to immediate action on Action Qualifier. This puts
	 * Action Qualifier control on PWM output from next TBCLK
	 */
	ehrpwm_modify(pc->mmio_base, AQSFRC, AQSFRC_RLDCSF_MASK,
		      AQSFRC_RLDCSF_IMDT);

	ehrpwm_modify(pc->mmio_base, AQCSFRC, aqcsfrc_mask, aqcsfrc_val);

	/* Disabling TBCLK on PWM disable */
	clk_disable(pc->tbclk);

	/* Disable clock on PWM disable */
	pm_runtime_put_sync(pwmchip_parent(chip));
}

static void ehrpwm_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	if (pwm_is_enabled(pwm)) {
		dev_warn(pwmchip_parent(chip), "Removing PWM device without disabling\n");
		pm_runtime_put_sync(pwmchip_parent(chip));
	}

	/* set period value to zero on free */
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

		err = ehrpwm_pwm_set_polarity(chip, pwm, state->polarity);
		if (err)
			return err;
	}

	if (!state->enabled) {
		if (enabled)
			ehrpwm_pwm_disable(chip, pwm);
		return 0;
	}

	err = ehrpwm_pwm_config(chip, pwm, state->duty_cycle, state->period);
	if (err)
		return err;

	if (!enabled)
		err = ehrpwm_pwm_enable(chip, pwm);

	return err;
}

static const struct pwm_ops ehrpwm_pwm_ops = {
	.free = ehrpwm_pwm_free,
	.apply = ehrpwm_pwm_apply,
	.get_state = ehrpwm_get_state,
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
	struct pwm_state state;
	struct pwm_chip *chip;
	struct clk *clk;
	bool tbclk_enabled;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, NUM_PWM_CHANNEL, sizeof(*pc));
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

	ehrpwm_get_state(chip, &chip->pwms[0], &state);

	if (state.enabled == true) {
		ret = clk_prepare_enable(pc->tbclk);
		if (ret) {
			dev_err_probe(&pdev->dev, ret, "clk_prepare_enable() failed");
			goto err_pwmchip_remove;
		}

		tbclk_enabled = true;
	}

	ret = pwmchip_add(chip);
	if (ret < 0) {
		dev_err(&pdev->dev, "pwmchip_add() failed: %d\n", ret);
		goto err_clk_unprepare;
	}

	platform_set_drvdata(pdev, chip);
	pm_runtime_enable(&pdev->dev);

	if (state.enabled == true) {
		ret = pm_runtime_get_sync(&pdev->dev);
		if (ret < 0) {
			dev_err_probe(&pdev->dev, ret, "pm_runtime_get_sync() failed");
			clk_disable_unprepare(pc->tbclk);
			goto err_pwmchip_remove;
		}
	}

	return 0;

err_pwmchip_remove:
	pwmchip_remove(chip);
err_clk_unprepare:
	if (tbclk_enabled)
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

	pc->ctx.tbctl = ehrpwm_read(pc->mmio_base, TBCTL);
	pc->ctx.tbprd = ehrpwm_read(pc->mmio_base, TBPRD);
	pc->ctx.cmpa = ehrpwm_read(pc->mmio_base, CMPA);
	pc->ctx.cmpb = ehrpwm_read(pc->mmio_base, CMPB);
	pc->ctx.aqctla = ehrpwm_read(pc->mmio_base, AQCTLA);
	pc->ctx.aqctlb = ehrpwm_read(pc->mmio_base, AQCTLB);
	pc->ctx.aqsfrc = ehrpwm_read(pc->mmio_base, AQSFRC);
	pc->ctx.aqcsfrc = ehrpwm_read(pc->mmio_base, AQCSFRC);

	pm_runtime_put_sync(pwmchip_parent(chip));
}

static void ehrpwm_pwm_restore_context(struct pwm_chip *chip)
{
	struct ehrpwm_pwm_chip *pc = to_ehrpwm_pwm_chip(chip);

	ehrpwm_write(pc->mmio_base, TBPRD, pc->ctx.tbprd);
	ehrpwm_write(pc->mmio_base, CMPA, pc->ctx.cmpa);
	ehrpwm_write(pc->mmio_base, CMPB, pc->ctx.cmpb);
	ehrpwm_write(pc->mmio_base, AQCTLA, pc->ctx.aqctla);
	ehrpwm_write(pc->mmio_base, AQCTLB, pc->ctx.aqctlb);
	ehrpwm_write(pc->mmio_base, AQSFRC, pc->ctx.aqsfrc);
	ehrpwm_write(pc->mmio_base, AQCSFRC, pc->ctx.aqcsfrc);
	ehrpwm_write(pc->mmio_base, TBCTL, pc->ctx.tbctl);
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
