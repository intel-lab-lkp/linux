// SPDX-License-Identifier: GPL-2.0-only
//
// CS2600  --  CIRRUS LOGIC Fractional-N Clock Synthesizer & Clock Multiplier
//
// Copyright (C) 2024-2026 Cirrus Logic, Inc. and
//			   Cirrus Logic International Semiconductor Ltd.

#include <dt-bindings/clock/cirrus,cs2600-clock.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/string.h>

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
	.reg_stride	= 2,
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

static inline bool cs2600_is_smart_mode(struct cs2600 *cs2600)
{
	return (cs2600->mode == CS2600_SMART_CLKIN_ONLY_MODE ||
		cs2600->mode == CS2600_SMART_MODE);
}

static int cs2600_ref_clk_set_divider(struct cs2600 *cs2600, unsigned long rate_in)
{
	unsigned int val;

	if ((rate_in >= 32000000) && (rate_in <= 75000000)) {
		val = 0x0;
	} else if ((rate_in >= 16000000) && (rate_in <= 37500000)) {
		val = 0x1;
	} else if ((rate_in >= 8000000) && (rate_in <= 18750000)) {
		val = 0x2;
	} else {
		dev_err(cs2600->dev, "Invalid refclk %lu\n", rate_in);
		return -EINVAL;
	}

	return regmap_update_bits(cs2600->regmap, CS2600_PLL_CFG3,
				  CS2600_REF_CLK_IN_DIV_MASK,
				  FIELD_PREP(CS2600_REF_CLK_IN_DIV_MASK, val));
}

static unsigned long cs2600_get_freq_variation(unsigned long freq,
					       unsigned long ppm)
{
	return (unsigned long)DIV_ROUND_UP_ULL((u64)freq * ppm, 1000000);
}

static int cs2600_calc_ratio(struct cs2600 *cs2600,
			     unsigned long rate,
			     unsigned long parent_rate,
			     unsigned int *ratio_out,
			     bool *hi_res)
{
	u64 rate_shifted = (u64)rate;
	u64 ratio;

	if (parent_rate == 0)
		return -EINVAL;

	if ((rate < CS2600_CLK_OUT_MIN) || (rate > CS2600_CLK_OUT_MAX))
		return -EINVAL;

	/* Try high-resolution mode first */
	*hi_res = true;
	rate_shifted <<= CS2600_12_20_SHIFT;
	ratio = div_u64(rate_shifted, parent_rate);

	if (ratio > CS2600_12_20_RATIO_MAX) {
		*hi_res = false;
		ratio >>= (CS2600_12_20_SHIFT - CS2600_20_12_SHIFT);

		if (ratio > CS2600_20_12_RATIO_MAX) {
			dev_err(cs2600->dev, "Ratio %lu:%lu too large\n", parent_rate, rate);
			return -EINVAL;
		}
	}

	*ratio_out = ratio;

	return 0;
}

static unsigned int cs2600_calc_smart_synth_ratio(struct cs2600 *cs2600,
						  unsigned long clkin_rate,
						  unsigned int m_ratio,
						  bool hi_res,
						  unsigned long refclk_rate)
{
	u64 m_rate_32_20 = (u64)clkin_rate * m_ratio;

	/* If low-res adjust 32.12 value to 32.20 */
	if (!hi_res)
		m_rate_32_20 <<= (CS2600_12_20_SHIFT - CS2600_20_12_SHIFT);

	return (unsigned int)div_u64(m_rate_32_20, refclk_rate);
}

static unsigned long cs2600_calc_rounded_integer_rate(struct cs2600 *cs2600,
						      unsigned long parent_rate,
						      unsigned long target_rate)
{
	unsigned int ratio;
	bool hi_res;
	u64 fout;
	int ret;

	ret = cs2600_calc_ratio(cs2600, target_rate, parent_rate, &ratio, &hi_res);
	if (ret)
		return 0;

	fout = (u64)ratio * parent_rate;
	if (hi_res)
		fout >>= CS2600_12_20_SHIFT;
	else
		fout >>= CS2600_20_12_SHIFT;

	/*
	 * If it's fractional round to the next higher integer, so that if
	 * it is passed to set_rate() it will produce the same ratio and
	 * target frequency we just calculated.
	 */
	if (fout != target_rate)
		fout++;

	if (fout < CS2600_CLK_OUT_MIN)
		return 0;

	return fout;
}

