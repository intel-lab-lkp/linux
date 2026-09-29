// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019-2021 Synaptics Incorporated
 *
 * Author: Jisheng Zhang <jszhang@kernel.org>
 */

#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "sl261x-pll.h"

#define FRAC_BITS		24

#define CTRLC			0x08
#define DIVR_MASK		GENMASK(8, 0)
#define CTRLD			0x0c
#define DIVF_MASK		GENMASK(8, 0)
#define CTRLE			0x10
#define DIVFF_MASK		GENMASK(23, 0)
#define CTRLF			0x14
#define DIVQ_MASK		GENMASK(4, 0)
#define CTRLG			0x18
#define DIVQF_MASK		GENMASK(2, 0)

#define SL261X_APLL
struct sl261x_pll {
	void __iomem *ctrl;
	struct clk_hw pllout;
	struct clk_hw plloutf;
};

#define to_pllout(_hw) container_of(_hw, struct sl261x_pll, pllout)
#define to_plloutf(_hw) container_of(_hw, struct sl261x_pll, plloutf)

static unsigned long sl261x_pll_recalc_rate(struct sl261x_pll *pll,
					    unsigned long parent_rate, bool fractional_output)
{
	u32 dm, dn, frac, dp;
	u64 rate;

	dm = (readl_relaxed(pll->ctrl + CTRLC) & DIVR_MASK) + 1;
	dn = (readl_relaxed(pll->ctrl + CTRLD) & DIVF_MASK) + 1;
	frac = readl_relaxed(pll->ctrl + CTRLE) & DIVFF_MASK;

	if (fractional_output)
		dp = (readl_relaxed(pll->ctrl + CTRLG) & DIVQF_MASK) + 1;
	else
		dp = ((readl_relaxed(pll->ctrl + CTRLF) & DIVQ_MASK) + 1) * 2;

	rate = (u64)parent_rate * dn;
	rate += ((u64)parent_rate * frac + BIT_ULL(FRAC_BITS - 1)) >> FRAC_BITS;
	rate = DIV_ROUND_CLOSEST_ULL(rate * 4, dm * dp);

	return rate;
}

static unsigned long sl261x_pllout_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	return sl261x_pll_recalc_rate(to_pllout(hw), parent_rate, false);
}

static unsigned long sl261x_plloutf_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	return sl261x_pll_recalc_rate(to_plloutf(hw), parent_rate, true);
}

static const struct clk_ops sl261x_pllout_ops = {
	.recalc_rate = sl261x_pllout_recalc_rate,
};

static const struct clk_ops sl261x_plloutf_ops = {
	.recalc_rate = sl261x_plloutf_recalc_rate,
};

static void sl261x_pll_put_clk(void *data)
{
	clk_put(data);
}

int sl261x_pll_register(struct device *dev, void __iomem *ctrl,
			const char *out, const char *outf)
{
	struct device_node *np = dev->of_node;
	struct clk_parent_data parent = {};
	struct clk_init_data init = {};
	struct sl261x_pll *pll;
	struct clk *input;
	int ret;

	if (!dev || !ctrl || !out || !outf)
		return -EINVAL;

	input = of_clk_get(np, 0);
	if (IS_ERR(input))
		return dev_err_probe(dev, PTR_ERR(input),
				     "failed to get PLL reference clock\n");
	parent.hw = __clk_get_hw(input);
	ret = devm_add_action_or_reset(dev, sl261x_pll_put_clk, input);
	if (ret)
		return ret;

	pll = devm_kzalloc(dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return -ENOMEM;
	pll->ctrl = ctrl;

	init.name = out;
	init.ops = &sl261x_pllout_ops;
	init.parent_data = &parent;
	init.num_parents = 1;
	init.flags = CLK_GET_RATE_NOCACHE;
	pll->pllout.init = &init;

	ret = devm_clk_hw_register(dev, &pll->pllout);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register pllout\n");

	init.name = outf;
	init.ops = &sl261x_plloutf_ops;
	pll->plloutf.init = &init;

	ret = devm_clk_hw_register(dev, &pll->plloutf);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register plloutf\n");

	return 0;
}
EXPORT_SYMBOL_GPL(sl261x_pll_register);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synaptics SL261x PLL clock driver");
