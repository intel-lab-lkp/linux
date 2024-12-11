// SPDX-License-Identifier: GPL-2.0
//
// CS2600  --  CIRRUS LOGIC Fractional-N Clock Synthesizer & Clock Multiplier
//
// Copyright (C) 2024 Cirrus Logic, Inc. and
//                    Cirrus Logic International Semiconductor Ltd.

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include "clk-cs2600.h"

#define hw_to_cs2600_clk(_hw)	container_of(_hw, struct cs2600_clk_hw, hw)

static const struct reg_default cs2600_reg[] = {
	{ CS2600_PLL_CFG1, 0x0080 },
	{ CS2600_PLL_CFG2, 0x0008 },
	{ CS2600_RATIO1_1, 0x0000 },
	{ CS2600_RATIO1_2, 0x0000 },
	{ CS2600_RATIO2_1, 0x0000 },
	{ CS2600_RATIO2_2, 0x0000 },
	{ CS2600_PLL_CFG3, 0x0000 },
	{ CS2600_OUTPUT_CFG1, 0x0000 },
	{ CS2600_OUTPUT_CFG2, 0x0000 },
	{ CS2600_PHASE_ALIGNMENT_CFG1, 0x0000 },
};

static bool cs2600_read_and_write_reg(unsigned int reg)
{
	switch (reg) {
	case CS2600_PLL_CFG1:
	case CS2600_PLL_CFG2:
	case CS2600_RATIO1_1:
	case CS2600_RATIO1_2:
	case CS2600_RATIO2_1:
	case CS2600_RATIO2_2:
	case CS2600_PLL_CFG3:
	case CS2600_OUTPUT_CFG1:
	case CS2600_OUTPUT_CFG2:
	case CS2600_PHASE_ALIGNMENT_CFG1:
	case CS2600_UNLOCK_INDICATORS:
	case CS2600_ERROR_STS:
		return true;
	default:
		return false;
	}
}

static bool cs2600_writeable_reg(struct device *dev, unsigned int reg)
{
	if (reg == CS2600_SW_RESET)
		return true;

	return cs2600_read_and_write_reg(reg);
}

static bool cs2600_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS2600_DEVICE_ID1:
	case CS2600_DEVICE_ID2:
		return true;
	default:
		return cs2600_read_and_write_reg(reg);
	}
}

static bool cs2600_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS2600_UNLOCK_INDICATORS:
	case CS2600_ERROR_STS:
	case CS2600_SW_RESET:
		return true;
	default:
		return false;
	}
}

static bool cs2600_precious_reg(struct device *dev, unsigned int reg)
{
	if (reg == CS2600_SW_RESET)
		return true;

	return false;
}

static const struct regmap_config cs2600_regmap_config = {
	.reg_bits	= 16,
	.val_bits	= 16,
	.max_register	= CS2600_MAX_REGISTER,
	.reg_defaults	= cs2600_reg,
	.num_reg_defaults = ARRAY_SIZE(cs2600_reg),
	.readable_reg	= cs2600_readable_reg,
	.writeable_reg	= cs2600_writeable_reg,
	.volatile_reg	= cs2600_volatile_reg,
	.precious_reg	= cs2600_precious_reg,
	.cache_type	= REGCACHE_MAPLE,
};

static const struct of_device_id cs2600_of_match[] = {
	{ .compatible = "cirrus,cs2600", },
	{}
};
MODULE_DEVICE_TABLE(of, cs2600_of_match);

static const struct i2c_device_id cs2600_id[] = {
	{ "cs2600", },
	{}
};
MODULE_DEVICE_TABLE(i2c, cs2600_id);

static inline void cs2600_set_freeze(struct cs2600 *cs2600)
{
	regmap_set_bits(cs2600->regmap, CS2600_PLL_CFG2, CS2600_FREEZE_EN);
}

static inline void cs2600_clear_freeze(struct cs2600 *cs2600)
{
	regmap_clear_bits(cs2600->regmap, CS2600_PLL_CFG2, CS2600_FREEZE_EN);
}

static inline bool cs2600_is_smart_mode(struct cs2600 *cs2600)
{
	return (cs2600->mode == CS2600_SMART_CLKIN_ONLY_MODE ||
		cs2600->mode == CS2600_SMART_MODE);
}

static int cs2600_ref_clk_bound_rate(struct cs2600 *cs2600,
				     unsigned long rate_in,
				     unsigned long *rate_out)
{
	unsigned int val;

	if (!rate_out)
		return -EINVAL;

	if (in_range(rate_in, 32000000, 75000000-32000000)) {
		val = 0x0;
		*rate_out = rate_in / 4;
	} else if (in_range(rate_in, 16000000, 37500000-16000000)) {
		val = 0x1;
		*rate_out = rate_in / 2;
	} else if (in_range(rate_in, 8000000, 18500000-8000000)) {
		val = 0x2;
		*rate_out = rate_in;
	} else {
		dev_err(cs2600->dev, "Invalid refclk %lu\n", rate_in);
		return -EINVAL;
	}

	return regmap_update_bits(cs2600->regmap, CS2600_PLL_CFG3,
				  CS2600_REF_CLK_IN_DIV_MASK,
				  CS2600_REF_CLK_IN_DIV(val));
}