static unsigned long cs2600_pll_out_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;

	if ((parent_rate == 0) || (cs2600->pll_target_rate == 0))
		return 0;

	if (parent_rate == cs2600->pll_parent_rate)
		return cs2600->pll_target_rate;

	return cs2600_calc_rounded_integer_rate(cs2600, parent_rate, cs2600->pll_target_rate);
}

static int cs2600_pll_out_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned long out_rate;

	out_rate = cs2600_calc_rounded_integer_rate(cs2600, req->best_parent_rate, req->rate);
	if (out_rate == 0)
		return -EINVAL;

	req->rate = out_rate;

	return 0;
}

static int cs2600_set_resolution_mode(struct cs2600 *cs2600, bool hi_res)
{
	unsigned int val = 0;

	if (hi_res)
		val = CS2600_RATIO_CFG;

	return regmap_update_bits(cs2600->regmap, CS2600_PLL_CFG3, CS2600_RATIO_CFG, val);
}

enum cs2600_ratio_slot {
	CS2600_RATIO_SLOT_1,
	CS2600_RATIO_SLOT_2,
};

static const struct {
	u8 h;
	u8 l;
} cs2600_ratio_regs[] = {
	[CS2600_RATIO_SLOT_1] = { .h = CS2600_RATIO1_1, .l = CS2600_RATIO1_2 },
	[CS2600_RATIO_SLOT_2] = { .h = CS2600_RATIO2_1, .l = CS2600_RATIO2_2 },
};

static int cs2600_write_ratio(struct cs2600 *cs2600, enum cs2600_ratio_slot slot,
			      unsigned int ratio)
{
	struct reg_sequence sequence[] = {
		{ .reg = cs2600_ratio_regs[slot].l, .def = ratio & 0xFFFF },
		{ .reg = cs2600_ratio_regs[slot].h, .def = ratio >> 16 },
	};

	return regmap_multi_reg_write(cs2600->regmap, sequence, ARRAY_SIZE(sequence));
}

static int cs2600_pll_out_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned int ratio, smart_s_ratio;
	bool hi_res;
	int ret;

	ret = cs2600_calc_ratio(cs2600, rate, parent_rate, &ratio, &hi_res);
	if (ret)
		return ret;

	switch (cs2600->mode) {
	case CS2600_SMART_MODE:
		/*
		 * Calc synth ratio to match the actual mult output frequency
		 * to minimise the frequency change when the PLL switches from
		 * synth to mult.
		 */
		smart_s_ratio = cs2600_calc_smart_synth_ratio(cs2600, parent_rate,
							      ratio, hi_res,
							      cs2600->refclk_rate);
		ret = cs2600_write_ratio(cs2600, CS2600_RATIO_SLOT_2, smart_s_ratio);
		if (ret < 0)
			return ret;
		break;
	case CS2600_SMART_CLKIN_ONLY_MODE:
		ret = cs2600_write_ratio(cs2600, CS2600_RATIO_SLOT_2, 0);
		if (ret < 0)
			return ret;
		break;
	default:
		/* Low-resolution ratios can only be used in mult mode */
		ret = regmap_test_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_MODE_SEL);
		if (ret < 0)
			return ret;

		if (!hi_res && !ret)
			return -EINVAL;

		break;
	}

	ret = cs2600_write_ratio(cs2600, CS2600_RATIO_SLOT_1, ratio);
	if (ret < 0)
		return ret;

	ret = cs2600_set_resolution_mode(cs2600, hi_res);
	if (ret < 0)
		return ret;

	cs2600->pll_target_rate = rate;
	cs2600->pll_parent_rate = parent_rate;

	return 0;
}

