// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 SpacemiT Technology Co. Ltd
 * Copyright (c) 2024-2025 Haylen Chu <heylenay@4d2.org>
 */

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/regmap.h>

#include "ccu_common.h"
#include "ccu_pll.h"

#define PLL_TIMEOUT_US		3000
#define PLL_DELAY_US		5

#define PLL_SWCR1_PREDIV	GENMASK(13, 12)
#define PLL_SWCR1_INTERNAL	BIT(29)
#define PLL_SWCR3_INT		GENMASK(30, 24)
#define PLL_SWCR3_FRAC		GENMASK(23, 0)

#define PLL_SWCR3_EN		((u32)BIT(31))
#define PLL_SWCR3_MASK		GENMASK(30, 0)

#define PLLA_SWCR2_EN		((u32)BIT(16))
#define PLLA_SWCR2_MASK		GENMASK(15, 8)

#define PLLA_SWCR1_USER_MODE	BIT(25)
#define PLLA_SWCR1_INT		GENMASK(22, 16)
#define PLLA_SWCR1_REFSEL	GENMASK(15, 14)
#define PLLA_SWCR1_FRAC		GENMASK(13, 0)
#define PLLA_SWCR3_PREDIV	GENMASK(21, 20)
#define PLL_FRAC_BITS		22

static const struct ccu_pll_rate_tbl *ccu_pll_lookup_best_rate(struct ccu_pll *pll,
							       unsigned long rate)
{
	struct ccu_pll_config *config = &pll->config;
	const struct ccu_pll_rate_tbl *best_entry;
	unsigned long best_delta = ULONG_MAX;
	int i;

	for (i = 0; i < config->tbl_num; i++) {
		const struct ccu_pll_rate_tbl *entry = &config->rate_tbl[i];
		unsigned long delta = abs_diff(entry->rate, rate);

		if (delta < best_delta) {
			best_delta = delta;
			best_entry = entry;
		}
	}

	return best_entry;
}

static const struct ccu_pll_rate_tbl *ccu_pll_lookup_matched_entry(struct ccu_pll *pll)
{
	struct ccu_pll_config *config = &pll->config;
	u32 swcr1, swcr3;
	int i;

	swcr1 = ccu_read(&pll->common, swcr1);
	swcr3 = ccu_read(&pll->common, swcr3);
	swcr3 &= PLL_SWCR3_MASK;

	for (i = 0; i < config->tbl_num; i++) {
		const struct ccu_pll_rate_tbl *entry = &config->rate_tbl[i];

		if (swcr1 == entry->swcr1 && swcr3 == entry->swcr3)
			return entry;
	}

	return NULL;
}

static void ccu_pll_update_param(struct ccu_pll *pll, const struct ccu_pll_rate_tbl *entry)
{
	struct ccu_common *common = &pll->common;

	regmap_write(common->regmap, common->reg_swcr1, entry->swcr1);
	ccu_update(common, swcr3, PLL_SWCR3_MASK, entry->swcr3);
}

static int ccu_pll_is_enabled(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return ccu_read(common, swcr3) & PLL_SWCR3_EN;
}

static int ccu_pll_enable(struct clk_hw *hw)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);
	struct ccu_common *common = &pll->common;
	unsigned int tmp;

	ccu_update(common, swcr3, PLL_SWCR3_EN, PLL_SWCR3_EN);

	/* check lock status */
	return regmap_read_poll_timeout_atomic(common->lock_regmap,
					       pll->config.reg_lock,
					       tmp,
					       tmp & pll->config.mask_lock,
					       PLL_DELAY_US, PLL_TIMEOUT_US);
}

static void ccu_pll_disable(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	ccu_update(common, swcr3, PLL_SWCR3_EN, 0);
}

/*
 * PLLs must be gated before changing rate, which is ensured by
 * flag CLK_SET_RATE_GATE.
 */
static int ccu_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);
	const struct ccu_pll_rate_tbl *entry;

	entry = ccu_pll_lookup_best_rate(pll, rate);
	ccu_pll_update_param(pll, entry);

	return 0;
}

static int ccu_pll_get_params(struct ccu_pll *pll,
			      struct ccu_pll_rate_tbl *params, bool plla)
{
	struct ccu_common *common = &pll->common;
	int ret;

	ret = regmap_read(common->regmap, common->reg_swcr1, &params->swcr1);
	if (ret)
		return ret;
	params->swcr2 = 0;
	if (plla) {
		ret = regmap_read(common->regmap, common->reg_swcr2, &params->swcr2);
		if (ret)
			return ret;
	}
	return regmap_read(common->regmap, common->reg_swcr3, &params->swcr3);
}

static unsigned long ccu_pll_calc_rate(const struct ccu_pll_rate_tbl *params,
				      unsigned long parent_rate)
{
	u32 swcr1 = params->swcr1, swcr3 = params->swcr3, prediv;
	s64 divider;
	u64 rate;

	/* The programmed divider is not used in internal configuration mode. */
	if (swcr1 & PLL_SWCR1_INTERNAL)
		return 0;

	prediv = FIELD_GET(PLL_SWCR1_PREDIV, swcr1) + 1;
	divider = (s64)FIELD_GET(PLL_SWCR3_INT, swcr3) << PLL_FRAC_BITS;
	/* The 24-bit fractional code is signed, with an LSB of 2^-22. */
	divider += sign_extend32(FIELD_GET(PLL_SWCR3_FRAC, swcr3), 23);
	if (divider <= 0)
		return 0;

	/* Fvco = Fref * Npre * (Nint + Nfrac). */
	rate = (u64)parent_rate * prediv * divider;
	return DIV_ROUND_CLOSEST_ULL(rate, BIT_ULL(PLL_FRAC_BITS));
}

