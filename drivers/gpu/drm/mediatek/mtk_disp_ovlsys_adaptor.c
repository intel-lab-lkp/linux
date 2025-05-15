// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 */

#include <drm/drm_fourcc.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mtk-cmdq.h>
#include <linux/soc/mediatek/mtk-mmsys.h>
#include <linux/soc/mediatek/mtk-mutex.h>

#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_disp_blender.h"
#include "mtk_disp_drv.h"
#include "mtk_disp_outproc.h"
#include "mtk_drm_drv.h"

#define OVLSYS_ADAPTOR_DRIVER_DATA_MAX_SIZE 8191

enum mtk_ovlsys_adaptor_comp_type {
	OVLSYS_ADAPTOR_TYPE_EXDMA = 0,
	OVLSYS_ADAPTOR_TYPE_BLENDER,
	OVLSYS_ADAPTOR_TYPE_OUTPROC,
	OVLSYS_ADAPTOR_TYPE_NUM,
};

enum mtk_ovlsys_adaptor_comp_id {
	OVLSYS_ADAPTOR_EXDMA0,
	OVLSYS_ADAPTOR_EXDMA1,
	OVLSYS_ADAPTOR_EXDMA2,
	OVLSYS_ADAPTOR_EXDMA3,
	OVLSYS_ADAPTOR_EXDMA4,
	OVLSYS_ADAPTOR_EXDMA5,
	OVLSYS_ADAPTOR_EXDMA6,
	OVLSYS_ADAPTOR_EXDMA7,
	OVLSYS_ADAPTOR_EXDMA8,
	OVLSYS_ADAPTOR_EXDMA9,
	OVLSYS_ADAPTOR_EXDMA10,
	OVLSYS_ADAPTOR_EXDMA11,
	OVLSYS_ADAPTOR_EXDMA12,
	OVLSYS_ADAPTOR_EXDMA13,
	OVLSYS_ADAPTOR_EXDMA14,
	OVLSYS_ADAPTOR_EXDMA15,
	OVLSYS_ADAPTOR_EXDMA16,
	OVLSYS_ADAPTOR_EXDMA17,
	OVLSYS_ADAPTOR_EXDMA18,
	OVLSYS_ADAPTOR_EXDMA19,
	OVLSYS_ADAPTOR_BLENDER0,
	OVLSYS_ADAPTOR_BLENDER1,
	OVLSYS_ADAPTOR_BLENDER2,
	OVLSYS_ADAPTOR_BLENDER3,
	OVLSYS_ADAPTOR_BLENDER4,
	OVLSYS_ADAPTOR_BLENDER5,
	OVLSYS_ADAPTOR_BLENDER6,
	OVLSYS_ADAPTOR_BLENDER7,
	OVLSYS_ADAPTOR_BLENDER8,
	OVLSYS_ADAPTOR_BLENDER9,
	OVLSYS_ADAPTOR_BLENDER10,
	OVLSYS_ADAPTOR_BLENDER11,
	OVLSYS_ADAPTOR_BLENDER12,
	OVLSYS_ADAPTOR_BLENDER13,
	OVLSYS_ADAPTOR_BLENDER14,
	OVLSYS_ADAPTOR_BLENDER15,
	OVLSYS_ADAPTOR_BLENDER16,
	OVLSYS_ADAPTOR_BLENDER17,
	OVLSYS_ADAPTOR_BLENDER18,
	OVLSYS_ADAPTOR_BLENDER19,
	OVLSYS_ADAPTOR_OUTPROC0,
	OVLSYS_ADAPTOR_OUTPROC1,
	OVLSYS_ADAPTOR_OUTPROC2,
	OVLSYS_ADAPTOR_OUTPROC3,
	OVLSYS_ADAPTOR_OUTPROC4,
	OVLSYS_ADAPTOR_OUTPROC5,
	OVLSYS_ADAPTOR_OUTPROC6,
	OVLSYS_ADAPTOR_OUTPROC7,
	OVLSYS_ADAPTOR_OUTPROC8,
	OVLSYS_ADAPTOR_OUTPROC9,
	OVLSYS_ADAPTOR_OUTPROC10,
	OVLSYS_ADAPTOR_OUTPROC11,
	OVLSYS_ADAPTOR_ID_MAX
};

