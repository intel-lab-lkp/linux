// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Collabora Ltd
 *		      AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/spmi.h>

#include "clk-mtk.h"
#include "clk-mtk-spmi.h"

int mtk_spmi_clk_simple_probe(struct platform_device *pdev)
{
	struct regmap_config mtk_spmi_clk_regmap_config = {
		.reg_bits = 16,
		.val_bits = 8,
		.fast_io = true
	};
	struct device_node *node = pdev->dev.of_node;
	const struct mtk_spmi_clk_desc *mscd;
	struct spmi_subdevice *sub_sdev;
	struct spmi_device *sparent;
	struct regmap *regmap;
	int ret;

	ret = of_property_read_u32(node, "reg", &mtk_spmi_clk_regmap_config.reg_base);
	if (ret)
		return ret;

	/* If the max_register was not declared the pdata is not valid */
	mscd = device_get_match_data(&pdev->dev);
	if (mscd->max_register == 0)
		return -EINVAL;

	mtk_spmi_clk_regmap_config.max_register = mscd->max_register;

	sparent = to_spmi_device(pdev->dev.parent);
	sub_sdev = devm_spmi_subdevice_alloc_and_add(&pdev->dev, sparent);
	if (IS_ERR(sub_sdev))
		return PTR_ERR(sub_sdev);

	regmap = devm_regmap_init_spmi_ext(&sub_sdev->sdev, &mtk_spmi_clk_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return mtk_clk_simple_probe_internal(pdev, node, mscd->desc, regmap);
}
EXPORT_SYMBOL_GPL(mtk_spmi_clk_simple_probe);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("SPMI");