static unsigned long cs2600_get_ratio(struct cs2600 *cs2600)
{
	unsigned int val, reg = CS2600_RATIO2_1;
	struct regmap *regmap = cs2600->regmap;
	unsigned long ratio;

	switch (cs2600->mode) {
	case CS2600_SMART_MODE:
		reg = CS2600_RATIO1_1;
		break;
	case CS2600_SMART_CLKIN_ONLY_MODE:
		break;
	default:
		if (regmap_test_bits(regmap, CS2600_PLL_CFG2,
				     CS2600_PLL_MODE_SEL))
			reg = CS2600_RATIO1_1;

		break;
	}

	regmap_read(regmap, reg, &val);
	ratio = (val & GENMASK(15, 0)) << 16;
	regmap_read(regmap, reg + 2, &val);
	ratio |= val & GENMASK(15, 0);

	return ratio;
}

static unsigned long cs2600_pll_out_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned long new_rate;
	u64 fout;

	if (!regmap_test_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_MODE_SEL) &&
	    !cs2600_is_smart_mode(cs2600)) {
		if (cs2600_ref_clk_bound_rate(cs2600, parent_rate, &new_rate))
			return 0; /* Invalid Parent Rate */

		fout = cs2600_get_ratio(cs2600) * new_rate;
		return (fout >> CS2600_12_20_SHIFT);

	} else {
		fout = cs2600_get_ratio(cs2600) * parent_rate;
		if (regmap_test_bits(regmap, CS2600_PLL_CFG3, CS2600_RATIO_CFG))
			return (fout >> CS2600_12_20_SHIFT);
		else
			return (fout >> CS2600_20_12_SHIFT);
	}
}

static unsigned long cs2600_get_freq_variation(struct cs2600 *cs2600,
					       unsigned long freq,
					       unsigned long ppm)
{
	unsigned long freq_var;

	/* Get frequency variation from parts per million */
	freq_var = (freq * ppm) / 1000000;
	if ((freq * ppm) % 1000000)
		freq_var++;

	return freq_var;
}

static int cs2600_pll_out_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned long ppm = CS2600_12_20_PPM;
	unsigned long mod, freq_var;
	bool clk_in;

	if (req->rate < CS2600_CLK_OUT_MIN || req->rate > CS2600_CLK_OUT_MAX) {
		dev_err(cs2600->dev, "Invalid request clock %lu\n", req->rate);
		return -EINVAL;
	}

	clk_in = (regmap_test_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_MODE_SEL)
		  || cs2600_is_smart_mode(cs2600));

	if (clk_in && (req->rate / req->best_parent_rate) > CS2600_HI_RES_MAX)
		ppm = CS2600_20_12_PPM;

	/* Calculate the frequency variation with the PPM resolution */
	freq_var = cs2600_get_freq_variation(cs2600, req->rate, ppm);

	mod = req->rate % freq_var;
	if (mod)
		req->rate = req->rate + freq_var - mod;

	return 0;
}

static int cs2600_set_synth_ratio(struct cs2600 *cs2600, unsigned long rate,
				  unsigned long parent_rate,
				  unsigned int *ratio)
{
	u64 out_rate = (u64)rate;
	unsigned long pll_rate;
	int ret;

	ret = cs2600_ref_clk_bound_rate(cs2600, parent_rate, &pll_rate);
	if (ret) {
		dev_err(cs2600->dev, "Cannot set refclk %d", ret);
		return ret;
	}

	if ((rate / pll_rate) > CS2600_PLL_MULTIPLY_MAX) {
		dev_err(cs2600->dev, "Ratio is too large %lu\n",
			rate / pll_rate);
		return -EINVAL;
	}

	*ratio = (out_rate << CS2600_12_20_SHIFT) / pll_rate;

	return 0;
}

static int cs2600_set_multi_ratio(struct cs2600 *cs2600, unsigned long rate,
				  unsigned long parent_rate,
				  unsigned int *ratio)
{
	struct regmap *regmap = cs2600->regmap;
	u64 product = (u64)rate;

	if ((rate / parent_rate) > CS2600_PLL_MULTIPLY_MAX) {
		dev_err(cs2600->dev, "Ratio is too large %lu\n",
			rate / parent_rate);
		return -EINVAL;
	}

	if ((rate / parent_rate) > CS2600_HI_RES_MAX) {
		regmap_clear_bits(regmap, CS2600_PLL_CFG3, CS2600_RATIO_CFG);
		*ratio = (product << CS2600_20_12_SHIFT) / parent_rate;

	} else {
		regmap_set_bits(regmap, CS2600_PLL_CFG3, CS2600_RATIO_CFG);
		*ratio = (product << CS2600_12_20_SHIFT) / parent_rate;
	}

	return 0;
}

