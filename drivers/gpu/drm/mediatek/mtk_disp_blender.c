// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021 MediaTek Inc.
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

#define DISP_REG_OVL_BLD_DATAPATH_CON			0x010
#define OVL_BLD_BGCLR_IN_SEL					BIT(0)
#define OVL_BLD_BGCLR_OUT_TO_PROC				BIT(4)
#define OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER				BIT(5)

#define DISP_REG_OVL_BLD_EN				0x020
#define OVL_BLD_EN						BIT(0)
#define OVL_BLD_FORCE_RELAY_MODE				BIT(4)
#define OVL_RELAY_MODE						BIT(5)
#define DISP_REG_OVL_BLD_RST				0x024
#define OVL_BLD_RST						BIT(0)
#define DISP_REG_OVL_BLD_SHADOW_CTRL			0x028
#define DISP_OVL_BLD_BYPASS_SHADOW				BIT(2)
#define DISP_OVL_BLD_BGCLR_BALCK			0xff000000
#define DISP_REG_OVL_BLD_ROI_SIZE			0x030
#define DISP_REG_OVL_BLD_L_EN				0x040
#define OVL_BLD_L_EN						BIT(0)
#define DISP_REG_BLD_OVL_OFFSET				0x044
#define DISP_REG_BLD_OVL_SRC_SIZE			0x048
#define DISP_REG_OVL_BLD_L0_CLRFMT			0x050
#define OVL_BLD_CON_FLD_CLRFMT					GENMASK(3, 0)
#define OVL_BLD_CON_CLRFMT_MAN					BIT(4)
#define OVL_BLD_CON_FLD_CLRFMT_NB				GENMASK(9, 8)
#define OVL_BLD_CON_CLRFMT_NB_10_BIT				BIT(8)
#define OVL_BLD_CON_BYTE_SWAP					BIT(16)
#define OVL_BLD_CON_RGB_SWAP					BIT(17)
#define OVL_BLD_CON_CLRFMT_RGB565				0x000
#define OVL_BLD_CON_CLRFMT_BGR888				0x001
#define OVL_BLD_CON_CLRFMT_BGRA8888				0x002
#define OVL_BLD_CON_CLRFMT_ABGRB8888				0x003
#define OVL_BLD_CON_CLRFMT_UYVY					0x004
#define OVL_BLD_CON_CLRFMT_YUYV					0x005
#define OVL_BLD_CON_CLRFMT_BGR565				(0x000 | OVL_BLD_CON_BYTE_SWAP)
#define OVL_BLD_CON_CLRFMT_RGB888				(0x001 | OVL_BLD_CON_BYTE_SWAP)
#define OVL_BLD_CON_CLRFMT_RGBA8888				(0x002 | OVL_BLD_CON_BYTE_SWAP)
#define OVL_BLD_CON_CLRFMT_ARGB8888				(0x003 | OVL_BLD_CON_BYTE_SWAP)
#define OVL_BLD_CON_CLRFMT_VYUY					(0x004 | OVL_BLD_CON_BYTE_SWAP)
#define OVL_BLD_CON_CLRFMT_YVYU					(0x005 | OVL_BLD_CON_BYTE_SWAP)
#define OVL_BLD_CON_CLRFMT_PBGRA8888				(0x003 | OVL_BLD_CON_CLRFMT_MAN)
#define OVL_BLD_CON_CLRFMT_PARGB8888				(OVL_BLD_CON_CLRFMT_PBGRA8888 | \
								 OVL_BLD_CON_BYTE_SWAP)
#define OVL_BLD_CON_CLRFMT_PRGBA8888				(OVL_BLD_CON_CLRFMT_PBGRA8888 | \
								 OVL_BLD_CON_RGB_SWAP)
#define OVL_BLD_CON_CLRFMT_PABGR8888				(OVL_BLD_CON_CLRFMT_PBGRA8888 | \
								 OVL_BLD_CON_RGB_SWAP | \
								 OVL_BLD_CON_BYTE_SWAP)
#define DISP_REG_OVL_BLD_BGCLR_CLR			0x104
#define DISP_REG_OVL_BLD_L_CON2				0x200
#define OVL_BLD_L_ALPHA						GENMASK(7, 0)
#define OVL_BLD_L_ALPHA_EN					BIT(12)
#define DISP_REG_OVL_BLD_L0_PITCH			0x208
#define OVL_L0_CONST_BLD					BIT(24)