struct ovlsys_adaptor_comp_match {
	enum mtk_ovlsys_adaptor_comp_type type;
	int alias_id;
	enum mtk_ddp_comp_id comp_id;
};

struct mtk_disp_ovlsys_adaptor {
	struct device *ovl_adaptor_comp[OVLSYS_ADAPTOR_ID_MAX];
	struct device *mmsys_dev;
	const unsigned int *path;
	unsigned int path_size;
	unsigned int layer_nr;
	bool children_bound;
};

static const char * const private_comp_stem[OVLSYS_ADAPTOR_TYPE_NUM] = {
	[OVLSYS_ADAPTOR_TYPE_EXDMA]	= "exdma",
	[OVLSYS_ADAPTOR_TYPE_BLENDER]	= "blender",
	[OVLSYS_ADAPTOR_TYPE_OUTPROC]	= "outproc",
};

static const struct ovlsys_adaptor_comp_match comp_matches[OVLSYS_ADAPTOR_ID_MAX] = {
	[OVLSYS_ADAPTOR_EXDMA2]	  = { OVLSYS_ADAPTOR_TYPE_EXDMA, 2, DDP_COMPONENT_OVL0_EXDMA2},
	[OVLSYS_ADAPTOR_EXDMA3]	  = { OVLSYS_ADAPTOR_TYPE_EXDMA, 3, DDP_COMPONENT_OVL0_EXDMA3},
	[OVLSYS_ADAPTOR_EXDMA4]	  = { OVLSYS_ADAPTOR_TYPE_EXDMA, 4, DDP_COMPONENT_OVL0_EXDMA4},
	[OVLSYS_ADAPTOR_EXDMA5]	  = { OVLSYS_ADAPTOR_TYPE_EXDMA, 5, DDP_COMPONENT_OVL0_EXDMA5},
	[OVLSYS_ADAPTOR_EXDMA6]	  = { OVLSYS_ADAPTOR_TYPE_EXDMA, 6, DDP_COMPONENT_OVL0_EXDMA6},
	[OVLSYS_ADAPTOR_EXDMA7]	  = { OVLSYS_ADAPTOR_TYPE_EXDMA, 7, DDP_COMPONENT_OVL0_EXDMA7},
	[OVLSYS_ADAPTOR_EXDMA8]	  = { OVLSYS_ADAPTOR_TYPE_EXDMA, 8, DDP_COMPONENT_OVL0_EXDMA8},
	[OVLSYS_ADAPTOR_EXDMA9]	  = { OVLSYS_ADAPTOR_TYPE_EXDMA, 9, DDP_COMPONENT_OVL0_EXDMA9},
	[OVLSYS_ADAPTOR_EXDMA12]   = { OVLSYS_ADAPTOR_TYPE_EXDMA, 12, DDP_COMPONENT_OVL1_EXDMA2},
	[OVLSYS_ADAPTOR_EXDMA13]   = { OVLSYS_ADAPTOR_TYPE_EXDMA, 13, DDP_COMPONENT_OVL1_EXDMA3},
	[OVLSYS_ADAPTOR_EXDMA14]   = { OVLSYS_ADAPTOR_TYPE_EXDMA, 14, DDP_COMPONENT_OVL1_EXDMA4},
	[OVLSYS_ADAPTOR_EXDMA15]   = { OVLSYS_ADAPTOR_TYPE_EXDMA, 15, DDP_COMPONENT_OVL1_EXDMA5},
	[OVLSYS_ADAPTOR_EXDMA16]   = { OVLSYS_ADAPTOR_TYPE_EXDMA, 16, DDP_COMPONENT_OVL1_EXDMA6},
	[OVLSYS_ADAPTOR_EXDMA17]   = { OVLSYS_ADAPTOR_TYPE_EXDMA, 17, DDP_COMPONENT_OVL1_EXDMA7},
	[OVLSYS_ADAPTOR_EXDMA18]   = { OVLSYS_ADAPTOR_TYPE_EXDMA, 18, DDP_COMPONENT_OVL1_EXDMA8},
	[OVLSYS_ADAPTOR_EXDMA19]   = { OVLSYS_ADAPTOR_TYPE_EXDMA, 19, DDP_COMPONENT_OVL1_EXDMA9},
	[OVLSYS_ADAPTOR_BLENDER1] = { OVLSYS_ADAPTOR_TYPE_BLENDER, 1, DDP_COMPONENT_OVL0_BLENDER1},
	[OVLSYS_ADAPTOR_BLENDER2] = { OVLSYS_ADAPTOR_TYPE_BLENDER, 2, DDP_COMPONENT_OVL0_BLENDER2},
	[OVLSYS_ADAPTOR_BLENDER3] = { OVLSYS_ADAPTOR_TYPE_BLENDER, 3, DDP_COMPONENT_OVL0_BLENDER3},
	[OVLSYS_ADAPTOR_BLENDER4] = { OVLSYS_ADAPTOR_TYPE_BLENDER, 4, DDP_COMPONENT_OVL0_BLENDER4},
	[OVLSYS_ADAPTOR_BLENDER5] = { OVLSYS_ADAPTOR_TYPE_BLENDER, 5, DDP_COMPONENT_OVL0_BLENDER5},
	[OVLSYS_ADAPTOR_BLENDER6] = { OVLSYS_ADAPTOR_TYPE_BLENDER, 6, DDP_COMPONENT_OVL0_BLENDER6},
	[OVLSYS_ADAPTOR_BLENDER7] = { OVLSYS_ADAPTOR_TYPE_BLENDER, 7, DDP_COMPONENT_OVL0_BLENDER7},
	[OVLSYS_ADAPTOR_BLENDER8] = { OVLSYS_ADAPTOR_TYPE_BLENDER, 8, DDP_COMPONENT_OVL0_BLENDER8},
	[OVLSYS_ADAPTOR_BLENDER9] = { OVLSYS_ADAPTOR_TYPE_BLENDER, 9, DDP_COMPONENT_OVL0_BLENDER9},
	[OVLSYS_ADAPTOR_BLENDER11] = {
		OVLSYS_ADAPTOR_TYPE_BLENDER, 11, DDP_COMPONENT_OVL1_BLENDER1},
	[OVLSYS_ADAPTOR_BLENDER12] = {
		OVLSYS_ADAPTOR_TYPE_BLENDER, 12, DDP_COMPONENT_OVL1_BLENDER2},
	[OVLSYS_ADAPTOR_BLENDER13] = {
		OVLSYS_ADAPTOR_TYPE_BLENDER, 13, DDP_COMPONENT_OVL1_BLENDER3},
	[OVLSYS_ADAPTOR_BLENDER14] = {
		OVLSYS_ADAPTOR_TYPE_BLENDER, 14, DDP_COMPONENT_OVL1_BLENDER4},
	[OVLSYS_ADAPTOR_BLENDER15] = {
		OVLSYS_ADAPTOR_TYPE_BLENDER, 15, DDP_COMPONENT_OVL1_BLENDER5},
	[OVLSYS_ADAPTOR_BLENDER16] = {
		OVLSYS_ADAPTOR_TYPE_BLENDER, 16, DDP_COMPONENT_OVL1_BLENDER6},
	[OVLSYS_ADAPTOR_BLENDER17] = {
		OVLSYS_ADAPTOR_TYPE_BLENDER, 17, DDP_COMPONENT_OVL1_BLENDER7},
	[OVLSYS_ADAPTOR_BLENDER18] = {
		OVLSYS_ADAPTOR_TYPE_BLENDER, 18, DDP_COMPONENT_OVL1_BLENDER8},
	[OVLSYS_ADAPTOR_BLENDER19] = {
		OVLSYS_ADAPTOR_TYPE_BLENDER, 19, DDP_COMPONENT_OVL1_BLENDER9},
	[OVLSYS_ADAPTOR_OUTPROC0] = { OVLSYS_ADAPTOR_TYPE_OUTPROC, 0, DDP_COMPONENT_OVL0_OUTPROC0},
	[OVLSYS_ADAPTOR_OUTPROC1] = { OVLSYS_ADAPTOR_TYPE_OUTPROC, 1, DDP_COMPONENT_OVL0_OUTPROC1},
	[OVLSYS_ADAPTOR_OUTPROC6] = { OVLSYS_ADAPTOR_TYPE_OUTPROC, 6, DDP_COMPONENT_OVL1_OUTPROC0},
};

