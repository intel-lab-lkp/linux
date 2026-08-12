// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021-2026 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include "clk-pll.h"

#define RTK_PLL_EMMC1_OFFSET            0x0
#define RTK_PLL_EMMC2_OFFSET            0x4
#define RTK_PLL_EMMC3_OFFSET            0x8
#define RTK_PLL_EMMC4_OFFSET            0xc
#define RTK_PLL_SSC_DIG_EMMC1_OFFSET    0x0
#define RTK_PLL_SSC_DIG_EMMC3_OFFSET    0xc
#define RTK_PLL_SSC_DIG_EMMC4_OFFSET    0x10

#define RTK_PLL_MMC_SSC_DIV_N_VAL       0x1b

#define RTK_PLL_PHRT0_MASK              BIT(0)
#define RTK_PLL_PHSEL_MASK              GENMASK(4, 0)
#define RTK_PLL_SSCPLL_RS_MASK          GENMASK(12, 10)
#define RTK_PLL_SSCPLL_ICP_MASK         GENMASK(9, 5)
#define RTK_PLL_SSC_DIV_EXT_F_MASK      GENMASK(25, 13)
#define RTK_PLL_PI_IBSELH_MASK          GENMASK(28, 27)
#define RTK_PLL_SSC_DIV_N_MASK          GENMASK(23, 16)
#define RTK_PLL_NCODE_SSC_EMMC_MASK     GENMASK(20, 13)
#define RTK_PLL_FCODE_SSC_EMMC_MASK     GENMASK(12, 0)
#define RTK_PLL_GRAN_EST_EM_MC_MASK     GENMASK(20, 0)
#define RTK_PLL_EN_SSC_EMMC_MASK        BIT(0)
#define RTK_PLL_FLAG_INITIAL_EMMC_MASK  BIT(8)

#define RTK_PLL_PHRT0_SHIFT             1
#define RTK_PLL_SSCPLL_RS_SHIFT         10
#define RTK_PLL_SSCPLL_ICP_SHIFT        5
#define RTK_PLL_SSC_DIV_EXT_F_SHIFT     13
#define RTK_PLL_PI_IBSELH_SHIFT         27
#define RTK_PLL_SSC_DIV_N_SHIFT         16
#define RTK_PLL_NCODE_SSC_EMMC_SHIFT    13
#define RTK_PLL_FLAG_INITIAL_EMMC_SHIFT 8

#define CYCLE_DEGREES                   360
#define PHASE_STEPS                     32
#define PHASE_SCALE_FACTOR              1125

static inline struct rtk_clk_regmap_pll_mmc *to_clk_pll_mmc(struct clk_hw *hw)
{
	struct rtk_clk_regmap *clkr = to_rtk_clk_regmap(hw);

	return container_of(clkr, struct rtk_clk_regmap_pll_mmc, clkr);
}

static inline int get_phrt0(struct rtk_clk_regmap_pll_mmc *clkm, u32 *val)
{
	u32 reg;
	int ret;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC1_OFFSET, &reg);
	if (ret)
		return ret;

	*val = (reg >> RTK_PLL_PHRT0_SHIFT) & RTK_PLL_PHRT0_MASK;

	return 0;
}

static inline int set_phrt0(struct rtk_clk_regmap_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC1_OFFSET,
				  RTK_PLL_PHRT0_MASK << RTK_PLL_PHRT0_SHIFT,
				  val << RTK_PLL_PHRT0_SHIFT);
}

static inline int get_phsel(struct rtk_clk_regmap_pll_mmc *clkm, int id, u32 *val)
{
	u32 sft = id ? 8 : 3;
	u32 raw_val;
	int ret;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC1_OFFSET, &raw_val);
	if (ret)
		return ret;

	*val = (raw_val >> sft) & RTK_PLL_PHSEL_MASK;

	return 0;
}

static inline int set_phsel(struct rtk_clk_regmap_pll_mmc *clkm, int id, u32 val)
{
	u32 sft = id ? 8 : 3;

	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC1_OFFSET,
				  RTK_PLL_PHSEL_MASK << sft, val << sft);
}

static inline int set_sscpll_rs(struct rtk_clk_regmap_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC2_OFFSET,
				  RTK_PLL_SSCPLL_RS_MASK, val << RTK_PLL_SSCPLL_RS_SHIFT);
}

static inline int set_sscpll_icp(struct rtk_clk_regmap_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC2_OFFSET,
				  RTK_PLL_SSCPLL_ICP_MASK, val << RTK_PLL_SSCPLL_ICP_SHIFT);
}

static inline int get_ssc_div_ext_f(struct rtk_clk_regmap_pll_mmc *clkm, u32 *val)
{
	u32 raw_val;
	int ret;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC2_OFFSET, &raw_val);
	if (ret)
		return ret;

	*val = (raw_val & RTK_PLL_SSC_DIV_EXT_F_MASK) >> RTK_PLL_SSC_DIV_EXT_F_SHIFT;

	return 0;
}

