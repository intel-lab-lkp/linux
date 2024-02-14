/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_SHARPEN_FLITER_H__
#define __INTEL_SHARPEN_FILTER_H__

#include <linux/types.h>

#define SHARP_COEFF_TO_REG_FORMAT(coefficient) ((u16)(coefficient.sign << 15 | \
			coefficient.exp << 12 | coefficient.mantissa << 3))

struct intel_crtc;
struct intel_crtc_state;
struct intel_atomic_state;

struct scaler_filter_coeff {
	u16 sign;
	u16 exp;
	u16 mantissa;
};

void intel_sharpen_filter_enable(struct intel_crtc_state *crtc_state);
void intel_sharpness_scaler_compute_config(struct intel_crtc_state *crtc_state);
int intel_filter_compute_config(struct intel_crtc_state *crtc_state);
#endif /* __INTEL_SHARPEN_FLITER_H__ */