static const unsigned int mt8196_mtk_ovl_main[] = {
	OVLSYS_ADAPTOR_EXDMA2,
	OVLSYS_ADAPTOR_BLENDER1,
	OVLSYS_ADAPTOR_EXDMA3,
	OVLSYS_ADAPTOR_BLENDER2,
	OVLSYS_ADAPTOR_EXDMA4,
	OVLSYS_ADAPTOR_BLENDER3,
	OVLSYS_ADAPTOR_EXDMA5,
	OVLSYS_ADAPTOR_BLENDER4,
	OVLSYS_ADAPTOR_OUTPROC0,
};

static const unsigned int mt8196_mtk_ovl_ext[] = {
	OVLSYS_ADAPTOR_EXDMA6,
	OVLSYS_ADAPTOR_BLENDER5,
	OVLSYS_ADAPTOR_EXDMA7,
	OVLSYS_ADAPTOR_BLENDER6,
	OVLSYS_ADAPTOR_EXDMA8,
	OVLSYS_ADAPTOR_BLENDER7,
	OVLSYS_ADAPTOR_EXDMA9,
	OVLSYS_ADAPTOR_BLENDER8,
	OVLSYS_ADAPTOR_OUTPROC1,
};

static const unsigned int mt8196_mtk_ovl_third[] = {
	OVLSYS_ADAPTOR_EXDMA12,
	OVLSYS_ADAPTOR_BLENDER11,
	OVLSYS_ADAPTOR_EXDMA13,
	OVLSYS_ADAPTOR_BLENDER12,
	OVLSYS_ADAPTOR_EXDMA14,
	OVLSYS_ADAPTOR_BLENDER13,
	OVLSYS_ADAPTOR_EXDMA15,
	OVLSYS_ADAPTOR_BLENDER14,
	OVLSYS_ADAPTOR_OUTPROC6,
};

