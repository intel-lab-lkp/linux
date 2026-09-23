// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015 Endless Mobile, Inc.
 * Author: Carlo Caione <carlo@endlessm.com>
 *
 * Copyright (c) 2018 Baylibre, SAS.
 * Author: Jerome Brunet <jbrunet@baylibre.com>
 */

/*
 * In the most basic form, a Meson PLL is composed as follows:
 *
 *                     PLL
 *        +--------------------------------+
 *        |                                |
 *        |             +--+               |
 *  in >>----------->|  |      +-----+  |
 *        |             |  |------| DCO |---->> out
 *        |  +--------->|  |      +--v--+  |
 *        |  |          +--+         |     |
 *        |  |                       |     |
 *        |  +--[ *(M + (F/Fmax) ]<--+     |
 *        |                                |
 *        +--------------------------------+
 *
 * out = in * (m + frac / frac_max)
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>

#include "clk-regmap.h"
#include "clk-pll.h"

static inline struct meson_clk_pll_data *
meson_clk_pll_data(struct clk_regmap *clk)
{
	return (struct meson_clk_pll_data *)clk->data;
}

static int __pll_round_closest_mult(struct meson_clk_pll_data *pll)
{
	if ((pll->flags & CLK_MESON_PLL_ROUND_CLOSEST) &&
	    !MESON_PARM_APPLICABLE(&pll->frac))
		return 1;

	return 0;
}

static unsigned long __pll_params_to_rate(unsigned long parent_rate,
					  unsigned int m, unsigned int frac,
					  struct meson_clk_pll_data *pll)
{
	u64 rate = (u64)parent_rate * m;
	unsigned int frac_max = pll->frac_max ? pll->frac_max :
						(1 << pll->frac.width);

	if (frac && MESON_PARM_APPLICABLE(&pll->frac)) {
		u64 frac_rate = (u64)parent_rate * frac;

		rate += DIV_ROUND_UP_ULL(frac_rate, frac_max);
	}

	return rate;
}

static unsigned long meson_clk_pll_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_pll_data *pll = meson_clk_pll_data(clk);
	unsigned int m, frac;

	m = meson_parm_read(clk->map, &pll->m);

	frac = MESON_PARM_APPLICABLE(&pll->frac) ?
		meson_parm_read(clk->map, &pll->frac) :
		0;

	return __pll_params_to_rate(parent_rate, m, frac, pll);
}

static unsigned int __pll_params_with_frac(unsigned long rate,
					   unsigned long parent_rate,
					   unsigned int m,
					   struct meson_clk_pll_data *pll)
{
	unsigned int frac_max = pll->frac_max ? pll->frac_max :
						(1 << pll->frac.width);
	u64 val = (u64)rate;

	/* Bail out if we are already over the requested rate */
	if (rate < parent_rate * m)
		return 0;

	if (pll->flags & CLK_MESON_PLL_ROUND_CLOSEST)
		val = DIV_ROUND_CLOSEST_ULL(val * frac_max, parent_rate);
	else
		val = div_u64(val * frac_max, parent_rate);

	val -= m * frac_max;

	return min((unsigned int)val, (frac_max - 1));
}

static int meson_clk_get_pll_multiplier(unsigned long rate,
					unsigned long parent_rate,
					unsigned int *m,
					struct meson_clk_pll_data *pll)
{
	if (!pll->range)
		return -EINVAL;

	if (rate <= pll->range->min * parent_rate) {
		*m = pll->range->min;
		return -ENODATA;
	} else if (rate >= pll->range->max * parent_rate) {
		*m = pll->range->max;
		return -ENODATA;
	}

	if (__pll_round_closest_mult(pll))
		*m = DIV_ROUND_CLOSEST_ULL(rate, parent_rate);
	else
		*m = div_u64(rate, parent_rate);

	/* Multiplier exceeds hardware range */
	if (*m >= (1 << pll->m.width))
		return -EINVAL;

	return 0;
}

