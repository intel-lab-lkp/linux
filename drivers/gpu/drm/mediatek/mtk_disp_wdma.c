// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_writeback.h>

#include <linux/align.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/soc/mediatek/mtk-cmdq.h>
#include <linux/wordpart.h>

#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"

#define DISP_REG_WDMA_INT_ENABLE		0x000
 #define WDMA_FRAME_COMPLETE_INT		BIT(0)
#define DISP_REG_WDMA_INT_STATUS		0x004
#define DISP_REG_WDMA_EN			0x008
 #define WDMA_ENGINE_EN				BIT(0)
#define DISP_REG_WDMA_CFG			0x014
 #define WDMA_CFG_OUT_FMT			GENMASK(7, 4)
  #define WDMA_OUT_FMT_RGB565			0
  #define WDMA_OUT_FMT_RGB888			1
  #define WDMA_OUT_FMT_RGBA8888			2
  #define WDMA_OUT_FMT_ARGB8888			3
  #define WDMA_OUT_FMT_UYVY			4
  #define WDMA_OUT_FMT_YUY2			5
  #define WDMA_OUT_FMT_P010			6
  #define WDMA_OUT_FMT_Y_ONLY			7
  #define WDMA_OUT_FMT_I420			8
  #define WDMA_OUT_FMT_ARGB2101010		11
  #define WDMA_OUT_FMT_NV12			12
 #define WDMA_CT_EN				BIT(11)
 #define WDMA_CFG_SWAP				BIT(16)
 #define WDMA_UFO_DCP_ENABLE			BIT(17)
 #define WDMA_INT_MTX_SEL			GENMASK(27, 23)
  #define WDMA_CT_COEF_RGB_TO_JPEG		0
  #define WDMA_CT_COEF_JPEG_TO_RGB		4
#define DISP_REG_WDMA_SRC_SIZE			0x018
#define DISP_REG_WDMA_CLIP_SIZE			0x01c
 #define WDMA_HEIGHT_PX				GENMASK(29, 16)
 #define WDMA_WIDTH_PX				GENMASK(13, 0)
#define DISP_REG_WDMA_CLIP_COORD		0x020
 #define WDMA_CLIP_Y_COORD			GENMASK(29, 16)
 #define WDMA_CLIP_X_COORD			GENMASK(13, 0)
#define DISP_REG_WDMA_SHADOW_CTRL		0x024
 #define WDMA_FORCE_COMMIT			BIT(0)
 #define WDMA_BYPASS_SHADOW			BIT(1)
#define DISP_REG_WDMA_DST_W_IN_BYTE		0x028
#define DISP_REG_WDMA_DST_UV_PITCH		0x078
 #define WDMA_UV_DST_W_IN_BYTE			GENMASK(15, 0)
#define DISP_REG_WDMA_DST_ADDR_LSB		0xf00
#define DISP_REG_WDMA_DST_ADDR_MSB_MT6893	0xf20
#define DISP_REG_WDMA_DST_ADDRX(r, x)		(r + (x * 0x4))

static const u32 mtk_wdma_wb_output_formats[] = {
	DRM_FORMAT_RGB888
};

static const u32 mt6893_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YUYV,
};

struct mtk_disp_wdma_data {
	u32 reg_wdma_dst_addr0_msb;
	const u32 *formats;
	size_t num_formats;
};

struct mtk_disp_wdma {
	struct device			*dev;
	struct clk			*clk;
	void __iomem			*regs;
	struct cmdq_client_reg		cmdq_reg;
	const struct mtk_disp_wdma_data	*data;
	void				(*vblank_cb)(void *data);
	void				*vblank_cb_data;
	int				irq;
	struct drm_writeback_connector	wb_connector;
	bool				wb_pending;
};

static inline struct mtk_disp_wdma *connector_to_wdma(struct drm_connector *connector)
{
	return container_of(connector, struct mtk_disp_wdma, wb_connector.base);
}