static enum mtk_ovlsys_adaptor_comp_type get_type(enum mtk_ovlsys_adaptor_comp_id id)
{
	return comp_matches[id].type;
}

static enum mtk_ddp_comp_id get_ddp_comp_id(enum mtk_ovlsys_adaptor_comp_id id)
{
	return comp_matches[id].comp_id;
}

int mtk_ovlsys_adaptor_power_on(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	struct device *comp;
	int ret = 0;
	int i;

	for (i = 0; i < priv->path_size; i++) {
		comp = priv->ovl_adaptor_comp[priv->path[i]];
		if (!comp || get_type(priv->path[i]) != OVLSYS_ADAPTOR_TYPE_EXDMA)
			continue;

		ret = pm_runtime_get_sync(comp);
		if (ret < 0) {
			dev_err(dev, "Failed to enable power domain %d, err %d\n", i, ret);
			goto pwr_err;
		}
	}

	return ret;

pwr_err:
	while (--i >= 0) {
		comp = priv->ovl_adaptor_comp[i];
		if (!comp || get_type(priv->path[i]) != OVLSYS_ADAPTOR_TYPE_EXDMA)
			continue;

		pm_runtime_put(priv->ovl_adaptor_comp[i]);
	}

	return ret;
}

void mtk_ovlsys_adaptor_power_off(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	struct device *comp;
	int i;

	for (i = 0; i < priv->path_size; i++) {
		comp = priv->ovl_adaptor_comp[priv->path[i]];
		if (!comp || get_type(priv->path[i]) != OVLSYS_ADAPTOR_TYPE_EXDMA)
			continue;

		pm_runtime_put(comp);
	}
}

