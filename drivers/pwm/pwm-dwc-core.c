// SPDX-License-Identifier: GPL-2.0
/*
 * DesignWare PWM Controller driver core
 *
 * Copyright (C) 2018-2020 Intel Corporation
 *
 * Author: Felipe Balbi (Intel)
 * Author: Jarkko Nikula <jarkko.nikula@linux.intel.com>
 * Author: Raymond Tan <raymond.tan@intel.com>
 */

#define DEFAULT_SYMBOL_NAMESPACE "dwc_pwm"

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>

#include "pwm-dwc.h"

static void __dwc_pwm_set_enable(struct dwc_pwm *dwc, int pwm, int enabled)
{
	u32 reg;

	reg = dwc_pwm_readl(dwc, DWC_TIM_CTRL(pwm));

	if (enabled)
		reg |= DWC_TIM_CTRL_EN;
	else
		reg &= ~DWC_TIM_CTRL_EN;

	dwc_pwm_writel(dwc, reg, DWC_TIM_CTRL(pwm));
}

static int __dwc_pwm_configure_timer(struct dwc_pwm *dwc,
				     struct pwm_device *pwm,
				     const struct pwm_state *state)
{
	u64 tmp, period_cyc;
	u32 ctrl;
	u32 high;
	u32 low;

	if (dwc->clk)
		dwc->clk_rate = clk_get_rate(dwc->clk);

	if (dwc->features & DWC_TIM_CTRL_0N100PWM_EN) {
		/*
		 * Calculate the total period in clock cycles first, then the
		 * duty cycle. Derive the complementary half as the remainder to
		 * avoid compounding two independent floor-truncation errors:
		 * floor(duty) + floor(period - duty) can be one cycle short of
		 * floor(period). The PWM core requires the maximal achievable
		 * period not exceeding the requested value.
		 */
		period_cyc = mul_u64_u64_div_u64(state->period, dwc->clk_rate,
						 NSEC_PER_SEC);
		if (!period_cyc)
			return -ERANGE;

		tmp = mul_u64_u64_div_u64(state->duty_cycle, dwc->clk_rate,
					  NSEC_PER_SEC);
		/*
		 * Calculate width of low and high period in terms of input
		 * clock periods and check are the result within HW limits
		 * between 0 and 2^32 periods.
		 */
		if (tmp >= (1ULL << 32) || period_cyc - tmp >= (1ULL << 32))
			return -ERANGE;

		/*
		 * The hardware has no polarity register. Polarity inversion is
		 * achieved by swapping the low and high load-count registers:
		 * NORMAL (active-high): duty_cycle ->
		 *				HIGH period (DWC_TIM_LD_CNT2)
		 * INVERSED (active-low): duty_cycle ->
		 *				LOW period (DWC_TIM_LD_CNT)
		 */
		if (state->polarity == PWM_POLARITY_NORMAL) {
			high = tmp;
			low = period_cyc - tmp;
		} else {
			low = tmp;
			high = period_cyc - tmp;
		}
	} else {
		/*
		 * Calculate width of low and high period in terms of input
		 * clock periods and check are the result within HW limits
		 * between 1 and 2^32 periods.
		 * Polarity inversion uses the same register-swap technique as
		 * the 0N100 path above.
		 * Derive the complementary half from the total period to avoid
		 * compounding two independent floor-truncation errors.
		 */
		tmp = mul_u64_u64_div_u64(state->duty_cycle, dwc->clk_rate,
					  NSEC_PER_SEC);
		if (tmp < 1 || tmp > (1ULL << 32))
			return -ERANGE;

		period_cyc = mul_u64_u64_div_u64(state->period, dwc->clk_rate,
						 NSEC_PER_SEC);
		/* period_cyc - tmp: complementary half; tmp <= period_cyc */
		if (period_cyc - tmp < 1 || period_cyc - tmp > (1ULL << 32))
			return -ERANGE;

		if (state->polarity == PWM_POLARITY_NORMAL) {
			high = tmp - 1;
			low = period_cyc - tmp - 1;
		} else {
			low = tmp - 1;
			high = period_cyc - tmp - 1;
		}
	}

	/*
	 * Specification says timer usage flow is to disable timer, then
	 * program it followed by enable. It also says Load Count is loaded
	 * into timer after it is enabled - either after a disable or
	 * a reset. Based on measurements it happens also without disable
	 * whenever Load Count is updated. But follow the specification.
	 */
	__dwc_pwm_set_enable(dwc, pwm->hwpwm, false);

	/*
	 * Write Load Count and Load Count 2 registers. Former defines the
	 * width of low period and latter the width of high period in terms
	 * multiple of input clock periods:
	 * Width = ((Count + 1) * input clock period).
	 * Width = (Count * input clock period) : supported 0% and 100%.
	 */
	dwc_pwm_writel(dwc, low, DWC_TIM_LD_CNT(pwm->hwpwm));
	dwc_pwm_writel(dwc, high, DWC_TIM_LD_CNT2(pwm->hwpwm));

	/*
	 * Set user-defined mode, timer reloads from Load Count registers
	 * when it counts down to 0.
	 * Set PWM mode, it makes output to toggle and width of low and high
	 * periods are set by Load Count registers.
	 */
	ctrl = DWC_TIM_CTRL_MODE_USER | DWC_TIM_CTRL_PWM;
	/*
	 * Mask interrupts to prevent unmasked timer interrupts on shared IRQ
	 * systems where no IRQ handler is installed.
	 */
	ctrl |= DWC_TIM_CTRL_INT_MASK;
	if (dwc->features & DWC_TIM_CTRL_0N100PWM_EN)
		ctrl |= DWC_TIM_CTRL_0N100PWM_EN;

	dwc_pwm_writel(dwc, ctrl, DWC_TIM_CTRL(pwm->hwpwm));

	/*
	 * Enable timer. Output starts from low period.
	 */
	__dwc_pwm_set_enable(dwc, pwm->hwpwm, state->enabled);

	return 0;
}