static irqreturn_t mtk_disp_wdma_irq_handler(int irq, void *dev_id)
{
	struct mtk_disp_wdma *wdma = dev_id;

	/* Clear frame completion interrupt */
	writel(0x0, wdma->regs + DISP_REG_WDMA_INT_STATUS);

	if (wdma->vblank_cb)
		wdma->vblank_cb(wdma->vblank_cb_data);

	/* TODO: Move completion signaling to CMDQ interrupt callback */
	if (wdma->wb_pending) {
		drm_writeback_signal_completion(&wdma->wb_connector, 0);
		wdma->wb_pending = false;
	}

	return IRQ_HANDLED;
}

static void wdma_update_bits(struct device *dev, unsigned int reg,
			     unsigned int mask, unsigned int val)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);
	unsigned int tmp = readl(wdma->regs + reg);

	tmp = (tmp & ~mask) | (val & mask);
	writel(tmp, wdma->regs + reg);
}

void mtk_wdma_register_vblank_cb(struct device *dev,
				 void (*vblank_cb)(void *),
				 void *vblank_cb_data)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	wdma->vblank_cb = vblank_cb;
	wdma->vblank_cb_data = vblank_cb_data;
}

void mtk_wdma_unregister_vblank_cb(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	wdma->vblank_cb = NULL;
	wdma->vblank_cb_data = NULL;
}

void mtk_wdma_enable_vblank(struct device *dev)
{
	wdma_update_bits(dev, DISP_REG_WDMA_INT_ENABLE, WDMA_FRAME_COMPLETE_INT,
			 WDMA_FRAME_COMPLETE_INT);
}

void mtk_wdma_disable_vblank(struct device *dev)
{
	wdma_update_bits(dev, DISP_REG_WDMA_INT_ENABLE, WDMA_FRAME_COMPLETE_INT, 0);
}

const u32 *mtk_wdma_get_formats(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	return wdma->data->formats;
}

size_t mtk_wdma_get_num_formats(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	return wdma->data->num_formats;
}

int mtk_wdma_clk_enable(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	return clk_prepare_enable(wdma->clk);
}

void mtk_wdma_clk_disable(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	clk_disable_unprepare(wdma->clk);
}

void mtk_wdma_start(struct device *dev)
{
	wdma_update_bits(dev, DISP_REG_WDMA_EN, WDMA_ENGINE_EN,
			 WDMA_ENGINE_EN);
}

void mtk_wdma_stop(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	if (wdma->wb_pending) {
		drm_writeback_signal_completion(&wdma->wb_connector, 0);
		wdma->wb_pending = false;
	}

	wdma_update_bits(dev, DISP_REG_WDMA_EN, WDMA_ENGINE_EN, 0);
}

void mtk_wdma_config(struct device *dev, unsigned int width,
		     unsigned int height, unsigned int vrefresh,
		     unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	writel(WDMA_FORCE_COMMIT | WDMA_BYPASS_SHADOW,
	       wdma->regs + DISP_REG_WDMA_SHADOW_CTRL);
}

static u32 wdma_fmt_convert(unsigned int fmt)
{
	switch (fmt) {
	default:
	case DRM_FORMAT_RGB565:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_RGB565);
	case DRM_FORMAT_BGR565:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_RGB565) | WDMA_CFG_SWAP;
	case DRM_FORMAT_RGB888:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_RGB888);
	case DRM_FORMAT_BGR888:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_RGB888) | WDMA_CFG_SWAP;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_RGBA8888);
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_RGBA8888) | WDMA_CFG_SWAP;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_ARGB8888);
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_ARGB8888) | WDMA_CFG_SWAP;
	case DRM_FORMAT_UYVY:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_UYVY);
	case DRM_FORMAT_YUYV:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_YUY2);
	case DRM_FORMAT_YUV420:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_I420);
	case DRM_FORMAT_YVU420:
		return FIELD_PREP(WDMA_CFG_OUT_FMT, WDMA_OUT_FMT_I420) | WDMA_CFG_SWAP;
	}
}

unsigned int mtk_wdma_layer_nr(struct device *dev)
{
	return 1;
}

