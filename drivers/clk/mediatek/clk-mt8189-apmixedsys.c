// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026 MediaTek Inc.
 *                    Qiqi Wang <qiqi.wang@mediatek.com>
 *                    Irving-CH Lin <irving-ch.lin@mediatek.com>
 * Copyright (C) 2026 Collabora Ltd.
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 *                    Louis-Alexis Eyraud <louisalexis.eyraud@collabora.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "clk-fhctl.h"
#include "clk-mtk.h"
#include "clk-pll.h"

#include <dt-bindings/clock/mediatek,mt8189-clk.h>

#define MT8189_PLL_FMAX		(3800UL * MHZ)
#define MT8189_PLL_FMIN		(1500UL * MHZ)
#define MT8189_PLLEN_OFS	0x70
#define MT8189_INTEGER_BITS	8

#define PLL_SETCLR(_id, _name, _reg, _en_setclr_bit,		\
			_rstb_setclr_bit, _flags, _pd_reg,	\
			_pd_shift, _tuner_reg, _tuner_en_reg,	\
			_tuner_en_bit, _pcw_reg, _pcw_shift,	\
			_pcwbits) {				\
		.id = _id,					\
		.name = _name,					\
		.en_reg = MT8189_PLLEN_OFS,			\
		.reg = _reg,					\
		.pll_en_bit = _en_setclr_bit,			\
		.rst_bar_mask = BIT(_rstb_setclr_bit),		\
		.flags = _flags,				\
		.fmax = MT8189_PLL_FMAX,			\
		.fmin = MT8189_PLL_FMIN,			\
		.pd_reg = _pd_reg,				\
		.pd_shift = _pd_shift,				\
		.tuner_reg = _tuner_reg,			\
		.tuner_en_reg = _tuner_en_reg,			\
		.tuner_en_bit = _tuner_en_bit,			\
		.pcw_reg = _pcw_reg,				\
		.pcw_shift = _pcw_shift,			\
		.pcwbits = _pcwbits,				\
		.pcwibits = MT8189_INTEGER_BITS,		\
	}

static const struct mtk_pll_data apmixed_plls[] = {
	PLL_SETCLR(CLK_APMIXED_ARMPLL_LL, "armpll-ll", 0x204, 18,
		   0, PLL_AO, 0x208, 24, 0, 0, 0, 0x208, 0, 22),
	PLL_SETCLR(CLK_APMIXED_ARMPLL_BL, "armpll-bl", 0x214, 17,
		   0, PLL_AO, 0x218, 24, 0, 0, 0, 0x218, 0, 22),
	PLL_SETCLR(CLK_APMIXED_CCIPLL, "ccipll", 0x224, 16,
		   0, PLL_AO, 0x228, 24, 0, 0, 0, 0x228, 0, 22),
	PLL_SETCLR(CLK_APMIXED_MAINPLL, "mainpll", 0x304, 15,
		   23, HAVE_RST_BAR | PLL_AO,
		   0x308, 24, 0, 0, 0, 0x308, 0, 22),
	PLL_SETCLR(CLK_APMIXED_UNIVPLL, "univpll", 0x314, 14,
		   23, HAVE_RST_BAR, 0x318, 24, 0, 0, 0, 0x318, 0, 22),
	PLL_SETCLR(CLK_APMIXED_MMPLL, "mmpll", 0x324, 13,
		   23, HAVE_RST_BAR, 0x328, 24, 0, 0, 0, 0x328, 0, 22),
	PLL_SETCLR(CLK_APMIXED_MFGPLL, "mfgpll", 0x504, 7,
		   0, 0, 0x508, 24, 0, 0, 0, 0x508, 0, 22),
	PLL_SETCLR(CLK_APMIXED_APLL1, "apll1", 0x404, 11,
		   0, 0, 0x408, 24, 0x040, 0x00c, 0, 0x40c, 0, 32),
	PLL_SETCLR(CLK_APMIXED_APLL2, "apll2", 0x418, 10,
		   0, 0, 0x41c, 24, 0x044, 0x00c, 1, 0x420, 0, 32),
	PLL_SETCLR(CLK_APMIXED_EMIPLL, "emipll", 0x334, 12,
		   0, PLL_AO, 0x338, 24, 0, 0, 0, 0x338, 0, 22),
	PLL_SETCLR(CLK_APMIXED_APUPLL2, "apupll2", 0x614, 2,
		   0, 0, 0x618, 24, 0, 0, 0, 0x618, 0, 22),
	PLL_SETCLR(CLK_APMIXED_APUPLL, "apupll", 0x604, 3,
		   0, 0, 0x608, 24, 0, 0, 0, 0x608, 0, 22),
	PLL_SETCLR(CLK_APMIXED_TVDPLL1, "tvdpll1", 0x42c, 9,
		   0, 0, 0x430, 24, 0, 0, 0, 0x430, 0, 22),
	PLL_SETCLR(CLK_APMIXED_TVDPLL2, "tvdpll2", 0x43c, 8,
		   0, 0, 0x440, 24, 0, 0, 0, 0x440, 0, 22),
	PLL_SETCLR(CLK_APMIXED_ETHPLL, "ethpll", 0x514, 6,
		   0, 0, 0x518, 24, 0, 0, 0, 0x518, 0, 22),
	PLL_SETCLR(CLK_APMIXED_MSDCPLL, "msdcpll", 0x524, 5,
		   0, 0, 0x528, 24, 0, 0, 0, 0x528, 0, 22),
	PLL_SETCLR(CLK_APMIXED_UFSPLL, "ufspll", 0x534, 4,
		   0, 0, 0x538, 24, 0, 0, 0, 0x538, 0, 22),
};

