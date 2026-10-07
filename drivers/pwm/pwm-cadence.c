// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cadence TTC PWM driver
 *
 * Configures Cadence Triple Timer Counter channels as PWM outputs.
 * Binds to auxiliary-bus devices created by the TTC clocksource driver.
 *
 * Limitations:
 * - When PWM is stopped, timer counter gets stopped immediately. This
 *   doesn't allow the current PWM period to complete and stops abruptly.
 * - Disabled PWM emits inactive level.
 * - When user requests a change in any parameter of PWM (period/duty cycle/polarity)
 *   while PWM is in enabled state:
 *	- PWM is stopped abruptly.
 *	- Requested parameter is changed.
 *	- Fresh PWM cycle is started.
 *
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/pwm.h>
#include <linux/timer-cadence-ttc.h>

struct cdns_ttc_pwm {
	const struct ttc_shared_data	*sd;
	u32				max_count;
};

static inline struct cdns_ttc_pwm *to_cdns_ttc_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static void cdns_ttc_pwm_enable(struct cdns_ttc_pwm *priv, unsigned int hwpwm)
{
	u32 ctrl;

	ctrl = ttc_ch_readl(priv->sd, hwpwm, TTC_CNT_CNTRL_OFFSET);
	ctrl |= TTC_CNT_CNTRL_INT | TTC_CNT_CNTRL_MATCH | TTC_CNT_CNTRL_RST;
	ctrl &= ~(TTC_CNT_CNTRL_DISABLE_MASK | TTC_CNT_CNTRL_WAVE_EN);
	ttc_ch_writel(priv->sd, hwpwm, TTC_CNT_CNTRL_OFFSET, ctrl);
}

static void cdns_ttc_pwm_disable(struct cdns_ttc_pwm *priv, unsigned int hwpwm)
{
	u32 ctrl;

	ctrl = ttc_ch_readl(priv->sd, hwpwm, TTC_CNT_CNTRL_OFFSET);
	ctrl |= TTC_CNT_CNTRL_DISABLE_MASK;
	ttc_ch_writel(priv->sd, hwpwm, TTC_CNT_CNTRL_OFFSET, ctrl);
}

static int cdns_ttc_pwm_apply(struct pwm_chip *chip,
			      struct pwm_device *pwm,
			      const struct pwm_state *state)
{
	struct cdns_ttc_pwm *priv = to_cdns_ttc_pwm(chip);
	unsigned long rate = priv->sd->rate;
	unsigned int hwpwm = pwm->hwpwm;
	u64 period_cycles, duty_cycles;
	bool use_prescaler = false;
	u32 ctrl, psv;

	if (!state->enabled) {
		cdns_ttc_pwm_disable(priv, hwpwm);
		return 0;
	}

	period_cycles = mul_u64_u64_div_u64(state->period, rate, NSEC_PER_SEC);

	if (period_cycles > priv->max_count) {
		unsigned int shift;

		/*
		 * Prescaler divides input clock by 2^(psv+1).
		 * Calculate required shift and derive PSV register value.
		 */
		shift = order_base_2(DIV_ROUND_UP_ULL(period_cycles,
						      priv->max_count));
		if (shift > TTC_CLK_CNTRL_PSV_MAX + 1)
			return -ERANGE;

		use_prescaler = true;
		psv = shift - 1;

		period_cycles = mul_u64_u64_div_u64(state->period, rate,
						    (u64)NSEC_PER_SEC << shift);
	}

	if (!period_cycles)
		return -EINVAL;

	cdns_ttc_pwm_disable(priv, hwpwm);

	ctrl = ttc_ch_readl(priv->sd, hwpwm, TTC_CNT_CNTRL_OFFSET);
	if (state->polarity == PWM_POLARITY_NORMAL)
		ctrl |= TTC_CNT_CNTRL_WAVE_POL;
	else
		ctrl &= ~TTC_CNT_CNTRL_WAVE_POL;
	ttc_ch_writel(priv->sd, hwpwm, TTC_CNT_CNTRL_OFFSET, ctrl);

	if (use_prescaler) {
		duty_cycles = mul_u64_u64_div_u64(state->duty_cycle, rate,
						  (u64)NSEC_PER_SEC << (psv + 1));
	} else {
		duty_cycles = mul_u64_u64_div_u64(state->duty_cycle, rate,
						  NSEC_PER_SEC);
	}

	ttc_ch_writel(priv->sd, hwpwm, TTC_INTR_VAL_OFFSET, (u32)period_cycles);
	ttc_ch_writel(priv->sd, hwpwm, TTC_MATCH_CNT_VAL_OFFSET, (u32)duty_cycles);

	ctrl = ttc_ch_readl(priv->sd, hwpwm, TTC_CLK_CNTRL_OFFSET);
	if (use_prescaler) {
		ctrl &= ~TTC_CLK_CNTRL_PSV_MASK;
		ctrl |= FIELD_PREP(TTC_CLK_CNTRL_PSV_MASK, psv);
		ctrl |= TTC_CLK_CNTRL_PS_EN;
	} else {
		ctrl &= ~TTC_CLK_CNTRL_PS_EN;
	}
	ttc_ch_writel(priv->sd, hwpwm, TTC_CLK_CNTRL_OFFSET, ctrl);

	cdns_ttc_pwm_enable(priv, hwpwm);

	return 0;
}

