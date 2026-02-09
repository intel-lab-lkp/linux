// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (c) 2026 Amlogic, Inc. All rights reserved
 */

#include <linux/err.h>
#include <linux/module.h>

#include "clk.h"
#include "clk-noglitch.h"

/*
 * Amlogic no-glitch clock module:
 *
 *             mux0       div0        gate0     noglitch_mux
 *              |  mux1    |    div1    |    gate1  |
 *              |   |      |     |      |      |    |
 *         +----|---|------|-----|------|------|----|----+
 *         |   \|/  |      |     |      |      |    |    |
 *         |   |\   |      |     |      |      |    |    |
 * clk0 ------>| |  |      |     |      |      |    |    |
 * clk1 ------>| |  |      |     |      |      |    |    |
 * clk2 ------>| |  |     \|/    |     \|/     |   \|/   |
 * clk3 ------>| |  |   +-----+  |   +------+  |   |\    |
 *         |   | |--o-->| div |--o-->| gate |--o-->| |   |
 * clk4 ------>| |  |   +-----+  |   +------+  |   | |   |
 * clk5 ------>| |  |            |             |   | |   |
 * clk6 ------>| |  |            |             |   | |   |
 * clk7 ------>| |  |            |             |   | |   |
 *         |   |/   |            |             |   | |   |
 *         |        |            |             |   | |   |
 *         |    +---+      +-----+      +------+   | |-------> clk out
 *         |   \|/         |            |          | |   |
 *         |   |\          |            |          | |   |
 * clk0 ------>| |         |            |          | |   |
 * clk1 ------>| |         |            |          | |   |
 * clk2 ------>| |        \|/          \|/         | |   |
 * clk3 ------>| |      +-----+     +------+       | |   |
 *         |   | |--- ->| div |---->| gate |------>| |   |
 * clk4 ------>| |      +-----+     +------+       |/    |
 * clk5 ------>| |                                       |
 * clk6 ------>| |                                       |
 * clk7 ------>| |                                       |
 *         |   |/                                        |
 *         |                                             |
 *         +---------------------------------------------+
 *
 * The purpose of the Amlogic no-glitch clock is to suppress glitches generated
 * during mux switching.
 *
 * Its internal implementation consists of two Amlogic composite clocks and one
 * noglitch_mux. The noglitch_mux filters out a certain number of clock cycles
 * from both the original channel and the new channel, and then continuously
 * outputs the clock from the new channel. This prevents glitches caused by an
 * insufficient settling time when switching clocks. The timing diagram of
 * noglitch_mux clock switching is shown below.
 *
 *        __    __    __    __    __    __    __    __
 * ori:  |  |__|  |__|  |__|  |__|  |__|  |__|  |__|  |__|
 *                   ^
 *                   1 * cycle original channel
 *        _   _   _   _   _   _   _   _   _   _   _   _
 * new:  | |_| |_| |_| |_| |_| |_| |_| |_| |_| |_| |_| |_|
 *                                       ^
 *                                       5 * cycles new channel
 *        __    __                        _   _   _   _
 * out:  |  |__|  |______________________| |_| |_| |_| |_|
 *              ^                        ^
 *              start switching mux      switch to new channel
 *
 * To prevent glitches from propagating to clk_out and affecting the normal
 * operation of glitch-sensitive modules, the no-glitch clock must be configured
 * following the specified sequence:
 *   - When the clock gate is disabled: configure it as a normal composite clock
 *     (any glitches generated will be blocked by the gate and will not
 *     propagate to clk_out).
 *   - When the clock gate is enabled: configure it according to the following
 *     sequence to suppress glitches:
 *       - Configure and enable the idle composite clock path of the
 *         noglitch_mux with the target frequency/parent clock.
 *       - Switch the noglitch_mux to the channel prepared in the previous step.
 *       - Disable the clock of the original noglitch_mux channel.
 */

union aml_clk_noglitch_reg {
	struct {
		u32 div0		:8;  /* bit0 - bit7 */
		u32 gate0		:1;  /* bit8 */
		u32 mux0		:3;  /* bit9 - bit11 */
		u32 reserved		:4;  /* bit12 - bit15 */
		u32 div1		:8;  /* bit16 - bit23 */
		u32 gate1		:1;  /* bit24 */
		u32 mux1		:3;  /* bit25 - bit27 */
		u32 reserved1		:3;  /* bit28 - bit30 */
		u32 noglitch_mux	:1;  /* bit31 */

	} bits;
	u32 val;
};

