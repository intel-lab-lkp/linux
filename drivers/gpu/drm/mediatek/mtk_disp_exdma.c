// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <drm/drm_fourcc.h>
#include <drm/drm_blend.h>
#include <drm/drm_framebuffer.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"

#define DISP_REG_OVL_EN_CON			0xc
#define OVL_OP_8BIT_MODE				BIT(4)
#define OVL_HG_FOVL_CK_ON				BIT(8)
#define OVL_HF_FOVL_CK_ON				BIT(10)
#define DISP_REG_OVL_DATAPATH_CON		0x014
#define DATAPATH_CON_LAYER_SMI_ID_EN			BIT(0)
#define DATAPATH_CON_GCLAST_EN				BIT(24)
#define DATAPATH_CON_HDR_GCLAST_EN			BIT(25)
#define DISP_REG_OVL_EN				0x020
#define DISP_OVL_EN					BIT(0)
#define DISP_REG_OVL_RST			0x024
#define DISP_OVL_RST					BIT(0)
#define DISP_REG_OVL_ROI_SIZE			0x030
#define DISP_REG_OVL_L0_EN			0x040
#define DISP_OVL_L0_EN					BIT(0)
#define DISP_REG_OVL_OFFSET			0x044
#define DISP_REG_OVL_SRC_SIZE			0x048
#define DISP_REG_OVL_L0_CLRFMT			0x050
#define OVL_CON_FLD_CLRFMT				GENMASK(3, 0)
#define OVL_CON_CLRFMT_MAN				BIT(4)
#define OVL_CON_FLD_CLRFMT_NB				GENMASK(9, 8)
#define OVL_CON_CLRFMT_NB_10_BIT			BIT(8)
#define OVL_CON_BYTE_SWAP				BIT(16)
#define OVL_CON_RGB_SWAP				BIT(17)
#define OVL_CON_CLRFMT_RGB565				0x000
#define OVL_CON_CLRFMT_BGR888				0x001
#define OVL_CON_CLRFMT_BGRA8888				0x002
#define OVL_CON_CLRFMT_ABGRB8888			0x003
#define OVL_CON_CLRFMT_UYVY				0x004
#define OVL_CON_CLRFMT_YUYV				0x005
#define OVL_CON_CLRFMT_BGR565				(0x000 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_RGB888				(0x001 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_RGBA8888				(0x002 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_ARGB8888				(0x003 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_VYUY				(0x004 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_YVYU				(0x005 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_PBGRA8888			(0x003 | OVL_CON_CLRFMT_MAN)
#define OVL_CON_CLRFMT_PARGB8888			(OVL_CON_CLRFMT_PBGRA8888 | \
							OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_PRGBA8888			(OVL_CON_CLRFMT_PBGRA8888 | \
							OVL_CON_RGB_SWAP)
#define OVL_CON_CLRFMT_PABGR8888			(OVL_CON_CLRFMT_PBGRA8888 | \
							OVL_CON_RGB_SWAP | \
							OVL_CON_BYTE_SWAP)
#define DISP_REG_OVL_RDMA0_CTRL			0x100
#define DISP_RDMA0_EN					BIT(0)
#define DISP_REG_OVL_RDMA_BURST_CON1		0x1f4
#define DISP_RDMA_BURST_CON1_BURST16_EN			BIT(28)
#define DISP_RDMA_BURST_CON1_DDR_EN			BIT(30)
#define DISP_RDMA_BURST_CON1_DDR_ACK_EN			BIT(31)
#define DISP_REG_OVL_DUMMY_REG			0x200
#define DISP_OVL_EXT_DDR_EN_OPT				BIT(2)
#define DISP_OVL_FORCE_EXT_DDR_EN			BIT(3)
#define DISP_REG_OVL_GDRDY_PRD			0x208
#define DISP_REG_OVL_PITCH_MSB			0x2f0
#define DISP_REG_OVL_PITCH			0x2f4
#define OVL_L0_SRC_PITCH				GENMASK(15, 0)
#define OVL_L0_CONST_BLD				BIT(28)
#define OVL_L0_SRC_PITCH_MASK				GENMASK(15, 0)
#define DISP_REG_OVL_L0_GUSER_EXT		0x2fc
#define OVL_RDMA0_L0_VCSEL				BIT(5)
#define OVL_RDMA0_HDR_L0_VCSEL				BIT(21)
#define DISP_REG_OVL_CON			0x300
#define DISP_OVL_CON_FLD_INT_MTX_SEL			GENMASK(19, 16)
#define DISP_OVL_CON_INT_MTX_BT601_TO_RGB		(6 << 16)
#define DISP_OVL_CON_INT_MTX_BT709_TO_RGB		(7 << 16)
#define DISP_OVL_CON_INT_MTX_EN				BIT(27)
#define DISP_REG_OVL_ADDR			0xf40
#define DISP_REG_OVL_MOUT			0xff0
#define OVL_MOUT_OUT_DATA				BIT(0)
#define OVL_MOUT_BGCLR_OUT				BIT(1)