#define FH(_pllid, _fhid, _offset) {				\
		.data = {					\
			.pll_id = _pllid,			\
			.fh_id = _fhid,				\
			.fh_ver = FHCTL_PLLFH_V2,		\
			.fhx_offset = _offset,			\
			.dds_mask = GENMASK(21, 0),		\
			.slope0_value = 0x6003c97,		\
			.slope1_value = 0x6003c97,		\
			.sfstrx_en = BIT(2),			\
			.frddsx_en = BIT(1),			\
			.fhctlx_en = BIT(0),			\
			.tgl_org = BIT(31),			\
			.dvfs_tri = BIT(31),			\
			.pcwchg = BIT(31),			\
			.dt_val = 0x0,				\
			.df_val = 0x9,				\
			.updnlmt_shft = 16,			\
			.msk_frddsx_dys = GENMASK(23, 20),	\
			.msk_frddsx_dts = GENMASK(19, 16),	\
		},						\
	}

static struct mtk_pllfh_data pllfhs[] = {
	FH(CLK_APMIXED_ARMPLL_LL, 0, 0x003C),
	FH(CLK_APMIXED_ARMPLL_BL, 1, 0x0050),
	FH(CLK_APMIXED_CCIPLL, 2, 0x0064),
	FH(CLK_APMIXED_MAINPLL, 3, 0x0078),
	FH(CLK_APMIXED_MMPLL, 4, 0x008C),
	FH(CLK_APMIXED_MFGPLL, 5, 0x00A0),
	FH(CLK_APMIXED_EMIPLL, 6, 0x00B4),
	FH(CLK_APMIXED_TVDPLL1, 7, 0x00C8),
	FH(CLK_APMIXED_TVDPLL2, 8, 0x00DC),
	FH(CLK_APMIXED_MSDCPLL, 9, 0x00F0),
	FH(CLK_APMIXED_UFSPLL, 10, 0x0104),
	FH(CLK_APMIXED_APUPLL, 11, 0x0118),
	FH(CLK_APMIXED_APUPLL2, 12, 0x012c),
};

static const struct of_device_id of_match_clk_mt8189_apmixed[] = {
	{ .compatible = "mediatek,mt8189-apmixedsys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8189_apmixed);

static int clk_mt8189_apmixed_probe(struct platform_device *pdev)
{
	int r;
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	const u8 *fhctl_node = "mediatek,mt8189-fhctl";

	clk_data = mtk_alloc_clk_data(ARRAY_SIZE(apmixed_plls));
	if (!clk_data)
		return -ENOMEM;

	fhctl_parse_dt(fhctl_node, pllfhs, ARRAY_SIZE(pllfhs));

	r = mtk_clk_register_pllfhs(&pdev->dev, apmixed_plls, ARRAY_SIZE(apmixed_plls),
				    pllfhs, ARRAY_SIZE(pllfhs), clk_data);
	if (r)
		goto free_apmixed_data;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		goto unregister_plls;

	platform_set_drvdata(pdev, clk_data);

	return 0;

unregister_plls:
	mtk_clk_unregister_pllfhs(apmixed_plls, ARRAY_SIZE(apmixed_plls), pllfhs,
				  ARRAY_SIZE(pllfhs), clk_data);
free_apmixed_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static void clk_mt8189_apmixed_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_pllfhs(apmixed_plls, ARRAY_SIZE(apmixed_plls), pllfhs,
				  ARRAY_SIZE(pllfhs), clk_data);
	mtk_free_clk_data(clk_data);
}

static struct platform_driver clk_mt8189_apmixed_drv = {
	.probe = clk_mt8189_apmixed_probe,
	.remove = clk_mt8189_apmixed_remove,
	.driver = {
		.name = "clk-mt8189-apmixed",
		.of_match_table = of_match_clk_mt8189_apmixed,
	},
};
module_platform_driver(clk_mt8189_apmixed_drv);

MODULE_DESCRIPTION("MediaTek MT8189 apmixed clocks driver");
MODULE_LICENSE("GPL");
