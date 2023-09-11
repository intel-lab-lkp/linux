// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mtk_disp_drv.h"
#include "mtk_drm_crtc.h"
#include "mtk_drm_ddp_comp.h"

/**
 * struct mtk_padding - basic information of Padding
 * @clk: Clock of the module
 * @regs: Virtual address of the Padding for CPU to access
 * @cmdq_reg: CMDQ setting of the Padding
 *
 * Every Padding should have different clock source, register base, and
 * CMDQ settings, we stored these differences all together.
 */
struct mtk_padding {
	struct clk		*clk;
	void __iomem		*regs;
	struct cmdq_client_reg	cmdq_reg;
};

int mtk_padding_clk_enable(struct device *dev)
{
	struct mtk_padding *padding = dev_get_drvdata(dev);

	return clk_prepare_enable(padding->clk);
}

void mtk_padding_clk_disable(struct device *dev)
{
	struct mtk_padding *padding = dev_get_drvdata(dev);

	clk_disable_unprepare(padding->clk);
}

void mtk_padding_config(struct device *dev, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_padding *padding = dev_get_drvdata(dev);

	/* bypass padding */
	mtk_ddp_write_mask(cmdq_pkt, GENMASK(1, 0), &padding->cmdq_reg, padding->regs, 0,
			   GENMASK(1, 0));
}

static int mtk_padding_bind(struct device *dev, struct device *master, void *data)
{
	return 0;
}

static void mtk_padding_unbind(struct device *dev, struct device *master, void *data)
{
}

static const struct component_ops mtk_padding_component_ops = {
	.bind	= mtk_padding_bind,
	.unbind = mtk_padding_unbind,
};

static int mtk_padding_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_padding *priv;
	struct resource *res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(dev, "failed to get clk\n");
		return PTR_ERR(priv->clk);
	}

	priv->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(priv->regs)) {
		dev_err(dev, "failed to do ioremap\n");
		return PTR_ERR(priv->regs);
	}

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret) {
		dev_err(dev, "failed to get gce client reg\n");
		return ret;
	}
#endif

	platform_set_drvdata(pdev, priv);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = component_add(dev, &mtk_padding_component_ops);
	if (ret) {
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "failed to add component\n");
	}

	return 0;
}

static int mtk_padding_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_padding_component_ops);
	return 0;
}

static const struct of_device_id mtk_padding_driver_dt_match[] = {
	{ .compatible = "mediatek,mt8188-padding" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_padding_driver_dt_match);

struct platform_driver mtk_padding_driver = {
	.probe		= mtk_padding_probe,
	.remove		= mtk_padding_remove,
	.driver		= {
		.name	= "mediatek-padding",
		.owner	= THIS_MODULE,
		.of_match_table = mtk_padding_driver_dt_match,
	},
};
