// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 SpacemiT Technology Co. Ltd
 * Copyright (c) 2024-2025 Haylen Chu <heylenay@4d2.org>
 */

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
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

#define PLL_OUTPUT_GATES	GENMASK(7, 0)
#define PLL_POWERDOWN_BYPASS	BIT(23)
#define PLL_SAFE_OUTPUT_GATE	(BIT(1) | BIT(4))
#define PLL_SAFE_MPMU_GATE	(BIT(16) | BIT(21))
#define PLL_CPU_SEL		GENMASK(2, 0)
#define PLL_CPU_FC		BIT(12)
#define PLL_CPU_TIMEOUT_US	10000
#define PLL_MAX_CPU_MUXES	4

static const struct ccu_pll_rate_tbl *ccu_pll_lookup_best_rate(struct ccu_pll *pll,
							       unsigned long rate)
{
	struct ccu_pll_config *config = &pll->config;
	const struct ccu_pll_rate_tbl *best_entry = NULL;
	unsigned long best_delta = ULONG_MAX;
	u32 i;

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

static int ccu_pll_update_param(struct ccu_pll *pll, const struct ccu_pll_rate_tbl *entry)
{
	struct ccu_common *common = &pll->common;
	int ret;

	ret = regmap_write(common->regmap, common->reg_swcr1, entry->swcr1);
	if (ret)
		return ret;
	return ccu_update(common, swcr3, PLL_SWCR3_MASK, entry->swcr3);
}

static int ccu_pll_is_enabled(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);
	u32 val;
	int ret;

	ret = regmap_read(common->regmap, common->reg_swcr3, &val);
	return ret ? ret : !!(val & PLL_SWCR3_EN);
}

static int ccu_pll_enable(struct clk_hw *hw)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);
	struct ccu_common *common = &pll->common;
	unsigned int tmp;
	int ret;

	ret = ccu_update(common, swcr3, PLL_SWCR3_EN, PLL_SWCR3_EN);
	if (ret)
		return ret;

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

static int ccu_pll_check_stopped(struct ccu_pll *pll)
{
	u32 val;
	int ret;

	ret = regmap_read(pll->common.lock_regmap, pll->config.reg_lock, &val);
	if (ret)
		return ret;
	return val & pll->config.mask_lock ? -EBUSY : 0;
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
	int ret;

	/* CLK_SET_RATE_GATE does not account for firmware-only users. */
	ret = ccu_pll_is_enabled(hw);
	if (ret)
		return ret < 0 ? ret : -EBUSY;
	ret = ccu_pll_check_stopped(pll);
	if (ret)
		return ret;

	entry = ccu_pll_lookup_best_rate(pll, rate);
	if (!entry)
		return -EINVAL;
	return ccu_pll_update_param(pll, entry);
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

static int ccu_pll_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);
	const struct ccu_pll_rate_tbl *entry;

	entry = ccu_pll_lookup_best_rate(pll, req->rate);
	if (!entry)
		return -EINVAL;
	req->rate = entry->rate;

	return 0;
}

static int ccu_plla_update_param(struct ccu_pll *pll, const struct ccu_pll_rate_tbl *entry)
{
	struct ccu_common *common = &pll->common;
	int ret;

	ret = regmap_write(common->regmap, common->reg_swcr1, entry->swcr1);
	if (ret)
		return ret;
	ret = regmap_write(common->regmap, common->reg_swcr3, entry->swcr3);
	if (ret)
		return ret;
	return ccu_update(common, swcr2, PLLA_SWCR2_MASK, entry->swcr2);
}

static int ccu_plla_is_enabled(struct clk_hw *hw)
{
	struct ccu_common *common = hw_to_ccu_common(hw);
	u32 val;
	int ret;

	ret = regmap_read(common->regmap, common->reg_swcr2, &val);
	return ret ? ret : !!(val & PLLA_SWCR2_EN);
}