struct mtk_disp_blender {
	void __iomem		*regs;
	struct clk		*clk;
	struct cmdq_client_reg	cmdq_reg;
};

static inline bool is_10bit_rgb(u32 fmt)
{
	switch (fmt) {
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_BGRA1010102:
		return true;
	}
	return false;
}

static unsigned int mtk_disp_blender_fmt_convert(unsigned int fmt, unsigned int blend_mode)
{
	/*
	 * DRM_FORMAT: bit 32->0, BLD_FMT: bit 0->32,
	 * so DRM_FORMAT_RGB888 = OVL_BLD_CON_CLRFMT_BGR888
	 */
	switch (fmt) {
	default:
	case DRM_FORMAT_BGR565:
		return OVL_BLD_CON_CLRFMT_RGB565;
	case DRM_FORMAT_RGB565:
		return OVL_BLD_CON_CLRFMT_BGR565;
	case DRM_FORMAT_RGB888:
		return OVL_BLD_CON_CLRFMT_BGR888;
	case DRM_FORMAT_BGR888:
		return OVL_BLD_CON_CLRFMT_RGB888;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_RGBA1010102:
		return ((blend_mode == DRM_MODE_BLEND_PREMULTI) ?
			OVL_BLD_CON_CLRFMT_PABGR8888 : OVL_BLD_CON_CLRFMT_ABGRB8888) |
			(is_10bit_rgb(fmt) ? OVL_BLD_CON_CLRFMT_NB_10_BIT : 0);
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_BGRA1010102:
		return ((blend_mode == DRM_MODE_BLEND_PREMULTI) ?
			OVL_BLD_CON_CLRFMT_PARGB8888 : OVL_BLD_CON_CLRFMT_ARGB8888) |
			(is_10bit_rgb(fmt) ? OVL_BLD_CON_CLRFMT_NB_10_BIT : 0);
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		return ((blend_mode == DRM_MODE_BLEND_PREMULTI) ?
			OVL_BLD_CON_CLRFMT_PBGRA8888 : OVL_BLD_CON_CLRFMT_BGRA8888) |
			(is_10bit_rgb(fmt) ? OVL_BLD_CON_CLRFMT_NB_10_BIT : 0);
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return ((blend_mode == DRM_MODE_BLEND_PREMULTI) ?
			OVL_BLD_CON_CLRFMT_PRGBA8888 : OVL_BLD_CON_CLRFMT_RGBA8888) |
			(is_10bit_rgb(fmt) ? OVL_BLD_CON_CLRFMT_NB_10_BIT : 0);
	case DRM_FORMAT_UYVY:
		return OVL_BLD_CON_CLRFMT_UYVY;
	case DRM_FORMAT_YUYV:
		return OVL_BLD_CON_CLRFMT_YUYV;
	}
}

void mtk_disp_blender_layer_config(struct device *dev, struct mtk_plane_state *state,
				   struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);
	struct mtk_plane_pending_state *pending = &state->pending;
	unsigned int align_width = ALIGN_DOWN(pending->width, 2);
	unsigned int alpha;
	unsigned int clrfmt;
	unsigned int blend_mode = DRM_MODE_BLEND_PIXEL_NONE;

	if (!pending->enable) {
		mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs,
				   DISP_REG_OVL_BLD_L_EN, OVL_BLD_L_EN);
		return;
	}

	mtk_ddp_write(cmdq_pkt, pending->y << 16 | pending->x, &priv->cmdq_reg, priv->regs,
		      DISP_REG_BLD_OVL_OFFSET);

	mtk_ddp_write(cmdq_pkt, pending->height << 16 | align_width, &priv->cmdq_reg, priv->regs,
		      DISP_REG_BLD_OVL_SRC_SIZE);

	if (state->base.fb && state->base.fb->format->has_alpha)
		blend_mode = state->base.pixel_blend_mode;

	clrfmt = mtk_disp_blender_fmt_convert(pending->format, blend_mode);

	mtk_ddp_write_mask(cmdq_pkt, clrfmt, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_L0_CLRFMT, OVL_BLD_CON_CLRFMT_MAN |
			   OVL_BLD_CON_RGB_SWAP |  OVL_BLD_CON_BYTE_SWAP |
			   OVL_BLD_CON_FLD_CLRFMT | OVL_BLD_CON_FLD_CLRFMT_NB);

	alpha = (0xFF & (state->base.alpha >> 8)) | OVL_BLD_L_ALPHA_EN;
	mtk_ddp_write_mask(cmdq_pkt, alpha, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_L_CON2, OVL_BLD_L_ALPHA_EN | OVL_BLD_L_ALPHA);

	mtk_ddp_write_mask(cmdq_pkt, OVL_BLD_L_EN, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_L_EN, OVL_BLD_L_EN);

	if (blend_mode == DRM_MODE_BLEND_PIXEL_NONE)
		mtk_ddp_write_mask(cmdq_pkt, OVL_L0_CONST_BLD, &priv->cmdq_reg, priv->regs,
				   DISP_REG_OVL_BLD_L0_PITCH, OVL_L0_CONST_BLD);
	else
		mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs,
				   DISP_REG_OVL_BLD_L0_PITCH, OVL_L0_CONST_BLD);
}

