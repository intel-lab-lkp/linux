// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spacemit clock type ddn
 *
 * Copyright (c) 2024 SpacemiT Technology Co. Ltd
 * Copyright (c) 2024 Haylen Chu <heylenay@4d2.org>
 */

#include <linux/clk-provider.h>
#include <linux/rational.h>

#include "ccu_ddn.h"

/*
 * DDN stands for "Divider Denominator Numerator", it's M/N clock with a
 * constant x2 factor. This clock hardware follows the equation below,
 *
 *	      numerator       Fin
 *	2 * ------------- = -------
 *	     denominator      Fout
 *
 * Thus, Fout could be calculated with,
 *
 *		Fin	denominator
 *	Fout = ----- * -------------
 *		 2	 numerator
 */

static unsigned long clk_ddn_calc_best_rate(struct ccu_ddn *ddn,
					    unsigned long rate, unsigned long prate,
					    unsigned long *num, unsigned long *den)
{
	rational_best_approximation(rate, prate / 2,
				    ddn->den_mask, ddn->num_mask,
				    den, num);
	return prate / 2 * *den / *num;
}

static long clk_ddn_round_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *prate)
{
	struct ccu_ddn *ddn = hw_to_ccu_ddn(hw);
	unsigned long num = 0, den = 0;

	return clk_ddn_calc_best_rate(ddn, rate, *prate, &num, &den);
}

static unsigned long clk_ddn_recalc_rate(struct clk_hw *hw, unsigned long prate)
{
	struct ccu_ddn *ddn = hw_to_ccu_ddn(hw);
	unsigned int val, num, den;

	ccu_read(ctrl, &ddn->common, &val);

	num = (val & ddn->num_mask) >> ddn->num_shift;
	den = (val & ddn->den_mask) >> ddn->den_shift;

	return prate / 2 * den / num;
}

static int clk_ddn_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long prate)
{
	struct ccu_ddn *ddn = hw_to_ccu_ddn(hw);
	unsigned long num, den;

	clk_ddn_calc_best_rate(ddn, rate, prate, &num, &den);

	ccu_update(ctrl, &ddn->common,
		   ddn->num_mask | ddn->den_mask,
		   (num << ddn->num_shift) | (den << ddn->den_shift));

	return 0;
}

const struct clk_ops spacemit_ccu_ddn_ops = {
	.recalc_rate	= clk_ddn_recalc_rate,
	.round_rate	= clk_ddn_round_rate,
	.set_rate	= clk_ddn_set_rate,
};
