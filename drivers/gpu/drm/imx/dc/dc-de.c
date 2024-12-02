// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2024 NXP
 */

#include <linux/component.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include "dc-de.h"
#include "dc-drv.h"

#define POLARITYCTRL		0xc
#define  POLEN_HIGH		BIT(2)

static const struct regmap_range dc_de_regmap_ranges[] = {
	regmap_reg_range(POLARITYCTRL, POLARITYCTRL),
};

static const struct regmap_access_table dc_de_regmap_access_table = {
	.yes_ranges = dc_de_regmap_ranges,
	.n_yes_ranges = ARRAY_SIZE(dc_de_regmap_ranges),
};

static const struct regmap_config dc_de_top_regmap_config = {
	.name = "display-engine-top",
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.wr_table = &dc_de_regmap_access_table,
	.rd_table = &dc_de_regmap_access_table,
};

static inline void dc_dec_init(struct dc_de *de)
{
	regmap_write_bits(de->reg_top, POLARITYCTRL, POLARITYCTRL, POLEN_HIGH);
}

static int dc_de_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct dc_drm_device *dc_drm = data;
	void __iomem *base_top;
	struct dc_de *de;
	int ret;

	de = devm_kzalloc(dev, sizeof(*de), GFP_KERNEL);
	if (!de)
		return -ENOMEM;

	base_top = devm_platform_ioremap_resource_byname(pdev, "top");
	if (IS_ERR(base_top))
		return PTR_ERR(base_top);

	de->reg_top = devm_regmap_init_mmio(dev, base_top,
					    &dc_de_top_regmap_config);
	if (IS_ERR(de->reg_top))
		return PTR_ERR(de->reg_top);

	de->irq_shdld = platform_get_irq_byname(pdev, "shdload");
	if (de->irq_shdld < 0)
		return de->irq_shdld;

	de->irq_framecomplete = platform_get_irq_byname(pdev, "framecomplete");
	if (de->irq_framecomplete < 0)
		return de->irq_framecomplete;

	de->irq_seqcomplete = platform_get_irq_byname(pdev, "seqcomplete");
	if (de->irq_seqcomplete < 0)
		return de->irq_seqcomplete;

	de->id = of_alias_get_id(dev->of_node, "dc0-display-engine");
	if (de->id < 0) {
		dev_err(dev, "failed to get alias id: %d\n", de->id);
		return de->id;
	}

	de->dev = dev;

	dev_set_drvdata(dev, de);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	dc_drm->de[de->id] = de;

	return 0;
}

static const struct component_ops dc_de_ops = {
	.bind = dc_de_bind,
};

static int dc_de_probe(struct platform_device *pdev)
{
	int ret;

	ret = devm_of_platform_populate(&pdev->dev);
	if (ret < 0)
		return ret;

	ret = component_add(&pdev->dev, &dc_de_ops);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to add component\n");

	return 0;
}

static void dc_de_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dc_de_ops);
}

static int dc_de_runtime_resume(struct device *dev)
{
	struct dc_de *de = dev_get_drvdata(dev);

	dc_dec_init(de);
	dc_fg_init(de->fg);
	dc_tc_init(de->tc);

	return 0;
}

static const struct dev_pm_ops dc_de_pm_ops = {
	RUNTIME_PM_OPS(NULL, dc_de_runtime_resume, NULL)
};

static const struct of_device_id dc_de_dt_ids[] = {
	{ .compatible = "fsl,imx8qxp-dc-display-engine", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, dc_de_dt_ids);

struct platform_driver dc_de_driver = {
	.probe = dc_de_probe,
	.remove = dc_de_remove,
	.driver = {
		.name = "imx8-dc-display-engine",
		.suppress_bind_attrs = true,
		.of_match_table = dc_de_dt_ids,
		.pm = pm_sleep_ptr(&dc_de_pm_ops),
	},
};
