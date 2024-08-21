// SPDX-License-Identifier: GPL-2.0
/*
 * Reset driver for MediaTek SMI module
 *
 * Copyright (C) 2024 MediaTek Inc.
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#include <dt-bindings/reset/mt8188-resets.h>

#define to_mtk_smi_reset_data(_rcdev)	\
	container_of(_rcdev, struct mtk_smi_reset_data, rcdev)

struct mtk_smi_larb_reset {
	unsigned int offset;
	unsigned int value;
};

static const struct mtk_smi_larb_reset rst_signal_mt8188[] = {
	[MT8188_SMI_RST_LARB10]		= { 0xC, BIT(0) }, /* larb10 */
	[MT8188_SMI_RST_LARB11A]	= { 0xC, BIT(0) }, /* larb11a */
	[MT8188_SMI_RST_LARB11C]	= { 0xC, BIT(0) }, /* larb11c */
	[MT8188_SMI_RST_LARB12]		= { 0xC, BIT(8) }, /* larb12 */
	[MT8188_SMI_RST_LARB11B]	= { 0xC, BIT(0) }, /* larb11b */
	[MT8188_SMI_RST_LARB15]		= { 0xC, BIT(0) }, /* larb15 */
	[MT8188_SMI_RST_LARB16B]	= { 0xA0, BIT(4) }, /* larb16b */
	[MT8188_SMI_RST_LARB17B]	= { 0xA0, BIT(4) }, /* larb17b */
	[MT8188_SMI_RST_LARB16A]	= { 0xA0, BIT(4) }, /* larb16a */
	[MT8188_SMI_RST_LARB17A]	= { 0xA0, BIT(4) }, /* larb17a */
};

struct mtk_smi_larb_plat {
	const struct mtk_smi_larb_reset		*reset_signal;
	const unsigned int			larb_reset_nr;
};

struct mtk_smi_reset_data {
	const struct mtk_smi_larb_plat *larb_plat;
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
};

static const struct mtk_smi_larb_plat mtk_smi_larb_mt8188 = {
	.reset_signal = rst_signal_mt8188,
	.larb_reset_nr = ARRAY_SIZE(rst_signal_mt8188),
};

static int mtk_smi_larb_reset(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct mtk_smi_reset_data *data = to_mtk_smi_reset_data(rcdev);
	const struct mtk_smi_larb_plat *larb_plat = data->larb_plat;
	const struct mtk_smi_larb_reset *larb_rst = larb_plat->reset_signal + id;
	int ret;

	ret = regmap_set_bits(data->regmap, larb_rst->offset, larb_rst->value);
	if (ret)
		return ret;
	ret = regmap_clear_bits(data->regmap, larb_rst->offset, larb_rst->value);

	return ret;
}

static int mtk_smi_larb_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct mtk_smi_reset_data *data = to_mtk_smi_reset_data(rcdev);
	const struct mtk_smi_larb_plat *larb_plat = data->larb_plat;
	const struct mtk_smi_larb_reset *larb_rst = larb_plat->reset_signal + id;
	int ret;

	ret = regmap_set_bits(data->regmap, larb_rst->offset, larb_rst->value);
	if (ret)
		dev_err(rcdev->dev, "[%s] Failed to shutdown larb %d\n", __func__, ret);

	return ret;
}

static int mtk_smi_larb_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	struct mtk_smi_reset_data *data = to_mtk_smi_reset_data(rcdev);
	const struct mtk_smi_larb_plat *larb_plat = data->larb_plat;
	const struct mtk_smi_larb_reset *larb_rst = larb_plat->reset_signal + id;
	int ret;

	ret = regmap_clear_bits(data->regmap, larb_rst->offset, larb_rst->value);
	if (ret)
		dev_err(rcdev->dev, "[%s] Failed to reopen larb %d\n", __func__, ret);

	return ret;
}

static const struct reset_control_ops mtk_smi_reset_ops = {
	.reset		= mtk_smi_larb_reset,
	.assert		= mtk_smi_larb_reset_assert,
	.deassert	= mtk_smi_larb_reset_deassert,
};

static int mtk_smi_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct mtk_smi_larb_plat *larb_plat = of_device_get_match_data(dev);
	struct device_node *np = dev->of_node, *reset_node;
	struct mtk_smi_reset_data *data;
	struct regmap *regmap;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	reset_node = of_parse_phandle(np, "mediatek,larb-rst-syscon", 0);
	if (!reset_node)
		return -EINVAL;

	regmap = device_node_to_regmap(reset_node);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	data->larb_plat = larb_plat;
	data->regmap = regmap;
	data->rcdev.owner = THIS_MODULE;
	data->rcdev.ops = &mtk_smi_reset_ops;
	data->rcdev.of_node = np;
	data->rcdev.nr_resets = larb_plat->larb_reset_nr;
	data->rcdev.dev = dev;
	platform_set_drvdata(pdev, data);

	return devm_reset_controller_register(dev, &data->rcdev);
}

static const struct of_device_id mtk_smi_larb_reset_of_match[] = {
	{ .compatible = "mediatek,smi-reset-mt8188", .data = &mtk_smi_larb_mt8188 },
	{ },
};
MODULE_DEVICE_TABLE(of, mtk_smi_larb_reset_of_match);

static struct platform_driver mtk_smi_reset_driver = {
	.probe = mtk_smi_reset_probe,
	.driver = {
		.name = "mediatek-smi-reset",
		.of_match_table = mtk_smi_larb_reset_of_match,
	},
};
module_platform_driver(mtk_smi_reset_driver);

MODULE_AUTHOR("Friday.Yang@mediatek.com");
MODULE_DESCRIPTION("MediaTek SMI Reset Driver");
MODULE_LICENSE("GPL");
