// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Guangjie Song <guangjie.song@mediatek.com>
 */
#include <dt-bindings/clock/mt8196-clk.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-pll.h"

/* PLL REG */
#define MAINPLL2_CON0	0x250
#define MAINPLL2_CON1	0x254
#define MAINPLL2_CON2	0x258
#define MAINPLL2_CON3	0x25c
#define UNIVPLL2_CON0	0x264
#define UNIVPLL2_CON1	0x268
#define UNIVPLL2_CON2	0x26c
#define UNIVPLL2_CON3	0x270
#define MMPLL2_CON0	0x278
#define MMPLL2_CON1	0x27c
#define MMPLL2_CON2	0x280
#define MMPLL2_CON3	0x284
#define IMGPLL_CON0	0x28c
#define IMGPLL_CON1	0x290
#define IMGPLL_CON2	0x294
#define IMGPLL_CON3	0x298
#define TVDPLL1_CON0	0x2a0
#define TVDPLL1_CON1	0x2a4
#define TVDPLL1_CON2	0x2a8
#define TVDPLL1_CON3	0x2ac
#define TVDPLL2_CON0	0x2b4
#define TVDPLL2_CON1	0x2b8
#define TVDPLL2_CON2	0x2bc
#define TVDPLL2_CON3	0x2c0
#define TVDPLL3_CON0	0x2c8
#define TVDPLL3_CON1	0x2cc
#define TVDPLL3_CON2	0x2d0
#define TVDPLL3_CON3	0x2d4

#define MT8196_PLL_FMAX		(3800UL * MHZ)
#define MT8196_PLL_FMIN		(1500UL * MHZ)
#define MT8196_INTEGER_BITS	8

#define PLL_FENC(_id, _name, _reg, _fenc_sta_ofs, _fenc_sta_bit,\
		 _flags, _pd_reg, _pd_shift,			\
		 _pcw_reg, _pcw_shift, _pcwbits) {		\
		.id = _id,					\
		.name = _name,					\
		.reg = _reg,					\
		.fenc_sta_ofs = _fenc_sta_ofs,			\
		.fenc_sta_bit = _fenc_sta_bit,			\
		.flags = (_flags) | CLK_FENC_ENABLE,		\
		.fmax = MT8196_PLL_FMAX,			\
		.fmin = MT8196_PLL_FMIN,			\
		.pd_reg = _pd_reg,				\
		.pd_shift = _pd_shift,				\
		.pcw_reg = _pcw_reg,				\
		.pcw_shift = _pcw_shift,			\
		.pcwbits = _pcwbits,				\
		.pcwibits = MT8196_INTEGER_BITS,		\
	}

static const struct mtk_pll_data apmixed2_plls[] = {
	PLL_FENC(CLK_APMIXED2_MAINPLL2, "mainpll2", MAINPLL2_CON0,
		 0x03c, 6, 0,
		 MAINPLL2_CON1, 24,
		 MAINPLL2_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED2_UNIVPLL2, "univpll2", UNIVPLL2_CON0,
		 0x03c, 5, 0,
		 UNIVPLL2_CON1, 24,
		 UNIVPLL2_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED2_MMPLL2, "mmpll2", MMPLL2_CON0,
		 0x03c, 4, 0,
		 MMPLL2_CON1, 24,
		 MMPLL2_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED2_IMGPLL, "imgpll", IMGPLL_CON0,
		 0x03c, 3, 0,
		 IMGPLL_CON1, 24,
		 IMGPLL_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED2_TVDPLL1, "tvdpll1", TVDPLL1_CON0,
		 0x03c, 2, 0,
		 TVDPLL1_CON1, 24,
		 TVDPLL1_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED2_TVDPLL2, "tvdpll2", TVDPLL2_CON0,
		 0x03c, 1, 0,
		 TVDPLL2_CON1, 24,
		 TVDPLL2_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED2_TVDPLL3, "tvdpll3", TVDPLL3_CON0,
		 0x03c, 0, 0,
		 TVDPLL3_CON1, 24,
		 TVDPLL3_CON1, 0, 22),
};

static int clk_mt8196_apmixed2_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	int num_plls = ARRAY_SIZE(apmixed2_plls);
	int r;

	clk_data = mtk_alloc_clk_data(num_plls);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_plls(node, apmixed2_plls, num_plls, clk_data);
	if (r) {
		mtk_free_clk_data(clk_data);
		return r;
	}

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r) {
		mtk_clk_unregister_plls(apmixed2_plls, num_plls, clk_data);
		mtk_free_clk_data(clk_data);
		return r;
	}

	return 0;
}

static void clk_mt8196_apmixed2_remove(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;

	of_clk_del_provider(node);
	mtk_clk_unregister_plls(apmixed2_plls, ARRAY_SIZE(apmixed2_plls), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt8196_apmixed2[] = {
	{ .compatible = "mediatek,mt8196-apmixedsys_gp2", },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_apmixed2_drv = {
	.probe = clk_mt8196_apmixed2_probe,
	.remove = clk_mt8196_apmixed2_remove,
	.driver = {
		.name = "clk-mt8196-apmixed2",
		.owner = THIS_MODULE,
		.of_match_table = of_match_clk_mt8196_apmixed2,
	},
};

module_platform_driver(clk_mt8196_apmixed2_drv);
MODULE_LICENSE("GPL");
