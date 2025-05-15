// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 */

#include <drm/drm_fourcc.h>
#include <drm/drm_blend.h>
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
#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"
#include "mtk_disp_blender.h"
#include "mtk_disp_ovl.h"

#define DISP_REG_OVL_BLD_DATAPATH_CON			0x010
#define OVL_BLD_BGCLR_IN_SEL					BIT(0)
#define OVL_BLD_BGCLR_OUT_TO_PROC				BIT(4)
#define OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER				BIT(5)
#define DISP_REG_OVL_BLD_EN				0x020
#define OVL_BLD_EN						BIT(0)
#define OVL_BLD_FORCE_RELAY_MODE				BIT(4)
#define OVL_BLD_RELAY_MODE					BIT(5)
#define DISP_REG_OVL_BLD_RST				0x024
#define OVL_BLD_RST						BIT(0)
#define DISP_REG_OVL_BLD_SHADOW_CTRL			0x028
#define OVL_BLD_BYPASS_SHADOW					BIT(2)
#define DISP_REG_OVL_BLD_BGCLR_BALCK			0xff000000
#define DISP_REG_OVL_BLD_ROI_SIZE			0x030
#define DISP_REG_OVL_BLD_L_EN				0x040
#define OVL_BLD_L_EN						BIT(0)
#define DISP_REG_OVL_BLD_OFFSET				0x044
#define DISP_REG_OVL_BLD_SRC_SIZE			0x048
#define DISP_REG_OVL_BLD_L0_CLRFMT			0x050
#define OVL_BLD_CON_FLD_CLRFMT					GENMASK(3, 0)
#define OVL_BLD_CON_CLRFMT_MAN					BIT(4)
#define OVL_BLD_CON_FLD_CLRFMT_NB				GENMASK(9, 8)
#define OVL_BLD_CON_CLRFMT_NB_10_BIT				BIT(8)
#define OVL_BLD_CON_BYTE_SWAP					BIT(16)
#define OVL_BLD_CON_RGB_SWAP					BIT(17)
#define DISP_REG_OVL_BLD_BGCLR_CLR			0x104
#define DISP_REG_OVL_BLD_L_CON2				0x200
#define OVL_BLD_L_ALPHA						GENMASK(7, 0)
#define OVL_BLD_L_ALPHA_EN					BIT(12)
#define DISP_REG_OVL_BLD_L0_ALPHA_SEL			0x208
#define OVL_BLD_L0_CONST					BIT(24)
#define DISP_REG_OVL_BLD_L0_CLR				0x20c
#define OVL_BLD_CON_CLRFMT_MAN					BIT(4)

struct mtk_disp_blender {
	void __iomem		*regs;
	struct clk		*clk;
	struct cmdq_client_reg	cmdq_reg;
};

void mtk_disp_blender_layer_config(struct device *dev, struct mtk_plane_state *state,
				   struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);
	struct mtk_plane_pending_state *pending = &state->pending;
	u32 alpha, clrfmt, ignore_pixel_alpha = 0;
	u32 blend_mode = mtk_ovl_get_blend_mode(state, MTK_OVL_SUPPORT_BLEND_MODES);

	if (!pending->enable) {
		mtk_ddp_write(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs, DISP_REG_OVL_BLD_L_EN);
		return;
	}

	mtk_ddp_write(cmdq_pkt, pending->y << 16 | pending->x, &priv->cmdq_reg, priv->regs,
		      DISP_REG_OVL_BLD_OFFSET);

	mtk_ddp_write(cmdq_pkt, pending->height << 16 | pending->width, &priv->cmdq_reg,
		      priv->regs, DISP_REG_OVL_BLD_SRC_SIZE);

	clrfmt = mtk_ovl_fmt_convert(pending->format, blend_mode, true, false, 0,
				     OVL_BLD_CON_CLRFMT_MAN, OVL_BLD_CON_BYTE_SWAP,
				     OVL_BLD_CON_RGB_SWAP);
	clrfmt |= mtk_ovl_is_10bit_rgb(pending->format) ? OVL_BLD_CON_CLRFMT_NB_10_BIT : 0;
	mtk_ddp_write_mask(cmdq_pkt, clrfmt, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_L0_CLRFMT, OVL_BLD_CON_CLRFMT_MAN |
			   OVL_BLD_CON_RGB_SWAP |  OVL_BLD_CON_BYTE_SWAP |
			   OVL_BLD_CON_FLD_CLRFMT | OVL_BLD_CON_FLD_CLRFMT_NB);

	if (mtk_ovl_is_ignore_pixel_alpha(state, blend_mode))
		ignore_pixel_alpha = OVL_BLD_L0_CONST;
	mtk_ddp_write_mask(cmdq_pkt, ignore_pixel_alpha, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_L0_ALPHA_SEL, OVL_BLD_L0_CONST);

	alpha = (OVL_BLD_L_ALPHA & (state->base.alpha >> 8)) | OVL_BLD_L_ALPHA_EN;
	mtk_ddp_write_mask(cmdq_pkt, alpha, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_L_CON2, OVL_BLD_L_ALPHA_EN | OVL_BLD_L_ALPHA);

	mtk_ddp_write(cmdq_pkt, OVL_BLD_L_EN, &priv->cmdq_reg, priv->regs, DISP_REG_OVL_BLD_L_EN);
}