static inline int set_ssc_div_ext_f(struct rtk_clk_regmap_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC2_OFFSET,
				  RTK_PLL_SSC_DIV_EXT_F_MASK,
				  val << RTK_PLL_SSC_DIV_EXT_F_SHIFT);
}

static inline int set_pi_ibselh(struct rtk_clk_regmap_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC2_OFFSET,
				  RTK_PLL_PI_IBSELH_MASK, val << RTK_PLL_PI_IBSELH_SHIFT);
}

static inline int set_ssc_div_n(struct rtk_clk_regmap_pll_mmc *clkm, u32 val)
{
	return regmap_update_bits(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC3_OFFSET,
				  RTK_PLL_SSC_DIV_N_MASK, val << RTK_PLL_SSC_DIV_N_SHIFT);
}

static inline int get_ssc_div_n(struct rtk_clk_regmap_pll_mmc *clkm, u32 *val)
{
	u32 raw_val;
	int ret;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC3_OFFSET, &raw_val);
	if (ret)
		return ret;

	*val = (raw_val & RTK_PLL_SSC_DIV_N_MASK) >> RTK_PLL_SSC_DIV_N_SHIFT;

	return 0;
}

static inline int set_pow_ctl(struct rtk_clk_regmap_pll_mmc *clkm, u32 val)
{
	return regmap_write(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC4_OFFSET, val);
}

static inline int get_pow_ctl(struct rtk_clk_regmap_pll_mmc *clkm, u32 *val)
{
	u32 raw_val;
	int ret;

	ret = regmap_read(clkm->clkr.regmap, clkm->pll_ofs + RTK_PLL_EMMC4_OFFSET, &raw_val);
	if (ret)
		return ret;

	*val = raw_val;

	return 0;
}

static int rtk_clk_regmap_pll_mmc_phase_set_phase(struct clk_hw *hw, int degrees)
{
	struct clk_hw *hwp = clk_hw_get_parent(hw);
	struct rtk_clk_regmap_pll_mmc *clkm;
	int phase_id, ret;
	u32 val;

	if (!hwp)
		return -ENOENT;

	clkm = to_clk_pll_mmc(hwp);
	phase_id = (hw == &clkm->phase0_hw) ? 0 : 1;
	val = DIV_ROUND_CLOSEST(degrees * 100, PHASE_SCALE_FACTOR);
	ret = set_phsel(clkm, phase_id, val);
	if (ret)
		return ret;

	usleep_range(10, 20);

	return 0;
}

static int rtk_clk_regmap_pll_mmc_phase_get_phase(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll_mmc *clkm;
	struct clk_hw *hwp;
	int phase_id, ret;
	u32 val;

	hwp = clk_hw_get_parent(hw);
	if (!hwp)
		return -ENOENT;

	clkm = to_clk_pll_mmc(hwp);
	phase_id = (hw == &clkm->phase0_hw) ? 0 : 1;
	ret = get_phsel(clkm, phase_id, &val);
	if (ret)
		return ret;

	val = DIV_ROUND_CLOSEST(val * CYCLE_DEGREES, PHASE_STEPS);

	return val;
}

const struct clk_ops rtk_clk_pll_mmc_phase_ops = {
	.set_phase = rtk_clk_regmap_pll_mmc_phase_set_phase,
	.get_phase = rtk_clk_regmap_pll_mmc_phase_get_phase,
};
EXPORT_SYMBOL_NS_GPL(rtk_clk_pll_mmc_phase_ops, "CLK_REALTEK");

static int rtk_clk_regmap_pll_mmc_prepare(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll_mmc *clkm = to_clk_pll_mmc(hw);

	return set_pow_ctl(clkm, 7);
}

static void rtk_clk_regmap_pll_mmc_unprepare(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll_mmc *clkm = to_clk_pll_mmc(hw);

	set_pow_ctl(clkm, 0);
}

static int rtk_clk_regmap_pll_mmc_is_prepared(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll_mmc *clkm = to_clk_pll_mmc(hw);
	u32 val;
	int ret;

	ret = get_pow_ctl(clkm, &val);
	if (ret)
		return 1;

	return val != 0x0;
}

static int rtk_clk_regmap_pll_mmc_enable(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll_mmc *clkm = to_clk_pll_mmc(hw);
	int ret;

	ret = set_phrt0(clkm, 1);
	if (ret)
		return ret;

	udelay(10);

	return 0;
}

static void rtk_clk_regmap_pll_mmc_disable(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll_mmc *clkm = to_clk_pll_mmc(hw);

	set_phrt0(clkm, 0);
	udelay(10);
}

static int rtk_clk_regmap_pll_mmc_is_enabled(struct clk_hw *hw)
{
	struct rtk_clk_regmap_pll_mmc *clkm = to_clk_pll_mmc(hw);
	u32 val;
	int ret;

	ret = get_phrt0(clkm, &val);
	if (ret)
		return 1;

	return val == 0x1;
}