static int meson_clk_pll_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_pll_data *pll = meson_clk_pll_data(clk);
	unsigned int m, frac;
	unsigned long round;
	int ret;

	ret = meson_clk_get_pll_multiplier(req->rate, req->best_parent_rate, &m, pll);
	if (ret && ret != -ENODATA)
		return ret;

	round = __pll_params_to_rate(req->best_parent_rate, m, 0, pll);

	if (!MESON_PARM_APPLICABLE(&pll->frac) || req->rate == round) {
		req->rate = round;
		return 0;
	}

	/*
	 * The rate provided by the setting is not an exact match, let's
	 * try to improve the result using the fractional parameter
	 */
	frac = __pll_params_with_frac(req->rate, req->best_parent_rate, m, pll);
	req->rate = __pll_params_to_rate(req->best_parent_rate, m, frac, pll);

	return 0;
}

static int meson_clk_pll_wait_lock(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_pll_data *pll = meson_clk_pll_data(clk);
	int delay = 5000;

	do {
		/* Is the clock locked now ? Time out after 100ms. */
		if (meson_parm_read(clk->map, &pll->l))
			return 0;

		udelay(20);
	} while (--delay);

	return -ETIMEDOUT;
}

static int meson_clk_pll_is_enabled(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_pll_data *pll = meson_clk_pll_data(clk);

	if (MESON_PARM_APPLICABLE(&pll->rst) &&
	    meson_parm_read(clk->map, &pll->rst))
		return 0;

	if (!meson_parm_read(clk->map, &pll->en) ||
	    !meson_parm_read(clk->map, &pll->l))
		return 0;

	return 1;
}

static int meson_clk_pll_init(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_pll_data *pll = meson_clk_pll_data(clk);
	int ret;

	ret = clk_regmap_init(hw);
	if (ret)
		return ret;

	/*
	 * Keep the clock running, which was already initialized and enabled
	 * from the bootloader stage, to avoid any glitches.
	 */
	if ((pll->flags & CLK_MESON_PLL_NOINIT_ENABLED) &&
	    meson_clk_pll_is_enabled(hw))
		return 0;

	if (pll->init_count) {
		if (MESON_PARM_APPLICABLE(&pll->rst))
			meson_parm_write(clk->map, &pll->rst, 1);

		regmap_multi_reg_write(clk->map, pll->init_regs,
				       pll->init_count);

		if (MESON_PARM_APPLICABLE(&pll->rst))
			meson_parm_write(clk->map, &pll->rst, 0);
	}

	return 0;
}

static int meson_clk_pcie_pll_enable(struct clk_hw *hw)
{
	int retries = 10;

	do {
		meson_clk_pll_init(hw);
		if (!meson_clk_pll_wait_lock(hw))
			return 0;
		pr_info("Retry enabling PCIe PLL clock\n");
	} while (--retries);

	return -EIO;
}

static int meson_clk_pll_enable(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_pll_data *pll = meson_clk_pll_data(clk);

	/* do nothing if the PLL is already enabled */
	if (clk_hw_is_enabled(hw))
		return 0;

	/* Make sure the pll is in reset */
	if (MESON_PARM_APPLICABLE(&pll->rst))
		meson_parm_write(clk->map, &pll->rst, 1);

	/* Enable the pll */
	meson_parm_write(clk->map, &pll->en, 1);

	/* Take the pll out reset */
	if (MESON_PARM_APPLICABLE(&pll->rst))
		meson_parm_write(clk->map, &pll->rst, 0);

	/*
	 * Compared with the previous SoCs, self-adaption current module
	 * is newly added for A1, keep the new power-on sequence to enable the
	 * PLL. The sequence is:
	 * 1. enable the pll, delay for 10us
	 * 2. enable the pll self-adaption current module, delay for 40us
	 * 3. enable the lock detect module
	 */
	if (MESON_PARM_APPLICABLE(&pll->current_en)) {
		udelay(10);
		meson_parm_write(clk->map, &pll->current_en, 1);
		udelay(40);
	}

	if (MESON_PARM_APPLICABLE(&pll->l_detect)) {
		meson_parm_write(clk->map, &pll->l_detect, 1);
		meson_parm_write(clk->map, &pll->l_detect, 0);
	}

	if (meson_clk_pll_wait_lock(hw))
		return -EIO;

	return 0;
}

