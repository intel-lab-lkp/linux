// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 SpacemiT Technology Co. Ltd
 * Copyright (c) 2024-2025 Haylen Chu <heylenay@4d2.org>
 *
 * MIX clock type is the combination of mux, factor or divider, and gate
 */

#include <linux/clk-provider.h>

#include "ccu_mix.h"

#define MIX_FC_TIMEOUT_US	10000
#define MIX_FC_DELAY_US		5

static u8 ccu_mux_get_parent(struct clk_hw *hw);

static void ccu_gate_disable(struct clk_hw *hw)
{
	struct ccu_mix *mix = hw_to_ccu_mix(hw);
	struct ccu_gate_config *gate = &mix->gate;
	u32 val = gate->inverted ? gate->mask : 0;

	ccu_update(&mix->common, ctrl, gate->mask, val);
}

static int ccu_gate_enable(struct clk_hw *hw)
{
	struct ccu_mix *mix = hw_to_ccu_mix(hw);
	struct ccu_gate_config *gate = &mix->gate;
	u32 val = gate->inverted ? 0 : gate->mask;

	ccu_update(&mix->common, ctrl, gate->mask, val);
	return 0;
}

static int ccu_gate_is_enabled(struct clk_hw *hw)
{
	struct ccu_mix *mix = hw_to_ccu_mix(hw);
	struct ccu_gate_config *gate = &mix->gate;
	u32 tmp = ccu_read(&mix->common, ctrl) & gate->mask;
	u32 val = gate->inverted ? 0 : gate->mask;

	return !!(tmp == val);
}

static unsigned long ccu_factor_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct ccu_mix *mix = hw_to_ccu_mix(hw);

	return parent_rate * mix->factor.mul / mix->factor.div;
}

static unsigned long ccu_div_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct ccu_mix *mix = hw_to_ccu_mix(hw);
	struct ccu_div_config *div = &mix->div;
	unsigned long val;

	if (div->bypass & BIT(ccu_mux_get_parent(hw)))
		return parent_rate;

	val = ccu_read(&mix->common, ctrl) >> div->shift;
	val &= (1 << div->width) - 1;

	return divider_recalc_rate(hw, parent_rate, val, NULL, 0, div->width);
}

/*
 * Some clocks require a "FC" (frequency change) bit to be set after changing
 * their rates or reparenting. This bit will be automatically cleared by
 * hardware in MIX_FC_TIMEOUT_US, which indicates the operation is completed.
 */
static int ccu_mix_trigger_fc(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);
	unsigned int val;

	if (!common->reg_fc)
		return 0;

	ccu_update(common, fc, common->mask_fc, common->mask_fc);

	return regmap_read_poll_timeout_atomic(common->regmap, common->reg_fc,
					       val, !(val & common->mask_fc),
					       MIX_FC_DELAY_US,
					       MIX_FC_TIMEOUT_US);
}

static int ccu_factor_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	req->rate = ccu_factor_recalc_rate(hw, req->best_parent_rate);

	return 0;
}

static int ccu_factor_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	return 0;
}

static unsigned long
ccu_mix_calc_best_rate(struct clk_hw *hw, unsigned long rate,
		       struct clk_hw **best_parent,
		       unsigned long *best_parent_rate,
		       u32 *div_val)
{
	struct ccu_mix *mix = hw_to_ccu_mix(hw);
	unsigned int parent_num = clk_hw_get_num_parents(hw);
	struct ccu_div_config *div = &mix->div;
	unsigned long best_rate = 0;
	unsigned long best_delta = ULONG_MAX;

	for (int i = 0; i < parent_num; i++) {
		struct clk_hw *parent = clk_hw_get_parent_by_index(hw, i);
		unsigned long parent_rate;
		u32 div_max = div->bypass & BIT(i) ? 1 : 1 << div->width;

		if (!parent)
			continue;

		parent_rate = clk_hw_get_rate(parent);
		if (!parent_rate)
			continue;

		for (int j = 1; j <= div_max; j++) {
			unsigned long tmp = DIV_ROUND_UP_ULL(parent_rate, j);
			unsigned long delta = abs_diff(tmp, rate);

			if (delta < best_delta) {
				best_delta = delta;
				best_rate = tmp;

				if (div_val)
					*div_val = j - 1;

				if (best_parent) {
					*best_parent      = parent;
					*best_parent_rate = parent_rate;
				}
			}
		}
	}

	return best_rate;
}

static int ccu_mix_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	req->rate = ccu_mix_calc_best_rate(hw, req->rate,
					   &req->best_parent_hw,
					   &req->best_parent_rate,
					   NULL);
	return req->rate ? 0 : -EINVAL;
}