void mtk_ovlsys_adaptor_layer_config(struct device *dev, unsigned int idx,
				     struct mtk_plane_state *state,
				     struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_plane_pending_state *pending = &state->pending;
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	struct device *exdma;
	struct device *blender;
	const struct drm_format_info *fmt_info = drm_format_info(pending->format);

	DRM_DEV_DEBUG_DRIVER(dev, "idx:%d enable:%d fmt:0x%x addr:0x%pad fb_w:%d {%d,%d,%d,%d}\n",
			     idx, pending->enable, pending->format,
			     &pending->addr, (pending->pitch / fmt_info->cpp[0]),
			     pending->x, pending->y, pending->width, pending->height);

	exdma = priv->ovl_adaptor_comp[priv->path[idx * 2]];
	if (!exdma) {
		dev_err(dev, "%s: exdma%d comp not found\n", __func__, idx);
		return;
	}

	blender = priv->ovl_adaptor_comp[priv->path[idx * 2 + 1]];
	if (!blender) {
		dev_err(dev, "%s: blender%d comp not found\n", __func__, idx);
		return;
	}

	if (pending->height == 0 || pending->width == 0 ||
	    pending->x > OVLSYS_ADAPTOR_DRIVER_DATA_MAX_SIZE ||
	    pending->y > OVLSYS_ADAPTOR_DRIVER_DATA_MAX_SIZE)
		pending->enable = false;

	mtk_disp_exdma_layer_config(exdma, state, cmdq_pkt);
	mtk_disp_blender_layer_config(blender, state, cmdq_pkt);
}

void mtk_ovlsys_adaptor_config(struct device *dev, unsigned int w,
			       unsigned int h, unsigned int vrefresh,
			       unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	int i;
	int blender_idx = 0;

	for (i = 0; i < priv->path_size; i++) {
		if (get_type(priv->path[i]) == OVLSYS_ADAPTOR_TYPE_BLENDER) {
			blender_idx++;

			mtk_disp_blender_config(priv->ovl_adaptor_comp[priv->path[i]], w, h,
						vrefresh, bpc, blender_idx == priv->layer_nr,
						blender_idx == 1, cmdq_pkt);
		} else if (get_type(priv->path[i]) == OVLSYS_ADAPTOR_TYPE_OUTPROC) {
			mtk_disp_outproc_config(priv->ovl_adaptor_comp[priv->path[i]], w, h,
						vrefresh, bpc, cmdq_pkt);
		}
	}
}

int mtk_ovlsys_adaptor_layer_check(struct device *dev,
				   unsigned int idx,
				   struct mtk_plane_state *mtk_state)
{
	struct drm_plane_state *state = &mtk_state->base;

	/* Check if any unsupported rotation is set */
	if (state->rotation & ~DRM_MODE_ROTATE_0)
		return -EINVAL;

	return 0;
}

void mtk_ovlsys_adaptor_start(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < (priv->path_size - 1); i += 2) {
		mtk_disp_exdma_start(priv->ovl_adaptor_comp[priv->path[i]], NULL);
		mtk_disp_blender_start(priv->ovl_adaptor_comp[priv->path[i + 1]], NULL);
	}

	mtk_disp_outproc_start(priv->ovl_adaptor_comp[priv->path[i]]);
}

void mtk_ovlsys_adaptor_stop(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < (priv->path_size - 1); i += 2) {
		mtk_disp_exdma_start(priv->ovl_adaptor_comp[priv->path[i]], NULL);
		mtk_disp_blender_start(priv->ovl_adaptor_comp[priv->path[i + 1]], NULL);
	}

	mtk_disp_outproc_start(priv->ovl_adaptor_comp[priv->path[i]]);
}

