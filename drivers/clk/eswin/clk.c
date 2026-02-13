// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026, Beijing ESWIN Computing Technology Co., Ltd..
 * All rights reserved.
 *
 * Authors:
 *	Yifeng Huang <huangyifeng@eswincomputing.com>
 *	Xuyang Dong <dongxuyang@eswincomputing.com>
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/math.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "common.h"

struct eswin_clock_data *eswin_clk_init(struct platform_device *pdev,
					size_t nr_clks)
{
	struct eswin_clock_data *eclk_data;

	eclk_data = devm_kzalloc(&pdev->dev,
				 struct_size(eclk_data, clk_data.hws, nr_clks),
				 GFP_KERNEL);
	if (!eclk_data)
		return NULL;

	eclk_data->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(eclk_data->base)) {
		dev_err(&pdev->dev, "failed to map clock registers\n");
		return NULL;
	}

	eclk_data->clk_data.num = nr_clks;
	/* Avoid returning NULL for unused id */
	memset_p((void **)eclk_data->clk_data.hws, ERR_PTR(-ENOENT), nr_clks);
	spin_lock_init(&eclk_data->lock);

	return eclk_data;
}
EXPORT_SYMBOL_GPL(eswin_clk_init);

/**
 * eswin_calc_pll - calculate PLL values
 * @frac_val: fractional divider
 * @fbdiv_val: feedback divider
 * @rate: reference rate
 *
 *   Calculate PLL values for frac and fbdiv
 */
static void eswin_calc_pll(u32 *frac_val, u32 *fbdiv_val, u64 rate)
{
	u32 rem, tmp1 = 0, tmp2 = 0;

	rate = rate * 4;
	rem = do_div(rate, 1000);
	if (rem)
		tmp1 = rem;

	rem = do_div(rate, 1000);
	if (rem)
		tmp2 = rem;

	rem = do_div(rate, 24);
	/* fbdiv = rate * 4 / 24000000 */
	*fbdiv_val = rate;
	/* frac = rate * 4 % 24000000 * (2 ^ 24) */
	*frac_val = ((1000 * (1000 * rem + tmp2) + tmp1) << 24) / 24
		     / 1000000;
}

static inline struct eswin_clk_pll *to_pll_clk(struct clk_hw *hw)
{
	return container_of(hw, struct eswin_clk_pll, hw);
}

static int clk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct eswin_clk_pll *clk = to_pll_clk(hw);
	u32 frac_val, fbdiv_val, val;
	int ret;

	eswin_calc_pll(&frac_val, &fbdiv_val, (u64)rate);

	/* First, disable pll */
	val = readl_relaxed(clk->ctrl_reg0);
	val &= ~(((1 << clk->pllen_width) - 1) << clk->pllen_shift);
	val |= 0 << clk->pllen_shift;
	writel_relaxed(val, clk->ctrl_reg0);

	val = readl_relaxed(clk->ctrl_reg0);
	val &= ~(((1 << clk->fbdiv_width) - 1) << clk->fbdiv_shift);
	val &= ~(((1 << clk->refdiv_width) - 1) << clk->refdiv_shift);
	val |= 1 << clk->refdiv_shift;
	val |= fbdiv_val << clk->fbdiv_shift;
	writel_relaxed(val, clk->ctrl_reg0);

	val = readl_relaxed(clk->ctrl_reg1);
	val &= ~(((1 << clk->frac_width) - 1) << clk->frac_shift);
	val |= frac_val << clk->frac_shift;
	writel_relaxed(val, clk->ctrl_reg1);

	/* Last, enable pll */
	val = readl_relaxed(clk->ctrl_reg0);
	val &= ~(((1 << clk->pllen_width) - 1) << clk->pllen_shift);
	val |= 1 << clk->pllen_shift;
	writel_relaxed(val, clk->ctrl_reg0);

	/* Usually the pll will lock in 50us */
	ret = readl_poll_timeout(clk->status_reg, val,
				 val & (1 << clk->lock_shift), 1, 50 * 2);
	if (ret)
		pr_err("failed to lock the pll!\n");

	return ret;
}

