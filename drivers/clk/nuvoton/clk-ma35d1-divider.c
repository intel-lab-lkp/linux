// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Nuvoton Technology Corp.
 * Author: Chi-Fang Li <cfli0@nuvoton.com>
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

#include "clk-ma35d1.h"

struct ma35d1_adc_clk_div {
	struct clk_hw hw;
	void __iomem *reg;
	u8 shift;
	u8 width;
	/* protects concurrent access to clock divider registers */
	spinlock_t *lock;
};

static inline struct ma35d1_adc_clk_div *to_ma35d1_adc_clk_div(struct clk_hw *_hw)
{
	return container_of(_hw, struct ma35d1_adc_clk_div, hw);
}

static unsigned long ma35d1_clkdiv_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct ma35d1_adc_clk_div *dclk = to_ma35d1_adc_clk_div(hw);
	unsigned int val;

	val = readl_relaxed(dclk->reg) >> dclk->shift;
	val &= clk_div_mask(dclk->width);

	return DIV_ROUND_CLOSEST_ULL((u64)parent_rate, 2 * (val + 1));
}

static int ma35d1_clkdiv_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	struct ma35d1_adc_clk_div *dclk = to_ma35d1_adc_clk_div(hw);
	unsigned int val;

	if (!req->rate)
		return -EINVAL;

	val = DIV_ROUND_UP(req->best_parent_rate, 2 * req->rate);
	if (val == 0)
		val = 1;
	if (val > (unsigned int)clk_div_mask(dclk->width) + 1)
		val = clk_div_mask(dclk->width) + 1;

	req->rate = DIV_ROUND_CLOSEST_ULL((u64)req->best_parent_rate, 2 * val);

	return 0;
}

static int ma35d1_clkdiv_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	struct ma35d1_adc_clk_div *dclk = to_ma35d1_adc_clk_div(hw);
	unsigned long flags;
	unsigned int val;
	u32 data;

	if (!rate)
		return -EINVAL;

	val = DIV_ROUND_UP(parent_rate, 2 * rate);
	if (val == 0)
		val = 1;
	if (val > (unsigned int)clk_div_mask(dclk->width) + 1)
		val = clk_div_mask(dclk->width) + 1;

	spin_lock_irqsave(dclk->lock, flags);

	data = readl_relaxed(dclk->reg);
	data &= ~(clk_div_mask(dclk->width) << dclk->shift);
	data |= (val - 1) << dclk->shift;
	writel_relaxed(data, dclk->reg);

	spin_unlock_irqrestore(dclk->lock, flags);

	return 0;
}

static const struct clk_ops ma35d1_adc_clkdiv_ops = {
	.recalc_rate = ma35d1_clkdiv_recalc_rate,
	.determine_rate = ma35d1_clkdiv_determine_rate,
	.set_rate = ma35d1_clkdiv_set_rate,
};

struct clk_hw *ma35d1_reg_adc_clkdiv(struct device *dev, const char *name,
				     struct clk_hw *parent_hw, spinlock_t *lock,
				     unsigned long flags, void __iomem *reg,
				     u8 shift, u8 width)
{
	struct clk_parent_data pdata = { .index = 0 };
	struct ma35d1_adc_clk_div *div;
	struct clk_init_data init = {};
	struct clk_hw *hw;
	int ret;

	div = devm_kzalloc(dev, sizeof(*div), GFP_KERNEL);
	if (!div)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &ma35d1_adc_clkdiv_ops;
	init.flags = flags;
	pdata.hw = parent_hw;
	init.parent_data = &pdata;
	init.num_parents = 1;

	div->reg = reg;
	div->shift = shift;
	div->width = width;
	div->lock = lock;
	div->hw.init = &init;

	hw = &div->hw;
	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ERR_PTR(ret);

	return hw;
}
EXPORT_SYMBOL_GPL(ma35d1_reg_adc_clkdiv);
