// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ambarella, Inc.
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/math64.h>
#include <linux/rational.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "ccu_pll.h"

#define AMB_PLL_MAX_SOUT	16UL
#define AMB_PLL_MAX_SDIV	16UL

struct amb_pll {
	struct clk_hw hw;
	struct regmap *map;
	u32 reg_offset[REG_NUM];
	const struct amb_pll_soc_data *soc_data;
	u32 fix_divider;
	bool frac_mode;
};

#define to_amb_pll(_hw) container_of(_hw, struct amb_pll, hw)

static void amb_pll_write_enable(struct regmap *map, u32 offset, u32 val)
{
	regmap_write(map, offset, val);
	regmap_write(map, offset, val | CTRL_WRITE_ENABLE);
	regmap_write(map, offset, val);
}

static unsigned long amb_pll_calc_vco(struct amb_pll *pll,
				      unsigned long parent_rate)
{
	const struct amb_pll_soc_data *soc_data = pll->soc_data;
	u32 *reg = pll->reg_offset;
	u32 pre_scaler = 1;
	u32 ctrl_val, ctrl2_val = 0, frac_val;
	u32 intp, sdiv, vcodiv, fsdiv;
	u64 frac = 0, vco;

	if (reg[PRES_OFFSET]) {
		regmap_read(pll->map, reg[PRES_OFFSET], &pre_scaler);
		pre_scaler = (pre_scaler >> 4) + 1;
	}

	regmap_read(pll->map, reg[CTRL_OFFSET], &ctrl_val);
	intp = ((ctrl_val >> 24) & 0x7f) + 1;
	sdiv = ((ctrl_val >> 12) & 0xf) + 1;

	if (soc_data->pll_version >= 2) {
		vcodiv = (ctrl_val & soc_data->vcodiv_mask) ==
			 soc_data->vcodiv_val ? 2 : 1;
		fsdiv = (ctrl_val & soc_data->fsdiv_mask) ==
			soc_data->fsdiv_val ? 2 : 1;
	} else {
		regmap_read(pll->map, reg[CTRL2_OFFSET], &ctrl2_val);
		vcodiv = (ctrl2_val & soc_data->vcodiv_mask) ==
			 soc_data->vcodiv_val ? 2 : 1;
		fsdiv = (ctrl2_val & soc_data->fsdiv_mask) ==
			soc_data->fsdiv_val ? 2 : 1;
	}

	vco = (u64)parent_rate * vcodiv * fsdiv * intp * sdiv;
	vco = div_u64(vco, pre_scaler);

	if (ctrl_val & CTRL_FRAC_MODE) {
		regmap_read(pll->map, reg[FRAC_OFFSET], &frac_val);
		frac = (u64)parent_rate * vcodiv * fsdiv * sdiv * frac_val;
		frac = div_u64(frac, pre_scaler) >> 32;
	}

	return vco + frac;
}

static unsigned long amb_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct amb_pll *pll = to_amb_pll(hw);
	const struct amb_pll_soc_data *soc_data = pll->soc_data;
	u32 *reg = pll->reg_offset;
	u32 pre_scaler = 1, post_scaler = 1;
	u32 ctrl_val, ctrl2_val = 0;
	u32 vcodiv, fsout, sout;
	u64 rate;

	regmap_read(pll->map, reg[CTRL_OFFSET], &ctrl_val);
	if (ctrl_val & (CTRL_POWER_DOWN | CTRL_HALT_VCO | CTRL_FORCE_RESET))
		return 0;

	if (reg[PRES_OFFSET]) {
		regmap_read(pll->map, reg[PRES_OFFSET], &pre_scaler);
		pre_scaler = (pre_scaler >> 4) + 1;
	}

	if (reg[POST_OFFSET]) {
		regmap_read(pll->map, reg[POST_OFFSET], &post_scaler);
		post_scaler = (post_scaler >> 4) + 1;
	}

	if (ctrl_val & CTRL_BYPASS)
		return parent_rate / pre_scaler / post_scaler;

	if (soc_data->pll_version >= 2) {
		vcodiv = (ctrl_val & soc_data->vcodiv_mask) ==
			 soc_data->vcodiv_val ? 2 : 1;
		fsout = (ctrl_val & soc_data->fsout_mask) ==
			soc_data->fsout_val ? 2 : 1;
	} else {
		regmap_read(pll->map, reg[CTRL2_OFFSET], &ctrl2_val);
		vcodiv = (ctrl2_val & soc_data->vcodiv_mask) ==
			 soc_data->vcodiv_val ? 2 : 1;
		fsout = (ctrl2_val & soc_data->fsout_mask) ==
			soc_data->fsout_val ? 2 : 1;
	}

	sout = ((ctrl_val >> 16) & 0xf) + 1;
	rate = amb_pll_calc_vco(pll, parent_rate);

	if (soc_data->pll_version >= 2) {
		if (!(ctrl_val & CTRL_BYPASS_HSDIV))
			rate = div_u64(rate, vcodiv * fsout * sout);
	} else {
		if (!(ctrl2_val & CTRL2_BYPASS_HSDIV))
			rate = div_u64(rate, vcodiv * fsout * sout);
	}

	rate = div_u64(rate, pll->fix_divider * post_scaler);

	return rate;
}