static unsigned long clk_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct eswin_clk_pll *clk = to_pll_clk(hw);
	u32 fbdiv_val, frac_val, rem, val;
	unsigned long rate;
	u64 tmp;

	val = readl_relaxed(clk->ctrl_reg0);
	val = val >> clk->fbdiv_shift;
	val &= ((1 << clk->fbdiv_width) - 1);
	fbdiv_val = val;

	val = readl_relaxed(clk->ctrl_reg1);
	val = val >> clk->frac_shift;
	val &= ((1 << clk->frac_width) - 1);
	frac_val = val;

	/* rate = 24000000 * (fbdiv + frac / (2 ^ 24)) / 4 */
	tmp = 1000 * frac_val;
	rem = do_div(tmp, BIT(24));
	if (rem)
		rate = (unsigned long)(6000 * (1000 * fbdiv_val + tmp) +
				      ((6000 * rem) >> 24) + 1);
	else
		rate = (unsigned long)(6000 * 1000 * fbdiv_val);

	return rate;
}

static int clk_pll_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct eswin_clk_pll *clk = to_pll_clk(hw);

	req->rate = clamp(req->rate, clk->min_rate, clk->max_rate);
	req->min_rate = clk->min_rate;
	req->max_rate = clk->max_rate;

	return 0;
}

int eswin_clk_register_fixed_rate(struct device *dev,
				  struct eswin_fixed_rate_clock *clks,
				  int nums, struct eswin_clock_data *data)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = devm_clk_hw_register_fixed_rate(dev, clks[i].name,
							 NULL, clks[i].flags,
							 clks[i].rate);
		if (IS_ERR(clk_hw))
			return PTR_ERR(clk_hw);

		clks[i].hw = *clk_hw;
		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(eswin_clk_register_fixed_rate);

static const struct clk_ops eswin_clk_pll_ops = {
	.set_rate = clk_pll_set_rate,
	.recalc_rate = clk_pll_recalc_rate,
	.determine_rate = clk_pll_determine_rate,
};

int eswin_clk_register_pll(struct device *dev, struct eswin_pll_clock *clks,
			   int nums, struct eswin_clock_data *data)
{
	struct eswin_clk_pll *p_clk = NULL;
	struct clk_init_data init;
	struct clk_hw *clk_hw;
	int i, ret;

	p_clk = devm_kzalloc(dev, sizeof(*p_clk) * nums, GFP_KERNEL);
	if (!p_clk)
		return -ENOMEM;

	for (i = 0; i < nums; i++) {
		p_clk->id = clks[i].id;
		p_clk->ctrl_reg0 = data->base + clks[i].ctrl_reg0;
		p_clk->pllen_shift = clks[i].pllen_shift;
		p_clk->pllen_width = clks[i].pllen_width;
		p_clk->refdiv_shift = clks[i].refdiv_shift;
		p_clk->refdiv_width = clks[i].refdiv_width;
		p_clk->fbdiv_shift = clks[i].fbdiv_shift;
		p_clk->fbdiv_width = clks[i].fbdiv_width;

		p_clk->ctrl_reg1 = data->base + clks[i].ctrl_reg1;
		p_clk->frac_shift = clks[i].frac_shift;
		p_clk->frac_width = clks[i].frac_width;

		p_clk->ctrl_reg2 = data->base + clks[i].ctrl_reg2;
		p_clk->postdiv1_shift = clks[i].postdiv1_shift;
		p_clk->postdiv1_width = clks[i].postdiv1_width;
		p_clk->postdiv2_shift = clks[i].postdiv2_shift;
		p_clk->postdiv2_width = clks[i].postdiv2_width;

		p_clk->status_reg = data->base + clks[i].status_reg;
		p_clk->lock_shift = clks[i].lock_shift;
		p_clk->lock_width = clks[i].lock_width;

		p_clk->max_rate = clks[i].max_rate;
		p_clk->min_rate = clks[i].min_rate;

		init.name = clks[i].name;
		init.flags = 0;
		init.parent_data = clks[i].parent_data;
		init.num_parents = 1;
		init.ops = &eswin_clk_pll_ops;
		p_clk->hw.init = &init;

		clk_hw = &p_clk->hw;

		ret = devm_clk_hw_register(dev, clk_hw);
		if (ret)
			return ret;

		clks[i].hw = *clk_hw;
		data->clk_data.hws[clks[i].id] = clk_hw;
		p_clk++;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(eswin_clk_register_pll);

int eswin_clk_register_fixed_factor(struct device *dev,
				    struct eswin_fixed_factor_clock *clks,
				    int nums, struct eswin_clock_data *data)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = devm_clk_hw_register_fixed_factor_index
				(dev, clks[i].name, clks[i].parent_data->index,
				clks[i].flags, clks[i].mult, clks[i].div);

		if (IS_ERR(clk_hw))
			return PTR_ERR(clk_hw);

		clks[i].hw = *clk_hw;
		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(eswin_clk_register_fixed_factor);

int eswin_clk_register_mux(struct device *dev, struct eswin_mux_clock *clks,
			   int nums, struct eswin_clock_data *data)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = devm_clk_hw_register_mux_parent_data_table
				(dev, clks[i].name, clks[i].parent_data,
				clks[i].num_parents, clks[i].flags,
				data->base + clks[i].offset, clks[i].shift,
				clks[i].width, clks[i].mux_flags, clks[i].table,
				&data->lock);

		if (IS_ERR(clk_hw))
			return PTR_ERR(clk_hw);

		clks[i].hw = *clk_hw;
		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(eswin_clk_register_mux);

static unsigned int _eswin_get_val(unsigned int div, unsigned long flags,
				   u8 width)
{
	unsigned int maxdiv;

	maxdiv = clk_div_mask(width);
	div = div > maxdiv ? maxdiv : div;

	if (flags & ESWIN_PRIV_DIV_MIN_2)
		return (div < 2) ? 2 : div;

	return div;
}

static unsigned int eswin_div_get_val(unsigned long rate,
				      unsigned long parent_rate, u8 width,
				      unsigned long flags)
{
	unsigned int div;

	div = DIV_ROUND_UP_ULL((u64)parent_rate, rate);

	return _eswin_get_val(div, flags, width);
}

static inline struct eswin_divider_clock *to_div_clk(struct clk_hw *hw)
{
	return container_of(hw, struct eswin_divider_clock, hw);
}

static int clk_div_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct eswin_divider_clock *dclk = to_div_clk(hw);
	unsigned long flags = 0;
	unsigned int value;
	u32 val;

	value = eswin_div_get_val(rate, parent_rate, dclk->width,
				  dclk->priv_flag);

	spin_lock_irqsave(dclk->lock, flags);

	val = readl_relaxed(dclk->reg);
	val &= ~(clk_div_mask(dclk->width) << dclk->shift);
	val |= (u32)value << dclk->shift;
	writel_relaxed(val, dclk->reg);

	spin_unlock_irqrestore(dclk->lock, flags);

	return 0;
}

static unsigned long clk_div_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct eswin_divider_clock *dclk = to_div_clk(hw);
	unsigned int div, val;

	val = readl_relaxed(dclk->reg) >> dclk->shift;
	val &= clk_div_mask(dclk->width);
	div = _eswin_get_val(val, dclk->priv_flag, dclk->width);

	return DIV_ROUND_UP_ULL((u64)parent_rate, div);
}

