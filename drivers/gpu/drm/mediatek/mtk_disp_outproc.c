// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 */

#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mtk-cmdq.h>
#include <linux/soc/mediatek/mtk-mmsys.h>

#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_disp_outproc.h"
#include "mtk_drm_drv.h"

#define DISP_REG_OVL_OUTPROC_INTEN				0x004
#define OVL_OUTPROC_FME_CPL_INTEN					BIT(1)
#define DISP_REG_OVL_OUTPROC_INTSTA				0x008
#define DISP_REG_OVL_OUTPROC_DATAPATH_CON			0x010
#define OVL_OUTPROC_DATAPATH_CON_OUTPUT_CLAMP				BIT(26)

#define DISP_REG_OVL_OUTPROC_EN					0x020
#define OVL_OUTPROC_OVL_EN						BIT(0)
#define DISP_REG_OVL_OUTPROC_RST				0x024
#define OVL_OUTPROC_RST							BIT(0)
#define DISP_REG_OVL_OUTPROC_SHADOW_CTRL			0x028
#define OVL_OUTPROC_BYPASS_SHADOW					BIT(2)
#define DISP_REG_OVL_OUTPROC_ROI_SIZE				0x030

struct mtk_disp_outproc {
	void __iomem		*regs;
	struct clk		*clk;
	void			(*vblank_cb)(void *data);
	void			*vblank_cb_data;
	int			irq;
	struct cmdq_client_reg	cmdq_reg;
};

void mtk_disp_outproc_register_vblank_cb(struct device *dev,
					 void (*vblank_cb)(void *),
					 void *vblank_cb_data)
{
	struct mtk_disp_outproc *priv = dev_get_drvdata(dev);

	priv->vblank_cb = vblank_cb;
	priv->vblank_cb_data = vblank_cb_data;
}

void mtk_disp_outproc_unregister_vblank_cb(struct device *dev)
{
	struct mtk_disp_outproc *priv = dev_get_drvdata(dev);

	priv->vblank_cb = NULL;
	priv->vblank_cb_data = NULL;
}

void mtk_disp_outproc_enable_vblank(struct device *dev)
{
	struct mtk_disp_outproc *priv = dev_get_drvdata(dev);

	writel(OVL_OUTPROC_FME_CPL_INTEN, priv->regs + DISP_REG_OVL_OUTPROC_INTEN);
}

void mtk_disp_outproc_disable_vblank(struct device *dev)
{
	struct mtk_disp_outproc *priv = dev_get_drvdata(dev);

	writel(0x0, priv->regs + DISP_REG_OVL_OUTPROC_INTEN);
}

static irqreturn_t mtk_disp_outproc_irq_handler(int irq, void *dev_id)
{
	struct mtk_disp_outproc *priv = dev_id;
	u32 val;

	val = readl(priv->regs + DISP_REG_OVL_OUTPROC_INTSTA);
	if (!val)
		return IRQ_NONE;

	writel(0x0, priv->regs + DISP_REG_OVL_OUTPROC_INTSTA);

	if (priv->vblank_cb)
		priv->vblank_cb(priv->vblank_cb_data);

	return IRQ_HANDLED;
}

void mtk_disp_outproc_config(struct device *dev, unsigned int w,
			     unsigned int h, unsigned int vrefresh,
			     unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_outproc *priv = dev_get_drvdata(dev);
	u32 tmp;

	dev_dbg(dev, "%s-w:%d, h:%d\n", __func__, w, h);

	writel((h << 16) | w, priv->regs + DISP_REG_OVL_OUTPROC_ROI_SIZE);

	tmp = readl(priv->regs + DISP_REG_OVL_OUTPROC_DATAPATH_CON);
	tmp = (tmp & ~OVL_OUTPROC_DATAPATH_CON_OUTPUT_CLAMP) |
		  (OVL_OUTPROC_DATAPATH_CON_OUTPUT_CLAMP & OVL_OUTPROC_DATAPATH_CON_OUTPUT_CLAMP);
	writel(tmp, priv->regs + DISP_REG_OVL_OUTPROC_DATAPATH_CON);
}