static int amb_pll_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct amb_pll *pll = to_amb_pll(hw);
	unsigned long half_refclk = req->best_parent_rate / 2;

	if (pll->frac_mode)
		return 0;

	if (!half_refclk)
		return -EINVAL;

	req->rate = roundup(req->rate, half_refclk);

	return 0;
}

static int amb_pll_calc_params(struct amb_pll *pll, unsigned long rate,
			       unsigned long parent_rate, u32 ctrl2_val,
			       u32 *intp, u32 *sdiv, u32 *sout,
			       u32 *vcodiv, u32 *fsdiv, u32 *fsout)
{
	const struct amb_pll_soc_data *soc_data = pll->soc_data;
	unsigned long max_numerator, max_denominator;
	unsigned long intp_ul, sout_ul, rate_tmp;
	u32 ctrl_val;

	*sdiv = 1;

	if (soc_data->pll_version >= 2) {
		ctrl_val = 0;
		*vcodiv = (ctrl_val & soc_data->vcodiv_mask) ==
			  soc_data->vcodiv_val ? 2 : 1;
		*fsdiv = (ctrl_val & soc_data->fsdiv_mask) ==
			 soc_data->fsdiv_val ? 2 : 1;
		*fsout = (ctrl_val & soc_data->fsout_mask) ==
			 soc_data->fsout_val ? 2 : 1;
	} else {
		*vcodiv = (ctrl2_val & soc_data->vcodiv_mask) ==
			  soc_data->vcodiv_val ? 2 : 1;
		*fsdiv = (ctrl2_val & soc_data->fsdiv_mask) ==
			 soc_data->fsdiv_val ? 2 : 1;
		*fsout = (ctrl2_val & soc_data->fsout_mask) ==
			 soc_data->fsout_val ? 2 : 1;
	}

	if (rate < parent_rate)
		return -EINVAL;

	max_numerator = soc_data->vco_max_mhz;
	max_numerator = div_u64(max_numerator * 1000000ULL, parent_rate);
	max_numerator = div_u64(max_numerator, *vcodiv * *fsdiv);
	max_numerator = min(128UL, max_numerator);
	if (!max_numerator)
		return -EINVAL;

	max_denominator = AMB_PLL_MAX_SOUT;
	rate_tmp = rate;
	rational_best_approximation(rate_tmp, parent_rate, max_numerator,
				    max_denominator, &intp_ul, &sout_ul);

	while (parent_rate * *fsdiv * intp_ul * *sdiv / *fsout / sout_ul >
	       rate) {
		unsigned long resolution = parent_rate / AMB_PLL_MAX_SOUT;

		/* Avoid an infinite loop when parent_rate < AMB_PLL_MAX_SOUT. */
		if (!resolution)
			resolution = 1;

		if (rate_tmp <= resolution)
			return -EINVAL;

		rate_tmp -= resolution;
		rational_best_approximation(rate_tmp, parent_rate,
					    max_numerator, max_denominator,
					    &intp_ul, &sout_ul);
	}

	while (parent_rate / 1000000 * *vcodiv * *fsdiv * intp_ul * *sdiv <
	       soc_data->vco_min_mhz) {
		if (sout_ul > 8 || intp_ul > 64)
			break;

		intp_ul *= 2;
		sout_ul *= 2;
	}

	if (intp_ul > max_numerator || sout_ul > max_denominator ||
	    *sdiv > AMB_PLL_MAX_SDIV)
		return -EINVAL;

	*intp = intp_ul;
	*sout = sout_ul;

	return 0;
}