static void meson_clk_pll_disable(struct clk_hw *hw)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_pll_data *pll = meson_clk_pll_data(clk);

	/* Put the pll is in reset */
	if (MESON_PARM_APPLICABLE(&pll->rst))
		meson_parm_write(clk->map, &pll->rst, 1);

	/* Disable the pll */
	meson_parm_write(clk->map, &pll->en, 0);

	/* Disable PLL internal self-adaption current module */
	if (MESON_PARM_APPLICABLE(&pll->current_en))
		meson_parm_write(clk->map, &pll->current_en, 0);
}

static int meson_clk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct clk_regmap *clk = to_clk_regmap(hw);
	struct meson_clk_pll_data *pll = meson_clk_pll_data(clk);
	unsigned int enabled, m, frac = 0;
	unsigned long old_rate;
	int ret;

	if (parent_rate == 0 || rate == 0)
		return -EINVAL;

	old_rate = clk_hw_get_rate(hw);

	ret = meson_clk_get_pll_multiplier(rate, parent_rate, &m, pll);
	if (ret && ret != -ENODATA)
		return ret;

	enabled = meson_parm_read(clk->map, &pll->en);
	if (enabled)
		meson_clk_pll_disable(hw);

	meson_parm_write(clk->map, &pll->m, m);

	if (MESON_PARM_APPLICABLE(&pll->frac)) {
		frac = __pll_params_with_frac(rate, parent_rate, m, pll);
		meson_parm_write(clk->map, &pll->frac, frac);
	}

	/* If the pll is stopped, bail out now */
	if (!enabled)
		return 0;

	ret = meson_clk_pll_enable(hw);
	if (ret) {
		pr_warn("%s: pll %s didn't lock, trying to set old rate %lu\n",
			__func__, clk_hw_get_name(hw), old_rate);
		/*
		 * FIXME: Do we really need/want this HACK ?
		 * It looks unsafe. what happens if the clock gets into a
		 * broken state and we can't lock back on the old_rate ? Looks
		 * like an infinite recursion is possible
		 */
		meson_clk_pll_set_rate(hw, old_rate, parent_rate);
	}

	return ret;
}

/*
 * The Meson G12A PCIE PLL is fined tuned to deliver a very precise
 * 100MHz reference clock for the PCIe Analog PHY, and thus requires
 * a strict register sequence to enable the PLL.
 * To simplify, re-use the _init() op to enable the PLL and keep
 * the other ops except set_rate since the rate is fixed.
 */
const struct clk_ops meson_clk_pcie_pll_ops = {
	.init		= clk_regmap_init,
	.recalc_rate	= meson_clk_pll_recalc_rate,
	.determine_rate	= meson_clk_pll_determine_rate,
	.is_enabled	= meson_clk_pll_is_enabled,
	.enable		= meson_clk_pcie_pll_enable,
	.disable	= meson_clk_pll_disable
};
EXPORT_SYMBOL_NS_GPL(meson_clk_pcie_pll_ops, "CLK_MESON");

const struct clk_ops meson_clk_pll_ops = {
	.init		= meson_clk_pll_init,
	.recalc_rate	= meson_clk_pll_recalc_rate,
	.determine_rate	= meson_clk_pll_determine_rate,
	.set_rate	= meson_clk_pll_set_rate,
	.is_enabled	= meson_clk_pll_is_enabled,
	.enable		= meson_clk_pll_enable,
	.disable	= meson_clk_pll_disable
};
EXPORT_SYMBOL_NS_GPL(meson_clk_pll_ops, "CLK_MESON");

const struct clk_ops meson_clk_pll_ro_ops = {
	.init		= clk_regmap_init,
	.recalc_rate	= meson_clk_pll_recalc_rate,
	.is_enabled	= meson_clk_pll_is_enabled,
};
EXPORT_SYMBOL_NS_GPL(meson_clk_pll_ro_ops, "CLK_MESON");

MODULE_DESCRIPTION("Amlogic PLL driver");
MODULE_AUTHOR("Carlo Caione <carlo@endlessm.com>");
MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CLK_MESON");