int mtk_ovlsys_adaptor_clk_enable(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	struct device *comp;
	int ret = 0;
	int i;

	for (i = 0; i < priv->path_size; i++) {
		comp = priv->ovl_adaptor_comp[priv->path[i]];
		if (!comp)
			continue;

		if (get_type(priv->path[i]) == OVLSYS_ADAPTOR_TYPE_EXDMA)
			ret = mtk_disp_exdma_clk_enable(comp);
		else if (get_type(priv->path[i]) == OVLSYS_ADAPTOR_TYPE_BLENDER)
			ret = mtk_disp_blender_clk_enable(comp);
		else
			ret = mtk_disp_outproc_clk_enable(comp);

		if (ret) {
			dev_err(dev, "Failed to enable clock %d, err %d\n", i, ret);
			goto clk_err;
		}
	}

	return ret;

clk_err:
	while (--i >= 0) {
		comp = priv->ovl_adaptor_comp[priv->path[i]];
		if (!comp)
			continue;

		if (get_type(priv->path[i]) == OVLSYS_ADAPTOR_TYPE_EXDMA)
			mtk_disp_exdma_clk_disable(comp);
		else if (get_type(priv->path[i]) == OVLSYS_ADAPTOR_TYPE_BLENDER)
			mtk_disp_blender_clk_disable(comp);
		else
			mtk_disp_outproc_clk_disable(comp);
	}

	return ret;
}

void mtk_ovlsys_adaptor_clk_disable(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	struct device *comp;
	int i;

	for (i = 0; i < priv->path_size; i++) {
		comp = priv->ovl_adaptor_comp[priv->path[i]];
		if (!comp)
			continue;

		if (get_type(priv->path[i]) == OVLSYS_ADAPTOR_TYPE_EXDMA)
			mtk_disp_exdma_clk_disable(comp);
		else if (get_type(priv->path[i]) == OVLSYS_ADAPTOR_TYPE_BLENDER)
			mtk_disp_blender_clk_disable(comp);
		else
			mtk_disp_outproc_clk_disable(comp);
	}
}

unsigned int mtk_ovlsys_adaptor_layer_nr(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	return priv->layer_nr;
}

struct device *mtk_ovlsys_adaptor_dma_dev_get(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	return priv->ovl_adaptor_comp[priv->path[0]];
}

void mtk_ovlsys_adaptor_register_vblank_cb(struct device *dev, void (*vblank_cb)(void *),
					   void *vblank_cb_data)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	mtk_disp_outproc_register_vblank_cb(priv->ovl_adaptor_comp[priv->path[priv->path_size - 1]],
					    vblank_cb, vblank_cb_data);
}

void mtk_ovlsys_adaptor_unregister_vblank_cb(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	struct device *comp = priv->ovl_adaptor_comp[priv->path[priv->path_size - 1]];

	mtk_disp_outproc_unregister_vblank_cb(comp);
}

void mtk_ovlsys_adaptor_enable_vblank(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	mtk_disp_outproc_enable_vblank(priv->ovl_adaptor_comp[priv->path[priv->path_size - 1]]);
}

void mtk_ovlsys_adaptor_disable_vblank(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	mtk_disp_outproc_disable_vblank(priv->ovl_adaptor_comp[priv->path[priv->path_size - 1]]);
}

u32 mtk_ovlsys_adaptor_get_blend_modes(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	return mtk_disp_blender_get_blend_modes(priv->ovl_adaptor_comp[priv->path[1]]);
}

const u32 *mtk_ovlsys_adaptor_get_formats(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	return mtk_disp_exdma_get_formats(priv->ovl_adaptor_comp[priv->path[0]]);
}

size_t mtk_ovlsys_adaptor_get_num_formats(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	return mtk_disp_exdma_get_num_formats(priv->ovl_adaptor_comp[priv->path[0]]);
}

void mtk_ovlsys_adaptor_add_comp(struct device *dev, struct mtk_mutex *mutex)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < priv->path_size; i++)
		mtk_mutex_add_comp(mutex, get_ddp_comp_id(priv->path[i]));
}

