/* SPDX-License-Identifier: MIT
 *
 * Copyright © 2024 Intel Corporation
 */

#ifndef _INTEL_ALPM_H
#define _INTEL_ALPM_H

#include <linux/types.h>

struct intel_dp;
struct intel_crtc_state;
struct drm_connector_state;

bool intel_alpm_get_aux_less_status(struct intel_dp *intel_dp);
bool intel_alpm_compute_params(struct intel_dp *intel_dp,
			       struct intel_crtc_state *crtc_state);
void intel_alpm_compute_lobf_config(struct intel_dp *intel_dp,
				    struct intel_crtc_state *crtc_state,
				    struct drm_connector_state *conn_state);
void intel_alpm_configure(struct intel_dp *intel_dp);

#endif
