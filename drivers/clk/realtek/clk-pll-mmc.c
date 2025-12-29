// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include "clk-pll.h"

#define PLL_EMMC1_OFFSET           0x0
#define PLL_EMMC2_OFFSET           0x4
#define PLL_EMMC3_OFFSET           0x8
#define PLL_EMMC4_OFFSET           0xc
#define PLL_SSC_DIG_EMMC1_OFFSET   0x0
#define PLL_SSC_DIG_EMMC3_OFFSET   0xc
#define PLL_SSC_DIG_EMMC4_OFFSET   0x10

#define PLL_PHRT0_MASK             BIT(1)
#define PLL_PHSEL_MASK             GENMASK(4, 0)
#define PLL_SSCPLL_RS_MASK         GENMASK(12, 10)
#define PLL_SSCPLL_ICP_MASK        GENMASK(9, 5)
#define PLL_SSC_DIV_EXT_F_MASK     GENMASK(25, 13)
#define PLL_PI_IBSELH_MASK         GENMASK(28, 27)
#define PLL_SSC_DIV_N_MASK         GENMASK(23, 16)
#define PLL_NCODE_SSC_EMMC_MASK    GENMASK(20, 13)
#define PLL_FCODE_SSC_EMMC_MASK    GENMASK(12, 0)
#define PLL_GRAN_EST_EM_MC_MASK    GENMASK(20, 0)
#define PLL_EN_SSC_EMMC_MASK       BIT(0)
#define PLL_FLAG_INITAL_EMMC_MASK  BIT(1)

#define PLL_PHRT0_SHIFT            1
#define PLL_SSCPLL_RS_SHIFT        10
#define PLL_SSCPLL_ICP_SHIFT       5
#define PLL_SSC_DIV_EXT_F_SHIFT    13
#define PLL_PI_IBSELH_SHIFT        27
#define PLL_SSC_DIV_N_SHIFT        16
#define PLL_NCODE_SSC_EMMC_SHIFT   13
#define PLL_FLAG_INITAL_EMMC_SHIFT 8

#define CYCLE_DEGREES              360
#define PHASE_STEPS                32
#define PHASE_SCALE_FACTOR         1125

static inline int get_phrt0(struct clk_pll_mmc *clkm, u32 *val)
{
	u32 reg;
	int ret;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC1_OFFSET, &reg);
	if (ret)
		return ret;

	*val = (reg >> PLL_PHRT0_SHIFT) & PLL_PHRT0_MASK;
	return 0;
}

static inline int set_phrt0(struct clk_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC1_OFFSET,
				  PLL_PHRT0_MASK, val << PLL_PHRT0_SHIFT);
}

static inline int get_phsel(struct clk_pll_mmc *clkm, int id, u32 *val)
{
	int ret;
	u32 raw_val;
	u32 sft = id ? 8 : 3;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC1_OFFSET, &raw_val);
	if (ret)
		return ret;

	*val = (raw_val >> sft) & PLL_PHSEL_MASK;
	return 0;
}

static inline int set_phsel(struct clk_pll_mmc *clkm, int id, u32 val)
{
	u32 sft = id ? 8 : 3;

	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC1_OFFSET,
				  PLL_PHSEL_MASK << sft, val << sft);
}

static inline int set_sscpll_rs(struct clk_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC2_OFFSET,
				  PLL_SSCPLL_RS_MASK, val << PLL_SSCPLL_RS_SHIFT);
}

static inline int set_sscpll_icp(struct clk_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC2_OFFSET,
				  PLL_SSCPLL_ICP_MASK, val << PLL_SSCPLL_ICP_SHIFT);
}

static inline int get_ssc_div_ext_f(struct clk_pll_mmc *clkm, u32 *val)
{
	u32 raw_val;
	int ret;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC2_OFFSET, &raw_val);
	if (ret)
		return ret;

	*val = (raw_val & PLL_SSC_DIV_EXT_F_MASK) >> PLL_SSC_DIV_EXT_F_SHIFT;
	return 0;
}

static inline int set_ssc_div_ext_f(struct clk_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC2_OFFSET,
				  PLL_SSC_DIV_EXT_F_MASK,
				  val << PLL_SSC_DIV_EXT_F_SHIFT);
}

static inline int set_pi_ibselh(struct clk_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC2_OFFSET,
				  PLL_PI_IBSELH_MASK, val << PLL_PI_IBSELH_SHIFT);
}

