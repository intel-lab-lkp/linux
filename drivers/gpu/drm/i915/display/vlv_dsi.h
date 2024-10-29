/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __VLV_DSI_H__
#define __VLV_DSI_H__

#include <linux/types.h>

#include <drm/drm_mipi_dsi.h>

enum port;
struct drm_i915_private;
struct intel_crtc_state;
struct intel_dsi;

#ifdef I915
void vlv_dsi_wait_for_fifo_empty(struct intel_dsi *intel_dsi, enum port port);
enum mipi_dsi_pixel_format pixel_format_from_register_bits(u32 fmt);
int vlv_dsi_min_cdclk(const struct intel_crtc_state *crtc_state);
void vlv_dsi_init(struct drm_i915_private *dev_priv);
#else
static inline void vlv_dsi_wait_for_fifo_empty(struct intel_dsi *intel_dsi, enum port port)
{
}
static inline enum mipi_dsi_pixel_format pixel_format_from_register_bits(u32 fmt)
{
	return 0;
}
static inline int vlv_dsi_min_cdclk(const struct intel_crtc_state *crtc_state)
{
	return 0;
}
static inline void vlv_dsi_init(struct drm_i915_private *dev_priv)
{
}
#endif

#endif /* __VLV_DSI_H__ */
