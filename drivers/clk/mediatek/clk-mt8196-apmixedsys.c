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
#define MAINPLL_CON0	0x250
#define MAINPLL_CON1	0x254
#define MAINPLL_CON2	0x258
#define MAINPLL_CON3	0x25c
#define UNIVPLL_CON0	0x264
#define UNIVPLL_CON1	0x268
#define UNIVPLL_CON2	0x26c
#define UNIVPLL_CON3	0x270
#define MSDCPLL_CON0	0x278
#define MSDCPLL_CON1	0x27c
#define MSDCPLL_CON2	0x280
#define MSDCPLL_CON3	0x284
#define ADSPPLL_CON0	0x28c
#define ADSPPLL_CON1	0x290
#define ADSPPLL_CON2	0x294
#define ADSPPLL_CON3	0x298
#define EMIPLL_CON0	0x2a0
#define EMIPLL_CON1	0x2a4
#define EMIPLL_CON2	0x2a8
#define EMIPLL_CON3	0x2ac
#define EMIPLL2_CON0	0x2b4
#define EMIPLL2_CON1	0x2b8
#define EMIPLL2_CON2	0x2bc
#define EMIPLL2_CON3	0x2c0

#define MT8196_PLL_FMAX		(3800UL * MHZ)
#define MT8196_PLL_FMIN		(1500UL * MHZ)
#define MT8196_INTEGER_BITS	8

#define PLL_FENC(_id, _name, _reg, _fenc_sta_ofs, _fenc_sta_bit,	\
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

static const struct mtk_pll_data apmixed_plls[] = {
	PLL_FENC(CLK_APMIXED_MAINPLL, "mainpll", MAINPLL_CON0,
		 0x003c, 7, PLL_AO,
		 MAINPLL_CON1, 24,
		 MAINPLL_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED_UNIVPLL, "univpll", UNIVPLL_CON0,
		 0x003c, 6, 0,
		 UNIVPLL_CON1, 24,
		 UNIVPLL_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED_MSDCPLL, "msdcpll", MSDCPLL_CON0,
		 0x003c, 5, 0,
		 MSDCPLL_CON1, 24,
		 MSDCPLL_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED_ADSPPLL, "adsppll", ADSPPLL_CON0,
		 0x003c, 4, 0,
		 ADSPPLL_CON1, 24,
		 ADSPPLL_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED_EMIPLL, "emipll", EMIPLL_CON0,
		 0x003c, 3, PLL_AO,
		 EMIPLL_CON1, 24,
		 EMIPLL_CON1, 0, 22),
	PLL_FENC(CLK_APMIXED_EMIPLL2, "emipll2", EMIPLL2_CON0,
		 0x003c, 2, PLL_AO,
		 EMIPLL2_CON1, 24,
		 EMIPLL2_CON1, 0, 22),
};

static int clk_mt8196_apmixed_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	int num_plls = ARRAY_SIZE(apmixed_plls);
	int r;

	clk_data = mtk_alloc_clk_data(num_plls);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_plls(node, apmixed_plls, num_plls, clk_data);
	if (r) {
		mtk_free_clk_data(clk_data);
		return r;
	}

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r) {
		mtk_clk_unregister_plls(apmixed_plls, num_plls, clk_data);
		mtk_free_clk_data(clk_data);
		return r;
	}

	return 0;
}

static void clk_mt8196_apmixed_remove(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);
	struct device_node *node = pdev->dev.of_node;

	of_clk_del_provider(node);
	mtk_clk_unregister_plls(apmixed_plls, ARRAY_SIZE(apmixed_plls), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt8196_apmixed[] = {
	{ .compatible = "mediatek,mt8196-apmixedsys", },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_apmixed_drv = {
	.probe = clk_mt8196_apmixed_probe,
	.remove = clk_mt8196_apmixed_remove,
	.driver = {
		.name = "clk-mt8196-apmixed",
		.owner = THIS_MODULE,
		.of_match_table = of_match_clk_mt8196_apmixed,
	},
};

module_platform_driver(clk_mt8196_apmixed_drv);
MODULE_LICENSE("GPL");
