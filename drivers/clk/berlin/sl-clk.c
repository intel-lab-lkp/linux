// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Synaptics Incorporated
 * Copyright (c) 2015 Marvell Technology Group Ltd.
 *
 * Author: Jisheng Zhang <jszhang@kernel.org>
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "sl-clk.h"

#define SL_CLK_EN		BIT(0)
#define SL_CLK_PLL_SEL		GENMASK(3, 1)
#define SL_CLK_PLL_SWITCH	BIT(4)
#define SL_CLK_DIV_SWITCH	BIT(5)
#define SL_CLK_DIV3_SWITCH	BIT(6)
#define SL_CLK_DIV_SEL		GENMASK(9, 7)
#define SL_MAX_PARENTS	6

struct sl_clk {
	struct clk_hw hw;
	void __iomem *reg;
	spinlock_t *lock;
	struct device *dev;
};

#define to_sl_clk(_hw) container_of(_hw, struct sl_clk, hw)

static const unsigned int sl_clk_values[] = { 1, 2, 3, 4, 6, 8, 12 };

static unsigned int sl_clk_value(u32 val)
{
	static const u8 div_table[] = { 0, 2, 4, 6, 8, 12, 0, 0 };

	if (val & SL_CLK_DIV3_SWITCH)
		return 3;
	if (!(val & SL_CLK_DIV_SWITCH))
		return 1;

	return div_table[FIELD_GET(SL_CLK_DIV_SEL, val)];
}

static unsigned long sl_clk_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct sl_clk *clk = to_sl_clk(hw);
	unsigned int divisor = sl_clk_value(readl_relaxed(clk->reg));

	if (WARN_ON_ONCE(!divisor))
		return 0;

	return parent_rate / divisor;
}

static unsigned int sl_clk_best(unsigned long parent_rate,
				unsigned long rate)
{
	unsigned long best_diff = ULONG_MAX;
	unsigned int best = 1, i;

	for (i = 0; i < ARRAY_SIZE(sl_clk_values); i++) {
		unsigned long candidate = parent_rate / sl_clk_values[i];
		unsigned long diff = candidate > rate ? candidate - rate :
							     rate - candidate;

		if (diff < best_diff) {
			best_diff = diff;
			best = sl_clk_values[i];
		}
	}

	return best;
}

static int sl_clk_determine_rate(struct clk_hw *hw,
				 struct clk_rate_request *req)
{
	struct clk_hw *parent = clk_hw_get_parent(hw);

	if (!parent)
		return -EINVAL;

	req->best_parent_hw = parent;
	req->best_parent_rate = clk_hw_get_rate(parent);
	req->rate = req->best_parent_rate /
		    sl_clk_best(req->best_parent_rate, req->rate);

	return 0;
}

static int sl_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long parent_rate)
{
	struct sl_clk *clk = to_sl_clk(hw);
	unsigned int divisor, select;
	unsigned long flags;
	u32 val;

	divisor = sl_clk_best(parent_rate, rate);
	if (parent_rate / divisor != rate)
		return -EINVAL;

	spin_lock_irqsave(clk->lock, flags);
	val = readl_relaxed(clk->reg);
	val &= ~(SL_CLK_DIV3_SWITCH | SL_CLK_DIV_SWITCH |
		 SL_CLK_DIV_SEL);
	if (divisor == 3) {
		val |= SL_CLK_DIV3_SWITCH;
	} else if (divisor != 1) {
		switch (divisor) {
		case 2:
			select = 1;
			break;
		case 4:
			select = 2;
			break;
		case 6:
			select = 3;
			break;
		case 8:
			select = 4;
			break;
		case 12:
			select = 5;
			break;
		default:
			spin_unlock_irqrestore(clk->lock, flags);
			return -EINVAL;
		}
		val |= SL_CLK_DIV_SWITCH |
			FIELD_PREP(SL_CLK_DIV_SEL, select);
	}
	writel_relaxed(val, clk->reg);
	spin_unlock_irqrestore(clk->lock, flags);

	return 0;
}