static void cs2600_set_ratio_slot(struct cs2600 *cs2600)
{
	struct regmap *regmap = cs2600->regmap;
	unsigned int s_slot, m_slot;

	switch (cs2600->mode) {
	case CS2600_SMART_MODE:
		s_slot = 2;
		m_slot = 1;
		break;
	case CS2600_SMART_CLKIN_ONLY_MODE:
		s_slot = 1;
		m_slot = 2;
		break;
	default:
		if (!regmap_test_bits(regmap, CS2600_PLL_CFG2,
				      CS2600_PLL_MODE_SEL))
			s_slot = m_slot = 2;
		else
			s_slot = m_slot = 1;
	}

	cs2600_set_freeze(cs2600);

	regmap_update_bits(regmap, CS2600_PLL_CFG1, CS2600_S_RATIO_SEL_MASK,
			   CS2600_S_RATIO_SEL(s_slot));
	regmap_update_bits(regmap, CS2600_PLL_CFG2, CS2600_M_RATIO_SEL_MASK,
			   CS2600_M_RATIO_SEL(m_slot));

	cs2600_clear_freeze(cs2600);
}

static int cs2600_pll_out_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned int ratio, reg;
	int ret;

	switch (cs2600->mode) {
	case CS2600_SMART_MODE:
		ret = cs2600_set_synth_ratio(cs2600, rate, cs2600->refclk_rate,
					     &ratio);
		if (ret)
			return ret;

		cs2600_set_ratio_slot(cs2600);
		regmap_write(regmap, CS2600_RATIO2_1, ratio >> 16);
		regmap_write(regmap, CS2600_RATIO2_2, ratio & 0xFFFF);

		ret = cs2600_set_multi_ratio(cs2600, rate, parent_rate, &ratio);
		if (ret)
			return ret;

		regmap_write(regmap, CS2600_RATIO1_1, ratio >> 16);
		return regmap_write(regmap, CS2600_RATIO1_2, ratio & 0xFFFF);
	case CS2600_SMART_CLKIN_ONLY_MODE:
		ret = cs2600_set_multi_ratio(cs2600, rate, parent_rate, &ratio);
		if (ret)
			return ret;

		cs2600_set_ratio_slot(cs2600);
		regmap_write(regmap, CS2600_RATIO2_1, ratio >> 16);
		return regmap_write(regmap, CS2600_RATIO2_2, ratio & 0xFFFF);
	default:
		if (!regmap_test_bits(regmap, CS2600_PLL_CFG2,
				      CS2600_PLL_MODE_SEL)) {
			ret = cs2600_set_synth_ratio(cs2600, rate,
						     cs2600->refclk_rate,
						     &ratio);
			if (ret)
				return ret;

			reg = CS2600_RATIO2_1;
		} else {
			ret = cs2600_set_multi_ratio(cs2600, rate, parent_rate,
						     &ratio);
			if (ret)
				return ret;

			reg = CS2600_RATIO1_1;
		}

		cs2600_set_ratio_slot(cs2600);
		regmap_write(regmap, reg, ratio >> 16);
		return regmap_write(regmap, reg + 2, ratio & 0xFFFF);
	}
}

static int cs2600_pll_out_is_prepared(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;

	return regmap_test_bits(regmap, CS2600_PLL_CFG1, CS2600_PLL_EN1);
}


static int cs2600_pll_out_prepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	int i;

	if (regmap_test_bits(regmap, CS2600_PLL_CFG1, CS2600_PLL_EN1))
		return 0;

	cs2600_set_freeze(cs2600);
	regmap_set_bits(regmap, CS2600_PLL_CFG1, CS2600_PLL_EN1);
	cs2600_clear_freeze(cs2600);
	regmap_set_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_EN2);

	regmap_write(regmap, CS2600_UNLOCK_INDICATORS, CS2600_CLEAR_INDICATORS);

	usleep_range(1000, 1100);
	for (i = 0; i < CS2600_LOCK_ATTEMPTS_MAX; i++) {
		if (!regmap_test_bits(regmap, CS2600_UNLOCK_INDICATORS,
				      CS2600_F_UNLOCK_STICKY))
			return 0;

		regmap_write(regmap, CS2600_UNLOCK_INDICATORS,
			     CS2600_CLEAR_INDICATORS);
		usleep_range(200, 300);
	}

	cs2600_set_freeze(cs2600);
	regmap_clear_bits(regmap, CS2600_PLL_CFG1, CS2600_PLL_EN1);
	cs2600_clear_freeze(cs2600);
	regmap_clear_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_EN2);

	dev_err(cs2600->dev, "PLL did not lock\n");
	return -ETIMEDOUT;
}