void mtk_ovlsys_adaptor_remove_comp(struct device *dev, struct mtk_mutex *mutex)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < priv->path_size; i++)
		mtk_mutex_remove_comp(mutex, get_ddp_comp_id(priv->path[i]));
}

void mtk_ovlsys_adaptor_connect(struct device *dev, struct device *mmsys_dev, unsigned int next)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < priv->path_size - 1; i++)
		mtk_mmsys_ddp_connect(mmsys_dev, get_ddp_comp_id(priv->path[i]),
				      get_ddp_comp_id(priv->path[i + 1]));

	mtk_mmsys_ddp_connect(mmsys_dev, get_ddp_comp_id(priv->path[i]), next);
}

void mtk_ovlsys_adaptor_disconnect(struct device *dev, struct device *mmsys_dev, unsigned int next)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < priv->path_size - 1; i++)
		mtk_mmsys_ddp_disconnect(mmsys_dev, get_ddp_comp_id(priv->path[i]),
					 get_ddp_comp_id(priv->path[i + 1]));

	mtk_mmsys_ddp_disconnect(mmsys_dev, get_ddp_comp_id(priv->path[i]), next);
}

static int ovlsys_adaptor_comp_get_id(struct device *dev, struct device_node *node,
				      enum mtk_ovlsys_adaptor_comp_type type)
{
	int alias_id = of_alias_get_id(node, private_comp_stem[type]);
	int i;

	for (i = 0; i < ARRAY_SIZE(comp_matches); i++)
		if (comp_matches[i].type == type &&
		    comp_matches[i].alias_id == alias_id)
			return i;

	dev_warn(dev, "Failed to get id. type: %d, alias: %d\n", type, alias_id);
	return -EINVAL;
}

static const struct of_device_id mtk_ovlsys_adaptor_comp_dt_ids[] = {
	{
		.compatible = "mediatek,mt8196-exdma",
		.data = (void *)OVLSYS_ADAPTOR_TYPE_EXDMA,
	}, {
		.compatible = "mediatek,mt8196-blender",
		.data = (void *)OVLSYS_ADAPTOR_TYPE_BLENDER,
	}, {
		.compatible = "mediatek,mt8196-outproc",
		.data = (void *)OVLSYS_ADAPTOR_TYPE_OUTPROC,
	},
	{},
};

static int ovlsys_adaptor_comp_init(struct device *dev, struct component_match **match)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	struct device_node *parent;
	struct platform_device *comp_pdev;
	int i;

	parent = dev->parent->parent->of_node->parent;

	for_each_child_of_node_scoped(parent, node) {
		enum mtk_ovlsys_adaptor_comp_type type;
		const struct of_device_id *of_id;
		int id;
		bool found = false;

		of_id = of_match_node(mtk_ovlsys_adaptor_comp_dt_ids, node);
		if (!of_id)
			continue;

		if (!of_device_is_available(node)) {
			dev_dbg(dev, "Skipping disabled component %pOF\n",
				node);
			continue;
		}

		type = (enum mtk_ovlsys_adaptor_comp_type)(uintptr_t)of_id->data;

		id = ovlsys_adaptor_comp_get_id(dev, node, type);
		if (id < 0) {
			dev_warn(dev, "Skipping unknown component %pOF\n",
				 node);
			continue;
		}

		for (i = 0; i < priv->path_size; i++)
			if (priv->path[i] == id)
				found = true;

		if (!found)
			continue;

		comp_pdev = of_find_device_by_node(node);
		if (!comp_pdev)
			return -EPROBE_DEFER;

		priv->ovl_adaptor_comp[id] = &comp_pdev->dev;

		drm_of_component_match_add(dev, match, component_compare_of, node);
		dev_dbg(dev, "Adding component match for %pOF\n", node);
	}

	if (!*match) {
		dev_err(dev, "No match device for ovlsys_adaptor\n");
		return -ENODEV;
	}

	return 0;
}

