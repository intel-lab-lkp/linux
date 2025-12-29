// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025, Beijing ESWIN Computing Technology Co., Ltd..
 * All rights reserved.
 *
 * Authors:
 *	Yifeng Huang <huangyifeng@eswincomputing.com>
 *	Xuyang Dong <dongxuyang@eswincomputing.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/slab.h>

#include "clk.h"

struct eswin_clock_data *eswin_clk_init(struct device *dev, int nr_clks)
{
	struct eswin_clock_data *eclk_data;

	eclk_data = devm_kzalloc(dev, struct_size(eclk_data, clk_data.hws,
						  nr_clks), GFP_KERNEL);
	if (!eclk_data)
		return NULL;

	eclk_data->base = devm_of_iomap(dev, dev->of_node, 0, NULL);
	if (IS_ERR(eclk_data->base)) {
		dev_err(dev, "failed to map clock registers\n");
		return NULL;
	}

	eclk_data->clk_data.num = nr_clks;
	/* Avoid returning NULL for unused id */
	memset_p((void **)eclk_data->clk_data.hws, ERR_PTR(-ENOENT), nr_clks);
	spin_lock_init(&eclk_data->lock);

	return eclk_data;
}

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
	u64 rem = 0;
	u32 tmp1 = 0, tmp2 = 0;

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
	*frac_val = (u64)((1000 * (1000 * rem + tmp2) + tmp1) << 24) / 24
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
	u32 postdiv1_val = 0, refdiv_val = 1;
	u32 frac_val, fbdiv_val, val;
	bool lock_flag = false;
	int try_count = 0;

	eswin_calc_pll(&frac_val,  &fbdiv_val, (u64)rate);

	/* First, disable pll */
	val = readl_relaxed(clk->ctrl_reg0);
	val &= ~(((1 << clk->pllen_width) - 1) << clk->pllen_shift);
	val |= 0 << clk->pllen_shift;
	writel_relaxed(val, clk->ctrl_reg0);

	val = readl_relaxed(clk->ctrl_reg0);
	val &= ~(((1 << clk->fbdiv_width) - 1) << clk->fbdiv_shift);
	val &= ~(((1 << clk->refdiv_width) - 1) << clk->refdiv_shift);
	val |= refdiv_val << clk->refdiv_shift;
	val |= fbdiv_val << clk->fbdiv_shift;
	writel_relaxed(val, clk->ctrl_reg0);

	val = readl_relaxed(clk->ctrl_reg1);
	val &= ~(((1 << clk->frac_width) - 1) << clk->frac_shift);
	val |= frac_val << clk->frac_shift;
	writel_relaxed(val, clk->ctrl_reg1);

	val = readl_relaxed(clk->ctrl_reg2);
	val &= ~(((1 << clk->postdiv1_width) - 1) << clk->postdiv1_shift);
	val |= postdiv1_val << clk->postdiv1_shift;
	writel_relaxed(val, clk->ctrl_reg2);

	/* Last, enable pll */
	val = readl_relaxed(clk->ctrl_reg0);
	val &= ~(((1 << clk->pllen_width) - 1) << clk->pllen_shift);
	val |= 1 << clk->pllen_shift;
	writel_relaxed(val, clk->ctrl_reg0);

	/* Usually the pll will lock in 50us */
	do {
		usleep_range(refdiv_val * 80, refdiv_val * 80 * 2);
		val = readl_relaxed(clk->status_reg);
		if (val & 1 << clk->lock_shift) {
			lock_flag = true;
			break;
		}
	} while (try_count++ < 10);

	if (!lock_flag) {
		pr_err("failed to lock the cpu pll!\n");
		return -EBUSY;
	}

	return 0;
}

static unsigned long clk_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct eswin_clk_pll *clk = to_pll_clk(hw);
	u64 fbdiv_val, frac_val, rate, rem, tmp;
	u32 val;

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
		rate = (u64)(6000 * (1000 * fbdiv_val + tmp) +
			    ((6000 * rem) >> 24) + 1);
	else
		rate = (u64)(6000 * 1000 * fbdiv_val);

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

int eswin_clk_register_fixed_rate(const struct eswin_fixed_rate_clock *clks,
				  int nums, struct eswin_clock_data *data,
				  struct device *dev)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = devm_clk_hw_register_fixed_rate(dev, clks[i].name,
							 clks[i].parent_name,
							 clks[i].flags,
							 clks[i].rate);
		if (IS_ERR(clk_hw))
			return dev_err_probe(dev, PTR_ERR(clk_hw),
					     "failed to register clock\n");

		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}