static void cs2600_pll_out_unprepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	cs2600_set_freeze(cs2600);
	regmap_clear_bits(regmap, CS2600_PLL_CFG1, CS2600_PLL_EN1);
	cs2600_clear_freeze(cs2600);
	regmap_clear_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_EN2);
}

static int cs2600_pll_out_set_parent(struct clk_hw *hw, u8 index)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	/*CLK_IN is always the parent in smart mode */
	if (cs2600_is_smart_mode(cs2600))
		return 0;

	if (index > CS2600_CLK_IN_INPUT) {
		dev_err(cs2600->dev, "Invalid parent index %d", index);
		return -EINVAL;
	}

	cs2600_set_freeze(cs2600);
	regmap_update_bits(regmap, CS2600_PLL_CFG2,
			   CS2600_PLL_MODE_SEL, index);
	cs2600_clear_freeze(cs2600);

	return 0;
}

static u8 cs2600_pll_out_get_parent(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	if (cs2600_is_smart_mode(cs2600))
		return CS2600_CLK_IN_INPUT;

	return (u8)regmap_test_bits(regmap, CS2600_PLL_CFG2,
				    CS2600_PLL_MODE_SEL);
}

static int cs2600_clk_out_prepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	cs2600_set_freeze(cs2600);
	regmap_clear_bits(regmap, CS2600_PLL_CFG1, CS2600_CLK_OUT_DIS);
	cs2600_clear_freeze(cs2600);

	return 0;
}

static void cs2600_clk_out_unprepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	cs2600_set_freeze(cs2600);
	regmap_set_bits(regmap, CS2600_PLL_CFG1, CS2600_CLK_OUT_DIS);
	cs2600_clear_freeze(cs2600);
}

static int cs2600_clk_out_is_prepared(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;

	return !regmap_test_bits(regmap, CS2600_PLL_CFG1, CS2600_CLK_OUT_DIS);
}

struct cs2600_clk_div {
	int div;
	unsigned int val;
};

static const struct cs2600_clk_div cs2600_bclk_div[] = {
	{ 1, CS2600_BCLK_DIV(0x0) },
	{ 2, CS2600_BCLK_DIV(0x1) },
	{ 3, CS2600_BCLK_DIV(0x2) },
	{ 4, CS2600_BCLK_DIV(0x3) },
	{ 6, CS2600_BCLK_DIV(0x4) },
	{ 8, CS2600_BCLK_DIV(0x5) },
	{ 12, CS2600_BCLK_DIV(0x6) },
	{ 16, CS2600_BCLK_DIV(0x7) },
	{ 24, CS2600_BCLK_DIV(0x8) },
	{ 32, CS2600_BCLK_DIV(0x9) },
	{ 48, CS2600_BCLK_DIV(0xA) }
};

static int cs2600_bclk_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned long ppm = CS2600_12_20_PPM;
	unsigned long calc_rate, freq_var;
	int i;

	if (rate > CS2600_BCLK_OUT_MAX || rate < CS2600_BCLK_OUT_MIN) {
		dev_err(cs2600->dev, "Invalid bclk rate %lu", rate);
		return -EINVAL;
	}

	if (parent_rate > CS2600_CLK_OUT_MAX ||
	    parent_rate < CS2600_CLK_OUT_MIN) {
		dev_err(cs2600->dev, "Invalid parent rate %lu", parent_rate);
		return -EINVAL;
	}

	if (regmap_test_bits(regmap, CS2600_PLL_CFG3, CS2600_RATIO_CFG))
		ppm = CS2600_20_12_PPM;

	freq_var = cs2600_get_freq_variation(cs2600, rate, ppm);
	for (i = 0; i < ARRAY_SIZE(cs2600_bclk_div); i++) {
		calc_rate = parent_rate / cs2600_bclk_div[i].div;
		if (calc_rate >= (rate - freq_var) &&
		    calc_rate <= (rate + freq_var)) {
			return regmap_update_bits(regmap, CS2600_OUTPUT_CFG1,
						  CS2600_BCLK_DIV_MASK,
						  cs2600_bclk_div[i].val);
		}
	}

	dev_err(cs2600->dev, "Cannot set bclk %lu from parent rate %lu\n", rate,
		parent_rate);
	return -EINVAL;
}