static int eswin_clk_bestdiv(unsigned long rate,
			     unsigned long best_parent_rate, u8 width,
			     unsigned long flags)
{
	unsigned long bestdiv, up_rate, down_rate;
	int up, down;

	if (!rate)
		rate = 1;

	/* closest round */
	up = DIV_ROUND_UP_ULL((u64)best_parent_rate, rate);
	down = best_parent_rate / rate;

	up_rate = DIV_ROUND_UP_ULL((u64)best_parent_rate, up);
	down_rate = DIV_ROUND_UP_ULL((u64)best_parent_rate, down);

	bestdiv = (rate - up_rate) <= (down_rate - rate) ? up : down;

	return bestdiv;
}

static int clk_div_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct eswin_divider_clock *dclk = to_div_clk(hw);
	int div;

	div = eswin_clk_bestdiv(req->rate, req->best_parent_rate, dclk->width,
				dclk->priv_flag);
	div = _eswin_get_val(div, dclk->priv_flag, dclk->width);
	req->rate = DIV_ROUND_UP_ULL((u64)req->best_parent_rate, div);

	return 0;
}

static const struct clk_ops eswin_clk_div_ops = {
	.set_rate = clk_div_set_rate,
	.recalc_rate = clk_div_recalc_rate,
	.determine_rate = clk_div_determine_rate,
};