static inline int set_ssc_div_n(struct clk_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC3_OFFSET,
				  PLL_SSC_DIV_N_MASK, val << PLL_SSC_DIV_N_SHIFT);
}

static inline int get_ssc_div_n(struct clk_pll_mmc *clkm, u32 *val)
{
	int ret;
	u32 raw_val;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC3_OFFSET, &raw_val);
	if (ret)
		return ret;

	*val = (raw_val & PLL_SSC_DIV_N_MASK) >> PLL_SSC_DIV_N_SHIFT;
	return 0;
}

static inline int set_pow_ctl(struct clk_pll_mmc *clkm, u32 val)
{
	return regmap_write(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC4_OFFSET, val);
}

static inline int get_pow_ctl(struct clk_pll_mmc *clkm, u32 *val)
{
	int ret;
	u32 raw_val;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + PLL_EMMC4_OFFSET, &raw_val);

	*val = raw_val;

	return ret;
}

static int clk_pll_mmc_phase_set_phase(struct clk_hw *hw, int degrees)
{
	struct clk_hw *hwp = clk_hw_get_parent(hw);
	struct clk_pll_mmc *clkm;
	int phase_id;
	u32 val;
	int ret;

	if (!hwp)
		return -ENOENT;

	clkm = to_clk_pll_mmc(hwp);
	phase_id = (hw - &clkm->phase0_hw) ? 1 : 0;
	val = DIV_ROUND_CLOSEST(degrees * 100, PHASE_SCALE_FACTOR);
	ret = set_phsel(clkm, phase_id, val);
	if (ret)
		return ret;

	usleep_range(10, 20);
	return 0;
}

static int clk_pll_mmc_phase_get_phase(struct clk_hw *hw)
{
	struct clk_hw *hwp;
	struct clk_pll_mmc *clkm;
	int phase_id;
	int ret;
	u32 val;

	hwp = clk_hw_get_parent(hw);
	if (!hwp)
		return -ENOENT;

	clkm = to_clk_pll_mmc(hwp);
	phase_id = (hw - &clkm->phase0_hw) ? 1 : 0;
	ret = get_phsel(clkm, phase_id, &val);
	if (ret)
		return ret;

	val = DIV_ROUND_CLOSEST(val * CYCLE_DEGREES, PHASE_STEPS);

	return val;
}

const struct clk_ops clk_pll_mmc_phase_ops = {
	.set_phase = clk_pll_mmc_phase_set_phase,
	.get_phase = clk_pll_mmc_phase_get_phase,
};
EXPORT_SYMBOL_GPL(clk_pll_mmc_phase_ops);

static int clk_pll_mmc_prepare(struct clk_hw *hw)
{
	struct clk_pll_mmc *clkm = to_clk_pll_mmc(hw);

	return set_pow_ctl(clkm, 7);
}

static void clk_pll_mmc_unprepare(struct clk_hw *hw)
{
	struct clk_pll_mmc *clkm = to_clk_pll_mmc(hw);

	set_pow_ctl(clkm, 0);
}

static int clk_pll_mmc_is_prepared(struct clk_hw *hw)
{
	struct clk_pll_mmc *clkm = to_clk_pll_mmc(hw);
	u32 val;
	int ret;

	ret = get_pow_ctl(clkm, &val);
	if (ret)
		return 1;

	return val != 0x0;
}

static void clk_pll_mmc_unprepare_unused(struct clk_hw *hw)
{
	/* This PLL has no unprepare_unused action */
}

static int clk_pll_mmc_enable(struct clk_hw *hw)
{
	struct clk_pll_mmc *clkm = to_clk_pll_mmc(hw);
	int ret;

	ret = set_phrt0(clkm, 1);
	if (ret)
		return ret;

	udelay(10);
	return 0;
}

static void clk_pll_mmc_disable(struct clk_hw *hw)
{
	struct clk_pll_mmc *clkm = to_clk_pll_mmc(hw);

	set_phrt0(clkm, 0);
	udelay(10);
}

static int clk_pll_mmc_is_enabled(struct clk_hw *hw)
{
	struct clk_pll_mmc *clkm = to_clk_pll_mmc(hw);
	u32 val;
	int ret;

	ret = get_phrt0(clkm, &val);
	if (ret)
		return 1;

	return val == 0x1;
}