static int cs2600_pll_out_is_prepared(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	int ret;

	ret = regmap_test_bits(cs2600->regmap, CS2600_PLL_CFG1, CS2600_PLL_EN1);
	if (ret < 0) {
		dev_err(cs2600->dev, "Failed to read PLL_CFG1:%d\n", ret);
		ret = 0;
	}

	return ret;
}

static int cs2600_enable_pll(struct cs2600 *cs2600, bool enable)
{
	int ret;

	ret = regmap_update_bits(cs2600->regmap, CS2600_PLL_CFG1,
				 CS2600_PLL_EN1,
				 FIELD_PREP(CS2600_PLL_EN1, !!enable));
	if (ret < 0)
		return ret;

	return regmap_update_bits(cs2600->regmap, CS2600_PLL_CFG2, CS2600_PLL_EN2,
				  FIELD_PREP(CS2600_PLL_EN2, !!enable));
}

static int cs2600_wait_for_pll_lock(struct cs2600 *cs2600)
{
	int i, ret;

	usleep_range(1000, 1100);
	for (i = 0; i < CS2600_LOCK_ATTEMPTS_MAX; i++) {
		ret = regmap_test_bits(cs2600->regmap, CS2600_UNLOCK_INDICATORS,
				       CS2600_F_UNLOCK_STICKY);
		if (ret < 0)
			return ret;

		if (ret == 0)
			return 0;

		ret = regmap_write(cs2600->regmap, CS2600_UNLOCK_INDICATORS,
				   CS2600_CLEAR_INDICATORS);
		if (ret < 0)
			return ret;

		usleep_range(200, 300);
	}

	dev_err(cs2600->dev, "PLL did not lock\n");

	return -ETIMEDOUT;
}

static int _cs2600_pll_out_prepare(struct cs2600 *cs2600)
{
	int ret;

	ret = cs2600_enable_pll(cs2600, true);
	if (ret < 0)
		return ret;

	/*
	 * clkin-only mode configures the PLL so that it only starts
	 * when clock is present on CLK_IN. So don't wait for lock here.
	 */
	if (cs2600->mode == CS2600_SMART_CLKIN_ONLY_MODE)
		return 0;

	ret = regmap_write(cs2600->regmap, CS2600_UNLOCK_INDICATORS,
			   CS2600_CLEAR_INDICATORS);
	if (!ret)
		ret = cs2600_wait_for_pll_lock(cs2600);

	if (ret < 0)
		cs2600_enable_pll(cs2600, false);

	return ret;
}

static int cs2600_pll_out_prepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	int ret;

	/* REF_CLK is always required to clock the PLL */
	ret = clk_prepare_enable(cs2600->ref_clk);
	if (ret < 0) {
		dev_err(cs2600->dev, "Failed to enable ref_clk: %d\n", ret);
		return ret;
	}

	ret = _cs2600_pll_out_prepare(cs2600);
	if (ret < 0) {
		clk_disable_unprepare(cs2600->ref_clk);

		return ret;
	}

	return 0;
}


static void cs2600_pll_out_unprepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;

	regmap_clear_bits(regmap, CS2600_PLL_CFG1, CS2600_PLL_EN1);
	regmap_clear_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_EN2);

	clk_disable_unprepare(cs2600->ref_clk);
}

static int cs2600_pll_out_set_parent(struct clk_hw *hw, u8 index)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	struct clk_hw *parent_clk_hw;
	unsigned long parent_rate;
	unsigned int old_mode_sel;
	int ret;

	parent_clk_hw = clk_hw_get_parent_by_index(hw, index);
	if (!parent_clk_hw)
		return -EINVAL;

	/* CLK_IN is the only parent in smart mode */
	if (cs2600_is_smart_mode(cs2600))
		return 0;

	parent_rate = clk_hw_get_rate(parent_clk_hw);

	ret = regmap_read(regmap, CS2600_PLL_CFG2, &old_mode_sel);
	if (ret) {
		dev_err(cs2600->dev, "Read PLL_CFG2 failed: %d\n", ret);
		return ret;
	}

	ret = regmap_update_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_MODE_SEL,
				 index);
	if (ret < 0)
		return ret;

	if (cs2600->pll_target_rate == 0)
		return 0;

	ret = cs2600_pll_out_set_rate(hw, cs2600->pll_target_rate, parent_rate);
	if (ret) {
		regmap_update_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_MODE_SEL, old_mode_sel);

		return ret;
	}

	return 0;
}