static int ccu_plla_enable(struct clk_hw *hw)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);
	struct ccu_common *common = &pll->common;
	unsigned int tmp;
	int ret;

	ret = ccu_update(common, swcr2, PLLA_SWCR2_EN, PLLA_SWCR2_EN);
	if (ret)
		return ret;

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
	int ret;

	ret = ccu_plla_is_enabled(hw);
	if (ret)
		return ret < 0 ? ret : -EBUSY;
	ret = ccu_pll_check_stopped(pll);
	if (ret)
		return ret;

	entry = ccu_pll_lookup_best_rate(pll, rate);
	if (!entry)
		return -EINVAL;
	return ccu_plla_update_param(pll, entry);
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

static bool ccu_pll_params_equal(const struct ccu_pll_rate_tbl *a,
				 const struct ccu_pll_rate_tbl *b, bool plla)
{
	if (a->swcr1 != b->swcr1)
		return false;
	if (plla)
		return a->swcr3 == b->swcr3 &&
		       !((a->swcr2 ^ b->swcr2) & PLLA_SWCR2_MASK);
	return !((a->swcr3 ^ b->swcr3) & PLL_SWCR3_MASK);
}

static unsigned long ccu_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct ccu_pll_rate_tbl params;

	if (ccu_pll_get_params(hw_to_ccu_pll(hw), &params, false))
		return 0;
	return ccu_pll_calc_rate(&params, parent_rate);
}

static unsigned long ccu_plla_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct ccu_pll_rate_tbl params;

	if (ccu_pll_get_params(hw_to_ccu_pll(hw), &params, true))
		return 0;
	return ccu_plla_calc_rate(&params, parent_rate);
}

struct ccu_pll_park {
	struct regmap *apmu;
	u32 saved[PLL_MAX_CPU_MUXES];
	u32 selected[PLL_MAX_CPU_MUXES];
	unsigned long parked;
};

static int ccu_pll_select_cpu(struct regmap *regmap, u32 reg, u32 sel)
{
	u32 val;
	int ret;

	ret = regmap_update_bits(regmap, reg, PLL_CPU_SEL, sel);
	if (ret)
		return ret;
	ret = regmap_update_bits(regmap, reg, PLL_CPU_FC, PLL_CPU_FC);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout_atomic(regmap, reg, val,
					     !(val & PLL_CPU_FC), PLL_DELAY_US,
					     PLL_CPU_TIMEOUT_US);
	if (ret)
		return ret;
	return (val & PLL_CPU_SEL) == sel ? 0 : -EIO;
}

static void ccu_pll_unpark(const struct ccu_pll_sync *sync,
			   struct ccu_pll_park *park)
{
	int i;

	/* Restore sharing-capable secondary clusters last. */
	for (i = sync->num_muxes - 1; i >= 0; i--) {
		if (!(park->parked & BIT(i)))
			continue;
		if (!ccu_pll_select_cpu(park->apmu, sync->muxes[i].reg,
					park->saved[i] & PLL_CPU_SEL))
			park->parked &= ~BIT(i);
	}

	/*
	 * The fallback gates remain critical: after an FC timeout the selector
	 * register alone cannot prove that the CPU has left the temporary path.
	 */
}