static unsigned long ccu_plla_calc_rate(const struct ccu_pll_rate_tbl *params,
				       unsigned long parent_rate)
{
	u32 swcr1 = params->swcr1, swcr2 = params->swcr2;
	u32 swcr3 = params->swcr3, prediv, frac;
	u64 divider, rate;

	/* Decode the software-controlled mode described by the PLL calculator. */
	if (!(swcr1 & PLLA_SWCR1_USER_MODE) ||
	    (swcr1 & PLLA_SWCR1_REFSEL))
		return 0;

	prediv = FIELD_GET(PLLA_SWCR3_PREDIV, swcr3) + 1;
	frac = FIELD_GET(PLLA_SWCR1_FRAC, swcr1) << 8;
	frac |= FIELD_GET(PLLA_SWCR2_MASK, swcr2);
	divider = (u64)FIELD_GET(PLLA_SWCR1_INT, swcr1) << PLL_FRAC_BITS;
	divider += frac;

	/* Fvco = Fref * Npre * (Nint + Nfrac), with an unsigned fraction. */
	rate = (u64)parent_rate * prediv * divider;
	return DIV_ROUND_CLOSEST_ULL(rate, BIT_ULL(PLL_FRAC_BITS));
}

static unsigned long ccu_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct ccu_pll_rate_tbl params;

	if (ccu_pll_get_params(hw_to_ccu_pll(hw), &params, false))
		return 0;
	return ccu_pll_calc_rate(&params, parent_rate);
}

static int ccu_pll_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);

	req->rate = ccu_pll_lookup_best_rate(pll, req->rate)->rate;

	return 0;
}

static int ccu_pll_init(struct clk_hw *hw)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);

	if (ccu_pll_lookup_matched_entry(pll))
		return 0;

	ccu_pll_disable(hw);
	ccu_pll_update_param(pll, &pll->config.rate_tbl[0]);

	return 0;
}

static const struct ccu_pll_rate_tbl *ccu_plla_lookup_matched_entry(struct ccu_pll *pll)
{
	struct ccu_pll_config *config = &pll->config;
	const struct ccu_pll_rate_tbl *entry;
	u32 i, swcr1, swcr2, swcr3;

	swcr1 = ccu_read(&pll->common, swcr1);
	swcr2 = ccu_read(&pll->common, swcr2);
	swcr2 &= PLLA_SWCR2_MASK;
	swcr3 = ccu_read(&pll->common, swcr3);

	for (i = 0; i < config->tbl_num; i++) {
		entry = &config->rate_tbl[i];

		if (swcr1 == entry->swcr1 &&
		    swcr2 == entry->swcr2 &&
		    swcr3 == entry->swcr3)
			return entry;
	}

	return NULL;
}

static void ccu_plla_update_param(struct ccu_pll *pll, const struct ccu_pll_rate_tbl *entry)
{
	struct ccu_common *common = &pll->common;

	regmap_write(common->regmap, common->reg_swcr1, entry->swcr1);
	regmap_write(common->regmap, common->reg_swcr3, entry->swcr3);
	ccu_update(common, swcr2, PLLA_SWCR2_MASK, entry->swcr2);
}

static int ccu_plla_is_enabled(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	return ccu_read(common, swcr2) & PLLA_SWCR2_EN;
}

static int ccu_plla_enable(struct clk_hw *hw)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);
	struct ccu_common *common = &pll->common;
	unsigned int tmp;

	ccu_update(common, swcr2, PLLA_SWCR2_EN, PLLA_SWCR2_EN);

	/* check lock status */
	return regmap_read_poll_timeout_atomic(common->lock_regmap,
					       pll->config.reg_lock,
					       tmp,
					       tmp & pll->config.mask_lock,
					       PLL_DELAY_US, PLL_TIMEOUT_US);
}

static void ccu_plla_disable(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);

	ccu_update(common, swcr2, PLLA_SWCR2_EN, 0);
}

/*
 * PLLAs must be gated before changing rate, which is ensured by
 * flag CLK_SET_RATE_GATE.
 */
static int ccu_plla_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);
	const struct ccu_pll_rate_tbl *entry;

	entry = ccu_pll_lookup_best_rate(pll, rate);
	ccu_plla_update_param(pll, entry);

	return 0;
}

static unsigned long ccu_plla_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct ccu_pll_rate_tbl params;

	if (ccu_pll_get_params(hw_to_ccu_pll(hw), &params, true))
		return 0;
	return ccu_plla_calc_rate(&params, parent_rate);
}

static int ccu_plla_init(struct clk_hw *hw)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);

	if (ccu_plla_lookup_matched_entry(pll))
		return 0;

	ccu_plla_disable(hw);
	ccu_plla_update_param(pll, &pll->config.rate_tbl[0]);

	return 0;
}

const struct clk_ops spacemit_ccu_pll_ops = {
	.init		= ccu_pll_init,
	.enable		= ccu_pll_enable,
	.disable	= ccu_pll_disable,
	.set_rate	= ccu_pll_set_rate,
	.recalc_rate	= ccu_pll_recalc_rate,
	.determine_rate = ccu_pll_determine_rate,
	.is_enabled	= ccu_pll_is_enabled,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_pll_ops, "CLK_SPACEMIT");

const struct clk_ops spacemit_ccu_plla_ops = {
	.init		= ccu_plla_init,
	.enable		= ccu_plla_enable,
	.disable	= ccu_plla_disable,
	.set_rate	= ccu_plla_set_rate,
	.recalc_rate	= ccu_plla_recalc_rate,
	.determine_rate	= ccu_pll_determine_rate,
	.is_enabled	= ccu_plla_is_enabled,
};
EXPORT_SYMBOL_NS_GPL(spacemit_ccu_plla_ops, "CLK_SPACEMIT");