static const u32 formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_ARGB2101010,
	DRM_FORMAT_RGBX1010102,
	DRM_FORMAT_RGBA1010102,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_BGRX1010102,
	DRM_FORMAT_BGRA1010102,
};

struct mtk_disp_exdma {
	void __iomem		*regs;
	struct clk		*clk;
	struct cmdq_client_reg	cmdq_reg;
	struct device		*larb;
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

static unsigned int mtk_disp_exdma_fmt_convert(unsigned int fmt, unsigned int blend_mode)
{
	/*
	 * DRM_FORMAT: bit 32->0, OVL_FMT: bit 0->32,
	 * so DRM_FORMAT_RGB888 = OVL_CON_CLRFMT_BGR888
	 */
	switch (fmt) {
	default:
	case DRM_FORMAT_BGR565:
		return OVL_CON_CLRFMT_RGB565;
	case DRM_FORMAT_RGB565:
		return OVL_CON_CLRFMT_BGR565;
	case DRM_FORMAT_RGB888:
		return OVL_CON_CLRFMT_BGR888;
	case DRM_FORMAT_BGR888:
		return OVL_CON_CLRFMT_RGB888;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_RGBX1010102:
		return ((blend_mode == DRM_MODE_BLEND_PREMULTI) ?
			OVL_CON_CLRFMT_PABGR8888 : OVL_CON_CLRFMT_ABGRB8888) |
			(is_10bit_rgb(fmt) ? OVL_CON_CLRFMT_NB_10_BIT : 0);
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRA1010102:
	case DRM_FORMAT_BGRX1010102:
		return ((blend_mode == DRM_MODE_BLEND_PREMULTI) ?
			OVL_CON_CLRFMT_PARGB8888 : OVL_CON_CLRFMT_ARGB8888) |
			(is_10bit_rgb(fmt) ? OVL_CON_CLRFMT_NB_10_BIT : 0);
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XRGB2101010:
		return ((blend_mode == DRM_MODE_BLEND_PREMULTI) ?
			OVL_CON_CLRFMT_PBGRA8888 : OVL_CON_CLRFMT_BGRA8888) |
			(is_10bit_rgb(fmt) ? OVL_CON_CLRFMT_NB_10_BIT : 0);
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_XBGR2101010:
		return ((blend_mode == DRM_MODE_BLEND_PREMULTI) ?
			OVL_CON_CLRFMT_PRGBA8888 : OVL_CON_CLRFMT_RGBA8888) |
			(is_10bit_rgb(fmt) ? OVL_CON_CLRFMT_NB_10_BIT : 0);
	case DRM_FORMAT_UYVY:
		return OVL_CON_CLRFMT_UYVY;
	case DRM_FORMAT_YUYV:
		return OVL_CON_CLRFMT_YUYV;
	}
}

static unsigned int exdma_color_convert(unsigned int color_encoding)
{
	switch (color_encoding) {
	default:
	case DRM_COLOR_YCBCR_BT709:
		return DISP_OVL_CON_INT_MTX_BT709_TO_RGB;
	case DRM_COLOR_YCBCR_BT601:
		return DISP_OVL_CON_INT_MTX_BT601_TO_RGB;
	}
}

void mtk_disp_exdma_start(struct device *dev, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_exdma *priv = dev_get_drvdata(dev);
	unsigned int value = 0, mask = 0;

	value = DISP_RDMA_BURST_CON1_BURST16_EN | DISP_RDMA_BURST_CON1_DDR_ACK_EN;
	mask = DISP_RDMA_BURST_CON1_BURST16_EN | DISP_RDMA_BURST_CON1_DDR_EN |
	       DISP_RDMA_BURST_CON1_DDR_ACK_EN;
	mtk_ddp_write_mask(cmdq_pkt, value, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_RDMA_BURST_CON1, mask);
	/*
	 * The dummy register is used in the configuration of the EXDMA engine to
	 * write commands to DRAM, ensuring that data transfers occur normally.
	 */
	value = DISP_OVL_EXT_DDR_EN_OPT | DISP_OVL_FORCE_EXT_DDR_EN;
	mask = DISP_OVL_EXT_DDR_EN_OPT | DISP_OVL_FORCE_EXT_DDR_EN;
	mtk_ddp_write_mask(cmdq_pkt, value, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_DUMMY_REG, mask);

	value = DATAPATH_CON_LAYER_SMI_ID_EN | DATAPATH_CON_HDR_GCLAST_EN | DATAPATH_CON_GCLAST_EN;
	mask = DATAPATH_CON_LAYER_SMI_ID_EN | DATAPATH_CON_HDR_GCLAST_EN | DATAPATH_CON_GCLAST_EN;
	mtk_ddp_write_mask(cmdq_pkt, value, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_DATAPATH_CON, mask);

	mtk_ddp_write_mask(cmdq_pkt, OVL_MOUT_BGCLR_OUT, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_MOUT, OVL_MOUT_BGCLR_OUT | OVL_MOUT_OUT_DATA);

	mtk_ddp_write(cmdq_pkt, ~0, &priv->cmdq_reg, priv->regs, DISP_REG_OVL_GDRDY_PRD);

	mtk_ddp_write_mask(cmdq_pkt, DISP_RDMA0_EN, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_RDMA0_CTRL, DISP_RDMA0_EN);
	mtk_ddp_write_mask(cmdq_pkt, DISP_OVL_L0_EN, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_L0_EN, DISP_OVL_L0_EN);

	mtk_ddp_write_mask(cmdq_pkt, DISP_OVL_EN, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_EN, DISP_OVL_EN);
}

void mtk_disp_exdma_stop(struct device *dev, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_exdma *priv = dev_get_drvdata(dev);

	mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs, DISP_REG_OVL_EN, DISP_OVL_EN);
	mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_RDMA0_CTRL, DISP_RDMA0_EN);
	mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_DATAPATH_CON, DATAPATH_CON_LAYER_SMI_ID_EN);
	mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_L0_EN, DISP_OVL_L0_EN);
	mtk_ddp_write_mask(cmdq_pkt, DISP_OVL_RST, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_RST, DISP_OVL_RST);
	mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_RST, DISP_OVL_RST);
}