void mtk_disp_outproc_start(struct device *dev)
{
	struct mtk_disp_outproc *priv = dev_get_drvdata(dev);
	unsigned int tmp;

	tmp = readl(priv->regs + DISP_REG_OVL_OUTPROC_SHADOW_CTRL);
	tmp = tmp | OVL_OUTPROC_BYPASS_SHADOW;
	writel(tmp, priv->regs + DISP_REG_OVL_OUTPROC_SHADOW_CTRL);

	writel(0, priv->regs + DISP_REG_OVL_OUTPROC_INTSTA);

	tmp = readl(priv->regs + DISP_REG_OVL_OUTPROC_EN);
	tmp = (tmp & ~OVL_OUTPROC_OVL_EN) | (OVL_OUTPROC_OVL_EN & OVL_OUTPROC_OVL_EN);
	writel(tmp, priv->regs + DISP_REG_OVL_OUTPROC_EN);
}

void mtk_disp_outproc_stop(struct device *dev)
{
	struct mtk_disp_outproc *priv = dev_get_drvdata(dev);

	writel(0, priv->regs + DISP_REG_OVL_OUTPROC_EN);
	writel(OVL_OUTPROC_RST, priv->regs + DISP_REG_OVL_OUTPROC_RST);
	writel(0, priv->regs + DISP_REG_OVL_OUTPROC_RST);
}

int mtk_disp_outproc_clk_enable(struct device *dev)
{
	struct mtk_disp_outproc *priv = dev_get_drvdata(dev);

	return clk_prepare_enable(priv->clk);
}

void mtk_disp_outproc_clk_disable(struct device *dev)
{
	struct mtk_disp_outproc *priv = dev_get_drvdata(dev);

	clk_disable_unprepare(priv->clk);
}

static int mtk_disp_outproc_bind(struct device *dev, struct device *master,
				 void *data)
{
	return 0;
}

static void mtk_disp_outproc_unbind(struct device *dev, struct device *master, void *data)
{
}

static const struct component_ops mtk_disp_outproc_component_ops = {
	.bind	= mtk_disp_outproc_bind,
	.unbind = mtk_disp_outproc_unbind,
};

static int mtk_disp_outproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_outproc *priv;
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs), "failed to ioremap outproc\n");

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk), "failed to get outproc clk\n");

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret)
		dev_dbg(dev, "No mediatek,gce-client-reg\n");
#endif

	if (of_property_read_u32_index(dev->of_node, "interrupts", 0, &ret)) {
		dev_dbg(dev, "interrupts not defined\n");
	} else {
		priv->irq = platform_get_irq(pdev, 0);
		if (priv->irq < 0)
			priv->irq = 0;

		if (priv->irq) {
			ret = devm_request_irq(dev, priv->irq, mtk_disp_outproc_irq_handler,
					       IRQF_TRIGGER_NONE, dev_name(dev), priv);
			if (ret < 0) {
				dev_err(dev, "Failed to request irq %d: %d\n", priv->irq, ret);
				return ret;
			}
		}
	}

	platform_set_drvdata(pdev, priv);

	ret = component_add(dev, &mtk_disp_outproc_component_ops);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add component\n");

	return ret;
}

static void mtk_disp_outproc_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_outproc_component_ops);
}

static const struct of_device_id mtk_disp_outproc_driver_dt_match[] = {
	{ .compatible = "mediatek,mt8196-outproc"},
	{},
};

MODULE_DEVICE_TABLE(of, mtk_disp_outproc_driver_dt_match);

struct platform_driver mtk_disp_outproc_driver = {
	.probe		= mtk_disp_outproc_probe,
	.remove		= mtk_disp_outproc_remove,
	.driver		= {
		.name	= "mediatek-disp-outproc",
		.owner	= THIS_MODULE,
		.of_match_table = mtk_disp_outproc_driver_dt_match,
	},
};

MODULE_AUTHOR("Nancy Lin <nancy.lin@mediatek.com>");
MODULE_DESCRIPTION("MediaTek Output processing Driver");
MODULE_LICENSE("GPL");
