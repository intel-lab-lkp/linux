// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024-2026 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include <linux/export.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include "clk-pll.h"

#define TIMEOUT 500

static inline struct rtk_clk_regmap_pll *to_rtk_clk_regmap_pll(struct clk_hw *hw)
{
	struct rtk_clk_regmap *clkr = to_rtk_clk_regmap(hw);

	return container_of(clkr, struct rtk_clk_regmap_pll, clkr);
}

static int wait_freq_ready(struct rtk_clk_regmap_pll *clkp)
{
	u32 pollval;

	/* reg == 0 means not configured.
	 * Register offset 0 is never a valid address on Realtek SoCs.
	 */
	if (!clkp->freq_ready_reg)
		return 0;

	return regmap_read_poll_timeout_atomic(clkp->clkr.regmap, clkp->freq_ready_reg, pollval,
		(pollval & clkp->freq_ready_mask) == clkp->freq_ready_val, 1, TIMEOUT);
}

static bool is_power_on(struct rtk_clk_regmap_pll *clkp)
{
	u32 val;

	/* reg == 0 means not configured (assume always on).
	 * Register offset 0 is never a valid address on Realtek SoCs.
	 */
	if (!clkp->power_reg)
		return true;

	if (regmap_read(clkp->clkr.regmap, clkp->power_reg, &val))
		return false;

	return (val & clkp->power_mask) == clkp->power_val_on;
}

static void rtk_clk_regmap_pll_disable(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll *clkp = to_rtk_clk_regmap_pll(hw);
	unsigned long flags;

	if (!clkp->seq_power_off)
		return;

	spin_lock_irqsave(&clkp->lock, flags);

	regmap_multi_reg_write(clkp->clkr.regmap, clkp->seq_power_off,
			       clkp->num_seq_power_off);

	spin_unlock_irqrestore(&clkp->lock, flags);
}

static int rtk_clk_regmap_pll_is_enabled(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll *clkp = to_rtk_clk_regmap_pll(hw);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&clkp->lock, flags);

	ret = is_power_on(clkp);

	spin_unlock_irqrestore(&clkp->lock, flags);

	return ret;
}

static int rtk_clk_regmap_pll_determine_rate(struct clk_hw *hw,
					     struct clk_rate_request *req)
{
	struct rtk_clk_regmap_pll *clkp = to_rtk_clk_regmap_pll(hw);
	const struct rtk_freq_table *ftblv = NULL;

	ftblv = ftbl_find_by_rate(clkp->freq_tbl, req->rate);

	if (ftblv && ftblv->rate >= req->min_rate) {
		req->rate = ftblv->rate;
		return 0;
	}

	/* floor result is below min_rate; find the smallest rate >= min_rate */
	ftblv = ftbl_find_ceil_by_rate(clkp->freq_tbl, req->min_rate);
	if (!ftblv || ftblv->rate > req->max_rate)
		return -EINVAL;

	req->rate = ftblv->rate;

	return 0;
}

static unsigned long rtk_clk_regmap_pll_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	struct rtk_clk_regmap_pll *clkp = to_rtk_clk_regmap_pll(hw);
	const struct rtk_freq_table *fv;
	unsigned long flags;
	u32 freq_val;

	spin_lock_irqsave(&clkp->lock, flags);

	if (regmap_read(clkp->clkr.regmap, clkp->freq_reg, &freq_val)) {
		spin_unlock_irqrestore(&clkp->lock, flags);
		return 0;
	}

	freq_val &= clkp->freq_mask;

	fv = ftbl_find_by_val_with_mask(clkp->freq_tbl, clkp->freq_mask,
					freq_val);

	spin_unlock_irqrestore(&clkp->lock, flags);

	return fv ? fv->rate : 0;
}

static int rtk_clk_regmap_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long parent_rate)
{
	struct rtk_clk_regmap_pll *clkp = to_rtk_clk_regmap_pll(hw);
	const struct rtk_freq_table *fv;
	unsigned long flags;
	int ret;

	fv = ftbl_find_by_rate(clkp->freq_tbl, rate);
	if (!fv || fv->rate != rate)
		return -EINVAL;

	spin_lock_irqsave(&clkp->lock, flags);

	if (clkp->seq_pre_set_freq) {
		ret = regmap_multi_reg_write(clkp->clkr.regmap, clkp->seq_pre_set_freq,
					     clkp->num_seq_pre_set_freq);
		if (ret)
			goto unlock;
	}

	ret = regmap_update_bits(clkp->clkr.regmap, clkp->freq_reg,
				 clkp->freq_mask, fv->val);
	if (ret)
		goto unlock;

	if (clkp->seq_post_set_freq) {
		ret = regmap_multi_reg_write(clkp->clkr.regmap, clkp->seq_post_set_freq,
					     clkp->num_seq_post_set_freq);
		if (ret)
			goto unlock;
	}

	if (is_power_on(clkp)) {
		ret = wait_freq_ready(clkp);
		if (ret)
			goto unlock;
	}

unlock:
	spin_unlock_irqrestore(&clkp->lock, flags);

	return ret;
}

static int rtk_clk_regmap_pll_enable(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll *clkp = to_rtk_clk_regmap_pll(hw);
	unsigned long flags;
	int ret = 0;

	if (!clkp->seq_power_on)
		return ret;

	spin_lock_irqsave(&clkp->lock, flags);

	if (is_power_on(clkp))
		goto unlock;

	ret = regmap_multi_reg_write(clkp->clkr.regmap, clkp->seq_power_on,
				     clkp->num_seq_power_on);
	if (ret)
		goto unlock;

	ret = wait_freq_ready(clkp);
	if (ret)
		goto unlock;

unlock:
	spin_unlock_irqrestore(&clkp->lock, flags);

	return ret;
}

const struct clk_ops rtk_clk_pll_ops = {
	.enable         = rtk_clk_regmap_pll_enable,
	.disable        = rtk_clk_regmap_pll_disable,
	.is_enabled     = rtk_clk_regmap_pll_is_enabled,
	.recalc_rate    = rtk_clk_regmap_pll_recalc_rate,
	.determine_rate = rtk_clk_regmap_pll_determine_rate,
	.set_rate       = rtk_clk_regmap_pll_set_rate,
};
EXPORT_SYMBOL_NS_GPL(rtk_clk_pll_ops, "CLK_REALTEK");

const struct clk_ops rtk_clk_pll_ro_ops = {
	.recalc_rate = rtk_clk_regmap_pll_recalc_rate,
};
EXPORT_SYMBOL_NS_GPL(rtk_clk_pll_ro_ops, "CLK_REALTEK");