static int cs2600_bclk_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned long parent_rate;
	int i;

	if (req->rate > CS2600_BCLK_OUT_MAX ||
	    req->rate < CS2600_BCLK_OUT_MIN) {
		dev_err(cs2600->dev, "Invalid bclk rate %lu", req->rate);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(cs2600_bclk_div); i++) {
		if (req->rate == (req->best_parent_rate /
				  cs2600_bclk_div[i].div))
			return 0;
	}

	/* Change parent rate to accommodate the BCLK rate */

	for (i = 0; i < ARRAY_SIZE(cs2600_bclk_div); i++) {
		parent_rate = req->rate * cs2600_bclk_div[i].div;
		if (parent_rate > CS2600_CLK_OUT_MAX ||
		    parent_rate < CS2600_CLK_OUT_MIN)
			continue;

		req->best_parent_rate = parent_rate;
		return 0;
	}

	dev_err(cs2600->dev, "Cannot determine parent rate\n");
	return -EINVAL;
}

static unsigned long cs2600_bclk_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned int val;
	int i;

	regmap_read(regmap, CS2600_OUTPUT_CFG1, &val);

	for (i = 0; i < ARRAY_SIZE(cs2600_bclk_div); i++) {
		if (cs2600_bclk_div[i].val == (val & CS2600_BCLK_DIV_MASK))
			return (parent_rate / cs2600_bclk_div[i].div);
	}

	dev_err(cs2600->dev, "Cannot find bclk divide value\n");
	return 0;
}

static int cs2600_bclk_prepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	regmap_set_bits(regmap, CS2600_PHASE_ALIGNMENT_CFG1,
			CS2600_PHASE_ALIGN_EN | CS2600_PHASE_ALIGN_STB_EN);

	cs2600_set_freeze(cs2600);
	regmap_clear_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_BCLK_OUT_DIS);
	cs2600_clear_freeze(cs2600);

	return 0;
}

static void cs2600_bclk_unprepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	if (regmap_test_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_FSYNC_OUT_DIS))
		regmap_clear_bits(regmap, CS2600_PHASE_ALIGNMENT_CFG1,
				  CS2600_PHASE_ALIGN_EN |
				  CS2600_PHASE_ALIGN_STB_EN);

	cs2600_set_freeze(cs2600);
	regmap_set_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_BCLK_OUT_DIS);
	cs2600_clear_freeze(cs2600);
}

static int cs2600_bclk_is_prepared(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;

	return !regmap_test_bits(regmap, CS2600_OUTPUT_CFG1,
				 CS2600_BCLK_OUT_DIS);
}

static const struct cs2600_clk_div cs2600_fsync_div[] = {
	{ 16, CS2600_FSYNC_DIV(0x0) },
	{ 32, CS2600_FSYNC_DIV(0x1) },
	{ 64, CS2600_FSYNC_DIV(0x2) },
	{ 128, CS2600_FSYNC_DIV(0x3) },
	{ 256, CS2600_FSYNC_DIV(0x4) },
	{ 512, CS2600_FSYNC_DIV(0x5) },
	{ 1024, CS2600_FSYNC_DIV(0x6) },
	{ 192, CS2600_FSYNC_DIV(0x7) },
	{ 384, CS2600_FSYNC_DIV(0x8) },
	{ 768, CS2600_FSYNC_DIV(0x9) },
	{ 1536, CS2600_FSYNC_DIV(0xA) },
	{ 576, CS2600_FSYNC_DIV(0xB) },
	{ 1152, CS2600_FSYNC_DIV(0xC) }
};

static int cs2600_fsync_set_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned long ppm = CS2600_12_20_PPM;
	unsigned long calc_rate, freq_var;
	int i;

	if (rate > CS2600_FSYNC_OUT_MAX || rate < CS2600_FSYNC_OUT_MIN) {
		dev_err(cs2600->dev, "Invalid fsync rate %lu", rate);
		return -EINVAL;
	}

	if (parent_rate > CS2600_CLK_OUT_MAX
	    || parent_rate < CS2600_CLK_OUT_MIN) {
		dev_err(cs2600->dev, "Invalid parent rate %lu", parent_rate);
		return -EINVAL;
	}

	if (regmap_test_bits(regmap, CS2600_PLL_CFG3, CS2600_RATIO_CFG))
		ppm = CS2600_20_12_PPM;

	freq_var = cs2600_get_freq_variation(cs2600, rate, ppm);
	for (i = 0; i < ARRAY_SIZE(cs2600_fsync_div); i++) {
		calc_rate = parent_rate / cs2600_fsync_div[i].div;
		if (calc_rate >= (rate - freq_var) &&
		    calc_rate <= (rate + freq_var)) {
			return regmap_update_bits(regmap, CS2600_OUTPUT_CFG1,
						  CS2600_FSYNC_DIV_MASK,
						  cs2600_fsync_div[i].val);
		}
	}

	dev_err(cs2600->dev, "Cannot set fsync %lu from parent rate %lu\n", rate,
		parent_rate);
	return -EINVAL;
}

