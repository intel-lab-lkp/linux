// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spacemit clock type pll
 *
 * Copyright (c) 2024 SpacemiT Technology Co. Ltd
 * Copyright (c) 2024 Haylen Chu <heylenay@4d2.org>
 */

#include <linux/clk-provider.h>
#include <linux/regmap.h>

#include "ccu_common.h"
#include "ccu_pll.h"

#define PLL_DELAY_TIME	3000

#define PLL_SWCR3_EN	BIT(31)

static int ccu_pll_is_enabled(struct clk_hw *hw)
{
	struct ccu_pll *p = hw_to_ccu_pll(hw);
	u32 tmp;

	ccu_read(swcr3, &p->common, &tmp);

	return tmp & PLL_SWCR3_EN;
}

/* frequency unit Mhz, return pll vco freq */
static unsigned long ccu_pll_get_vco_freq(struct clk_hw *hw)
{
	const struct ccu_pll_rate_tbl *pll_rate_table;
	struct ccu_pll *p = hw_to_ccu_pll(hw);
	struct ccu_common *common = &p->common;
	u32 swcr1, swcr3, size;
	int i;

	ccu_read(swcr1, common, &swcr1);
	ccu_read(swcr3, common, &swcr3);
	swcr3 &= ~PLL_SWCR3_EN;

	pll_rate_table = p->pll.rate_tbl;
	size = p->pll.tbl_size;

	for (i = 0; i < size; i++) {
		if (pll_rate_table[i].swcr1 == swcr1 &&
		    pll_rate_table[i].swcr3 == swcr3)
			return pll_rate_table[i].rate;
	}

	WARN_ON_ONCE(1);

	return 0;
}

static int ccu_pll_enable(struct clk_hw *hw)
{
	struct ccu_pll *p = hw_to_ccu_pll(hw);
	struct ccu_common *common = &p->common;
	unsigned int tmp;
	int ret;

	if (ccu_pll_is_enabled(hw))
		return 0;

	ccu_update(swcr3, common, PLL_SWCR3_EN, PLL_SWCR3_EN);

	/* check lock status */
	ret = regmap_read_poll_timeout_atomic(common->lock_regmap,
					      p->pll.reg_lock,
					      tmp,
					      tmp & p->pll.lock_enable_bit,
					      5, PLL_DELAY_TIME);

	return ret;
}

static void ccu_pll_disable(struct clk_hw *hw)
{
	struct ccu_pll *p = hw_to_ccu_pll(hw);
	struct ccu_common *common = &p->common;

	ccu_update(swcr3, common, PLL_SWCR3_EN, 0);
}

/*
 * PLLs must be gated before changing rate, which is ensured by
 * flag CLK_SET_RATE_GATE.
 */
static int ccu_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct ccu_pll *p = hw_to_ccu_pll(hw);
	struct ccu_common *common = &p->common;
	struct ccu_pll_config *params = &p->pll;
	const struct ccu_pll_rate_tbl *entry = NULL;
	int i;

	for (i = 0; i < params->tbl_size; i++) {
		if (rate == params->rate_tbl[i].rate) {
			entry = &params->rate_tbl[i];
			break;
		}
	}

	if (WARN_ON_ONCE(!entry))
		return -EINVAL;

	ccu_update(swcr1, common, entry->swcr1, entry->swcr1);
	ccu_update(swcr3, common, (u32)~PLL_SWCR3_EN, entry->swcr3);

	return 0;
}

static unsigned long ccu_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	return ccu_pll_get_vco_freq(hw);
}

static long ccu_pll_round_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *prate)
{
	struct ccu_pll *p = hw_to_ccu_pll(hw);
	struct ccu_pll_config *params = &p->pll;
	unsigned int i;

	for (i = 0; i < params->tbl_size; i++) {
		if (params->rate_tbl[i].rate > rate) {
			i--;
			break;
		}
	}

	return rate;
}

const struct clk_ops spacemit_ccu_pll_ops = {
	.enable		= ccu_pll_enable,
	.disable	= ccu_pll_disable,
	.set_rate	= ccu_pll_set_rate,
	.recalc_rate	= ccu_pll_recalc_rate,
	.round_rate	= ccu_pll_round_rate,
	.is_enabled	= ccu_pll_is_enabled,
};