static int ccu_pll_park_cpus(const struct ccu_pll_sync *sync,
			     struct ccu_pll_park *park, bool plla,
			     unsigned long rate, unsigned long parent_rate)
{
	struct ccu_pll *pll1 = sync->safe_pll;
	struct ccu_common *safe = &pll1->common;
	struct ccu_pll_rate_tbl params;
	struct device_node *np;
	unsigned long safe_rate;
	u32 val;
	u32 i;
	int ret;
	bool needed = false;

	if (!sync->num_muxes)
		return 0;
	if (sync->num_muxes > PLL_MAX_CPU_MUXES)
		return -EINVAL;
	/* Like CCU probe, create the regmap before its clocks are registered. */
	np = of_find_compatible_node(NULL, NULL, sync->apmu_compatible);
	if (!np)
		return -ENODEV;
	park->apmu = device_node_to_regmap(np);
	of_node_put(np);
	if (IS_ERR(park->apmu))
		return PTR_ERR(park->apmu);

	for (i = 0; i < sync->num_muxes; i++) {
		const struct ccu_pll_cpu_mux *mux = &sync->muxes[i];

		ret = regmap_read(park->apmu, mux->reg, &park->saved[i]);
		if (ret)
			return ret;
		if (park->saved[i] & PLL_CPU_FC)
			return -EBUSY;
		/* K3 selector 4 is unmodeled; do not infer a live parent. */
		if (plla && (park->saved[i] & PLL_CPU_SEL) == 4)
			return -EINVAL;
		needed |= (park->saved[i] & mux->mask) == mux->value;
	}
	if (!needed)
		return 0;

	/* PLL1 is never repaired here, nor used as a fallback if unrecognized. */
	if (!safe->regmap || !safe->lock_regmap)
		return -ENODEV;
	if (!pll1->config.tbl_num)
		return -EINVAL;
	ret = ccu_pll_get_params(pll1, &params, plla);
	if (ret)
		return ret;
	if (!ccu_pll_params_equal(&params, &pll1->config.rate_tbl[0], plla))
		return -EINVAL;
	ret = regmap_read(safe->lock_regmap, pll1->config.reg_lock, &val);
	if (ret)
		return ret;
	if (!(val & pll1->config.mask_lock))
		return -EBUSY;

	safe_rate = plla ? ccu_plla_calc_rate(&params, parent_rate) :
			   ccu_pll_calc_rate(&params, parent_rate);
	for (i = 0; i < sync->num_muxes; i++) {
		u32 old = park->saved[i], sel = old & PLL_CPU_SEL;
		u32 div = ((old >> 3) & 7) + 1;
		unsigned long cpu_rate, fast_rate, slow_rate;

		if ((old & sync->muxes[i].mask) != sync->muxes[i].value)
			continue;
		if (plla) {
			cpu_rate = rate;
			fast_rate = safe_rate / 2;
		} else {
			cpu_rate = rate / (sel == 5 ? 3 : (old & BIT(13) ? 1 : 2));
			cpu_rate /= div;
			fast_rate = safe_rate / 2 / div;
		}
		slow_rate = safe_rate / 5 / div;
		/* Never increase a CPU's rate without a corresponding voltage vote. */
		if (fast_rate <= cpu_rate)
			park->selected[i] = sync->safe_sel;
		else if (slow_rate <= cpu_rate)
			park->selected[i] = sync->slow_sel;
		else
			return -ERANGE;
	}
	/* Both candidate parents are derived from the always-on PLL1. */
	ret = ccu_update(safe, swcr2, PLL_SAFE_OUTPUT_GATE, PLL_SAFE_OUTPUT_GATE);
	if (ret)
		return ret;
	ret = regmap_update_bits(safe->lock_regmap, sync->reg_safe_gate,
				 PLL_SAFE_MPMU_GATE, PLL_SAFE_MPMU_GATE);
	if (ret)
		return ret;

	for (i = 0; i < sync->num_muxes; i++) {
		const struct ccu_pll_cpu_mux *mux = &sync->muxes[i];

		if ((park->saved[i] & mux->mask) != mux->value)
			continue;
		park->parked |= BIT(i);
		ret = ccu_pll_select_cpu(park->apmu, mux->reg, park->selected[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int ccu_pll_stop(struct ccu_pll *pll, bool plla)
{
	struct ccu_common *common = &pll->common;
	u32 val;
	int ret;

	ret = plla ? ccu_update(common, swcr2, PLLA_SWCR2_EN, 0) :
		     ccu_update(common, swcr3, PLL_SWCR3_EN, 0);
	if (ret)
		return ret;
	/* Hardware or firmware may override the software enable bit. */
	return regmap_read_poll_timeout_atomic(common->lock_regmap,
					      pll->config.reg_lock, val,
					      !(val & pll->config.mask_lock),
					      PLL_DELAY_US, PLL_TIMEOUT_US);
}

static int ccu_pll_sync_init(struct clk_hw *hw, bool plla)
{
	struct ccu_pll *pll = hw_to_ccu_pll(hw);
	const struct ccu_pll_sync *sync = pll->config.sync;
	const struct ccu_pll_rate_tbl *entry = NULL;
	unsigned long (*calc)(const struct ccu_pll_rate_tbl *params,
			      unsigned long parent_rate);
	int (*update)(struct ccu_pll *pll, const struct ccu_pll_rate_tbl *params);
	int (*enable)(struct clk_hw *hw);
	struct ccu_pll_rate_tbl old;
	struct ccu_pll_park park = {};
	struct clk_hw *parent;
	unsigned long parent_rate, rate;
	u32 outputs, lock, i;
	bool enabled;
	int ret;

	/* Synchronization is opt-in; in particular PLL1 has no sync descriptor. */
	if (!sync)
		return 0;
	parent = clk_hw_get_parent_by_index(hw, 0);
	if (!parent)
		return 0;
	parent_rate = clk_hw_get_rate(parent);
	calc = plla ? ccu_plla_calc_rate : ccu_pll_calc_rate;
	update = plla ? ccu_plla_update_param : ccu_pll_update_param;
	enable = plla ? ccu_plla_enable : ccu_pll_enable;
	ret = ccu_pll_get_params(pll, &old, plla);
	if (ret)
		goto warn;
	rate = calc(&old, parent_rate);
	if (!rate)
		return 0;

	/* Compare encoded rates, including fractional-divider quantization. */
	for (i = 0; i < pll->config.tbl_num; i++) {
		if (rate == calc(&pll->config.rate_tbl[i], parent_rate)) {
			entry = &pll->config.rate_tbl[i];
			break;
		}
	}
	if (!entry || ccu_pll_params_equal(&old, entry, plla))
		return 0;
	if ((plla ? old.swcr3 : old.swcr1) & PLL_POWERDOWN_BYPASS)
		return 0;
	ret = regmap_read(pll->common.regmap, pll->common.reg_swcr2, &outputs);
	if (ret)
		goto warn;
	/* Do not interrupt peripheral users, including unregistered consumers. */
	if (outputs & PLL_OUTPUT_GATES & ~sync->cpu_outputs)
		return 0;
	ret = regmap_read(pll->common.lock_regmap, pll->config.reg_lock, &lock);
	if (ret)
		goto warn;
	enabled = plla ? old.swcr2 & PLLA_SWCR2_EN : old.swcr3 & PLL_SWCR3_EN;
	if (enabled != !!(lock & pll->config.mask_lock))
		return 0;

	/*
	 * .init runs under the CCF prepare lock, before this PLL is linked to
	 * its children. Restore the hardware muxes before CCF adopts them.
	 */
	ret = ccu_pll_park_cpus(sync, &park, plla, rate, parent_rate);
	if (ret)
		goto unpark;
	ret = ccu_pll_stop(pll, plla);
	if (ret)
		goto restart;
	ret = update(pll, entry);
	if (!ret && enabled)
		ret = enable(hw);
	if (!ret)
		goto unpark;

	/* Restore the old parameters before considering the original parents. */
	if (ccu_pll_stop(pll, plla) || update(pll, &old))
		goto warn;
restart:
	if (enabled && enable(hw))
		goto warn;
unpark:
	ccu_pll_unpark(sync, &park);
	if (park.parked)
		ret = -ETIMEDOUT;
warn:
	if (ret)
		pr_warn("%s: PLL synchronization failed: %d; retaining safe clocks\n",
			clk_hw_get_name(hw), ret);
	/* Failed synchronization must not unwind clocks needed to keep booting. */
	return 0;
}

static int ccu_pll_init(struct clk_hw *hw)
{
	return ccu_pll_sync_init(hw, false);
}

static int ccu_plla_init(struct clk_hw *hw)
{
	return ccu_pll_sync_init(hw, true);
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