static int amb_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct amb_pll *pll = to_amb_pll(hw);
	const struct amb_pll_soc_data *soc_data = pll->soc_data;
	u32 *reg = pll->reg_offset;
	u32 ctrl_val, ctrl2_val = 0, ctrl3_val, frac_val = 0;
	u32 intp, sdiv, sout, vcodiv, fsdiv, fsout;
	unsigned long old_rate, new_rate, rate_tmp;
	int ret;

	if (!rate) {
		regmap_read(pll->map, reg[CTRL_OFFSET], &ctrl_val);
		ctrl_val |= CTRL_POWER_DOWN | CTRL_HALT_VCO;
		amb_pll_write_enable(pll->map, reg[CTRL_OFFSET], ctrl_val);
		return 0;
	}

	rate *= pll->fix_divider;

	if (soc_data->ctrl2_val)
		ctrl2_val = soc_data->ctrl2_val;
	else
		regmap_read(pll->map, reg[CTRL2_OFFSET], &ctrl2_val);

	ret = amb_pll_calc_params(pll, rate, parent_rate, ctrl2_val,
				  &intp, &sdiv, &sout, &vcodiv, &fsdiv,
				  &fsout);
	if (ret)
		return ret;

	if (soc_data->ctrl2_val)
		regmap_write(pll->map, reg[CTRL2_OFFSET], soc_data->ctrl2_val);

	ctrl_val = ((intp - 1) & 0x7f) << 24;
	ctrl_val |= ((sdiv - 1) & 0xf) << 12;
	ctrl_val |= ((sout - 1) & 0xf) << 16;
	if (soc_data->pll_version >= 2) {
		ctrl_val |= vcodiv == 2 ? soc_data->vcodiv_val : 0;
		ctrl_val |= fsdiv == 2 ? soc_data->fsdiv_val : 0;
		ctrl_val |= fsout == 2 ? soc_data->fsout_val : 0;
	}

	regmap_write(pll->map, reg[CTRL_OFFSET], ctrl_val);
	regmap_write(pll->map, reg[FRAC_OFFSET], 0);

	old_rate = amb_pll_recalc_rate(hw, parent_rate) * pll->fix_divider;
	rate_tmp = old_rate > rate ? 0 : rate - old_rate;
	if (rate_tmp && pll->frac_mode) {
		u64 dividend, divider;

		dividend = (u64)rate_tmp * sout * fsout;
		dividend <<= 32;
		divider = (u64)sdiv * fsdiv * parent_rate;
		frac_val = DIV_ROUND_CLOSEST_ULL(dividend, divider);
		regmap_write(pll->map, reg[FRAC_OFFSET], frac_val);
		ctrl_val |= CTRL_FRAC_MODE;
	}

	if (soc_data->pll_version >= 2) {
		ctrl3_val = soc_data->ctrl3_val;
		regmap_write(pll->map, reg[CTRL3_OFFSET],
			     ctrl3_val | CTRL3_VCO_CLAMP);
		regmap_write(pll->map, reg[CTRL_OFFSET],
			     ctrl_val | CTRL_FORCE_RESET);
		ndelay(100);
		regmap_write(pll->map, reg[CTRL_OFFSET],
			     ctrl_val & ~CTRL_FORCE_RESET);
		ndelay(100);
		regmap_write(pll->map, reg[CTRL3_OFFSET],
			     ctrl3_val & ~CTRL3_VCO_CLAMP);
	} else {
		u32 fvco_mhz, range;

		fvco_mhz = amb_pll_calc_vco(pll, parent_rate) / 1000000UL;
		for (range = 0; range < ARRAY_SIZE(soc_data->vco_range);
		     range++) {
			if (fvco_mhz > soc_data->vco_range[range])
				break;
		}
		range = ARRAY_SIZE(soc_data->vco_range) - range - 1;

		regmap_read(pll->map, reg[CTRL3_OFFSET], &ctrl3_val);
		ctrl3_val &= ~CTRL3_VCO_RANGE_MASK;
		ctrl3_val |= range << 1;
		regmap_write(pll->map, reg[CTRL3_OFFSET], ctrl3_val);

		if (frac_val) {
			ctrl_val |= CTRL_FORCE_RESET;
			amb_pll_write_enable(pll->map, reg[CTRL_OFFSET],
					     ctrl_val);
		}

		ctrl_val &= ~CTRL_FORCE_RESET;
		amb_pll_write_enable(pll->map, reg[CTRL_OFFSET], ctrl_val);
	}

	new_rate = amb_pll_recalc_rate(hw, parent_rate);
	rate_tmp = rate / pll->fix_divider;
	if (max(new_rate, rate_tmp) - min(new_rate, rate_tmp) > 10)
		pr_warn("%s: requested %lu, got %lu\n", clk_hw_get_name(hw),
			rate_tmp, new_rate);

	return 0;
}

static const struct clk_ops amb_pll_ops = {
	.recalc_rate = amb_pll_recalc_rate,
	.determine_rate = amb_pll_determine_rate,
	.set_rate = amb_pll_set_rate,
};

struct clk_hw *amb_pll_register(struct device *dev, struct regmap *map,
				const struct amb_pll_desc *desc)
{
	struct amb_pll *pll;
	struct clk_init_data init = {};
	const struct clk_hw *parent = desc->parent;
	int ret;

	pll = devm_kzalloc(dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);

	pll->map = map;
	memcpy(pll->reg_offset, desc->reg_offset, sizeof(pll->reg_offset));
	pll->soc_data = desc->soc_data;
	pll->fix_divider = 1;
	pll->frac_mode = desc->frac_mode;

	init.name = desc->name;
	init.ops = &amb_pll_ops;
	init.flags = CLK_GET_RATE_NOCACHE | CLK_IS_CRITICAL;
	init.parent_hws = &parent;
	init.num_parents = 1;

	pll->hw.init = &init;

	ret = devm_clk_hw_register(dev, &pll->hw);
	if (ret)
		return ERR_PTR(ret);

	return &pll->hw;
}
