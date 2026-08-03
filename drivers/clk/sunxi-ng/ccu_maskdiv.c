// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 Juan Manuel López Carrillo
 *
 * Cycle-masking divider: the M factor masks M pulses out of every
 * 2^width parent cycles instead of dividing the parent rate, so
 *
 *	rate = parent * (2^width - M) / 2^width
 *
 * The masked output is not an even pulse train: the surviving pulses
 * keep the parent period. Rate selection therefore prefers, among the
 * parents that reach the requested rate, the one needing the least
 * masking.
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/math64.h>

#include "ccu_gate.h"
#include "ccu_maskdiv.h"

static unsigned long ccu_maskdiv_calc_rate(unsigned long parent_rate,
					   unsigned int m, unsigned int width)
{
	unsigned int n = 1 << width;

	return div_u64((u64)parent_rate * (n - m), n);
}

/*
 * Smallest M (least masking) whose output does not exceed the requested
 * rate; masking everything (M == 2^width) is never returned.
 */
static unsigned int ccu_maskdiv_find_m(unsigned long parent_rate,
				       unsigned long rate, unsigned int width)
{
	unsigned int n = 1 << width;
	u64 kept;

	if (!parent_rate || rate >= parent_rate)
		return 0;

	kept = div64_ul((u64)rate * n, parent_rate);
	if (!kept)
		kept = 1;

	return n - (unsigned int)kept;
}

static void ccu_maskdiv_disable(struct clk_hw *hw)
{
	struct ccu_maskdiv *cmd = hw_to_ccu_maskdiv(hw);

	return ccu_gate_helper_disable(&cmd->common, cmd->enable);
}

static int ccu_maskdiv_enable(struct clk_hw *hw)
{
	struct ccu_maskdiv *cmd = hw_to_ccu_maskdiv(hw);

	return ccu_gate_helper_enable(&cmd->common, cmd->enable);
}

static int ccu_maskdiv_is_enabled(struct clk_hw *hw)
{
	struct ccu_maskdiv *cmd = hw_to_ccu_maskdiv(hw);

	return ccu_gate_helper_is_enabled(&cmd->common, cmd->enable);
}

static unsigned long ccu_maskdiv_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct ccu_maskdiv *cmd = hw_to_ccu_maskdiv(hw);
	unsigned int m;
	u32 reg;

	reg = readl(cmd->common.base + cmd->common.reg);
	m = (reg >> cmd->shift) & ((1 << cmd->width) - 1);

	return ccu_maskdiv_calc_rate(parent_rate, m, cmd->width);
}

static int ccu_maskdiv_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	struct ccu_maskdiv *cmd = hw_to_ccu_maskdiv(hw);
	unsigned long best_rate = 0, best_parent_rate = 0;
	struct clk_hw *best_parent = NULL;
	unsigned int best_m = UINT_MAX;
	unsigned int i;

	for (i = 0; i < clk_hw_get_num_parents(hw); i++) {
		struct clk_hw *parent = clk_hw_get_parent_by_index(hw, i);
		unsigned long parent_rate, new_rate;
		unsigned int m;

		if (!parent)
			continue;

		parent_rate = clk_hw_get_rate(parent);
		m = ccu_maskdiv_find_m(parent_rate, req->rate, cmd->width);
		new_rate = ccu_maskdiv_calc_rate(parent_rate, m, cmd->width);

		if (new_rate > req->rate)
			continue;

		/*
		 * Reject rates outside the framework's bounds: a maskdiv
		 * rounds by masking parent cycles, so it can only produce
		 * sub-multiples of a parent rate; without this check a
		 * consumer asking for, say, a tight [max_rate, max_rate]
		 * window would silently get a smaller rate.
		 */
		if (new_rate < req->min_rate || new_rate > req->max_rate)
			continue;

		/* Closest rate first; on ties, the least masking */
		if (new_rate > best_rate ||
		    (new_rate == best_rate && m < best_m)) {
			best_rate = new_rate;
			best_parent_rate = parent_rate;
			best_parent = parent;
			best_m = m;
		}
	}

	if (!best_parent)
		return -EINVAL;

	req->best_parent_hw = best_parent;
	req->best_parent_rate = best_parent_rate;
	req->rate = best_rate;

	return 0;
}

static int ccu_maskdiv_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct ccu_maskdiv *cmd = hw_to_ccu_maskdiv(hw);
	unsigned int m;
	unsigned long flags;
	u32 reg;

	m = ccu_maskdiv_find_m(parent_rate, rate, cmd->width);

	spin_lock_irqsave(cmd->common.lock, flags);

	reg = readl(cmd->common.base + cmd->common.reg);
	reg &= ~GENMASK(cmd->shift + cmd->width - 1, cmd->shift);
	if (cmd->common.features & CCU_FEATURE_KEY_FIELD)
		reg |= CCU_MUX_KEY_VALUE;
	if (cmd->common.features & CCU_FEATURE_UPDATE_BIT)
		reg |= CCU_SUNXI_UPDATE_BIT;
	writel(reg | (m << cmd->shift), cmd->common.base + cmd->common.reg);

	spin_unlock_irqrestore(cmd->common.lock, flags);

	return 0;
}

static u8 ccu_maskdiv_get_parent(struct clk_hw *hw)
{
	struct ccu_maskdiv *cmd = hw_to_ccu_maskdiv(hw);

	return ccu_mux_helper_get_parent(&cmd->common, &cmd->mux);
}

static int ccu_maskdiv_set_parent(struct clk_hw *hw, u8 index)
{
	struct ccu_maskdiv *cmd = hw_to_ccu_maskdiv(hw);

	return ccu_mux_helper_set_parent(&cmd->common, &cmd->mux, index);
}

static int ccu_maskdiv_set_rate_and_parent(struct clk_hw *hw,
					   unsigned long rate,
					   unsigned long parent_rate, u8 index)
{
	/*
	 * Same ordering rule as clk_composite_set_rate_and_parent(): if
	 * switching the mux with the current M would overshoot the
	 * requested rate, program the divider first, so the
	 * intermediate rate never exceeds both the old and the new
	 * rate.
	 */
	if (ccu_maskdiv_recalc_rate(hw, parent_rate) > rate) {
		ccu_maskdiv_set_rate(hw, rate, parent_rate);
		ccu_maskdiv_set_parent(hw, index);
	} else {
		ccu_maskdiv_set_parent(hw, index);
		ccu_maskdiv_set_rate(hw, rate, parent_rate);
	}

	return 0;
}

const struct clk_ops ccu_maskdiv_ops = {
	.disable	= ccu_maskdiv_disable,
	.enable		= ccu_maskdiv_enable,
	.is_enabled	= ccu_maskdiv_is_enabled,

	.get_parent	= ccu_maskdiv_get_parent,
	.set_parent	= ccu_maskdiv_set_parent,

	.determine_rate	= ccu_maskdiv_determine_rate,
	.recalc_rate	= ccu_maskdiv_recalc_rate,
	.set_rate	= ccu_maskdiv_set_rate,
	.set_rate_and_parent = ccu_maskdiv_set_rate_and_parent,
};
EXPORT_SYMBOL_NS_GPL(ccu_maskdiv_ops, "SUNXI_CCU");