static int mtk_disp_ovlsys_adaptor_comp_bind(struct device *dev, struct device *master,
					     void *data)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	if (!priv->children_bound)
		return -EPROBE_DEFER;

	return 0;
}

static void mtk_disp_ovlsys_adaptor_comp_unbind(struct device *dev, struct device *master,
						void *data)
{
}

static const struct component_ops mtk_disp_ovlsys_adaptor_comp_ops = {
	.bind	= mtk_disp_ovlsys_adaptor_comp_bind,
	.unbind = mtk_disp_ovlsys_adaptor_comp_unbind,
};

static int mtk_disp_ovlsys_adaptor_master_bind(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);
	int ret, i;

	ret = component_bind_all(dev, priv->mmsys_dev);

	if (ret)
		return dev_err_probe(dev, ret, "component_bind_all failed!\n");

	priv->children_bound = true;
	priv->layer_nr = 0;

	for (i = 0; i < priv->path_size; i++)
		if (comp_matches[priv->path[i]].type == OVLSYS_ADAPTOR_TYPE_BLENDER)
			priv->layer_nr++;

	return 0;
}

static void mtk_disp_ovlsys_adaptor_master_unbind(struct device *dev)
{
	struct mtk_disp_ovlsys_adaptor *priv = dev_get_drvdata(dev);

	priv->children_bound = false;
}

static const struct component_master_ops mtk_disp_ovlsys_adaptor_master_ops = {
	.bind		= mtk_disp_ovlsys_adaptor_master_bind,
	.unbind		= mtk_disp_ovlsys_adaptor_master_unbind,
};

static int mtk_disp_ovlsys_adaptor_probe(struct platform_device *pdev)
{
	struct mtk_disp_ovlsys_adaptor *priv;
	struct device *dev = &pdev->dev;
	struct component_match *match = NULL;
	struct mtk_ovlsys_platform_data *ovlsys_priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	ovlsys_priv = pdev->dev.platform_data;
	priv->mmsys_dev = ovlsys_priv->mmsys_dev;

	if (ovlsys_priv->use_path == CRTC_MAIN) {
		priv->path = mt8196_mtk_ovl_main;
		priv->path_size = ARRAY_SIZE(mt8196_mtk_ovl_main);
	} else if (ovlsys_priv->use_path == CRTC_EXT) {
		priv->path = mt8196_mtk_ovl_ext;
		priv->path_size = ARRAY_SIZE(mt8196_mtk_ovl_ext);
	} else if (ovlsys_priv->use_path == CRTC_THIRD) {
		priv->path = mt8196_mtk_ovl_third;
		priv->path_size = ARRAY_SIZE(mt8196_mtk_ovl_third);
	}

	ret = ovlsys_adaptor_comp_init(dev, &match);
	if (ret < 0)
		return ret;

	component_master_add_with_match(dev, &mtk_disp_ovlsys_adaptor_master_ops, match);

	pm_runtime_enable(dev);

	ret = component_add(dev, &mtk_disp_ovlsys_adaptor_comp_ops);
	if (ret != 0) {
		pm_runtime_disable(dev);
		dev_err(dev, "Failed to add component: %d\n", ret);
	}

	return ret;
}

static void mtk_disp_ovlsys_adaptor_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &mtk_disp_ovlsys_adaptor_master_ops);
	pm_runtime_disable(&pdev->dev);
}

struct platform_driver mtk_disp_ovlsys_adaptor_driver = {
	.probe		= mtk_disp_ovlsys_adaptor_probe,
	.remove		= mtk_disp_ovlsys_adaptor_remove,
	.driver		= {
		.name	= "mediatek-disp-ovlsys-adaptor",
		.owner	= THIS_MODULE,
	},
};

MODULE_AUTHOR("Nancy Lin <nancy.lin@mediatek.com>");
MODULE_DESCRIPTION("MediaTek Ovlsys Adaptor Driver");
MODULE_LICENSE("GPL");