static void mtk_wdma_ddp_write_dst_addr(struct cmdq_pkt *cmdq_pkt, u64 val,
					u8 reg_id, struct mtk_disp_wdma *wdma)
{
	mtk_ddp_write(cmdq_pkt, lower_32_bits(val), &wdma->cmdq_reg, wdma->regs,
		      DISP_REG_WDMA_DST_ADDRX(DISP_REG_WDMA_DST_ADDR_LSB, reg_id));

	if (wdma->data->reg_wdma_dst_addr0_msb == 0)
		return;

	mtk_ddp_write(cmdq_pkt, upper_32_bits(val), &wdma->cmdq_reg, wdma->regs,
		      DISP_REG_WDMA_DST_ADDRX(wdma->data->reg_wdma_dst_addr0_msb, reg_id));
}

static void mtk_wdma_format_config(struct mtk_disp_wdma *wdma,
				   struct mtk_plane_pending_state *pending,
				   const struct drm_format_info *fmt_info,
				   struct cmdq_pkt *cmdq_pkt)
{
	unsigned int u_off, u_stride, u_size, v_off;
	u32 val;

	/*
	 * For RGB formats, this sets the image destination address;
	 * For YUV formats, this sets the Y component destination address.
	 */
	mtk_wdma_ddp_write_dst_addr(cmdq_pkt, pending->addr, 0, wdma);

	if (!fmt_info->is_yuv) {
		/* Disable color transform matrix and data compression */
		mtk_ddp_write_mask(cmdq_pkt, 0, &wdma->cmdq_reg, wdma->regs,
				   DISP_REG_WDMA_CFG,
				   WDMA_UFO_DCP_ENABLE | WDMA_CT_EN);
		return;
	}

	/* Additional format config required only for 420 sampling */
	if (!drm_format_info_is_yuv_sampling_420(fmt_info))
		return;

	u_off = pending->pitch * pending->height;
	u_stride = pending->pitch / 2;

	if (drm_format_info_is_yuv_planar(fmt_info)) {
		/* YUV420 or YVU420 */
		u_stride = ALIGN(u_stride, 16);
		u_size = u_stride * pending->height / 2;
		v_off = u_off + u_size;
	} else {
		/* NV12 or NV21 */
		u_size = u_stride * pending->height / 2;
		v_off = 0;
	}

	/* Set U and V components destination addresses */
	mtk_wdma_ddp_write_dst_addr(cmdq_pkt, pending->addr + u_off, 1, wdma);
	mtk_wdma_ddp_write_dst_addr(cmdq_pkt, pending->addr + v_off, 2, wdma);

	mtk_ddp_write(cmdq_pkt, FIELD_PREP(WDMA_UV_DST_W_IN_BYTE, u_stride),
		      &wdma->cmdq_reg, wdma->regs, DISP_REG_WDMA_DST_UV_PITCH);

	/* Color transform coefficient selection */
	val = FIELD_PREP_CONST(WDMA_INT_MTX_SEL, WDMA_CT_COEF_JPEG_TO_RGB);
	mtk_ddp_write_mask(cmdq_pkt, val, &wdma->cmdq_reg, wdma->regs,
			   DISP_REG_WDMA_CFG, WDMA_INT_MTX_SEL);

	/* Enable color transform matrix, disable data compression */
	mtk_ddp_write_mask(cmdq_pkt, WDMA_CT_EN, &wdma->cmdq_reg, wdma->regs,
			   DISP_REG_WDMA_CFG, WDMA_UFO_DCP_ENABLE | WDMA_CT_EN);
}