static u8 aml_clk_noglitch_get_parent(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_noglitch_data *noglitch = clk->data;
	union aml_clk_noglitch_reg reg;
	int ret;

	ret = regmap_read(clk->map, noglitch->reg_offset, &reg.val);
	if (ret)
		return ret;

	if (reg.bits.noglitch_mux)  /* mux1 */
		return clk_mux_val_to_index(hw, noglitch->table, 0,
					    reg.bits.mux1);
	else  /* mux0 */
		return clk_mux_val_to_index(hw, noglitch->table, 0,
					    reg.bits.mux0);
}

static int aml_clk_noglitch_set_parent(struct clk_hw *hw, u8 index)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_noglitch_data *noglitch = clk->data;
	unsigned int val = clk_mux_index_to_val(noglitch->table, 0, index);
	union aml_clk_noglitch_reg reg;
	int ret;

	ret = regmap_read(clk->map, noglitch->reg_offset, &reg.val);
	if (ret)
		return ret;

	if (reg.bits.noglitch_mux) {  /* mux1 */
		/*
		 * When the no-glitch output is enabled, the clock must be
		 * configured with the following timing sequence to suppress
		 * glitches:
		 *
		 * step1: Configure and enable Channel 0 clock.
		 * step2: Switch the no-glitch mux to Channel 0.
		 * step3: Disable Channel 1 clock.
		 */
		if (reg.bits.gate1) {
			/* step1: Configure and enable Channel 0 clock */
			reg.bits.mux0 = val;
			reg.bits.gate0 = 1;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			/* step2: Switch the no-glitch mux to Channel 0 */
			reg.bits.noglitch_mux = 0;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			/* step3: Disable Channel 1 clock */
			reg.bits.gate1 = 0;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		} else {
			/*
			 * When no-glitch is disabled, no glitch will occur,
			 * allowing direct clock source switching.
			 */
			reg.bits.mux1 = val;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		}
	} else {  /* mux0 */
		if (reg.bits.gate0) {
			reg.bits.mux1 = val;
			reg.bits.gate1 = 1;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			reg.bits.noglitch_mux = 1;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			reg.bits.gate0 = 0;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		} else {
			reg.bits.mux0 = val;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static unsigned long aml_clk_noglitch_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_noglitch_data *noglitch = clk->data;
	union aml_clk_noglitch_reg reg;
	int ret;

	ret = regmap_read(clk->map, noglitch->reg_offset, &reg.val);
	if (ret)
		return ret;

	if (reg.bits.noglitch_mux)  /* mux1 */
		return divider_recalc_rate(hw, parent_rate, reg.bits.div1, NULL,
					   0, CLK_NOGLITCH_DIV_WIDTH);
	else  /* mux0 */
		return divider_recalc_rate(hw, parent_rate, reg.bits.div0, NULL,
					   0, CLK_NOGLITCH_DIV_WIDTH);
}

static int
aml_clk_noglitch_determine_rate_for_parent(struct clk_hw *rate_hw,
					   struct clk_rate_request *req,
					   struct clk_hw *parent_hw)
{
	req->best_parent_hw = parent_hw;
	req->best_parent_rate = clk_hw_get_rate(parent_hw);

	return divider_determine_rate(rate_hw, req, NULL,
				      CLK_NOGLITCH_DIV_WIDTH, 0);
}

static int aml_clk_noglitch_determine_rate(struct clk_hw *hw,
					   struct clk_rate_request *req)
{
	struct clk_hw *parent;
	struct clk_rate_request tmp_req;
	unsigned long rate_diff;
	unsigned long best_rate_diff = ULONG_MAX;
	unsigned long best_rate = 0;
	int i, ret;

	req->best_parent_hw = NULL;

	parent = clk_hw_get_parent(hw);
	clk_hw_forward_rate_request(hw, req, parent, &tmp_req, req->rate);
	ret = aml_clk_noglitch_determine_rate_for_parent(hw, &tmp_req,
							 parent);
	if (ret)
		return ret;

	/*
	 * Check if rate can be satisfied by current parent clock. Avoid parent
	 * switching when possible to reduce glitches.
	 */
	if (req->rate == tmp_req.rate ||
	    (clk_hw_get_flags(hw) & CLK_SET_RATE_NO_REPARENT)) {
		req->rate = tmp_req.rate;
		req->best_parent_hw = tmp_req.best_parent_hw;
		req->best_parent_rate = tmp_req.best_parent_rate;

		return 0;
	}

	for (i = 0; i < clk_hw_get_num_parents(hw); i++) {
		parent = clk_hw_get_parent_by_index(hw, i);
		if (!parent)
			continue;

		clk_hw_forward_rate_request(hw, req, parent, &tmp_req,
					    req->rate);
		ret = aml_clk_noglitch_determine_rate_for_parent(hw, &tmp_req,
								 parent);
		if (ret)
			continue;

		if (req->rate >= tmp_req.rate)
			rate_diff = req->rate - tmp_req.rate;
		else
			rate_diff = tmp_req.rate - req->rate;

		if (!rate_diff || !req->best_parent_hw ||
		    best_rate_diff > rate_diff) {
			req->best_parent_hw = parent;
			req->best_parent_rate = tmp_req.best_parent_rate;
			best_rate_diff = rate_diff;
			best_rate = tmp_req.rate;
		}

		if (!rate_diff)
			return 0;
	}

	req->rate = best_rate;
	return 0;
}

static int aml_clk_noglitch_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_noglitch_data *noglitch = clk->data;
	union aml_clk_noglitch_reg reg;
	int ret, div;

	div = divider_get_val(rate, parent_rate, NULL,
			      CLK_NOGLITCH_DIV_WIDTH, 0);
	if (div < 0)
		return div;

	ret = regmap_read(clk->map, noglitch->reg_offset, &reg.val);
	if (ret)
		return ret;

	if (reg.bits.noglitch_mux) {  /* mux1 */
		/*
		 * When the no-glitch output is enabled, the clock must be
		 * configured with the following timing sequence to suppress
		 * glitches:
		 *
		 * step1: Configure and enable Channel 0 clock.
		 * step2: Switch the no-glitch mux to Channel 0.
		 * step3: Disable Channel 1 clock.
		 */
		if (reg.bits.gate1) {
			/* step1: Configure and enable Channel 0 clock */
			reg.bits.div0 = div;
			reg.bits.gate0 = 1;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			/* step2: Switch the no-glitch mux to Channel 0 */
			reg.bits.noglitch_mux = 0;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			/* step3: Disable Channel 1 clock */
			reg.bits.gate1 = 0;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		} else {
			/*
			 * When no-glitch is disabled, no glitch will occur,
			 * allowing direct clock source switching.
			 */
			reg.bits.div1 = div;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		}
	} else {  /* mux0 */
		if (reg.bits.gate0) {
			reg.bits.div1 = div;
			reg.bits.gate1 = 1;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			reg.bits.noglitch_mux = 1;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			reg.bits.gate0 = 0;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		} else {
			reg.bits.div0 = div;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		}
	}

	regmap_read(clk->map, noglitch->reg_offset, &reg.val);

	return 0;
}

