// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021 Samuel Holland <samuel@sholland.org>
 */

#include <linux/clk-provider.h>
#include <linux/io.h>

#include "ccu_common.h"

#include "ccu_gate.h"
#include "ccu_rtc.h"

static int ccu_iosc_enable(struct clk_hw *hw)
{
	struct ccu_common *cm = hw_to_ccu_common(hw);

	return ccu_gate_helper_enable(cm, DCXO_CTRL_CLK16M_RC_EN);
}

static void ccu_iosc_disable(struct clk_hw *hw)
{
	struct ccu_common *cm = hw_to_ccu_common(hw);

	return ccu_gate_helper_disable(cm, DCXO_CTRL_CLK16M_RC_EN);
}

static int ccu_iosc_is_enabled(struct clk_hw *hw)
{
	struct ccu_common *cm = hw_to_ccu_common(hw);

	return ccu_gate_helper_is_enabled(cm, DCXO_CTRL_CLK16M_RC_EN);
}

static unsigned long ccu_iosc_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct ccu_common *cm = hw_to_ccu_common(hw);

	if (cm->features & CCU_FEATURE_IOSC_CALIBRATION) {
		u32 reg = readl(cm->base + IOSC_CLK_CALI_REG);
		/*
		 * Recover the IOSC frequency by shifting the ones place of
		 * (fixed-point divider * 32768) into bit zero.
		 */
		if (reg & IOSC_CLK_CALI_EN)
			return reg >> (IOSC_CLK_CALI_DIV_ONES - LOSC_RATE_SHIFT);
	}

	return IOSC_RATE;
}

static unsigned long ccu_iosc_recalc_accuracy(struct clk_hw *hw,
					      unsigned long parent_accuracy)
{
	return IOSC_ACCURACY;
}

const struct clk_ops ccu_iosc_ops = {
	.enable			= ccu_iosc_enable,
	.disable		= ccu_iosc_disable,
	.is_enabled		= ccu_iosc_is_enabled,
	.recalc_rate		= ccu_iosc_recalc_rate,
	.recalc_accuracy	= ccu_iosc_recalc_accuracy,
};

static int ccu_iosc_32k_prepare(struct clk_hw *hw)
{
	struct ccu_common *cm = hw_to_ccu_common(hw);
	u32 val;

	if (!(cm->features & CCU_FEATURE_IOSC_CALIBRATION))
		return 0;

	val = readl(cm->base + IOSC_CLK_CALI_REG);
	writel(val | IOSC_CLK_CALI_EN | IOSC_CLK_CALI_SRC_SEL,
	       cm->base + IOSC_CLK_CALI_REG);

	return 0;
}

static void ccu_iosc_32k_unprepare(struct clk_hw *hw)
{
	struct ccu_common *cm = hw_to_ccu_common(hw);
	u32 val;

	if (!(cm->features & CCU_FEATURE_IOSC_CALIBRATION))
		return;

	val = readl(cm->base + IOSC_CLK_CALI_REG);
	writel(val & ~(IOSC_CLK_CALI_EN | IOSC_CLK_CALI_SRC_SEL),
	       cm->base + IOSC_CLK_CALI_REG);
}

static unsigned long ccu_iosc_32k_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct ccu_common *cm = hw_to_ccu_common(hw);
	u32 val;

	if (cm->features & CCU_FEATURE_IOSC_CALIBRATION) {
		val = readl(cm->base + IOSC_CLK_CALI_REG);

		/* Assume the calibrated 32k clock is accurate. */
		if (val & IOSC_CLK_CALI_SRC_SEL)
			return LOSC_RATE;
	}

	val = readl(cm->base + IOSC_32K_CLK_DIV_REG) & IOSC_32K_CLK_DIV;

	return parent_rate / IOSC_32K_PRE_DIV / (val + 1);
}

static unsigned long ccu_iosc_32k_recalc_accuracy(struct clk_hw *hw,
						  unsigned long parent_accuracy)
{
	struct ccu_common *cm = hw_to_ccu_common(hw);
	u32 val;

	if (cm->features & CCU_FEATURE_IOSC_CALIBRATION) {
		val = readl(cm->base + IOSC_CLK_CALI_REG);

		/* Assume the calibrated 32k clock is accurate. */
		if (val & IOSC_CLK_CALI_SRC_SEL)
			return 0;
	}

	return parent_accuracy;
}

const struct clk_ops ccu_iosc_32k_ops = {
	.prepare		= ccu_iosc_32k_prepare,
	.unprepare		= ccu_iosc_32k_unprepare,
	.recalc_rate		= ccu_iosc_32k_recalc_rate,
	.recalc_accuracy	= ccu_iosc_32k_recalc_accuracy,
};
