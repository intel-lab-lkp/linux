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
#include <linux/pm_runtime.h>

#include "clk-mtk.h"
#include "clk-pll.h"

#define MFGPLL_CON0	0x008
#define MFGPLL_CON1	0x00c
#define MFGPLL_CON2	0x010
#define MFGPLL_CON3	0x014
#define MFGPLL_SC0_CON0	0x008
#define MFGPLL_SC0_CON1	0x00c
#define MFGPLL_SC0_CON2	0x010
#define MFGPLL_SC0_CON3	0x014
#define MFGPLL_SC1_CON0	0x008
#define MFGPLL_SC1_CON1	0x00c
#define MFGPLL_SC1_CON2	0x010
#define MFGPLL_SC1_CON3	0x014

#define MT8196_PLL_FMAX		(3800UL * MHZ)
#define MT8196_PLL_FMIN		(1500UL * MHZ)
#define MT8196_INTEGER_BITS	8

#define PLL(_id, _name, _reg, _en_reg, _en_mask, _pll_en_bit,	\
	    _flags, _rst_bar_mask,				\
	    _pd_reg, _pd_shift, _tuner_reg,			\
	    _tuner_en_reg, _tuner_en_bit,			\
	    _pcw_reg, _pcw_shift, _pcwbits, _rpm_clks) {	\
		.id = _id,					\
		.name = _name,					\
		.reg = _reg,					\
		.en_reg = _en_reg,				\
		.en_mask = _en_mask,				\
		.pll_en_bit = _pll_en_bit,			\
		.flags = _flags,				\
		.rst_bar_mask = _rst_bar_mask,			\
		.fmax = MT8196_PLL_FMAX,			\
		.fmin = MT8196_PLL_FMIN,			\
		.pd_reg = _pd_reg,				\
		.pd_shift = _pd_shift,				\
		.tuner_reg = _tuner_reg,			\
		.tuner_en_reg = _tuner_en_reg,			\
		.tuner_en_bit = _tuner_en_bit,			\
		.pcw_reg = _pcw_reg,				\
		.pcw_shift = _pcw_shift,			\
		.pcwbits = _pcwbits,				\
		.pcwibits = MT8196_INTEGER_BITS,		\
		.rpm_clk_names = _rpm_clks,			\
		.num_rpm_clks = ARRAY_SIZE(_rpm_clks),		\
	}

static const char * const mfgpll_rpm_clk_names[] = {
	NULL
};

static const struct mtk_pll_data mfg_ao_plls[] = {
	PLL(CLK_MFG_AO_MFGPLL, "mfgpll", MFGPLL_CON0, MFGPLL_CON0, 0, 0, 0,
	    BIT(0), MFGPLL_CON1, 24, 0, 0, 0, MFGPLL_CON1, 0, 22,
	    mfgpll_rpm_clk_names),
};

static const struct mtk_pll_data mfgsc0_ao_plls[] = {
	PLL(CLK_MFGSC0_AO_MFGPLL_SC0, "mfgpll-sc0", MFGPLL_SC0_CON0,
	    MFGPLL_SC0_CON0, 0, 0, 0, BIT(0), MFGPLL_SC0_CON1, 24, 0, 0, 0,
	    MFGPLL_SC0_CON1, 0, 22, mfgpll_rpm_clk_names),
};

static const struct mtk_pll_data mfgsc1_ao_plls[] = {
	PLL(CLK_MFGSC1_AO_MFGPLL_SC1, "mfgpll-sc1", MFGPLL_SC1_CON0,
	    MFGPLL_SC1_CON0, 0, 0, 0, BIT(0), MFGPLL_SC1_CON1, 24, 0, 0, 0,
	    MFGPLL_SC1_CON1, 0, 22, mfgpll_rpm_clk_names),
};

struct clk_mt8196_mfg {
	struct clk_hw_onecell_data *clk_data;
	struct clk_bulk_data *rpm_clks;
	unsigned int num_rpm_clks;
};

static int __maybe_unused clk_mt8196_mfg_resume(struct device *dev)
{
	struct clk_mt8196_mfg *clk_mfg = dev_get_drvdata(dev);

	if (!clk_mfg || !clk_mfg->rpm_clks)
		return 0;

	return clk_bulk_prepare_enable(clk_mfg->num_rpm_clks, clk_mfg->rpm_clks);
}

