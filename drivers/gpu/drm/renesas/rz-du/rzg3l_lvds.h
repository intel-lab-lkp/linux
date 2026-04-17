/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RZ/G3L LVDS Encoder
 *
 * Copyright (C) 2026 Renesas Electronics Corporation
 *
 */

#ifndef __RZG3L_LVDS_H__
#define __RZG3L_LVDS_H__

struct drm_bridge;

#if IS_ENABLED(CONFIG_DRM_RZG3L_LVDS)
bool rzg3l_lvds_is_connected(struct drm_bridge *bridge);
#else
static inline bool rzg3l_lvds_is_connected(struct drm_bridge *bridge)
{
	return false;
}
#endif /* CONFIG_DRM_RZG3L_LVDS */
#endif /* __RZG3L_LVDS_H__ */