static int aml_clk_noglitch_set_rate_and_parent(struct clk_hw *hw,
					       unsigned long rate,
					       unsigned long parent_rate,
					       u8 index)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_noglitch_data *noglitch = clk->data;
	unsigned int parent_index = clk_mux_index_to_val(noglitch->table, 0,
							 index);
	union aml_clk_noglitch_reg reg;
	int ret, div;

	div = divider_get_val(rate, parent_rate, NULL,
			      CLK_NOGLITCH_DIV_WIDTH, 0);
	if (div < 0)
		return div;

	ret = regmap_read(clk->map, noglitch->reg_offset, &reg.val);
	if (ret)
		return ret;

	if (reg.bits.noglitch_mux) {  /* mux1 */
		/*
		 * When the no-glitch output is enabled, the clock must be
		 * configured with the following timing sequence to suppress
		 * glitches:
		 *
		 * step1: Configure and enable Channel 0 clock.
		 * step2: Switch the no-glitch mux to Channel 0.
		 * step3: Disable Channel 1 clock.
		 */
		if (reg.bits.gate1) {
			/* step1: Configure and enable Channel 0 clock */
			reg.bits.mux0 = parent_index;
			reg.bits.div0 = div;
			reg.bits.gate0 = 1;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			/* step2: Switch the no-glitch mux to Channel 0 */
			reg.bits.noglitch_mux = 0;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			/* step3: Disable Channel 1 clock */
			reg.bits.gate1 = 0;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		} else {
			/*
			 * When no-glitch is disabled, no glitch will occur,
			 * allowing direct clock source switching.
			 */
			reg.bits.mux1 = parent_index;
			reg.bits.div1 = div;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		}
	} else {  /* mux0 */
		if (reg.bits.gate0) {
			reg.bits.mux1 = parent_index;
			reg.bits.div1 = div;
			reg.bits.gate1 = 1;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			reg.bits.noglitch_mux = 1;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;

			reg.bits.gate0 = 0;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		} else {
			reg.bits.mux0 = parent_index;
			reg.bits.div0 = div;
			ret = regmap_write(clk->map, noglitch->reg_offset,
					   reg.val);
			if (ret)
				return ret;
		}
	}

	regmap_read(clk->map, noglitch->reg_offset, &reg.val);

	return 0;
}

