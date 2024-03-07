// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 *
 */

#include "i915_reg.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "skl_scaler.h"

#define MAX_NUM_UNIQUE_COEF_FOR_SHARPNESS_FILTER 7
#define SCALER_FILTER_NUM_TAPS 7
#define SCALER_FILTER_NUM_PHASES 17
#define filter_coeff_0_125 125
#define filter_coeff_0_25 250
#define filter_coeff_0_5 500
#define filter_coeff_1_0 1000
#define filter_coeff_0_0 0

void intel_sharpen_filter_enable(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	int id = crtc_state->scaler_state.scaler_id;

	intel_de_write_fw(dev_priv, GLK_PS_COEF_INDEX_SET(crtc->pipe, id, 0),
			  PS_COEF_INDEX_AUTO_INC);

	intel_de_write_fw(dev_priv, GLK_PS_COEF_INDEX_SET(crtc->pipe, id, 1),
			  PS_COEF_INDEX_AUTO_INC);

	for (int index = 0; index < 60; index++) {
		intel_de_write_fw(dev_priv, GLK_PS_COEF_DATA_SET(crtc->pipe, id, 0),
				  crtc_state->hw.casf_params.scaler_coefficient[index]);
		intel_de_write_fw(dev_priv, GLK_PS_COEF_DATA_SET(crtc->pipe, id, 1),
				  crtc_state->hw.casf_params. scaler_coefficient[index]);
	}

	casf_scaler_enable(crtc_state);
}

int intel_filter_compute_config(struct intel_crtc_state *crtc_state)
{
	crtc_state->hw.casf_params.need_scaler = true;

	return 0;
}

static void convert_sharpness_coef_binary(struct scaler_filter_coeff *coeff,
					  u16 coefficient)
{
	if (coefficient < 25) {
		coeff->mantissa = (coefficient * 2048) / 100;
		coeff->exp = 3;
	}

	else if (coefficient < 50) {
		coeff->mantissa = (coefficient * 1024) / 100;
		coeff->exp = 2;
	}

	else if (coefficient < 100) {
		coeff->mantissa = (coefficient * 512) / 100;
		coeff->exp = 1;
	} else {
		coeff->mantissa = (coefficient * 256) / 100;
		coeff->exp = 0;
	}
}

static void intel_sharpness_filter_coeff(struct intel_crtc_state *crtc_state)
{
	u16 filtercoeff[MAX_NUM_UNIQUE_COEF_FOR_SHARPNESS_FILTER];
	u16 sumcoeff = 0;
	u8 i;

	if (crtc_state->hw.casf_params.win_size == 0) {
		filtercoeff[0] = filter_coeff_0_0;
		filtercoeff[1] = filter_coeff_0_0;
		filtercoeff[2] = filter_coeff_0_5;
		filtercoeff[3] = filter_coeff_1_0;
		filtercoeff[4] = filter_coeff_0_5;
		filtercoeff[5] = filter_coeff_0_0;
		filtercoeff[6] = filter_coeff_0_0;
	}

	else if (crtc_state->hw.casf_params.win_size == 1) {
		filtercoeff[0] = filter_coeff_0_0;
		filtercoeff[1] = filter_coeff_0_25;
		filtercoeff[2] = filter_coeff_0_5;
		filtercoeff[3] = filter_coeff_1_0;
		filtercoeff[4] = filter_coeff_0_5;
		filtercoeff[5] = filter_coeff_0_25;
		filtercoeff[6] = filter_coeff_0_0;
	} else {
		filtercoeff[0] = filter_coeff_0_125;
		filtercoeff[1] = filter_coeff_0_25;
		filtercoeff[2] = filter_coeff_0_5;
		filtercoeff[3] = filter_coeff_1_0;
		filtercoeff[4] = filter_coeff_0_5;
		filtercoeff[5] = filter_coeff_0_25;
		filtercoeff[6] = filter_coeff_0_125;
	}

	for (i = 0; i < MAX_NUM_UNIQUE_COEF_FOR_SHARPNESS_FILTER; i++)
		sumcoeff += filtercoeff[i];

	for (i = 0; i < MAX_NUM_UNIQUE_COEF_FOR_SHARPNESS_FILTER; i++) {
		filtercoeff[i] = (filtercoeff[i] * 100 / sumcoeff);
		convert_sharpness_coef_binary(&crtc_state->hw.casf_params.coeff[i],
					      filtercoeff[i]);
	}
}

void intel_sharpness_scaler_compute_config(struct intel_crtc_state *crtc_state)
{
	u16 phase, tapindex, phaseoffset;
	u16 *coeff = (u16 *)crtc_state->hw.casf_params.scaler_coefficient;

	intel_sharpness_filter_coeff(crtc_state);

	for (phase = 0; phase < SCALER_FILTER_NUM_PHASES; phase++) {
		phaseoffset = SCALER_FILTER_NUM_TAPS * phase;
		for (tapindex = 0; tapindex < SCALER_FILTER_NUM_TAPS; tapindex++) {
			coeff[phaseoffset + tapindex] =
				SHARP_COEFF_TO_REG_FORMAT(crtc_state->hw.casf_params.coeff[tapindex]);
		}
	}
}