static const struct clk_ops eswin_clk_pll_ops = {
	.set_rate = clk_pll_set_rate,
	.recalc_rate = clk_pll_recalc_rate,
	.determine_rate = clk_pll_determine_rate,
};

int eswin_clk_register_pll(const struct eswin_pll_clock *clks, int nums,
			   struct eswin_clock_data *data, struct device *dev)
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
		init.parent_names = clks[i].parent_name ?
					&clks[i].parent_name : NULL;
		init.num_parents = clks[i].parent_name ? 1 : 0;
		init.ops = &eswin_clk_pll_ops;
		p_clk->hw.init = &init;

		clk_hw = &p_clk->hw;
		ret = devm_clk_hw_register(dev, clk_hw);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register clock\n");

		data->clk_data.hws[clks[i].id] = clk_hw;
		p_clk++;
	}

	return 0;
}

int eswin_clk_register_fixed_factor(const struct eswin_fixed_factor_clock *clks,
				    int nums, struct eswin_clock_data *data,
				    struct device *dev)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = devm_clk_hw_register_fixed_factor(dev, clks[i].name,
							   clks[i].parent_name,
							   clks[i].flags,
							   clks[i].mult,
							   clks[i].div);
		if (IS_ERR(clk_hw))
			return dev_err_probe(dev, PTR_ERR(clk_hw),
					     "failed to register clock\n");

		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}

int eswin_clk_register_mux(const struct eswin_mux_clock *clks, int nums,
			   struct eswin_clock_data *data, struct device *dev)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = devm_clk_hw_register_mux(dev, clks[i].name,
						  clks[i].parent_names,
						  clks[i].num_parents,
						  clks[i].flags,
						  data->base + clks[i].offset,
						  clks[i].shift,
						  clks[i].width,
						  clks[i].mux_flags,
						  &data->lock);
		if (IS_ERR(clk_hw))
			return dev_err_probe(dev, PTR_ERR(clk_hw),
					     "failed to register clock\n");

		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}

int eswin_clk_register_mux_tbl(const struct eswin_mux_clock *clks,
			       int nums, struct eswin_clock_data *data,
			       struct device *dev)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = clk_hw_register_mux_table(dev, clks[i].name,
						   clks[i].parent_names,
						   clks[i].num_parents,
						   clks[i].flags,
						   data->base + clks[i].offset,
						   clks[i].shift,
						   BIT(clks[i].width) - 1,
						   clks[i].mux_flags,
						   clks[i].table, &data->lock);

		if (IS_ERR(clk_hw)) {
			while (i--)
				clk_hw_unregister_mux
					(data->clk_data.hws[clks[i].id]);
			return dev_err_probe(dev, PTR_ERR(clk_hw),
					     "failed to register clock\n");
		}

		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}

int eswin_clk_register_divider(const struct eswin_divider_clock *clks,
			       int nums, struct eswin_clock_data *data,
			       struct device *dev)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = devm_clk_hw_register_divider(dev, clks[i].name,
						      clks[i].parent_name,
						      clks[i].flags,
						      data->base +
							clks[i].offset,
						      clks[i].shift,
						      clks[i].width,
						      clks[i].div_flags,
						      &data->lock);
		if (IS_ERR(clk_hw))
			return dev_err_probe(dev, PTR_ERR(clk_hw),
					     "failed to register clock\n");

		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}

int eswin_clk_register_gate(const struct eswin_gate_clock *clks, int nums,
			    struct eswin_clock_data *data, struct device *dev)
{
	struct clk_hw *clk_hw;
	int i;

	for (i = 0; i < nums; i++) {
		clk_hw = devm_clk_hw_register_gate(dev, clks[i].name,
						   clks[i].parent_name,
						   clks[i].flags,
						   data->base + clks[i].offset,
						   clks[i].bit_idx,
						   clks[i].gate_flags,
						   &data->lock);

		if (IS_ERR(clk_hw))
			return dev_err_probe(dev, PTR_ERR(clk_hw),
					     "failed to register clock\n");

		data->clk_data.hws[clks[i].id] = clk_hw;
	}

	return 0;
}