void mtk_wdma_layer_config(struct device *dev, unsigned int idx,
			   struct mtk_plane_state *state,
			   struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);
	struct mtk_plane_pending_state *pending = &state->pending;
	unsigned int pitch = pending->pitch & 0xffff;
	unsigned int fmt = pending->format;
	unsigned int con = wdma_fmt_convert(fmt);
	const struct drm_format_info *fmt_info = drm_format_info(fmt);
	u16 clip_sz_h = pending->height;
	u16 clip_sz_w = pending->width;
	u32 val;

	val = FIELD_PREP(WDMA_HEIGHT_PX, pending->height);
	val |= FIELD_PREP(WDMA_WIDTH_PX, pending->width);
	mtk_ddp_write(cmdq_pkt, val, &wdma->cmdq_reg, wdma->regs,
		      DISP_REG_WDMA_SRC_SIZE);

	val = FIELD_PREP(WDMA_HEIGHT_PX, pending->y);
	val |= FIELD_PREP(WDMA_WIDTH_PX, pending->x);
	mtk_ddp_write(cmdq_pkt, val, &wdma->cmdq_reg, wdma->regs,
		      DISP_REG_WDMA_CLIP_COORD);

	if (fmt_info->is_yuv) {
		if ((pending->y + pending->height) % 2)
			clip_sz_h--;

		if ((pending->x + pending->width) % 2)
			clip_sz_w--;
	}
	val = FIELD_PREP(WDMA_HEIGHT_PX, clip_sz_h);
	val |= FIELD_PREP(WDMA_WIDTH_PX, clip_sz_w);
	mtk_ddp_write(cmdq_pkt, val, &wdma->cmdq_reg, wdma->regs,
		      DISP_REG_WDMA_CLIP_SIZE);

	mtk_ddp_write(cmdq_pkt, con, &wdma->cmdq_reg, wdma->regs,
		      DISP_REG_WDMA_CFG);
	mtk_ddp_write(cmdq_pkt, pitch, &wdma->cmdq_reg, wdma->regs,
		      DISP_REG_WDMA_DST_W_IN_BYTE);

	mtk_wdma_format_config(wdma, pending, fmt_info, cmdq_pkt);

	drm_writeback_queue_job(&wdma->wb_connector, wdma->wb_connector.base.state);
}

static enum drm_connector_status
mtk_wdma_wb_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static const struct drm_connector_funcs mtk_wdma_wb_connector_funcs = {
	.detect = mtk_wdma_wb_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int mtk_wdma_wb_atomic_check(struct drm_encoder *encoder,
				    struct drm_crtc_state *crtc_state,
				    struct drm_connector_state *conn_state)
{
	const struct drm_display_mode *mode = &crtc_state->mode;
	struct drm_framebuffer *fb;
	int i;

	if (!conn_state->writeback_job || !conn_state->writeback_job->fb)
		return 0;

	fb = conn_state->writeback_job->fb;
	if (fb->width != mode->hdisplay || fb->height != mode->vdisplay)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(mtk_wdma_wb_output_formats); i++) {
		if (fb->format->format == mtk_wdma_wb_output_formats[i])
			return 0;
	}

	return -EINVAL;
}

static const struct drm_encoder_helper_funcs mtk_wdma_wb_encoder_helper_funcs = {
	.atomic_check = mtk_wdma_wb_atomic_check,
};

static int mtk_wdma_wb_connector_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;

	return drm_add_modes_noedid(connector, dev->mode_config.max_width,
				    dev->mode_config.max_height);
}

static enum drm_mode_status
mtk_wdma_wb_connector_mode_valid(struct drm_connector *connector,
				 const struct drm_display_mode *mode)
{
	struct drm_device *dev = connector->dev;
	struct drm_mode_config *mode_config = &dev->mode_config;
	int w = mode->hdisplay, h = mode->vdisplay;

	if (w < mode_config->min_width || w > mode_config->max_width)
		return MODE_BAD_HVALUE;

	if (h < mode_config->min_height || h > mode_config->max_height)
		return MODE_BAD_VVALUE;

	return MODE_OK;
}

static void mtk_wdma_wb_connector_atomic_commit(struct drm_connector *connector,
						struct drm_atomic_commit *state)
{
	struct drm_connector_state *conn_state =
		drm_atomic_get_new_connector_state(state, connector);
	struct mtk_disp_wdma *wdma = connector_to_wdma(connector);
	struct drm_framebuffer *fb;
	struct drm_gem_object *gem;
	struct drm_gem_dma_object *dma_obj;
	dma_addr_t addr;

	if (WARN_ON(!conn_state->writeback_job))
		return;

	fb = conn_state->writeback_job->fb;
	if (!fb)
		return;

	gem = fb->obj[0];
	dma_obj = to_drm_gem_dma_obj(gem);
	addr = dma_obj->dma_addr;

	/* Store writeback pending state before queuing the job */
	wdma->wb_pending = true;