static u8 cs2600_pll_out_get_parent(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	int ret;

	/* CLK_IN is the only parent in smart mode */
	if (cs2600_is_smart_mode(cs2600))
		return 0;

	ret = regmap_test_bits(regmap, CS2600_PLL_CFG2, CS2600_PLL_MODE_SEL);
	if (ret < 0)
		return 0;

	return (u8)ret;
}

static int cs2600_clk_out_prepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;

	return regmap_clear_bits(regmap, CS2600_PLL_CFG1, CS2600_CLK_OUT_DIS);
}

static void cs2600_clk_out_unprepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;

	regmap_set_bits(regmap, CS2600_PLL_CFG1, CS2600_CLK_OUT_DIS);
}

static int cs2600_clk_out_is_prepared(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	int ret;

	ret = regmap_test_bits(cs2600->regmap, CS2600_PLL_CFG1, CS2600_CLK_OUT_DIS);
	if (ret < 0) {
		dev_err(cs2600->dev, "Failed to read PLL_CFG1:%d\n", ret);
		return 0;
	}

	return !ret;
}

static const u16 cs2600_bclk_div[] = {
	1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48,
	0 /* terminator */
};

static int cs2600_get_clk_div_index(const u16 *divs, unsigned int rate, unsigned int parent_rate)
{
	unsigned long calc_rate, freq_var;
	int i;

	freq_var = cs2600_get_freq_variation(rate, CS2600_20_12_PPM);

	for (i = 0; divs[i]; i++) {
		calc_rate = parent_rate / divs[i];
		if (in_range(rate, calc_rate - freq_var, 2 * freq_var))
			return i;
	}

	return -EINVAL;
}

static int cs2600_bclk_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	int div_index;

	if (rate > CS2600_BCLK_OUT_MAX || rate < CS2600_BCLK_OUT_MIN)
		return -EINVAL;

	if (parent_rate > CS2600_CLK_OUT_MAX || parent_rate < CS2600_CLK_OUT_MIN)
		return -EINVAL;

	div_index = cs2600_get_clk_div_index(cs2600_bclk_div, rate, parent_rate);
	if (div_index < 0) {
		dev_err(cs2600->dev, "Cannot set bclk %lu from parent rate %lu\n", rate,
			parent_rate);

		return -EINVAL;
	}

	return regmap_update_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_BCLK_DIV_MASK,
				  FIELD_PREP(CS2600_BCLK_DIV_MASK, div_index));
}

static int cs2600_bclk_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	int div_index;

	if (req->rate > CS2600_BCLK_OUT_MAX || req->rate < CS2600_BCLK_OUT_MIN)
		return -EINVAL;

	div_index = cs2600_get_clk_div_index(cs2600_bclk_div, req->rate, req->best_parent_rate);
	if (div_index < 0) {
		dev_err(cs2600->dev, "BCLK_OUT cannot be derived from the parent rate");
		return -EINVAL;
	}

	req->rate = req->best_parent_rate / cs2600_bclk_div[div_index];

	return 0;
}

static unsigned long cs2600_bclk_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned int val;
	int ret;

	ret = regmap_read(regmap, CS2600_OUTPUT_CFG1, &val);
	if (ret) {
		dev_err(cs2600->dev, "Read bclk divider failed: %d\n", ret);
		return 0;
	}

	val = FIELD_GET(CS2600_BCLK_DIV_MASK, val);
	if ((val < ARRAY_SIZE(cs2600_bclk_div)) && cs2600_bclk_div[val])
		return (parent_rate / cs2600_bclk_div[val]);

	dev_err(cs2600->dev, "Cannot find bclk divide value for index=%#x\n", val);

	return 0;
}