static int cdns_ttc_pwm_get_state(struct pwm_chip *chip,
				  struct pwm_device *pwm,
				  struct pwm_state *state)
{
	struct cdns_ttc_pwm *priv = to_cdns_ttc_pwm(chip);
	unsigned long rate = priv->sd->rate;
	unsigned int hwpwm = pwm->hwpwm;
	u32 ctrl, clk_ctrl, pres;
	u64 tmp;

	ctrl = ttc_ch_readl(priv->sd, hwpwm, TTC_CNT_CNTRL_OFFSET);

	state->polarity = (ctrl & TTC_CNT_CNTRL_WAVE_POL) ?
			  PWM_POLARITY_NORMAL : PWM_POLARITY_INVERSED;

	if (ctrl & TTC_CNT_CNTRL_DISABLE_MASK) {
		state->enabled = false;
		state->period = 0;
		state->duty_cycle = 0;
		return 0;
	}

	state->enabled = true;

	clk_ctrl = ttc_ch_readl(priv->sd, hwpwm, TTC_CLK_CNTRL_OFFSET);
	pres = 0;
	if (clk_ctrl & TTC_CLK_CNTRL_PS_EN)
		pres = FIELD_GET(TTC_CLK_CNTRL_PSV_MASK, clk_ctrl) + 1;

	tmp = (u64)ttc_ch_readl(priv->sd, hwpwm, TTC_INTR_VAL_OFFSET) << pres;
	state->period = mul_u64_u64_div_u64(tmp, NSEC_PER_SEC, rate);
	if (mul_u64_u64_div_u64(state->period, rate, NSEC_PER_SEC) < tmp)
		state->period++;

	tmp = (u64)ttc_ch_readl(priv->sd, hwpwm, TTC_MATCH_CNT_VAL_OFFSET) << pres;
	state->duty_cycle = mul_u64_u64_div_u64(tmp, NSEC_PER_SEC, rate);
	if (mul_u64_u64_div_u64(state->duty_cycle, rate, NSEC_PER_SEC) < tmp)
		state->duty_cycle++;

	return 0;
}

static const struct pwm_ops cdns_ttc_pwm_ops = {
	.apply = cdns_ttc_pwm_apply,
	.get_state = cdns_ttc_pwm_get_state,
};

static int cdns_ttc_pwm_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	const struct ttc_shared_data *sd = dev_get_platdata(&auxdev->dev);
	struct device *dev = &auxdev->dev;
	struct cdns_ttc_pwm *priv;
	struct pwm_chip *chip;
	int ret;

	if (!sd)
		return -EINVAL;

	chip = devm_pwmchip_alloc(dev, TTC_NUM_CHANNELS, sizeof(*priv));
	if (IS_ERR(chip))
		return PTR_ERR(chip);

	priv = to_cdns_ttc_pwm(chip);
	priv->sd = sd;
	priv->max_count = BIT_ULL(sd->timer_width) - 1;

	ret = devm_clk_rate_exclusive_get(dev, sd->clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to lock clock rate\n");

	chip->ops = &cdns_ttc_pwm_ops;

	ret = devm_pwmchip_add(dev, chip);
	if (ret)
		return dev_err_probe(dev, ret, "could not register PWM chip\n");

	return 0;
}

static const struct auxiliary_device_id cdns_ttc_pwm_id_table[] = {
	{ .name = "timer_cadence_ttc.pwm" },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, cdns_ttc_pwm_id_table);

static struct auxiliary_driver cdns_ttc_pwm_driver = {
	.name     = "cdns_ttc_pwm",
	.probe    = cdns_ttc_pwm_probe,
	.id_table = cdns_ttc_pwm_id_table,
};
module_auxiliary_driver(cdns_ttc_pwm_driver);

MODULE_AUTHOR("Mubin Sayyed <mubin.sayyed@amd.com>");
MODULE_AUTHOR("Meher Thanmaiee <meherthanmaiee.vanukuri@amd.com>");
MODULE_DESCRIPTION("Cadence TTC PWM driver");
MODULE_LICENSE("GPL");
