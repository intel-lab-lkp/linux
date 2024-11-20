/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_PFIT_H__
#define __INTEL_PFIT_H__

struct drm_connector_state;
struct intel_crtc_state;

int intel_gmch_panel_fitting(struct intel_crtc_state *crtc_state,
			     const struct drm_connector_state *conn_state);
int intel_pch_panel_fitting(struct intel_crtc_state *crtc_state,
			    const struct drm_connector_state *conn_state);

#endif /* __INTEL_PFIT_H__ */
