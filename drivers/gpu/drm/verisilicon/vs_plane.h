/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) VeriSilicon Holdings Co., Ltd.
 */

#ifndef __VS_PLANE_H__
#define __VS_PLANE_H__

#include <drm/drm_plane.h>

struct vs_plane_info;

struct vs_plane_state {
	struct drm_plane_state base;
	dma_addr_t dma_addr[DRM_FORMAT_MAX_PLANES];
};

struct vs_plane {
	struct drm_plane base;
	u8 id;
};

struct vs_plane *vs_plane_create(struct drm_device *drm_dev,
				 struct vs_plane_info *info,
				 unsigned int layer_num,
				 unsigned int possible_crtcs);
#endif /* __VS_PLANE_H__ */