static unsigned long rtk_clk_regmap_pll_mmc_recalc_rate(struct clk_hw *hw,
							unsigned long parent_rate)
{
	struct rtk_clk_regmap_pll_mmc *clkm = to_clk_pll_mmc(hw);
	u32 val, ext_f;
	u64 rate, base;
	int ret;

	ret = get_ssc_div_n(clkm, &val);
	if (ret)
		return 0;

	ret = get_ssc_div_ext_f(clkm, &ext_f);
	if (ret)
		return 0;

	base = parent_rate / 4;
	rate = base * (val + 2);
	rate += div_u64(base * ext_f, 8192);

	return rate;
}

static int rtk_clk_regmap_pll_mmc_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	u32 val = RTK_PLL_MMC_SSC_DIV_N_VAL + 2;
	u64 tmp;

	if (!req->best_parent_rate)
		return -EINVAL;

	req->rate = req->best_parent_rate / 4 * val;
	tmp = (u64)(req->best_parent_rate / 4) * 1517;
	req->rate += tmp >> 13;

	return 0;
}

static int rtk_clk_regmap_pll_mmc_set_rate(struct clk_hw *hw, unsigned long rate,
					   unsigned long parent_rate)
{
	struct rtk_clk_regmap_pll_mmc *clkm = to_clk_pll_mmc(hw);
	u32 val = RTK_PLL_MMC_SSC_DIV_N_VAL;
	int ret;

	/*
	 * The 'rate' and 'parent_rate' are intentionally unused here.
	 *
	 * Despite receiving various rate requests (e.g., 26MHz, 52MHz, 200MHz),
	 * this function consistently configures the hardware for 27MHz (0x1b).
	 * This is because these settings reflect the input reference clock
	 * frequency to the SSCPLL, not the final PLL output frequency.
	 *
	 * The actual frequency division to achieve the requested eMMC rate
	 * is handled internally by the downstream eMMC host controller.
	 */

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + RTK_PLL_SSC_DIG_EMMC1_OFFSET,
				 RTK_PLL_FLAG_INITIAL_EMMC_MASK,
				 0x0 << RTK_PLL_FLAG_INITIAL_EMMC_SHIFT);
	if (ret)
		return ret;

	ret = set_ssc_div_n(clkm, val);
	if (ret)
		return ret;

	ret = set_ssc_div_ext_f(clkm, 1517);
	if (ret)
		return ret;

	ret = set_pi_ibselh(clkm, 2);
	if (ret)
		return ret;

	ret = set_sscpll_rs(clkm, 3);
	if (ret)
		return ret;

	ret = set_sscpll_icp(clkm, 1);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + RTK_PLL_SSC_DIG_EMMC3_OFFSET,
				 RTK_PLL_NCODE_SSC_EMMC_MASK,
				 27 << RTK_PLL_NCODE_SSC_EMMC_SHIFT);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + RTK_PLL_SSC_DIG_EMMC3_OFFSET,
				 RTK_PLL_FCODE_SSC_EMMC_MASK, 321);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + RTK_PLL_SSC_DIG_EMMC4_OFFSET,
				 RTK_PLL_GRAN_EST_EM_MC_MASK, 5985);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + RTK_PLL_SSC_DIG_EMMC1_OFFSET,
				 RTK_PLL_EN_SSC_EMMC_MASK, 0x1);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + RTK_PLL_SSC_DIG_EMMC1_OFFSET,
				 RTK_PLL_EN_SSC_EMMC_MASK, 0x0);
	if (ret)
		return ret;

	ret = regmap_update_bits(clkm->clkr.regmap,
				 clkm->ssc_dig_ofs + RTK_PLL_SSC_DIG_EMMC1_OFFSET,
				 RTK_PLL_FLAG_INITIAL_EMMC_MASK,
				 0x1 << RTK_PLL_FLAG_INITIAL_EMMC_SHIFT);
	if (ret)
		return ret;

	usleep_range(10, 20);

	return 0;
}

const struct clk_ops rtk_clk_pll_mmc_ops = {
	.prepare          = rtk_clk_regmap_pll_mmc_prepare,
	.unprepare        = rtk_clk_regmap_pll_mmc_unprepare,
	.is_prepared      = rtk_clk_regmap_pll_mmc_is_prepared,
	.enable           = rtk_clk_regmap_pll_mmc_enable,
	.disable          = rtk_clk_regmap_pll_mmc_disable,
	.is_enabled       = rtk_clk_regmap_pll_mmc_is_enabled,
	.recalc_rate      = rtk_clk_regmap_pll_mmc_recalc_rate,
	.determine_rate   = rtk_clk_regmap_pll_mmc_determine_rate,
	.set_rate         = rtk_clk_regmap_pll_mmc_set_rate,
};
EXPORT_SYMBOL_NS_GPL(rtk_clk_pll_mmc_ops, "CLK_REALTEK");