void mtk_disp_exdma_config(struct device *dev, struct mtk_plane_state *state,
			   struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_exdma *priv = dev_get_drvdata(dev);
	struct mtk_plane_pending_state *pending = &state->pending;
	const struct drm_format_info *fmt_info = drm_format_info(pending->format);
	unsigned int align_width = 0;
	bool csc_enable = fmt_info->is_yuv ? true : false;
	unsigned int blend_mode = DRM_MODE_BLEND_PIXEL_NONE;
	unsigned int clrfmt = 0;
	unsigned int clrfmt_mask = OVL_CON_RGB_SWAP |
				   OVL_CON_BYTE_SWAP |
				   OVL_CON_CLRFMT_MAN |
				   OVL_CON_FLD_CLRFMT |
				   OVL_CON_FLD_CLRFMT_NB;

	/* OVLSYS is in 1T2P domain, width needs to be 2 pixels align */
	align_width = ALIGN_DOWN(pending->width, 2);

	mtk_ddp_write(cmdq_pkt, pending->height << 16 | align_width, &priv->cmdq_reg,
		      priv->regs, DISP_REG_OVL_ROI_SIZE);

	mtk_ddp_write(cmdq_pkt, pending->height << 16 | align_width, &priv->cmdq_reg,
		      priv->regs, DISP_REG_OVL_SRC_SIZE);
	mtk_ddp_write(cmdq_pkt, pending->height << 16 | align_width, &priv->cmdq_reg,
		      priv->regs, DISP_REG_OVL_SRC_SIZE);
	mtk_ddp_write(cmdq_pkt, pending->addr, &priv->cmdq_reg,
		      priv->regs, DISP_REG_OVL_ADDR);
	mtk_ddp_write_mask(cmdq_pkt, pending->pitch, &priv->cmdq_reg, priv->regs, OVL_L0_SRC_PITCH,
			   OVL_L0_SRC_PITCH_MASK);
	mtk_ddp_write_mask(cmdq_pkt, pending->pitch >> 16, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_PITCH_MSB, 0xf);

	if (csc_enable)
		mtk_ddp_write_mask(cmdq_pkt, exdma_color_convert(pending->color_encoding) |
				   DISP_OVL_CON_INT_MTX_EN, &priv->cmdq_reg, priv->regs,
				   DISP_REG_OVL_CON, DISP_OVL_CON_FLD_INT_MTX_SEL |
				   DISP_OVL_CON_INT_MTX_EN);
	else
		mtk_ddp_write_mask(cmdq_pkt, 0, &priv->cmdq_reg, priv->regs, DISP_REG_OVL_CON,
				   DISP_OVL_CON_INT_MTX_EN);

	/* alpha blend setting */
	if (state->base.fb && state->base.fb->format->has_alpha)
		blend_mode = state->base.pixel_blend_mode;

	clrfmt = mtk_disp_exdma_fmt_convert(pending->format, blend_mode);

	mtk_ddp_write_mask(cmdq_pkt, clrfmt, &priv->cmdq_reg, priv->regs,
			   DISP_REG_OVL_L0_CLRFMT, clrfmt_mask);

	mtk_ddp_write_mask(cmdq_pkt, OVL_OP_8BIT_MODE | OVL_HG_FOVL_CK_ON | OVL_HF_FOVL_CK_ON,
			   &priv->cmdq_reg, priv->regs, DISP_REG_OVL_EN_CON,
			   OVL_OP_8BIT_MODE | OVL_HG_FOVL_CK_ON | OVL_HF_FOVL_CK_ON);

	mtk_ddp_write_mask(cmdq_pkt, OVL_RDMA0_L0_VCSEL | OVL_RDMA0_HDR_L0_VCSEL,
			   &priv->cmdq_reg, priv->regs, DISP_REG_OVL_L0_GUSER_EXT,
			   OVL_RDMA0_L0_VCSEL | OVL_RDMA0_HDR_L0_VCSEL);

	if (blend_mode == DRM_MODE_BLEND_PIXEL_NONE) {
		mtk_ddp_write_mask(cmdq_pkt, OVL_L0_CONST_BLD | pending->pitch,
				   &priv->cmdq_reg, priv->regs,
				   DISP_REG_OVL_PITCH, OVL_L0_CONST_BLD | OVL_L0_SRC_PITCH);
	} else {
		mtk_ddp_write_mask(cmdq_pkt, pending->pitch, &priv->cmdq_reg, priv->regs,
				   DISP_REG_OVL_PITCH, OVL_L0_CONST_BLD | OVL_L0_SRC_PITCH);
	}
}

