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

/*
 * Default LUT values to be loaded one time.
 */
static const u16 lut_data[] = {
	4095, 2047, 1364, 1022, 816, 678, 579,
	504, 444, 397, 357, 323, 293, 268, 244, 224,
	204, 187, 170, 154, 139, 125, 111, 98, 85,
	73, 60, 48, 36, 24, 12, 0
};

void intel_filter_lut_load(struct intel_crtc *crtc,
			   const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	int i;

	intel_de_write(dev_priv, SHRPLUT_INDEX(crtc->pipe), INDEX_AUTO_INCR | INDEX_VALUE(0));

	for (i = 0; i < ARRAY_SIZE(lut_data); i++)
		intel_de_write(dev_priv, SHRPLUT_DATA(crtc->pipe), lut_data[i]);
}

/*
 * HW takes a value in form (1.0 + strength) in 4.4 fixed format.
 * Strength is from 0.0-14.9375 ie from 0-239.
 * User can give value from 0-255 but is clamped to 239.
 * Ex. User gives 85 which is 5.3125 and adding 1.0 gives 6.3125.
 * 6.3125 in 4.4 format is 01100101 which is equal to 101.
 * Also 85 + 16 = 101.
 */
static void intel_filter_strength_compute(struct intel_crtc_state *crtc_state, u8 *val)
{
	*val = min(crtc_state->hw.casf_params.strength, 0xEF) + 0x10;
}

static void intel_filter_size_compute(struct intel_crtc_state *crtc_state)
{
	const struct drm_display_mode *mode = &crtc_state->hw.adjusted_mode;

	if (mode->hdisplay <= 1920 && mode->vdisplay <= 1080)
		crtc_state->hw.casf_params.win_size = 0;
	else if (mode->hdisplay <= 3840 && mode->vdisplay <= 2160)
		crtc_state->hw.casf_params.win_size = 1;
	else
		crtc_state->hw.casf_params.win_size = 2;
}

void intel_sharpen_strength_changed(struct intel_atomic_state *state)
{
	int i;
	struct intel_crtc_state *old_crtc_state, *new_crtc_state;
	struct intel_crtc *crtc;

	for_each_oldnew_intel_crtc_in_state(state, crtc, old_crtc_state,
					    new_crtc_state, i) {
		if (new_crtc_state->uapi.sharpeness_strength !=
				old_crtc_state->uapi.sharpeness_strength)
			new_crtc_state->hw.casf_params.strength_changed = true;
	}
}

void intel_sharpen_filter_enable(struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	int id = crtc_state->scaler_state.scaler_id;
	u32 sharpness_ctl;
	u8 val;

	intel_filter_strength_compute(crtc_state, &val);
	drm_dbg(&dev_priv->drm, "Filter strength value: %d\n", val);

	sharpness_ctl =	FILTER_EN | FILTER_STRENGTH(val) |
		FILTER_SIZE(crtc_state->hw.casf_params.win_size);

	intel_de_write(dev_priv, SHARPNESS_CTL(crtc->pipe),
		       sharpness_ctl);

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
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(intel_crtc->base.dev);

	if (crtc_state->uapi.sharpeness_strength == 0)
		return -EINVAL;

	crtc_state->hw.casf_params.strength =
		crtc_state->uapi.sharpeness_strength;
	crtc_state->hw.casf_params.need_scaler = true;
	
	intel_filter_size_compute(crtc_state);
	drm_dbg(&dev_priv->drm, "Tap Size: %d\n",
		crtc_state->hw.casf_params.win_size);

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
