/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#ifndef MTK_CRTC_H
#define MTK_CRTC_H

#include <drm/drm_crtc.h>
#include "mtk_ddp_comp.h"
#include "mtk_drm_drv.h"
#include "mtk_plane.h"

#define MTK_MAX_BPC	10
#define MTK_MIN_BPC	3

/**
 * struct mtk_crtc_crc - crc related information
 * @ofs: register offset of crc
 * @rst_ofs: register offset of crc reset
 * @rst_msk: register mask of crc reset
 * @cnt: count of crc
 * @va: pointer to the start of crc array
 * @pa: physical address of the crc for gce to access
 * @cmdq_event: the event to trigger the cmdq
 * @cmdq_reg: address of the register that cmdq is going to access
 * @cmdq_client: handler to control cmdq (mbox channel, thread ...etc.)
 * @cmdq_handle: cmdq packet to store the commands
 */
struct mtk_crtc_crc {
	const u32 *ofs;
	u32 rst_ofs;
	u32 rst_msk;
	size_t cnt;
	u32 *va;
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	dma_addr_t pa;
	u32 cmdq_event;
	struct cmdq_client_reg *cmdq_reg;
	struct cmdq_client cmdq_client;
	struct cmdq_pkt cmdq_handle;
#endif
};

void mtk_crtc_commit(struct drm_crtc *crtc);
int mtk_crtc_create(struct drm_device *drm_dev, const unsigned int *path,
		    unsigned int path_len, int priv_data_index,
		    const struct mtk_drm_route *conn_routes,
		    unsigned int num_conn_routes);
int mtk_crtc_plane_check(struct drm_crtc *crtc, struct drm_plane *plane,
			 struct mtk_plane_state *state);
void mtk_crtc_async_update(struct drm_crtc *crtc, struct drm_plane *plane,
			   struct drm_atomic_state *plane_state);
struct device *mtk_crtc_dma_dev_get(struct drm_crtc *crtc);

void mtk_crtc_init_crc(struct mtk_crtc_crc *crc, const u32 *crc_offset_table,
		       size_t crc_count, u32 reset_offset, u32 reset_mask);
void mtk_crtc_read_crc(struct mtk_crtc_crc *crc, void __iomem *reg);
void mtk_crtc_destroy_crc(struct mtk_crtc_crc *crc);
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
void mtk_crtc_create_crc_cmdq(struct device *dev, struct mtk_crtc_crc *crc);
void mtk_crtc_start_crc_cmdq(struct mtk_crtc_crc *crc);
void mtk_crtc_stop_crc_cmdq(struct mtk_crtc_crc *crc);
#endif

#endif /* MTK_CRTC_H */