const u32 *mtk_disp_exdma_get_formats(struct device *dev)
{
	return formats;
}

size_t mtk_disp_exdma_get_num_formats(struct device *dev)
{
	return ARRAY_SIZE(formats);
}

int mtk_disp_exdma_clk_enable(struct device *dev)
{
	struct mtk_disp_exdma *exdma = dev_get_drvdata(dev);

	return clk_prepare_enable(exdma->clk);
}

void mtk_disp_exdma_clk_disable(struct device *dev)
{
	struct mtk_disp_exdma *exdma = dev_get_drvdata(dev);

	clk_disable_unprepare(exdma->clk);
}

static int mtk_disp_exdma_bind(struct device *dev, struct device *master,
			       void *data)
{
	return 0;
}

static void mtk_disp_exdma_unbind(struct device *dev, struct device *master,
				  void *data)
{
}

static const struct component_ops mtk_disp_exdma_component_ops = {
	.bind	= mtk_disp_exdma_bind,
	.unbind = mtk_disp_exdma_unbind,
};

static int mtk_disp_exdma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *larb_pdev = NULL;
	struct device_node *larb_node = NULL;
	struct resource *res;
	struct mtk_disp_exdma *priv;
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->regs)) {
		dev_err(dev, "failed to ioremap exdma\n");
		return PTR_ERR(priv->regs);
	}

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk)) {
		dev_err(dev, "failed to get exdma clk\n");
		return PTR_ERR(priv->clk);
	}

	larb_node = of_parse_phandle(dev->of_node, "mediatek,larb", 0);
	if (larb_node) {
		larb_pdev = of_find_device_by_node(larb_node);
		if (larb_pdev)
			priv->larb = &larb_pdev->dev;
		of_node_put(larb_node);
	}

	if (!priv->larb) {
		dev_dbg(dev, "not find larb dev");
		return -EPROBE_DEFER;
	}
	device_link_add(dev, priv->larb, DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret)
		dev_dbg(dev, "No mediatek,gce-client-reg\n");
#endif
	platform_set_drvdata(pdev, priv);

	pm_runtime_enable(dev);

	ret = component_add(dev, &mtk_disp_exdma_component_ops);
	if (ret != 0) {
		pm_runtime_disable(dev);
		dev_err(dev, "Failed to add component: %d\n", ret);
	}
	return ret;
}

static void mtk_disp_exdma_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_exdma_component_ops);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id mtk_disp_exdma_driver_dt_match[] = {
	{ .compatible = "mediatek,mt8196-exdma", },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_disp_exdma_driver_dt_match);

struct platform_driver mtk_disp_exdma_driver = {
	.probe = mtk_disp_exdma_probe,
	.remove = mtk_disp_exdma_remove,
	.driver = {
		.name = "mediatek-disp-exdma",
		.owner = THIS_MODULE,
		.of_match_table = mtk_disp_exdma_driver_dt_match,
	},
};

MODULE_AUTHOR("Nancy Lin <nancy.lin@mediatek.com>");
MODULE_DESCRIPTION("MediaTek Exdma Driver");
MODULE_LICENSE("GPL");