static int cs2600_bclk_prepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	int ret;

	ret = regmap_set_bits(regmap, CS2600_PHASE_ALIGNMENT_CFG1,
			      CS2600_PHASE_ALIGN_EN | CS2600_PHASE_ALIGN_STB_EN);
	if (ret < 0)
		return ret;

	return regmap_clear_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_BCLK_OUT_DIS);
}

static void cs2600_bclk_unprepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	int ret;

	ret = regmap_test_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_FSYNC_OUT_DIS);
	if (ret > 0)
		regmap_clear_bits(regmap, CS2600_PHASE_ALIGNMENT_CFG1,
				  CS2600_PHASE_ALIGN_EN |
				  CS2600_PHASE_ALIGN_STB_EN);

	regmap_set_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_BCLK_OUT_DIS);
}

static int cs2600_bclk_is_prepared(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	int ret;

	ret = regmap_test_bits(cs2600->regmap, CS2600_OUTPUT_CFG1, CS2600_BCLK_OUT_DIS);
	if (ret < 0) {
		dev_err(cs2600->dev, "Failed to read OUTPUT_CFG1:%d\n", ret);
		return 0;
	}

	return !ret;
}

static const u16 cs2600_fsync_div[] = {
	16, 32, 64, 128, 256, 512, 1024, 192, 384, 768, 1536, 576, 1152,
	0 /* terminator */
};

static int cs2600_fsync_set_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	int div_index;

	if (rate > CS2600_FSYNC_OUT_MAX || rate < CS2600_FSYNC_OUT_MIN)
		return -EINVAL;

	if (parent_rate > CS2600_CLK_OUT_MAX || parent_rate < CS2600_CLK_OUT_MIN)
		return -EINVAL;

	div_index = cs2600_get_clk_div_index(cs2600_fsync_div, rate, parent_rate);
	if (div_index < 0) {
		dev_err(cs2600->dev, "Cannot set fsync %lu from parent rate %lu\n",
			rate, parent_rate);

		return -EINVAL;
	}

	return regmap_update_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_FSYNC_DIV_MASK,
				  FIELD_PREP(CS2600_FSYNC_DIV_MASK, div_index));
}

static int cs2600_fsync_determine_rate(struct clk_hw *hw,
				       struct clk_rate_request *req)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	int div_index;

	if (req->rate > CS2600_FSYNC_OUT_MAX || req->rate < CS2600_FSYNC_OUT_MIN)
		return -EINVAL;

	div_index = cs2600_get_clk_div_index(cs2600_fsync_div, req->rate, req->best_parent_rate);
	if (div_index < 0) {
		if (req->best_parent_rate)
			dev_err(cs2600->dev, "FSYNC_OUT cannot be derived from the parent rate");

		return -EINVAL;
	}

	req->rate = req->best_parent_rate / cs2600_fsync_div[div_index];

	return 0;
}

static unsigned long cs2600_fsync_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	struct cs2600 *cs2600 = clk_hw->priv;
	unsigned int val;
	int ret;

	ret = regmap_read(regmap, CS2600_OUTPUT_CFG1, &val);
	if (ret) {
		dev_err(cs2600->dev, "Read fsync divider failed: %d\n", ret);
		return 0;
	}

	val = FIELD_GET(CS2600_FSYNC_DIV_MASK, val);
	if ((val < ARRAY_SIZE(cs2600_fsync_div)) && cs2600_fsync_div[val])
		return (parent_rate / cs2600_fsync_div[val]);

	dev_err(cs2600->dev, "Cannot find fsync divide value for index=%#x\n", val);

	return 0;
}

static int cs2600_fsync_prepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	int ret;

	ret = regmap_set_bits(regmap, CS2600_PHASE_ALIGNMENT_CFG1,
			      CS2600_PHASE_ALIGN_EN | CS2600_PHASE_ALIGN_STB_EN);
	if (ret < 0)
		return ret;

	return regmap_clear_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_FSYNC_OUT_DIS);
}

