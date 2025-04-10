// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/mediatek,mt6893-clk.h>

#include "clk-fhctl.h"
#include "clk-mtk.h"
#include "clk-pll.h"
#include "clk-pllfh.h"

#define MT6893_PLL_FMAX		(3800UL * MHZ)
#define MT6893_PLL_FMIN		(1500UL * MHZ)
#define MT6893_INTEGER_BITS	(8)

#define PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags,		\
	     _rst_bar_mask, _pcwbits, _pd_reg,				\
	     _tuner_reg, _tuner_en_reg, _tuner_en_bit, _pcw_reg) {	\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = _rst_bar_mask,				\
		.fmax = MT6893_PLL_FMAX,				\
		.fmin = MT6893_PLL_FMIN,				\
		.pcwbits = _pcwbits,					\
		.pcwibits = MT6893_INTEGER_BITS,			\
		.pd_reg = _pd_reg,					\
		.pd_shift = 24,						\
		.tuner_reg = _tuner_reg,				\
		.tuner_en_reg = _tuner_en_reg,				\
		.tuner_en_bit = _tuner_en_bit,				\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = 0,						\
		.pcw_chg_reg = 0,					\
		.en_reg = 0,						\
		.pll_en_bit = 0,					\
	}

static const struct mtk_pll_data plls[] = {
	/*
	 * armpll_ll/armpll_bl(x)/ccipll are main clock source of AP MCU,
	 * should not be closed in Linux world.
	 */
	PLL(CLK_APMIXED_ARMPLL_LL, "armpll_ll", 0x208, 0x214, 0,
	    PLL_AO, BIT(0), 22, 0x020c, 0, 0, 0, 0x20c),
	PLL(CLK_APMIXED_ARMPLL_BL0, "armpll_bl0", 0x218, 0x224, 0,
	    PLL_AO, BIT(0), 22, 0x021c, 0, 0, 0, 0x21c),
	PLL(CLK_APMIXED_ARMPLL_BL1, "armpll_bl1", 0x228, 0x234, 0,
	    PLL_AO, BIT(0), 22, 0x022c, 0, 0, 0, 0x22c),
	PLL(CLK_APMIXED_ARMPLL_BL2, "armpll_bl2", 0x238, 0x244, 0,
	    PLL_AO, BIT(0), 22, 0x023c, 0, 0, 0, 0x23c),
	PLL(CLK_APMIXED_ARMPLL_BL3, "armpll_bl3", 0x248, 0x254, 0,
	    PLL_AO, BIT(0), 22, 0x024c, 0, 0, 0, 0x24c),
	PLL(CLK_APMIXED_CCIPLL, "ccipll", 0x258, 0x264, 0,
	    PLL_AO, BIT(0), 22, 0x025c, 0, 0, 0, 0x25c),
	PLL(CLK_APMIXED_MAINPLL, "mainpll", 0x340, 0x34c, GENMASK(31, 24),
	    HAVE_RST_BAR | PLL_AO, BIT(23), 22, 0x0344, 0, 0, 0, 0x344),
	PLL(CLK_APMIXED_UNIVPLL, "univpll", 0x308, 0x314, GENMASK(31, 24),
	    HAVE_RST_BAR, BIT(23), 22, 0x030c, 0, 0, 0, 0x30c),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", 0x350, 0x35c, 0,
	    0, BIT(0), 22, 0x354, 0, 0, 0, 0x354),
	PLL(CLK_APMIXED_MMPLL, "mmpll", 0x360, 0x36c, GENMASK(31, 24),
	    HAVE_RST_BAR, BIT(23), 22, 0x364, 0, 0, 0, 0x364),
	PLL(CLK_APMIXED_ADSPPLL, "adsppll", 0x370, 0x37c, 0,
	    0, BIT(0), 22, 0x374, 0, 0, 0, 0x374),
	PLL(CLK_APMIXED_MFGPLL, "mfgpll", 0x268, 0x274, 0,
	    0, BIT(0), 22, 0x26c, 0, 0, 0, 0x26c),
	PLL(CLK_APMIXED_TVDPLL, "tvdpll", 0x380, 0x38c, 0,
	    0, BIT(0), 22, 0x384, 0, 0, 0, 0x384),
	PLL(CLK_APMIXED_APLL1, "apll1", 0x318, 0x328, 0,
	    0, BIT(0), 32, 0x31c, 0x040, 0x00c, 0, 0x320),
	PLL(CLK_APMIXED_APLL2, "apll2", 0x32c, 0x33c, 0,
	    0, BIT(0), 32, 0x330, 0x044, 0x00c, 5, 0x334),
	PLL(CLK_APMIXED_MPLL, "mpll", 0x32c, 0x39c, 0,
	    PLL_AO, BIT(0), 22, 0x394, 0, 0, 0, 0x394),
	PLL(CLK_APMIXED_APUPLL, "apupll", 0x3a0, 0x3ac, 0,
	    0, BIT(0), 22, 0x3a4, 0, 0, 0, 0x3a4),
};

static const struct of_device_id of_match_clk_mt6893_apmixed[] = {
	{ .compatible = "mediatek,mt6893-apmixedsys", },
	{}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6893_apmixed);

static int clk_mt6893_apmixed_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	int ret;

	clk_data = mtk_devm_alloc_clk_data(&pdev->dev, ARRAY_SIZE(plls));
	if (!clk_data)
		return -ENOMEM;

	ret = mtk_clk_register_plls(node, plls, ARRAY_SIZE(plls), clk_data);
	if (ret)
		return ret;

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, clk_data);

	return 0;
}

static void clk_mt6893_apmixed_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_plls(plls, ARRAY_SIZE(plls), clk_data);
}

static struct platform_driver clk_mt6893_apmixed_drv = {
	.probe = clk_mt6893_apmixed_probe,
	.remove = clk_mt6893_apmixed_remove,
	.driver = {
		.name = "clk-mt6893-apmixed",
		.of_match_table = of_match_clk_mt6893_apmixed,
	},
};
module_platform_driver(clk_mt6893_apmixed_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6893 apmixedsys clocks driver");
MODULE_LICENSE("GPL");
