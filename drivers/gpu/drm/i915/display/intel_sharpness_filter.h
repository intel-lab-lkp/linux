/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_SHARPNESS_FILTER_H__
#define __INTEL_SHARPNESS_FILTER_H__

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

struct intel_sharpness_filter {
#define SCALER_FILTER_NUM_TAPS 7
#define SCLAER_FILTER_COEFF 119
	struct scaler_filter_coeff coeff[SCALER_FILTER_NUM_TAPS];
	u32 scaler_coefficient[SCLAER_FILTER_COEFF];
	bool strength_changed;
	u8 win_size;
	bool need_scaler;
	u8 strength;
};

void intel_sharpness_filter_enable(struct intel_crtc_state *crtc_state);
void intel_sharpness_scaler_compute_config(struct intel_crtc_state *crtc_state);
int intel_sharpness_filter_compute_config(struct intel_crtc_state *crtc_state);
void intel_filter_lut_load(struct intel_crtc *crtc,
			   const struct intel_crtc_state *crtc_state);
bool intel_sharpness_strength_changed(struct intel_atomic_state *state);
void intel_sharpness_disable(struct intel_crtc_state *crtc_state);
bool sharp_compute(struct intel_crtc_state *crtc_state);

#endif /* __INTEL_SHARPEN_FILTER_H__ */