static void cs2600_fsync_unprepare(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct regmap *regmap = clk_hw->priv->regmap;
	int ret;

	ret = regmap_test_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_BCLK_OUT_DIS);
	if (ret > 0)
		regmap_clear_bits(regmap, CS2600_PHASE_ALIGNMENT_CFG1,
				  CS2600_PHASE_ALIGN_EN |
				  CS2600_PHASE_ALIGN_STB_EN);

	regmap_set_bits(regmap, CS2600_OUTPUT_CFG1, CS2600_FSYNC_OUT_DIS);
}

static int cs2600_fsync_is_prepared(struct clk_hw *hw)
{
	struct cs2600_clk_hw *clk_hw = hw_to_cs2600_clk(hw);
	struct cs2600 *cs2600 = clk_hw->priv;
	int ret;

	ret = regmap_test_bits(cs2600->regmap, CS2600_OUTPUT_CFG1, CS2600_FSYNC_OUT_DIS);
	if (ret < 0) {
		dev_err(cs2600->dev, "Failed to read OUTPUT_CFG1:%d\n", ret);
		return 0;
	}

	return !ret;
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
		.flags = CLK_SET_PARENT_GATE | CLK_SET_RATE_GATE,
	},
	[CS2600_CLK_OUT] = {
		.name = "clk_out",
		.ops = {
			.prepare	= cs2600_clk_out_prepare,
			.unprepare	= cs2600_clk_out_unprepare,
			.is_prepared	= cs2600_clk_out_is_prepared,
		},
		.flags = CLK_SET_RATE_PARENT | CLK_SET_RATE_GATE,
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
	},
};

static int cs2600_clk_get(struct cs2600 *cs2600)
{
	struct clk *clk_in, *ref_clk;

	ref_clk = devm_clk_get(cs2600->dev, "ref_clk_in");
	if (IS_ERR(ref_clk))
		return PTR_ERR(ref_clk);

	cs2600->refclk_rate = clk_get_rate(ref_clk);
	if (cs2600->refclk_rate < 8000000 || cs2600->refclk_rate > 75000000) {
		return dev_err_probe(cs2600->dev, -EINVAL,
				     "Invalid REFCLK Frequency %lu\n",
				     cs2600->refclk_rate);
	}

	clk_in = devm_clk_get_optional(cs2600->dev, "clk_in");
	if (IS_ERR(clk_in))
		return PTR_ERR(clk_in);

	cs2600->ref_clk = ref_clk;
	cs2600->clk_in = clk_in;

	return 0;
}

static struct clk_hw *cs2600_of_clk_get(struct of_phandle_args *clkspec,
					void *data)
{
	unsigned int index = clkspec->args[0];
	struct cs2600 *cs2600 = data;

	/* PLL_OUT is an internal clock */
	if (index > CS2600_OUT_CLK_MAX - 1) {
		dev_err(cs2600->dev, "Invalid clock index %d\n", index);
		return ERR_PTR(-EINVAL);
	}

	return &cs2600->hw[index].hw;
}

static int cs2600_clk_register(struct cs2600 *cs2600)
{
	struct clk_init_data init[ARRAY_SIZE(cs2600->hw)] = { };
	const char *names[ARRAY_SIZE(cs2600_clks)];
	const struct clk_hw *out_clocks_parent;
	int ret, i, n, input_num = 0;
	const char *parent_names[2];

	static_assert(ARRAY_SIZE(cs2600_clks) == ARRAY_SIZE(cs2600->hw));

	for (i = 0; i < ARRAY_SIZE(cs2600->hw); i++) {
		init[i].name = cs2600_clks[i].name;
		init[i].ops = &cs2600_clks[i].ops;
		init[i].flags = cs2600_clks[i].flags;
		cs2600->hw[i].hw.init = &init[i];
		cs2600->hw[i].priv = cs2600;
	}

	n = of_property_read_string_array(cs2600->dev->of_node,
					  "clock-output-names",
					  names, ARRAY_SIZE(names));
	for (i = 0; i < n; i++)
		init[i].name = names[i];

	/* ref_clk_in is only a parent option in manual mode */
	if (!cs2600_is_smart_mode(cs2600))
		parent_names[input_num++] = __clk_get_name(cs2600->ref_clk);

	if (cs2600->clk_in)
		parent_names[input_num++] = __clk_get_name(cs2600->clk_in);

	if (input_num == 0)
		return dev_err_probe(cs2600->dev, -EINVAL, "No parent clocks\n");

	init[CS2600_PLL_OUT].parent_names = parent_names;
	init[CS2600_PLL_OUT].num_parents = input_num;

	/* The parent of all out clocks is the internal pll_out */
	out_clocks_parent = &cs2600->hw[CS2600_PLL_OUT].hw;
	for (i = CS2600_CLK_OUT; i < ARRAY_SIZE(cs2600->hw); i++) {
		init[i].parent_hws = &out_clocks_parent;
		init[i].num_parents = 1;
	}

	for (i = 0; i < ARRAY_SIZE(cs2600->hw); i++) {
		ret = devm_clk_hw_register(cs2600->dev, &cs2600->hw[i].hw);
		if (ret)
			return ret;
	}

	return devm_of_clk_add_hw_provider(cs2600->dev, cs2600_of_clk_get, cs2600);
}