	mtk_wdma_ddp_write_dst_addr(NULL, addr, 0, wdma);
	drm_writeback_queue_job(&wdma->wb_connector, conn_state);
}

static const struct drm_connector_helper_funcs mtk_wdma_wb_connector_helper_funcs = {
	.get_modes = mtk_wdma_wb_connector_get_modes,
	.mode_valid = mtk_wdma_wb_connector_mode_valid,
	.atomic_commit = mtk_wdma_wb_connector_atomic_commit,
};

static int mtk_disp_wdma_bind(struct device *dev, struct device *master,
			      void *data)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	int crtcs, ret;

	crtcs = mtk_find_possible_crtcs(drm_dev, wdma->dev);
	if (crtcs < 0)
		return crtcs;

	drm_connector_helper_add(&wdma->wb_connector.base,
				 &mtk_wdma_wb_connector_helper_funcs);

	ret = drm_writeback_connector_init(drm_dev, &wdma->wb_connector,
					   &mtk_wdma_wb_connector_funcs,
					   &mtk_wdma_wb_encoder_helper_funcs,
					   mtk_wdma_wb_output_formats,
					   ARRAY_SIZE(mtk_wdma_wb_output_formats),
					   crtcs);
	if (ret)
		return ret;

	ret = pm_runtime_get_sync(dev);
	if (ret)
		dev_warn(dev, "Could not get PM Runtime!\n");

	/*
	 * Disable and clear pending interrupts - write unconditionally as no
	 * crash (SError, etc) will happen as if unpowered/unclocked this is
	 * simply going to be ignored (and the registers will be 0 at reset).
	 */
	writel(0x0, wdma->regs + DISP_REG_WDMA_INT_ENABLE);
	writel(0x0, wdma->regs + DISP_REG_WDMA_INT_STATUS);

	/* Put RPM only if refcount was incremented before to avoid unbalancing */
	if (ret >= 0)
		pm_runtime_put(dev);

	enable_irq(wdma->irq);
	return 0;
}

static void mtk_disp_wdma_unbind(struct device *dev, struct device *master,
				 void *data)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	disable_irq(wdma->irq);
}

static const struct component_ops mtk_disp_wdma_component_ops = {
	.bind	= mtk_disp_wdma_bind,
	.unbind = mtk_disp_wdma_unbind,
};

static int mtk_disp_wdma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_wdma *priv;
	struct resource *res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return priv->irq;

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "failed to get wdma clk\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs),
				     "failed to ioremap wdma\n");
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret)
		dev_dbg(dev, "get mediatek,gce-client-reg fail!\n");
#endif

	priv->data = of_device_get_match_data(dev);
	platform_set_drvdata(pdev, priv);

	ret = devm_request_irq(dev, priv->irq, mtk_disp_wdma_irq_handler,
			       IRQF_NO_AUTOEN, dev_name(dev), priv);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to request irq\n");

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = component_add(dev, &mtk_disp_wdma_component_ops);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add component\n");

	return 0;
}

static void mtk_disp_wdma_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_wdma_component_ops);
}

static const struct mtk_disp_wdma_data mt6893_wdma_driver_data = {
	.reg_wdma_dst_addr0_msb = DISP_REG_WDMA_DST_ADDR_MSB_MT6893,
	.formats = mt6893_formats,
	.num_formats = ARRAY_SIZE(mt6893_formats),
};

static const struct mtk_disp_wdma_data mt8173_wdma_driver_data = {
	.formats = mt6893_formats,
	.num_formats = ARRAY_SIZE(mt6893_formats),
};

static const struct of_device_id mtk_disp_wdma_driver_dt_match[] = {
	{ .compatible = "mediatek,mt6893-disp-wdma", .data = &mt6893_wdma_driver_data },
	{ .compatible = "mediatek,mt8173-disp-wdma", .data = &mt8173_wdma_driver_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_disp_wdma_driver_dt_match);

struct platform_driver mtk_disp_wdma_driver = {
	.probe		= mtk_disp_wdma_probe,
	.remove		= mtk_disp_wdma_remove,
	.driver		= {
		.name	= "mediatek-disp-wdma",
		.of_match_table = mtk_disp_wdma_driver_dt_match,
	},
};