static int cs2600_fsync_determine_rate(struct clk_hw *hw,
				       struct clk_rate_request *req)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned long parent_rate;
	int i;

	if (req->rate > CS2600_FSYNC_OUT_MAX
	    || req->rate < CS2600_FSYNC_OUT_MIN) {
		dev_err(cs2600->dev, "Invalid fsync rate %lu", req->rate);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(cs2600_fsync_div); i++) {
		if (req->rate == (req->best_parent_rate /
				  cs2600_fsync_div[i].div))
			return 0;
	}

	/* Change parent rate to accommodate the FSYNC rate */

	for (i = 0; i < ARRAY_SIZE(cs2600_fsync_div); i++) {
		parent_rate = req->rate * cs2600_fsync_div[i].div;
		if (parent_rate > CS2600_CLK_OUT_MAX ||
		    parent_rate < CS2600_CLK_OUT_MIN)
			continue;

		req->best_parent_rate = parent_rate;
		return 0;
	}

	dev_err(cs2600->dev, "Cannot determine parent rate\n");
	return -EINVAL;
}

static unsigned long cs2600_fsync_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned int val;
	int i;

	regmap_read(regmap, CS2600_OUTPUT_CFG1, &val);

	for (i = 0; i < ARRAY_SIZE(cs2600_fsync_div); i++) {
		if (cs2600_fsync_div[i].val == (val & CS2600_FSYNC_DIV_MASK))
			return (parent_rate / cs2600_fsync_div[i].div);
	}

	dev_err(cs2600->dev, "Cannot find fsync divide value\n");
	return 0;
}

static int cs2600_fsync_prepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	regmap_set_bits(regmap, CS2600_PHASE_ALIGNMENT_CFG1,
			CS2600_PHASE_ALIGN_EN | CS2600_PHASE_ALIGN_STB_EN);

	cs2600_set_freeze(cs2600);
	regmap_clear_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_FSYNC_OUT_DIS);
	cs2600_clear_freeze(cs2600);

	return 0;
}

static void cs2600_fsync_unprepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	if (regmap_test_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_BCLK_OUT_DIS))
		regmap_clear_bits(regmap, CS2600_PHASE_ALIGNMENT_CFG1,
				  CS2600_PHASE_ALIGN_EN |
				  CS2600_PHASE_ALIGN_STB_EN);

	cs2600_set_freeze(cs2600);
	regmap_set_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_FSYNC_OUT_DIS);
	cs2600_clear_freeze(cs2600);
}

static int cs2600_fsync_is_prepared(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;

	return !regmap_test_bits(regmap, CS2600_OUTPUT_CFG1,
				 CS2600_FSYNC_OUT_DIS);
}

struct cs2600_clk {
	const char *name;
	const struct clk_ops ops;
	unsigned long flags;
};

static const struct cs2600_clk cs2600_clks[CS2600_OUT_CLK_MAX] = {
	[CS2600_PLL_OUT] = { /* Internal clock */
		.name = "pll_out",
		.ops = {
			.get_parent	= cs2600_pll_out_get_parent,
			.set_parent	= cs2600_pll_out_set_parent,
			.recalc_rate	= cs2600_pll_out_recalc_rate,
			.determine_rate = cs2600_pll_out_determine_rate,
			.set_rate	= cs2600_pll_out_set_rate,
			.prepare	= cs2600_pll_out_prepare,
			.is_prepared	= cs2600_pll_out_is_prepared,
			.unprepare	= cs2600_pll_out_unprepare,
		},
		.flags = 0,
	},
	[CS2600_CLK_OUT] = {
		.name = "clk_out",
		.ops = {
			.prepare	= cs2600_clk_out_prepare,
			.unprepare	= cs2600_clk_out_unprepare,
			.is_prepared	= cs2600_clk_out_is_prepared,
		},
		.flags = CLK_SET_RATE_PARENT,
	},
	[CS2600_BCLK_OUT] = {
		.name = "bclk_out",
		.ops = {
			.recalc_rate	= cs2600_bclk_recalc_rate,
			.determine_rate = cs2600_bclk_determine_rate,
			.set_rate	= cs2600_bclk_set_rate,
			.prepare	= cs2600_bclk_prepare,
			.unprepare	= cs2600_bclk_unprepare,
			.is_prepared	= cs2600_bclk_is_prepared,

		},
		.flags = CLK_SET_RATE_PARENT,
	},
	[CS2600_FSYNC_OUT] = {
		.name = "fsync_out",
		.ops = {
			.recalc_rate	= cs2600_fsync_recalc_rate,
			.determine_rate = cs2600_fsync_determine_rate,
			.set_rate	= cs2600_fsync_set_rate,
			.prepare	= cs2600_fsync_prepare,
			.unprepare	= cs2600_fsync_unprepare,
			.is_prepared	= cs2600_fsync_is_prepared,

		},
		.flags = CLK_SET_RATE_PARENT,
	},
};