static int cs2600_set_mode(struct cs2600 *cs2600)
{
	unsigned int s_slot = 0;
	int ret;

	if (device_property_present(cs2600->dev, "cirrus,smart-mode")) {
		cs2600->mode = CS2600_SMART_MODE;
		s_slot = 1;

		if (device_property_present(cs2600->dev, "cirrus,smart-mode-clkin-only"))
			cs2600->mode = CS2600_SMART_CLKIN_ONLY_MODE;
	}

	ret = regmap_clear_bits(cs2600->regmap, CS2600_PLL_CFG2, CS2600_M_RATIO_SEL_MASK);
	if (ret)
		return ret;

	return regmap_update_bits(cs2600->regmap, CS2600_PLL_CFG1, CS2600_S_RATIO_SEL_MASK,
				  FIELD_PREP(CS2600_S_RATIO_SEL_MASK, s_slot));
}

static int cs2600_set_output_inverts(struct cs2600 *cs2600)
{
	unsigned int field_val = 0;

	if (device_property_present(cs2600->dev, "cirrus,bclk-invert"))
		field_val |= CS2600_BCLK_INV;

	if (device_property_present(cs2600->dev, "cirrus,fsync-invert"))
		field_val |= CS2600_FSYNC_INV;

	return regmap_update_bits(cs2600->regmap, CS2600_OUTPUT_CFG1,
				  CS2600_BCLK_INV | CS2600_FSYNC_INV,
				  field_val);
}

static int cs2600_set_fsync_duty(struct cs2600 *cs2600)
{
	u32 val;
	int ret;

	ret = device_property_read_u32(cs2600->dev, "cirrus,fsync-duty-cycles", &val);
	if (ret == -EINVAL)
		return 0;

	if (ret < 0)
		return ret;

	if ((val < CS2600_FSYNC_MIN_CYCLES) || (val > CS2600_FSYNC_MAX_CYCLES) ||
	    !is_power_of_2(val))
		return dev_err_probe(cs2600->dev, -EINVAL, "Invalid FSYNC duty cycle %u\n", val);

	return regmap_update_bits(cs2600->regmap, CS2600_OUTPUT_CFG1,
				  CS2600_FSYNC_DUTY_CYCLE_MASK,
				  FIELD_PREP(CS2600_FSYNC_DUTY_CYCLE_MASK,
				  ffs(val)));
}

static const char * const cs2600_aux1_output_names[] = {
	"ref_clk_in",
	"clk_in",
	"freq_unlock",
	"phase_unlock",
	"clkin_missing",
};

static const u8 cs2600_aux1_output_modes[] =  {
	CS2600_AUX1_OUT_REF_CLK_IN_VAL,
	CS2600_AUX1_OUT_CLK_IN_VAL,
	CS2600_AUX1_OUT_FREQ_UNLOCK_VAL,
	CS2600_AUX1_OUT_PHASE_UNLOCK_VAL,
	CS2600_AUX1_OUT_NO_CLKIN_VAL,
};

