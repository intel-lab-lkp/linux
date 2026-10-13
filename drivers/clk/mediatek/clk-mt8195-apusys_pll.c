// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (c) 2021 MediaTek Inc.
// Author: Chun-Jie Chen <chun-jie.chen@mediatek.com>

#include "clk-mtk.h"
#include "clk-pll.h"

#include <dt-bindings/clock/mt8195-clk.h>
#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#define MT8195_PLL_FMAX		(3800UL * MHZ)
#define MT8195_PLL_FMIN		(1500UL * MHZ)
#define MT8195_INTEGER_BITS	(8)
#define MT8195_PCW_BITS		(22)
#define MT8195_POSDIV_SHIFT	(24)
#define MT8195_PLL_EN_BIT	(0)
#define MT8195_PCW_SHIFT	(0)

/*
 * The "en_reg" and "pcw_chg_reg" fields are standard offset register compared
 * with "reg" field, so set zero to imply it.
 * No tuner control in apu pll, so set "tuner_XXX" as zero to imply it.
 * No rst or post divider enable in apu pll, so set "rst_bar_mask" and "en_mask"
 * as zero to imply it.
 */
#define PLL(_id, _name, _reg, _pwr_reg, _pd_reg, _pcw_reg) {		\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = 0,						\
		.flags = 0,						\
		.rst_bar_mask = 0,					\
		.fmax = MT8195_PLL_FMAX,				\
		.fmin = MT8195_PLL_FMIN,				\
		.pcwbits = MT8195_PCW_BITS,				\
		.pcwibits = MT8195_INTEGER_BITS,			\
		.pd_reg = _pd_reg,					\
		.pd_shift = MT8195_POSDIV_SHIFT,			\
		.tuner_reg = 0,						\
		.tuner_en_reg = 0,					\
		.tuner_en_bit = 0,					\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = MT8195_PCW_SHIFT,				\
		.pcw_chg_reg = 0,					\
		.en_reg = 0,						\
		.pll_en_bit = MT8195_PLL_EN_BIT,			\
	}

static const struct mtk_pll_data apusys_plls[] = {
	PLL(CLK_APUSYS_PLL_APUPLL, "apusys_pll_apupll", 0x008, 0x014, 0x00c, 0x00c),
	PLL(CLK_APUSYS_PLL_NPUPLL, "apusys_pll_npupll", 0x018, 0x024, 0x01c, 0x01c),
	PLL(CLK_APUSYS_PLL_APUPLL1, "apusys_pll_apupll1", 0x028, 0x034, 0x02c, 0x02c),
	PLL(CLK_APUSYS_PLL_APUPLL2, "apusys_pll_apupll2", 0x038, 0x044, 0x03c, 0x03c),
};

static const struct mtk_clk_desc apu_pll_desc = {
	.plls = apusys_plls,
	.num_plls = ARRAY_SIZE(apusys_plls),
};

static const struct of_device_id of_match_clk_mt8195_apusys_pll[] = {
	{ .compatible = "mediatek,mt8195-apusys_pll", .data = &apu_pll_desc },
	{}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8195_apusys_pll);

static struct platform_driver clk_mt8195_apusys_pll_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8195-apusys_pll",
		.of_match_table = of_match_clk_mt8195_apusys_pll,
	},
};
module_platform_driver(clk_mt8195_apusys_pll_drv);

MODULE_DESCRIPTION("MediaTek MT8195 AI Processing Unit PLL clocks driver");
MODULE_LICENSE("GPL");
