// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 *                    Guangjie Song <guangjie.song@mediatek.com>
 * Copyright (c) 2025 Collabora Ltd.
 *                    Laura Nao <laura.nao@collabora.com>
 */
#include <dt-bindings/clock/mediatek,mt8196-clock.h>

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-pll.h"

/* APMIXEDSYS PLL control register offsets */
#define MAINPLL_CON0	0x250
#define MAINPLL_CON1	0x254
#define UNIVPLL_CON0	0x264
#define UNIVPLL_CON1	0x268
#define MSDCPLL_CON0	0x278
#define MSDCPLL_CON1	0x27c
#define ADSPPLL_CON0	0x28c
#define ADSPPLL_CON1	0x290
#define EMIPLL_CON0	0x2a0
#define EMIPLL_CON1	0x2a4
#define EMIPLL2_CON0	0x2b4
#define EMIPLL2_CON1	0x2b8
#define NET1PLL_CON0	0x2c8
#define NET1PLL_CON1	0x2cc
#define SGMIIPLL_CON0	0x2dc
#define SGMIIPLL_CON1	0x2e0

/* APMIXEDSYS_GP2 PLL control register offsets*/
#define MAINPLL2_CON0	0x250
#define MAINPLL2_CON1	0x254
#define UNIVPLL2_CON0	0x264
#define UNIVPLL2_CON1	0x268
#define MMPLL2_CON0	0x278
#define MMPLL2_CON1	0x27c
#define IMGPLL_CON0	0x28c
#define IMGPLL_CON1	0x290
#define TVDPLL1_CON0	0x2a0
#define TVDPLL1_CON1	0x2a4
#define TVDPLL2_CON0	0x2b4
#define TVDPLL2_CON1	0x2b8
#define TVDPLL3_CON0	0x2c8
#define TVDPLL3_CON1	0x2cc

#define PLLEN_ALL	0x080
#define PLLEN_ALL_SET	0x084
#define PLLEN_ALL_CLR	0x088

#define FENC_STATUS_CON0	0x03c

#define MT8196_PLL_FMAX		(3800UL * MHZ)
#define MT8196_PLL_FMIN		(1500UL * MHZ)
#define MT8196_INTEGER_BITS	8

#define PLL_FENC(_id, _name, _reg, _fenc_sta_ofs, _fenc_sta_bit,\
			_flags, _pd_reg, _pd_shift,		\
			_pcw_reg, _pcw_shift, _pcwbits,		\
			_pll_en_bit) {				\
		.id = _id,					\
		.name = _name,					\
		.reg = _reg,					\
		.fenc_sta_ofs = _fenc_sta_ofs,			\
		.fenc_sta_bit = _fenc_sta_bit,			\
		.flags = _flags,				\
		.fmax = MT8196_PLL_FMAX,			\
		.fmin = MT8196_PLL_FMIN,			\
		.pd_reg = _pd_reg,				\
		.pd_shift = _pd_shift,				\
		.pcw_reg = _pcw_reg,				\
		.pcw_shift = _pcw_shift,			\
		.pcwbits = _pcwbits,				\
		.pcwibits = MT8196_INTEGER_BITS,		\
		.en_reg = PLLEN_ALL,				\
		.en_set_reg = PLLEN_ALL_SET,			\
		.en_clr_reg = PLLEN_ALL_CLR,			\
		.pll_en_bit = _pll_en_bit,			\
		.ops = &mtk_pll_fenc_clr_set_ops,		\
}