void mtk_disp_blender_config(struct device *dev, unsigned int w,
			     unsigned int h, unsigned int vrefresh,
			     unsigned int bpc, enum mtk_disp_blender_layer blender,
			     struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);
	unsigned int tmp;

	dev_dbg(dev, "%s-w:%d, h:%d\n", __func__, w, h);

	tmp = readl(priv->regs + DISP_REG_OVL_BLD_SHADOW_CTRL);
	tmp = tmp | DISP_OVL_BLD_BYPASS_SHADOW;
	writel(tmp, priv->regs + DISP_REG_OVL_BLD_SHADOW_CTRL);

	mtk_ddp_write(cmdq_pkt, h << 16 | w, &priv->cmdq_reg, priv->regs,
		      DISP_REG_OVL_BLD_ROI_SIZE);
	mtk_ddp_write(cmdq_pkt, DISP_OVL_BLD_BGCLR_BALCK, &priv->cmdq_reg, priv->regs,
		      DISP_REG_OVL_BLD_BGCLR_CLR);

	if (blender == FIRST_BLENDER)
		mtk_ddp_write_mask(cmdq_pkt, OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER,
				   &priv->cmdq_reg, priv->regs, DISP_REG_OVL_BLD_DATAPATH_CON,
				   OVL_BLD_BGCLR_OUT_TO_PROC | OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER |
				   OVL_BLD_BGCLR_IN_SEL);
	else if (blender == LAST_BLENDER)
		mtk_ddp_write_mask(cmdq_pkt, OVL_BLD_BGCLR_OUT_TO_PROC | OVL_BLD_BGCLR_IN_SEL,
				   &priv->cmdq_reg, priv->regs, DISP_REG_OVL_BLD_DATAPATH_CON,
				   OVL_BLD_BGCLR_OUT_TO_PROC | OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER |
				   OVL_BLD_BGCLR_IN_SEL);
	else if (blender == SINGLE_BLENDER)
		mtk_ddp_write_mask(cmdq_pkt, OVL_BLD_BGCLR_OUT_TO_PROC,
				   &priv->cmdq_reg, priv->regs, DISP_REG_OVL_BLD_DATAPATH_CON,
				   OVL_BLD_BGCLR_OUT_TO_PROC | OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER |
				   OVL_BLD_BGCLR_IN_SEL);
	else
		mtk_ddp_write_mask(cmdq_pkt, OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER | OVL_BLD_BGCLR_IN_SEL,
				   &priv->cmdq_reg, priv->regs, DISP_REG_OVL_BLD_DATAPATH_CON,
				   OVL_BLD_BGCLR_OUT_TO_PROC | OVL_BLD_BGCLR_OUT_TO_NEXT_LAYER |
				   OVL_BLD_BGCLR_IN_SEL);
}

void mtk_disp_blender_start(struct device *dev, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);

	mtk_ddp_write_mask(cmdq_pkt, OVL_BLD_EN, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_EN, OVL_BLD_EN);
}

void mtk_disp_blender_stop(struct device *dev, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_blender *priv = dev_get_drvdata(dev);

	mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_BLD_L_EN, OVL_BLD_L_EN);
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