void mtk_disp_blender_config(struct device *dev, unsigned int w,
			     unsigned int h, unsigned int vrefresh,
			     unsigned int bpc, bool most_top,
			     bool most_bottom, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);
	u32 datapath;

	dev_dbg(dev, "%s-w:%d, h:%d\n", __func__, w, h);
	mtk_ddp_write(cmdq_pkt, h << 16 | w, &priv->cmdq_reg, priv->regs,
		      DISP_REG_OVL_BLD_ROI_SIZE);
	mtk_ddp_write(cmdq_pkt, DISP_REG_OVL_BLD_BGCLR_BALCK, &priv->cmdq_reg, priv->regs,
		      DISP_REG_OVL_BLD_BGCLR_CLR);

	/*
	 * The input source of blender layer can be EXDMA layer output or
	 * the blender constant layer itself.
	 * This color setting is configured for the blender constant layer.
	 */
	mtk_ddp_write(cmdq_pkt, DISP_REG_OVL_BLD_BGCLR_BALCK, &priv->cmdq_reg, priv->regs,
		      DISP_REG_OVL_BLD_L0_CLR);

	if (most_top)
		datapath = OVL_BLD_BGCLR_OUT_TO_PROC;
	else
		datapath = OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER;
	/*
	 * The primary input is from EXDMA and the second input
	 * is optionally from another blender
	 */
	if (!most_bottom)
		datapath |= OVL_BLD_BGCLR_IN_SEL;

	mtk_ddp_write_mask(cmdq_pkt, datapath,
			   &priv->cmdq_reg, priv->regs, DISP_REG_OVL_BLD_DATAPATH_CON,
			   OVL_BLD_BGCLR_OUT_TO_PROC | OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER |
			   OVL_BLD_BGCLR_IN_SEL);
}

void mtk_disp_blender_start(struct device *dev, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);

	mtk_ddp_write_mask(cmdq_pkt, OVL_BLD_BYPASS_SHADOW, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_SHADOW_CTRL, OVL_BLD_BYPASS_SHADOW);
	mtk_ddp_write_mask(cmdq_pkt, OVL_BLD_EN, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_EN, OVL_BLD_EN);
}

void mtk_disp_blender_stop(struct device *dev, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);

	mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_EN, OVL_BLD_EN);

	mtk_ddp_write_mask(cmdq_pkt, OVL_BLD_RST, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_RST, OVL_BLD_RST);
	mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_RST, OVL_BLD_RST);
}

int mtk_disp_blender_clk_enable(struct device *dev)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);

	return clk_prepare_enable(priv->clk);
}

void mtk_disp_blender_clk_disable(struct device *dev)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);

	clk_disable_unprepare(priv->clk);
}

u32 mtk_disp_blender_get_blend_modes(struct device *dev)
{
	return MTK_OVL_SUPPORT_BLEND_MODES;
}

static int mtk_disp_blender_bind(struct device *dev, struct device *master,
				 void *data)
{
	return 0;
}

static void mtk_disp_blender_unbind(struct device *dev, struct device *master, void *data)
{
}

static const struct component_ops mtk_disp_blender_component_ops = {
	.bind	= mtk_disp_blender_bind,
	.unbind = mtk_disp_blender_unbind,
};

static int mtk_disp_blender_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct mtk_disp_blender *priv;
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->regs)) {
		dev_err(dev, "failed to ioremap blender\n");
		return PTR_ERR(priv->regs);
	}

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(dev, "failed to get blender clk\n");
		return PTR_ERR(priv->clk);
	}

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret)
		dev_dbg(dev, "No mediatek,gce-client-reg\n");
#endif
	platform_set_drvdata(pdev, priv);

	ret = component_add(dev, &mtk_disp_blender_component_ops);
	if (ret)
		dev_notice(dev, "Failed to add component: %d\n", ret);

	return ret;
}

static void mtk_disp_blender_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_blender_component_ops);
}

static const struct of_device_id mtk_disp_blender_driver_dt_match[] = {
	{ .compatible = "mediatek,mt8196-blender"},
	{},
};

MODULE_DEVICE_TABLE(of, mtk_disp_blender_driver_dt_match);

struct platform_driver mtk_disp_blender_driver = {
	.probe		= mtk_disp_blender_probe,
	.remove		= mtk_disp_blender_remove,
	.driver		= {
		.name	= "mediatek-disp-blender",
		.owner	= THIS_MODULE,
		.of_match_table = mtk_disp_blender_driver_dt_match,
	},
};

MODULE_AUTHOR("Nancy Lin <nancy.lin@mediatek.com>");
MODULE_DESCRIPTION("MediaTek Blender Driver");
MODULE_LICENSE("GPL");