static int cs2600_clk_get(struct cs2600 *cs2600)
{
	struct clk *clk_in, *ref_clk;

	clk_in = devm_clk_get_optional(cs2600->dev, "clk_in");
	if (IS_ERR(clk_in))
		return PTR_ERR(clk_in);

	ref_clk = devm_clk_get_optional(cs2600->dev, "ref_clk");
	if (IS_ERR(ref_clk))
		return PTR_ERR(ref_clk);

	cs2600->ref_clk = ref_clk;
	cs2600->clk_in = clk_in;

	return 0;
}

static struct clk_hw *cs2600_of_clk_get(struct of_phandle_args *clkspec,
					void *data)
{
	unsigned int idx = clkspec->args[0];
	struct cs2600 *cs2600 = data;

	/* PLL_OUT is an internal clock */
	if (idx > CS2600_OUT_CLK_MAX - 1) {
		dev_err(cs2600->dev, "Invalid clock index %d\n", idx);
		return ERR_PTR(-EINVAL);
	}

	return &cs2600->hw[idx].hw;
}

static int cs2600_clk_register(struct cs2600 *cs2600)
{
	struct device_node *np = cs2600->dev->of_node;
	static const char *parent_names[2];
	int ret, i, input_num = 0;
	const char *name;

	if (cs2600->ref_clk)
		parent_names[input_num++] = __clk_get_name(cs2600->ref_clk);

	if (cs2600->clk_in)
		parent_names[input_num++] = __clk_get_name(cs2600->clk_in);

	cs2600->hw[CS2600_PLL_OUT].init.name = cs2600_clks[CS2600_PLL_OUT].name;
	cs2600->hw[CS2600_PLL_OUT].init.ops = &cs2600_clks[CS2600_PLL_OUT].ops;
	cs2600->hw[CS2600_PLL_OUT].init.parent_names = parent_names;
	cs2600->hw[CS2600_PLL_OUT].init.num_parents = input_num;
	cs2600->hw[CS2600_PLL_OUT].hw.init = &cs2600->hw[CS2600_PLL_OUT].init;
	cs2600->hw[CS2600_PLL_OUT].priv = cs2600;

	ret = devm_clk_hw_register(cs2600->dev, &cs2600->hw[CS2600_PLL_OUT].hw);
	if (ret)
		return ret;

	for (i = 1; i < ARRAY_SIZE(cs2600->hw); i++) {
		if (of_property_read_string_index(np, "clock-output-names",
		    i - 1, &name))
			name = cs2600_clks[i].name;

		cs2600->hw[i].init.name = name;
		cs2600->hw[i].init.ops = &cs2600_clks[i].ops;

		/* CLK_OUT is the only parent for BCLK_OUT and FSYNC_OUT */
		cs2600->hw[i].init.parent_names =
					&cs2600->hw[CS2600_PLL_OUT].init.name;
		cs2600->hw[i].init.num_parents = 1;
		cs2600->hw[i].init.flags = cs2600_clks[i].flags;
		cs2600->hw[i].hw.init = &cs2600->hw[i].init;
		cs2600->hw[i].priv = cs2600;

		ret = devm_clk_hw_register(cs2600->dev, &cs2600->hw[i].hw);
		if (ret)
			return ret;
	}

	return devm_of_clk_add_hw_provider(cs2600->dev, cs2600_of_clk_get,
					   cs2600);
}

static int cs2600_set_mode(struct cs2600 *cs2600, unsigned int val)
{
	switch (val) {
	case CS2600_SMART_MODE:
	case CS2600_SMART_CLKIN_ONLY_MODE:
		cs2600->mode = val;
		return 0;
	default:
		dev_err(cs2600->dev, "Invalid clock mode set %d\n", val);
		return -EINVAL;
	}
}

static int cs2600_set_aux_output(struct cs2600 *cs2600, unsigned int aux_out)
{
	unsigned int val;

	switch (aux_out) {
	case CS2600_AUX_OUT_FREQ_UNLOCK:
		val = CS2600_AUX_OUT_FREQ_UNLOCK_VAL;
		break;
	case CS2600_AUX_OUT_PHASE_UNLOCK:
		val = CS2600_AUX_OUT_PHASE_UNLOCK_VAL;
		break;
	case CS2600_AUX_OUT_NO_CLKIN:
		val = CS2600_AUX_OUT_NO_CLKIN_VAL;
		break;
	default:
		return dev_err_probe(cs2600->dev, -EINVAL,
				     "Invalid aux output %d\n", aux_out);
	}

	return regmap_update_bits(cs2600->regmap, CS2600_OUTPUT_CFG2,
				  CS2600_AUX1OUT_SEL, CS2600_AUXOUT1_SRC(val));
}