static const struct mtk_pll_data apmixed_plls[] = {
	PLL_FENC(CLK_APMIXED_MAINPLL, "mainpll", MAINPLL_CON0, FENC_STATUS_CON0,
		 7, PLL_AO, MAINPLL_CON1, 24, MAINPLL_CON1, 0, 22, 0),
	PLL_FENC(CLK_APMIXED_UNIVPLL, "univpll", UNIVPLL_CON0, FENC_STATUS_CON0,
		 6, 0, UNIVPLL_CON1, 24, UNIVPLL_CON1, 0, 22, 1),
	PLL_FENC(CLK_APMIXED_MSDCPLL, "msdcpll", MSDCPLL_CON0, FENC_STATUS_CON0,
		 5, 0, MSDCPLL_CON1, 24, MSDCPLL_CON1, 0, 22, 2),
	PLL_FENC(CLK_APMIXED_ADSPPLL, "adsppll", ADSPPLL_CON0, FENC_STATUS_CON0,
		 4, 0, ADSPPLL_CON1, 24, ADSPPLL_CON1, 0, 22, 3),
	PLL_FENC(CLK_APMIXED_EMIPLL, "emipll", EMIPLL_CON0, FENC_STATUS_CON0, 3,
		 PLL_AO, EMIPLL_CON1, 24, EMIPLL_CON1, 0, 22, 4),
	PLL_FENC(CLK_APMIXED_EMIPLL2, "emipll2", EMIPLL2_CON0, FENC_STATUS_CON0,
		 2, PLL_AO, EMIPLL2_CON1, 24, EMIPLL2_CON1, 0, 22, 5),
	PLL_FENC(CLK_APMIXED_NET1PLL, "net1pll", NET1PLL_CON0, FENC_STATUS_CON0,
		 1, 0, NET1PLL_CON1, 24, NET1PLL_CON1, 0, 22, 6),
	PLL_FENC(CLK_APMIXED_SGMIIPLL, "sgmiipll", SGMIIPLL_CON0, FENC_STATUS_CON0,
		 0, 0, SGMIIPLL_CON1, 24, SGMIIPLL_CON1, 0, 22, 7),
};

static const struct mtk_clk_desc apmixed_desc = {
	.plls = apmixed_plls,
	.num_plls = ARRAY_SIZE(apmixed_plls),
};

static const struct mtk_pll_data apmixed2_plls[] = {
	PLL_FENC(CLK_APMIXED2_MAINPLL2, "mainpll2", MAINPLL2_CON0, FENC_STATUS_CON0,
		 6, 0, MAINPLL2_CON1, 24, MAINPLL2_CON1, 0, 22, 0),
	PLL_FENC(CLK_APMIXED2_UNIVPLL2, "univpll2", UNIVPLL2_CON0, FENC_STATUS_CON0,
		 5, 0, UNIVPLL2_CON1, 24, UNIVPLL2_CON1, 0, 22, 1),
	PLL_FENC(CLK_APMIXED2_MMPLL2, "mmpll2", MMPLL2_CON0, FENC_STATUS_CON0,
		 4, 0, MMPLL2_CON1, 24, MMPLL2_CON1, 0, 22, 2),
	PLL_FENC(CLK_APMIXED2_IMGPLL, "imgpll", IMGPLL_CON0, FENC_STATUS_CON0,
		 3, 0, IMGPLL_CON1, 24, IMGPLL_CON1, 0, 22, 3),
	PLL_FENC(CLK_APMIXED2_TVDPLL1, "tvdpll1", TVDPLL1_CON0, FENC_STATUS_CON0,
		 2, 0, TVDPLL1_CON1, 24, TVDPLL1_CON1, 0, 22, 4),
	PLL_FENC(CLK_APMIXED2_TVDPLL2, "tvdpll2", TVDPLL2_CON0, FENC_STATUS_CON0,
		 1, 0, TVDPLL2_CON1, 24, TVDPLL2_CON1, 0, 22, 5),
	PLL_FENC(CLK_APMIXED2_TVDPLL3, "tvdpll3", TVDPLL3_CON0, FENC_STATUS_CON0,
		 0, 0, TVDPLL3_CON1, 24, TVDPLL3_CON1, 0, 22, 6),
};

static const struct mtk_clk_desc apmixed2_desc = {
	.plls = apmixed2_plls,
	.num_plls = ARRAY_SIZE(apmixed2_plls),
};

static const struct of_device_id of_match_clk_mt8196_apmixed[] = {
	{ .compatible = "mediatek,mt8196-apmixedsys", .data = &apmixed_desc },
	{ .compatible = "mediatek,mt8196-apmixedsys-gp2",
	  .data = &apmixed2_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8196_apmixed);

static struct platform_driver clk_mt8196_apmixed_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8196-apmixed",
		.of_match_table = of_match_clk_mt8196_apmixed,
	},
};
module_platform_driver(clk_mt8196_apmixed_drv);

MODULE_DESCRIPTION("MediaTek MT8196 apmixedsys clocks driver");
MODULE_LICENSE("GPL");