static int cs2600_set_aux1_output(struct cs2600 *cs2600)
{
	const char *source;
	unsigned int val;
	int i, ret;

	if (device_property_read_string(cs2600->dev, "cirrus,aux1-output-source", &source))
		return 0;

	i = match_string(cs2600_aux1_output_names, ARRAY_SIZE(cs2600_aux1_output_names), source);
	if (i < 0)
		return dev_err_probe(cs2600->dev, -EINVAL, "Invalid aux1 output %s\n", source);

	static_assert(ARRAY_SIZE(cs2600_aux1_output_modes) == ARRAY_SIZE(cs2600_aux1_output_names));
	val = cs2600_aux1_output_modes[i];

	ret = regmap_update_bits(cs2600->regmap, CS2600_OUTPUT_CFG2,
				 CS2600_AUX1OUT_SEL, FIELD_PREP(CS2600_AUX1OUT_SEL, val));
	if (ret)
		return ret;

	return regmap_clear_bits(cs2600->regmap, CS2600_PLL_CFG1, CS2600_AUX1_OUT_DIS);
}

static int cs2600_set_refclk_source(struct cs2600 *cs2600)
{
	unsigned int src_field_val;

	if (device_property_present(cs2600->dev, "cirrus,internal-oscillator")) {
		if (cs2600->refclk_rate != CS2600_INTERNAL_OSC_RATE) {
			return dev_err_probe(cs2600->dev, -EINVAL,
					     "Illegal rate for internal oscillator\n");
		}
		src_field_val = CS2600_SYSCLK_SRC_OSC;
	} else {
		src_field_val = CS2600_SYSCLK_SRC_REFCLK;
	}

	return regmap_update_bits(cs2600->regmap, CS2600_PLL_CFG3,
				  CS2600_SYSCLK_SRC_MASK,
				  FIELD_PREP(CS2600_SYSCLK_SRC_MASK, src_field_val));
}

static int cs2600_parse_properties(struct cs2600 *cs2600)
{
	int ret;

	ret = cs2600_set_mode(cs2600);
	if (ret)
		return ret;

	ret = cs2600_set_output_inverts(cs2600);
	if (ret)
		return ret;

	ret = cs2600_set_fsync_duty(cs2600);
	if (ret)
		return ret;

	ret = cs2600_set_aux1_output(cs2600);
	if (ret)
		return ret;

	ret = cs2600_set_refclk_source(cs2600);
	if (ret)
		return ret;

	return 0;
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
		return dev_err_probe(dev, -ENODEV, "Invalid device id %#x\n",
				     dev_id);

	ret = regmap_read(cs2600->regmap, CS2600_DEVICE_ID2, &rev);
	if (ret)
		return dev_err_probe(dev, ret, "Can't read device revision\n");

	dev_dbg(dev, "ID %x Rev %X", dev_id, rev);

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

	/* Set outputs to HiZ */
	ret = regmap_set_bits(cs2600->regmap, CS2600_PLL_CFG1,
			      CS2600_CLK_OUT_DIS | CS2600_AUX1_OUT_DIS);
	if (ret)
		return ret;

	ret = regmap_set_bits(cs2600->regmap, CS2600_OUTPUT_CFG1,
			      CS2600_BCLK_OUT_DIS | CS2600_FSYNC_OUT_DIS);
	if (ret)
		return ret;

	ret = cs2600_parse_properties(cs2600);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot parse dt params\n");

	ret = cs2600_ref_clk_set_divider(cs2600, cs2600->refclk_rate);
	if (ret) {
		return dev_err_probe(dev, ret, "Invalid ref_clk %lu Hz\n",
				     cs2600->refclk_rate);
	}

	ret = cs2600_clk_register(cs2600);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register clocks\n");

	return 0;
}

static const struct of_device_id cs2600_of_match[] = {
	{ .compatible = "cirrus,cs2600", },
	{}
};
MODULE_DEVICE_TABLE(of, cs2600_of_match);

static const struct i2c_device_id cs2600_id[] = {
	{ .name = "cs2600", },
	{}
};
MODULE_DEVICE_TABLE(i2c, cs2600_id);

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