static int ccu_mix_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct ccu_mix *mix = hw_to_ccu_mix(hw);
	struct ccu_common *common = &mix->common;
	struct ccu_div_config *div = &mix->div;
	u32 current_div, target_div = 0, mask;
	unsigned long best_delta = ULONG_MAX;

	if (div->bypass & BIT(ccu_mux_get_parent(hw)))
		return rate == parent_rate ? 0 : -EINVAL;

	/* set_rate must use the parent selected by CCF, not search other parents. */
	for (u32 i = 1; i <= BIT(div->width); i++) {
		unsigned long divided = DIV_ROUND_UP_ULL(parent_rate, i);
		unsigned long delta = abs_diff(divided, rate);

		if (delta < best_delta) {
			best_delta = delta;
			target_div = i - 1;
		}
	}

	current_div = ccu_read(common, ctrl) >> div->shift;
	current_div &= (1 << div->width) - 1;

	if (current_div == target_div)
		return 0;

	mask = GENMASK(div->width + div->shift - 1, div->shift);

	ccu_update(common, ctrl, mask, target_div << div->shift);

	return ccu_mix_trigger_fc(hw);
}

static u8 ccu_mux_get_parent(struct clk_hw *hw)
{
	struct ccu_mix *mix = hw_to_ccu_mix(hw);
	struct ccu_mux_config *mux = &mix->mux;
	u8 parent;

	parent = ccu_read(&mix->common, ctrl) >> mux->shift;
	parent &= (1 << mux->width) - 1;

	return parent;
}

static int ccu_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct ccu_mix *mix = hw_to_ccu_mix(hw);
	struct ccu_mux_config *mux = &mix->mux;
	u32 mask;

	mask = GENMASK(mux->width + mux->shift - 1, mux->shift);

	ccu_update(&mix->common, ctrl, mask, index << mux->shift);

	return ccu_mix_trigger_fc(hw);
}

const struct clk_ops spacemit_ccu_gate_ops = {
	.disable	= ccu_gate_disable,
	.enable		= ccu_gate_enable,
	.is_enabled	= ccu_gate_is_enabled,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_gate_ops, "CLK_SPACEMIT");

const struct clk_ops spacemit_ccu_factor_ops = {
	.determine_rate = ccu_factor_determine_rate,
	.recalc_rate	= ccu_factor_recalc_rate,
	.set_rate	= ccu_factor_set_rate,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_factor_ops, "CLK_SPACEMIT");

const struct clk_ops spacemit_ccu_mux_ops = {
	.determine_rate = ccu_mix_determine_rate,
	.get_parent	= ccu_mux_get_parent,
	.set_parent	= ccu_mux_set_parent,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_mux_ops, "CLK_SPACEMIT");

const struct clk_ops spacemit_ccu_div_ops = {
	.determine_rate = ccu_mix_determine_rate,
	.recalc_rate	= ccu_div_recalc_rate,
	.set_rate	= ccu_mix_set_rate,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_div_ops, "CLK_SPACEMIT");

const struct clk_ops spacemit_ccu_factor_gate_ops = {
	.disable	= ccu_gate_disable,
	.enable		= ccu_gate_enable,
	.is_enabled	= ccu_gate_is_enabled,

	.determine_rate = ccu_factor_determine_rate,
	.recalc_rate	= ccu_factor_recalc_rate,
	.set_rate	= ccu_factor_set_rate,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_factor_gate_ops, "CLK_SPACEMIT");

const struct clk_ops spacemit_ccu_mux_gate_ops = {
	.disable	= ccu_gate_disable,
	.enable		= ccu_gate_enable,
	.is_enabled	= ccu_gate_is_enabled,

	.determine_rate = ccu_mix_determine_rate,
	.get_parent	= ccu_mux_get_parent,
	.set_parent	= ccu_mux_set_parent,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_mux_gate_ops, "CLK_SPACEMIT");

const struct clk_ops spacemit_ccu_div_gate_ops = {
	.disable	= ccu_gate_disable,
	.enable		= ccu_gate_enable,
	.is_enabled	= ccu_gate_is_enabled,

	.determine_rate = ccu_mix_determine_rate,
	.recalc_rate	= ccu_div_recalc_rate,
	.set_rate	= ccu_mix_set_rate,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_div_gate_ops, "CLK_SPACEMIT");

const struct clk_ops spacemit_ccu_mux_div_gate_ops = {
	.disable	= ccu_gate_disable,
	.enable		= ccu_gate_enable,
	.is_enabled	= ccu_gate_is_enabled,

	.get_parent	= ccu_mux_get_parent,
	.set_parent	= ccu_mux_set_parent,

	.determine_rate = ccu_mix_determine_rate,
	.recalc_rate	= ccu_div_recalc_rate,
	.set_rate	= ccu_mix_set_rate,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_mux_div_gate_ops, "CLK_SPACEMIT");

const struct clk_ops spacemit_ccu_mux_div_ops = {
	.get_parent	= ccu_mux_get_parent,
	.set_parent	= ccu_mux_set_parent,

	.determine_rate = ccu_mix_determine_rate,
	.recalc_rate	= ccu_div_recalc_rate,
	.set_rate	= ccu_mix_set_rate,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_mux_div_ops, "CLK_SPACEMIT");