static void clk_pll_mmc_disable_unused(struct clk_hw *hw)
{
	/* This PLL has no disable_unused action */
}

static unsigned long clk_pll_mmc_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct clk_pll_mmc *clkm = to_clk_pll_mmc(hw);
	u32 val, ext_f;
	int ret;

	ret = get_ssc_div_n(clkm, &val);
	if (ret)
		return ret;

	ret = get_ssc_div_ext_f(clkm, &ext_f);
	if (ret)
		return ret;

	return parent_rate / 4 * (val + 2) + (parent_rate / 4 * ext_f) / 8192;
}

static int clk_pll_mmc_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	u32 val = DIV_ROUND_CLOSEST(req->rate * 4, req->best_parent_rate);

	return req->best_parent_rate * val / 4;
}

static u32 eval_ssc_div_n(struct clk_pll_mmc *clkm, unsigned long rate)
{
	return 0x1b;
}

static int clk_pll_mmc_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	struct clk_pll_mmc *clkm = to_clk_pll_mmc(hw);
	u32 val = eval_ssc_div_n(clkm, rate);
	int ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + PLL_SSC_DIG_EMMC1_OFFSET,
				 PLL_FLAG_INITAL_EMMC_MASK, 0x0 << PLL_FLAG_INITAL_EMMC_SHIFT);
	if (ret)
		return ret;

	ret = set_ssc_div_n(clkm, val);
	if (ret)
		return ret;

	ret = set_ssc_div_ext_f(clkm, 1517);
	if (ret)
		return ret;

	switch (val) {
	case 31 ... 46:
		ret |= set_pi_ibselh(clkm, 3);
		ret |= set_sscpll_rs(clkm, 3);
		ret |= set_sscpll_icp(clkm, 2);
		break;
	case 20 ... 30:
		ret |= set_pi_ibselh(clkm, 2);
		ret |= set_sscpll_rs(clkm, 3);
		ret |= set_sscpll_icp(clkm, 1);
		break;
	case 10 ... 19:
		ret |= set_pi_ibselh(clkm, 1);
		ret |= set_sscpll_rs(clkm, 2);
		ret |= set_sscpll_icp(clkm, 1);
		break;
	case 5 ... 9:
		ret |= set_pi_ibselh(clkm, 0);
		ret |= set_sscpll_rs(clkm, 2);
		ret |= set_sscpll_icp(clkm, 0);
		break;
	}
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + PLL_SSC_DIG_EMMC3_OFFSET,
				 PLL_NCODE_SSC_EMMC_MASK,
				 27 << PLL_NCODE_SSC_EMMC_SHIFT);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + PLL_SSC_DIG_EMMC3_OFFSET,
				 PLL_FCODE_SSC_EMMC_MASK, 321);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + PLL_SSC_DIG_EMMC4_OFFSET,
				 PLL_GRAN_EST_EM_MC_MASK, 5985);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + PLL_SSC_DIG_EMMC1_OFFSET,
				 PLL_EN_SSC_EMMC_MASK, 0x1);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + PLL_SSC_DIG_EMMC1_OFFSET,
				 PLL_EN_SSC_EMMC_MASK, 0x0);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + PLL_SSC_DIG_EMMC1_OFFSET,
				 PLL_FLAG_INITAL_EMMC_MASK,
				 0x1 << PLL_FLAG_INITAL_EMMC_SHIFT);
	if (ret)
		return ret;

	usleep_range(10, 20);
	return 0;
}

const struct clk_ops clk_pll_mmc_ops = {
	.prepare          = clk_pll_mmc_prepare,
	.unprepare        = clk_pll_mmc_unprepare,
	.is_prepared      = clk_pll_mmc_is_prepared,
	.unprepare_unused = clk_pll_mmc_unprepare_unused,
	.enable           = clk_pll_mmc_enable,
	.disable          = clk_pll_mmc_disable,
	.is_enabled       = clk_pll_mmc_is_enabled,
	.disable_unused   = clk_pll_mmc_disable_unused,
	.recalc_rate      = clk_pll_mmc_recalc_rate,
	.determine_rate   = clk_pll_mmc_determine_rate,
	.set_rate         = clk_pll_mmc_set_rate,
};
EXPORT_SYMBOL_GPL(clk_pll_mmc_ops);
MODULE_LICENSE("GPL");