static int dwc_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			 const struct pwm_state *state)
{
	struct dwc_pwm *dwc = to_dwc_pwm(chip);
	int ret;

	if (state->enabled) {
		if (!pwm->state.enabled) {
			ret = pm_runtime_resume_and_get(pwmchip_parent(chip));
			if (ret < 0)
				return ret;
		}
		ret = __dwc_pwm_configure_timer(dwc, pwm, state);
		if (ret && !pwm->state.enabled)
			pm_runtime_put_sync(pwmchip_parent(chip));
		return ret;
	} else {
		if (pwm->state.enabled) {
			__dwc_pwm_set_enable(dwc, pwm->hwpwm, false);
			pm_runtime_put_sync(pwmchip_parent(chip));
		}
	}

	return 0;
}

static int dwc_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			     struct pwm_state *state)
{
	struct dwc_pwm *dwc = to_dwc_pwm(chip);
	unsigned long clk_rate;
	u32 ctrl, ld, ld2;
	u64 duty, period;
	int ret;

	ret = pm_runtime_resume_and_get(pwmchip_parent(chip));
	if (ret < 0)
		return ret;

	if (dwc->clk)
		dwc->clk_rate = clk_get_rate(dwc->clk);

	clk_rate = dwc->clk_rate;
	if (!clk_rate) {
		pm_runtime_put_sync(pwmchip_parent(chip));
		return -EINVAL;
	}

	ctrl = dwc_pwm_readl(dwc, DWC_TIM_CTRL(pwm->hwpwm));
	ld = dwc_pwm_readl(dwc, DWC_TIM_LD_CNT(pwm->hwpwm));
	ld2 = dwc_pwm_readl(dwc, DWC_TIM_LD_CNT2(pwm->hwpwm));

	state->enabled = !!(ctrl & DWC_TIM_CTRL_EN);

	/*
	 * The hardware has no polarity status register; polarity is encoded
	 * implicitly by which of DWC_TIM_LD_CNT / DWC_TIM_LD_CNT2 holds the
	 * duty-cycle period (see __dwc_pwm_configure_timer). Report the
	 * polarity that was last programmed by apply(). On the initial read
	 * (before any apply call), pwm->state.polarity defaults to
	 * PWM_POLARITY_NORMAL, which is the natural zero-initialised value.
	 */
	state->polarity = pwm->state.polarity;

	/*
	 * If we're not in PWM, technically the output is a 50-50
	 * based on the timer load-count only.
	 */
	if (ctrl & DWC_TIM_CTRL_PWM) {
		if (ctrl & DWC_TIM_CTRL_0N100PWM_EN) {
			/*
			 * NORMAL: duty_cycle was written to DWC_TIM_LD_CNT2.
			 * INVERSED: duty_cycle was written to DWC_TIM_LD_CNT.
			 */
			if (state->polarity == PWM_POLARITY_NORMAL)
				duty = ld2;
			else
				duty = ld;
			period = (u64)ld + ld2;
		} else {
			if (state->polarity == PWM_POLARITY_NORMAL)
				duty = ld2 + 1;
			else
				duty = ld + 1;
			period = (u64)ld + ld2 + 2;
		}
	} else {
		duty = ld + 1;
		period = duty * 2;
		state->polarity = PWM_POLARITY_INVERSED;
	}

	state->period = mul_u64_u64_div_u64(period, NSEC_PER_SEC, clk_rate);
	state->duty_cycle = mul_u64_u64_div_u64(duty, NSEC_PER_SEC, clk_rate);

	pm_runtime_put_sync(pwmchip_parent(chip));

	return 0;
}

static const struct pwm_ops dwc_pwm_ops = {
	.apply = dwc_pwm_apply,
	.get_state = dwc_pwm_get_state,
};

struct pwm_chip *dwc_pwm_alloc(struct device *dev)
{
	struct pwm_chip *chip;
	struct dwc_pwm *dwc;

	chip = devm_pwmchip_alloc(dev, DWC_TIMERS_TOTAL, sizeof(*dwc));
	if (IS_ERR(chip))
		return chip;
	dwc = to_dwc_pwm(chip);

	dwc->clk_rate = NSEC_PER_SEC / 10;
	chip->ops = &dwc_pwm_ops;

	return chip;
}
EXPORT_SYMBOL_GPL(dwc_pwm_alloc);

MODULE_AUTHOR("Felipe Balbi (Intel)");
MODULE_AUTHOR("Jarkko Nikula <jarkko.nikula@linux.intel.com>");
MODULE_AUTHOR("Raymond Tan <raymond.tan@intel.com>");
MODULE_DESCRIPTION("DesignWare PWM Controller");
MODULE_LICENSE("GPL");