static u8 sl_clk_get_parent(struct clk_hw *hw)
{
	struct sl_clk *clk = to_sl_clk(hw);
	u32 val = readl_relaxed(clk->reg);
	u8 selector;
	unsigned int index;

	if (!(val & SL_CLK_PLL_SWITCH))
		return 0;

	selector = FIELD_GET(SL_CLK_PLL_SEL, val);
	index = selector + 1;
	if (index < clk_hw_get_num_parents(hw))
		return index;

	dev_warn(clk->dev, "%s: unsupported parent selector %u\n",
		 clk_hw_get_name(hw), selector);
	return 0;
}

static int sl_clk_set_parent(struct clk_hw *hw, u8 index)
{
	struct sl_clk *clk = to_sl_clk(hw);
	unsigned long flags;
	u32 val;

	if (index >= clk_hw_get_num_parents(hw) || index >= SL_MAX_PARENTS)
		return -EINVAL;

	spin_lock_irqsave(clk->lock, flags);
	val = readl_relaxed(clk->reg);
	if (!index) {
		val &= ~SL_CLK_PLL_SWITCH;
	} else {
		val &= ~SL_CLK_PLL_SEL;
		val |= SL_CLK_PLL_SWITCH |
			FIELD_PREP(SL_CLK_PLL_SEL, index - 1);
	}
	writel_relaxed(val, clk->reg);
	spin_unlock_irqrestore(clk->lock, flags);

	return 0;
}

static int sl_clk_enable(struct clk_hw *hw)
{
	struct sl_clk *clk = to_sl_clk(hw);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(clk->lock, flags);
	val = readl_relaxed(clk->reg);
	writel_relaxed(val | SL_CLK_EN, clk->reg);
	spin_unlock_irqrestore(clk->lock, flags);

	return 0;
}

static void sl_clk_disable(struct clk_hw *hw)
{
	struct sl_clk *clk = to_sl_clk(hw);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(clk->lock, flags);
	val = readl_relaxed(clk->reg);
	writel_relaxed(val & ~SL_CLK_EN, clk->reg);
	spin_unlock_irqrestore(clk->lock, flags);
}

static int sl_clk_is_enabled(struct clk_hw *hw)
{
	struct sl_clk *clk = to_sl_clk(hw);

	return !!(readl_relaxed(clk->reg) & SL_CLK_EN);
}

static const struct clk_ops sl_clk_ops = {
	.get_parent = sl_clk_get_parent,
	.set_parent = sl_clk_set_parent,
	.recalc_rate = sl_clk_recalc_rate,
	.determine_rate = sl_clk_determine_rate,
	.set_rate = sl_clk_set_rate,
	.enable = sl_clk_enable,
	.disable = sl_clk_disable,
	.is_enabled = sl_clk_is_enabled,
};

struct clk_hw *sl_clk_register(struct device *dev, void __iomem *reg,
			       const char *name, const char * const *parent_names,
			       unsigned int num_parents, unsigned long flags,
			       spinlock_t *lock)
{
	struct clk_init_data init = {};
	struct sl_clk *clk;
	unsigned int i;
	int ret;

	if (!dev || !reg || !name || !parent_names || !num_parents ||
	    num_parents > SL_MAX_PARENTS || !lock)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < num_parents; i++)
		if (!parent_names[i])
			return ERR_PTR(-EINVAL);

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	clk->dev = dev;
	clk->reg = reg;
	clk->lock = lock;

	init.name = name;
	init.ops = &sl_clk_ops;
	init.parent_names = parent_names;
	init.num_parents = num_parents;
	init.flags = flags;
	clk->hw.init = &init;

	ret = devm_clk_hw_register(dev, &clk->hw);
	if (ret)
		return ERR_PTR(ret);

	return &clk->hw;
}
EXPORT_SYMBOL_GPL(sl_clk_register);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synaptics SL261x clock divider support");