static int cs2600_parse_dt_params(struct cs2600 *cs2600)
{
	struct regmap *regmap = cs2600->regmap;
	struct device *dev = cs2600->dev;
	unsigned int val;
	int ret = 0;

	cs2600_set_freeze(cs2600);
	if (!device_property_read_u32(dev, "cirrus,aux-output-source", &val)) {
		ret = cs2600_set_aux_output(cs2600, val);
		if (ret) {
			cs2600_clear_freeze(cs2600);
			return ret;
		}
	} else {
		/* Set to HiZ if not used */
		regmap_set_bits(regmap, CS2600_PLL_CFG1, CS2600_AUX1_OUT_DIS);
	}

	if (!device_property_read_u32(dev, "cirrus,clock-mode", &val))
		ret = cs2600_set_mode(cs2600, val);

	cs2600_clear_freeze(cs2600);

	return ret;
}

static int cs2600_check_device_id(struct cs2600 *cs2600)
{
	struct device *dev = cs2600->dev;
	unsigned int dev_id, rev;
	int ret;

	ret = regmap_read(cs2600->regmap, CS2600_DEVICE_ID1, &dev_id);
	if (ret)
		return dev_err_probe(dev, ret, "Can't read device ID\n");

	if (dev_id != CS2600_DEVICE_ID_VALUE)
		return dev_err_probe(dev, -ENODEV, "Invalid device id 0x%x\n",
				     dev_id);

	ret = regmap_read(cs2600->regmap, CS2600_DEVICE_ID2, &rev);
	if (ret)
		return dev_err_probe(dev, ret, "Can't read device revision\n");

	dev_dbg(dev, "Device ID %x Rev %x", dev_id, rev);

	return 0;
}

static int cs2600_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct cs2600 *cs2600;
	int ret;

	cs2600 = devm_kzalloc(dev, sizeof(*cs2600), GFP_KERNEL);
	if (!cs2600)
		return -ENOMEM;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "Error with vdd supply\n");

	cs2600->dev = dev;
	i2c_set_clientdata(client, cs2600);

	cs2600->regmap = devm_regmap_init_i2c(client, &cs2600_regmap_config);
	if (IS_ERR(cs2600->regmap))
		return dev_err_probe(dev, PTR_ERR(cs2600->regmap),
				     "Regmap not created\n");

	/* Required to wait at least 20ms after vdd is enabled */
	usleep_range(20000, 21000);
	ret = cs2600_check_device_id(cs2600);
	if (ret)
		return ret;

	ret = regmap_write(cs2600->regmap, CS2600_SW_RESET, CS2600_SW_RST_VAL);
	if (ret)
		return ret;

	/* Required to wait at least 5ms after software reset */
	usleep_range(5000, 6000);
	ret = cs2600_clk_get(cs2600);
	if (ret)
		return dev_err_probe(dev, ret, "Invalid parent clocks\n");

	/* Set output clocks to HiZ */
	cs2600_set_freeze(cs2600);
	regmap_set_bits(cs2600->regmap, CS2600_PLL_CFG1, CS2600_CLK_OUT_DIS);
	regmap_set_bits(cs2600->regmap, CS2600_OUTPUT_CFG1,
			CS2600_BCLK_OUT_DIS | CS2600_FSYNC_OUT_DIS);
	cs2600_clear_freeze(cs2600);

	ret = cs2600_parse_dt_params(cs2600);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot parse dt params\n");

	ret = cs2600_clk_register(cs2600);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register clocks\n");

	if (cs2600->ref_clk) {
		cs2600->refclk_rate = clk_get_rate(cs2600->ref_clk);
		regmap_update_bits(cs2600->regmap, CS2600_PLL_CFG3,
				   CS2600_SYSCLK_SRC_MASK,
				   CS2600_SYSCLK_SRC_REFCLK);
	} else {
		cs2600->refclk_rate = CS2600_INTERNAL_OSC_RATE;
		regmap_update_bits(cs2600->regmap, CS2600_PLL_CFG3,
				   CS2600_SYSCLK_SRC_MASK,
				   CS2600_SYSCLK_SRC_OSC);
	}

	if (cs2600->refclk_rate < 8000000 || cs2600->refclk_rate > 75000000)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid REFCLK Frequency %lu\n",
				     cs2600->refclk_rate);

	return 0;
}

static struct i2c_driver cs2600_driver = {
	.driver = {
		.name = "cs2600",
		.of_match_table = cs2600_of_match,
	},
	.probe		= cs2600_i2c_probe,
	.id_table	= cs2600_id,
};

module_i2c_driver(cs2600_driver);

MODULE_DESCRIPTION("CS2600 clock driver");
MODULE_AUTHOR("Paul Handrigan <paulha@opensource.cirrus.com>");
MODULE_LICENSE("GPL");