struct clk_hw *eswin_register_clkdiv(struct device *dev, unsigned int id,
				     const char *name,
				     const struct clk_hw *parent_hw,
				     unsigned long flags, void __iomem *reg,
				     u8 shift, u8 width,
				     unsigned long clk_divider_flags,
				     unsigned long priv_flag, spinlock_t *lock)
{
	struct eswin_divider_clock *dclk;
	struct clk_init_data init;
	struct clk_hw *clk_hw;
	int ret;

	dclk = devm_kzalloc(dev, sizeof(*dclk), GFP_KERNEL);
	if (!dclk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &eswin_clk_div_ops;
	init.flags = flags;
	init.parent_hws = &parent_hw;
	init.num_parents = 1;

	/* struct clk_divider assignments */
	dclk->id = id;
	dclk->reg = reg;
	dclk->shift = shift;
	dclk->width = width;
	dclk->div_flags = clk_divider_flags;
	dclk->priv_flag = priv_flag;
	dclk->lock = lock;
	dclk->hw.init = &init;

	/* register the clock */
	clk_hw = &dclk->hw;
	ret = devm_clk_hw_register(dev, clk_hw);
	if (ret) {
		dev_err(dev, "failed to register divider clock!\n");
		return ERR_PTR(ret);
	}

	return clk_hw;
}
EXPORT_SYMBOL_GPL(eswin_register_clkdiv);

int eswin_clk_register_divider(struct device *dev,
			       struct eswin_divider_clock *clks,
			       int nums, struct eswin_clock_data *data)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = clk_hw_register_divider_parent_data
				(dev, clks[i].name, clks[i].parent_data,
				clks[i].flags, data->base + clks[i].offset,
				clks[i].shift, clks[i].width, clks[i].div_flags,
				&data->lock);

		if (IS_ERR(clk_hw)) {
			while (i--)
				clk_hw_unregister_divider
					(data->clk_data.hws[clks[i].id]);
			return PTR_ERR(clk_hw);
		}

		clks[i].hw = *clk_hw;
		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(eswin_clk_register_divider);

int eswin_clk_register_gate(struct device *dev, struct eswin_gate_clock *clks,
			    int nums, struct eswin_clock_data *data)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = devm_clk_hw_register_gate_parent_data
				(dev, clks[i].name, clks[i].parent_data,
				clks[i].flags, data->base + clks[i].offset,
				clks[i].bit_idx, clks[i].gate_flags,
				&data->lock);

		if (IS_ERR(clk_hw))
			return PTR_ERR(clk_hw);

		clks[i].hw = *clk_hw;
		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(eswin_clk_register_gate);

int eswin_clk_register_clks(struct device *dev, struct eswin_clk_info *clks,
			    int nums, struct eswin_clock_data *data)
{
	struct eswin_clk_info *info;
	const struct clk_hw *phw = NULL;
	struct clk_hw *hw;
	int i;

	for (i = 0; i < nums; i++) {
		info = &clks[i];
		switch (info->type) {
		case CLK_FIXED_FACTOR: {
			const struct eswin_fixed_factor_clock *factor;

			factor = &info->data.factor;
			phw = data->clk_data.hws[info->pid];
			hw = devm_clk_hw_register_fixed_factor_parent_hw
					(dev, factor->name, phw, factor->flags,
					factor->mult, factor->div);
			break;
		}
		case CLK_MUX: {
			const struct eswin_mux_clock *mux = &info->data.mux;

			hw = devm_clk_hw_register_mux_parent_data_table
				(dev, mux->name, mux->parent_data,
				mux->num_parents, mux->flags,
				data->base + mux->offset, mux->shift,
				mux->width, mux->mux_flags, mux->table,
				&data->lock);
			break;
		}
		case CLK_DIVIDER: {
			const struct eswin_divider_clock *div = &info->data.div;

			phw = data->clk_data.hws[info->pid];
			if (div->priv_flag)
				hw = eswin_register_clkdiv
				(dev, div->id, div->name, phw,
				div->flags, data->base + div->offset,
				div->shift, div->width, div->div_flags,
				div->priv_flag, &data->lock);
			else
				hw = devm_clk_hw_register_divider_parent_hw
				(dev, div->name, phw, div->flags,
				data->base + div->offset, div->shift,
				div->width, div->div_flags, &data->lock);
			break;
		}
		case CLK_GATE: {
			const struct eswin_gate_clock *gate = &info->data.gate;

			phw = data->clk_data.hws[info->pid];
			hw = devm_clk_hw_register_gate_parent_hw
				(dev, gate->name, phw, gate->flags,
				data->base + gate->offset, gate->bit_idx,
				gate->gate_flags, &data->lock);
			break;
		}
		default:
			dev_err(dev, "Unidentifiable clock type!\n");
			return -EINVAL;
		}
		if (IS_ERR(hw))
			return PTR_ERR(hw);

		info->hw = *hw;
		data->clk_data.hws[info->id] = hw;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(eswin_clk_register_clks);
