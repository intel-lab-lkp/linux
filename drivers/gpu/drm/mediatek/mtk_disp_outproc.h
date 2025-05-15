/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 MediaTek Inc.
 */

#ifndef __MTK_DISP_OUTPROC_H__
#define __MTK_DISP_OUTPROC_H__

void mtk_disp_outproc_start(struct device *dev);
void mtk_disp_outproc_stop(struct device *dev);
int mtk_disp_outproc_clk_enable(struct device *dev);
void mtk_disp_outproc_clk_disable(struct device *dev);
void mtk_disp_outproc_config(struct device *dev, unsigned int w,
			     unsigned int h, unsigned int vrefresh,
			     unsigned int bpc, struct cmdq_pkt *cmdq_pkt);
void mtk_disp_outproc_register_vblank_cb(struct device *dev,
					 void (*vblank_cb)(void *),
					 void *vblank_cb_data);
void mtk_disp_outproc_unregister_vblank_cb(struct device *dev);
void mtk_disp_outproc_enable_vblank(struct device *dev);
void mtk_disp_outproc_disable_vblank(struct device *dev);
#endif
