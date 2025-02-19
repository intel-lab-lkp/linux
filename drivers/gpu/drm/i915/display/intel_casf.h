/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __INTEL_CASF_H__
#define __INTEL_CASF_H__

#include <linux/types.h>

struct intel_crtc_state;
struct intel_atomic_state;
struct intel_crtc;

void intel_casf_enable(struct intel_crtc_state *crtc_state);
void intel_casf_scaler_compute_config(struct intel_crtc_state *crtc_state);
int intel_casf_compute_config(struct intel_crtc_state *crtc_state);
bool intel_casf_needs_scaler(const struct intel_crtc_state *crtc_state);
void intel_filter_lut_load(struct intel_crtc *crtc,
			   const struct intel_crtc_state *crtc_state);
void intel_casf_update_strength(struct intel_crtc_state *new_crtc_state);
void intel_casf_disable(const struct intel_crtc_state *crtc_state);

#endif /* __INTEL_CASF_H__ */
