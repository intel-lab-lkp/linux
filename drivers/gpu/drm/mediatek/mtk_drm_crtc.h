/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#ifndef MTK_DRM_CRTC_H
#define MTK_DRM_CRTC_H

#include <drm/drm_crtc.h>
#include "mtk_drm_ddp_comp.h"
#include "mtk_drm_plane.h"

#define MTK_LUT_SIZE	512
#define MTK_MAX_BPC	10
#define MTK_MIN_BPC	3

/*
 * struct mtk_drm_crtc - MediaTek specific crtc structure.
 * @base: crtc object.
 * @enabled: records whether crtc_enable succeeded
 * @planes: array of 4 drm_plane structures, one for each overlay plane
 * @pending_planes: whether any plane has pending changes to be applied
 * @mmsys_dev: pointer to the mmsys device for configuration registers
 * @mutex: handle to one of the ten disp_mutex streams
 * @ddp_comp_nr: number of components in ddp_comp
 * @ddp_comp: array of pointers the mtk_ddp_comp structures used by this crtc
 *
 * TODO: Needs update: this header is missing a bunch of member descriptions.
 */
struct mtk_drm_crtc {
	struct drm_crtc			base;
	bool				enabled;

	bool				pending_needs_vblank;
	struct drm_pending_vblank_event	*event;

	struct drm_plane		*planes;
	unsigned int			layer_nr;
	bool				pending_planes;
	bool				pending_async_planes;

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	struct cmdq_client		cmdq_client;
	struct cmdq_pkt			cmdq_handle;
	u32				cmdq_event;
	u32				cmdq_vblank_cnt;
	wait_queue_head_t		cb_blocking_queue;
#endif

	struct device			*mmsys_dev;
	struct device			*dma_dev;
	struct mtk_mutex		*mutex;
	unsigned int			ddp_comp_nr;
	struct mtk_ddp_comp		**ddp_comp;

	/* lock for display hardware access */
	struct mutex			hw_lock;
	bool				config_updating;
};

void mtk_drm_crtc_commit(struct drm_crtc *crtc);
int mtk_drm_crtc_create(struct drm_device *drm_dev,
			const unsigned int *path,
			unsigned int path_len,
			int priv_data_index);
int mtk_drm_crtc_plane_check(struct drm_crtc *crtc, struct drm_plane *plane,
			     struct mtk_plane_state *state);
void mtk_drm_crtc_async_update(struct drm_crtc *crtc, struct drm_plane *plane,
			       struct drm_atomic_state *plane_state);
struct device *mtk_drm_crtc_dma_dev_get(struct drm_crtc *crtc);

#endif /* MTK_DRM_CRTC_H */