static int __maybe_unused clk_mt8196_mfg_suspend(struct device *dev)
{
	struct clk_mt8196_mfg *clk_mfg = dev_get_drvdata(dev);

	if (!clk_mfg || !clk_mfg->rpm_clks)
		return 0;

	clk_bulk_disable_unprepare(clk_mfg->num_rpm_clks, clk_mfg->rpm_clks);

	return 0;
}

static const struct of_device_id of_match_clk_mt8196_mfg[] = {
	{ .compatible = "mediatek,mt8196-mfgpll-pll-ctrl",
	  .data = &mfg_ao_plls },
	{ .compatible = "mediatek,mt8196-mfgpll-sc0-pll-ctrl",
	  .data = &mfgsc0_ao_plls },
	{ .compatible = "mediatek,mt8196-mfgpll-sc1-pll-ctrl",
	  .data = &mfgsc1_ao_plls },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8196_mfg);

static int clk_mt8196_mfg_probe(struct platform_device *pdev)
{
	const struct mtk_pll_data *plls;
	struct clk_mt8196_mfg *clk_mfg;
	struct device_node *node = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	const int num_plls = 1;
	int r, i;

	plls = of_device_get_match_data(dev);
	if (!plls)
		return -EINVAL;

	clk_mfg = devm_kzalloc(dev, sizeof(*clk_mfg), GFP_KERNEL);
	if (!clk_mfg)
		return -ENOMEM;

	clk_mfg->num_rpm_clks = plls[0].num_rpm_clks;

	if (clk_mfg->num_rpm_clks) {
		clk_mfg->rpm_clks = devm_kcalloc(dev, clk_mfg->num_rpm_clks,
						 sizeof(*clk_mfg->rpm_clks),
						 GFP_KERNEL);
		if (!clk_mfg->rpm_clks)
			return -ENOMEM;

		for (i = 0; i < clk_mfg->num_rpm_clks; i++)
			clk_mfg->rpm_clks->id = plls[0].rpm_clk_names[i];

		r = devm_clk_bulk_get(dev, clk_mfg->num_rpm_clks,
				      clk_mfg->rpm_clks);
		if (r)
			return r;
	}

	clk_mfg->clk_data = mtk_alloc_clk_data(num_plls);
	if (!clk_mfg->clk_data)
		return -ENOMEM;

	dev_set_drvdata(dev, clk_mfg);
	pm_runtime_enable(dev);

	r = mtk_clk_register_plls(dev, plls, num_plls, clk_mfg->clk_data);
	if (r)
		goto free_clk_data;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get,
				   clk_mfg->clk_data);
	if (r)
		goto unregister_plls;

	return r;

unregister_plls:
	mtk_clk_unregister_plls(plls, num_plls, clk_mfg->clk_data);
free_clk_data:
	mtk_free_clk_data(clk_mfg->clk_data);

	return r;
}

static void clk_mt8196_mfg_remove(struct platform_device *pdev)
{
	const struct mtk_pll_data *plls = of_device_get_match_data(&pdev->dev);
	struct clk_mt8196_mfg *clk_mfg = dev_get_drvdata(&pdev->dev);
	struct device_node *node = pdev->dev.of_node;

	of_clk_del_provider(node);
	mtk_clk_unregister_plls(plls, 1, clk_mfg->clk_data);
	mtk_free_clk_data(clk_mfg->clk_data);
}

static DEFINE_RUNTIME_DEV_PM_OPS(clk_mt8196_mfg_pm_ops,
				 clk_mt8196_mfg_suspend,
				 clk_mt8196_mfg_resume,
				 NULL);

static struct platform_driver clk_mt8196_mfg_drv = {
	.probe = clk_mt8196_mfg_probe,
	.remove = clk_mt8196_mfg_remove,
	.driver = {
		.name = "clk-mt8196-mfg",
		.of_match_table = of_match_clk_mt8196_mfg,
		.pm = pm_ptr(&clk_mt8196_mfg_pm_ops),
	},
};
module_platform_driver(clk_mt8196_mfg_drv);

MODULE_DESCRIPTION("MediaTek MT8196 GPU mfg clocks driver");
MODULE_LICENSE("GPL");