static int aml_clk_noglitch_is_enabled(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_noglitch_data *noglitch = clk->data;
	union aml_clk_noglitch_reg reg;
	int ret;

	ret = regmap_read(clk->map, noglitch->reg_offset, &reg.val);
	if (ret)
		return ret;

	if (reg.bits.noglitch_mux)  /* mux1 */
		return reg.bits.gate1;
	else
		return reg.bits.gate0;
}

static int aml_clk_noglitch_enable(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_noglitch_data *noglitch = clk->data;
	union aml_clk_noglitch_reg reg;
	int ret;

	ret = regmap_read(clk->map, noglitch->reg_offset, &reg.val);
	if (ret)
		return ret;

	if (reg.bits.noglitch_mux)  /* mux1 */
		reg.bits.gate1 = 1;
	else
		reg.bits.gate0 = 1;

	return regmap_write(clk->map, noglitch->reg_offset, reg.val);
}

static void aml_clk_noglitch_disable(struct clk_hw *hw)
{
	struct aml_clk *clk = to_aml_clk(hw);
	struct aml_clk_noglitch_data *noglitch = clk->data;
	union aml_clk_noglitch_reg reg;

	if (regmap_read(clk->map, noglitch->reg_offset, &reg.val))
		return;

	if (reg.bits.noglitch_mux)  /* mux1 */
		reg.bits.gate1 = 0;
	else
		reg.bits.gate0 = 0;

	regmap_write(clk->map, noglitch->reg_offset, reg.val);
}

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static void
aml_clk_noglitch_debug_init(struct clk_hw *hw, struct dentry *dentry)
{
	debugfs_create_file("clk_type", 0444, dentry, hw, &aml_clk_type_fops);
	debugfs_create_file("clk_available_rates", 0444, dentry, hw,
			    &aml_clk_div_available_rates_fops);
}
#endif /* CONFIG_DEBUG_FS */

const struct clk_ops aml_clk_noglitch_ops = {
	.get_parent = aml_clk_noglitch_get_parent,
	.set_parent = aml_clk_noglitch_set_parent,
	.determine_rate = aml_clk_noglitch_determine_rate,
	.recalc_rate = aml_clk_noglitch_recalc_rate,
	.set_rate = aml_clk_noglitch_set_rate,
	.set_rate_and_parent = aml_clk_noglitch_set_rate_and_parent,
	.enable = aml_clk_noglitch_enable,
	.disable = aml_clk_noglitch_disable,
	.is_enabled = aml_clk_noglitch_is_enabled,
#ifdef CONFIG_DEBUG_FS
	.debug_init = aml_clk_noglitch_debug_init,
#endif /* CONFIG_DEBUG_FS */
};
EXPORT_SYMBOL_NS_GPL(aml_clk_noglitch_ops, "CLK_AMLOGIC");

MODULE_DESCRIPTION("Amlogic Noglitch Clock Driver");
MODULE_AUTHOR("Chuan Liu <chuan.liu@amlogic.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_AMLOGIC");
