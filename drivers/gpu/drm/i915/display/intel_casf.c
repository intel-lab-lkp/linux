// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 *
 */
#include "i915_reg.h"
#include "intel_casf.h"
#include "intel_casf_regs.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "skl_scaler.h"

#define FILTER_COEFF_0_125 125
#define FILTER_COEFF_0_25 250
#define FILTER_COEFF_0_5 500
#define FILTER_COEFF_1_0 1000
#define FILTER_COEFF_0_0 0
#define SET_POSITIVE_SIGN(x) ((x) & (~SIGN))

#define MAX_PIXELS_FOR_3_TAP_FILTER (1920 * 1080)
#define MAX_PIXELS_FOR_5_TAP_FILTER (3840 * 2160)

/**
 * DOC: Content Adaptive Sharpness Filter (CASF)
 *
 * From LNL onwards the display engine based adaptive
 * sharpening filter is supported. This helps in
 * improving the image quality. The display hardware
 * uses one of the pipe scaler for implementing casf.
 * It works on a region of pixels depending on the
 * tap size. The coefficients are used to generate an
 * alpha value which is used to blend the sharpened image
 * to original image.
 */

const u16 filtercoeff_1[] = {
	FILTER_COEFF_0_0, FILTER_COEFF_0_0, FILTER_COEFF_0_5,
	FILTER_COEFF_1_0, FILTER_COEFF_0_5, FILTER_COEFF_0_0,
	FILTER_COEFF_0_0,
};

const u16 filtercoeff_2[] = {
	FILTER_COEFF_0_0, FILTER_COEFF_0_25, FILTER_COEFF_0_5,
	FILTER_COEFF_1_0, FILTER_COEFF_0_5, FILTER_COEFF_0_25,
	FILTER_COEFF_0_0,
};

const u16 filtercoeff_3[] = {
	FILTER_COEFF_0_125, FILTER_COEFF_0_25, FILTER_COEFF_0_5,
	FILTER_COEFF_1_0, FILTER_COEFF_0_5, FILTER_COEFF_0_25,
	FILTER_COEFF_0_125,
};

static int casf_coeff_tap(int i)
{
	return i % SCALER_FILTER_NUM_TAPS;
}

static u16 casf_coeff(struct intel_crtc_state *crtc_state, int t)
{
	struct scaler_filter_coeff value;
	u16 coeff;

	value = crtc_state->hw.casf_params.coeff[t];
	coeff = SET_POSITIVE_SIGN(0) | EXPONENT(value.exp) | MANTISSA(value.mantissa);

	return coeff;
}

/* Default LUT values to be loaded one time. */
static const u16 lut_data[] = {
	4095, 2047, 1364, 1022, 816, 678, 579,
	504, 444, 397, 357, 323, 293, 268, 244, 224,
	204, 187, 170, 154, 139, 125, 111, 98, 85,
	73, 60, 48, 36, 24, 12, 0
};

void intel_filter_lut_load(struct intel_crtc *crtc,
			   const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	int i;

	intel_de_write(display, SHRPLUT_INDEX(crtc->pipe),
		       INDEX_AUTO_INCR | INDEX_VALUE(0));

	for (i = 0; i < ARRAY_SIZE(lut_data); i++)
		intel_de_write(display, SHRPLUT_DATA(crtc->pipe),
			       lut_data[i]);
}

static void intel_casf_size_compute(struct intel_crtc_state *crtc_state)
{
	const struct drm_display_mode *mode = &crtc_state->hw.adjusted_mode;
	u16 total_pixels = mode->hdisplay * mode->vdisplay;

	if (total_pixels <= MAX_PIXELS_FOR_3_TAP_FILTER)
		crtc_state->hw.casf_params.win_size = 0;
	else if (total_pixels <= MAX_PIXELS_FOR_5_TAP_FILTER)
		crtc_state->hw.casf_params.win_size = 1;
	else
		crtc_state->hw.casf_params.win_size = 2;
}

void intel_casf_update_strength(struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	u32 sharpness_ctl;

	sharpness_ctl = FILTER_EN | FILTER_STRENGTH(crtc_state->hw.casf_params.strength);

	switch (crtc_state->hw.casf_params.win_size) {
	case 0:
		sharpness_ctl |= SHARPNESS_FILTER_SIZE_3X3;
		break;
	case 1:
		sharpness_ctl |= SHARPNESS_FILTER_SIZE_5X5;
		break;
	default:
		sharpness_ctl |= SHARPNESS_FILTER_SIZE_7X7;
		break;
	}

	intel_de_write(display, SHARPNESS_CTL(crtc->pipe), sharpness_ctl);
}

/*
 * 17 phase of 7 taps requires 119 coefficients in 60 dwords per set.
 * To enable casf:  program scaler coefficients with the coeffients
 * that are calculated and stored in hw.casf_params.coeff as per
 * SCALER_COEFFICIENT_FORMAT
 *
 */
static void intel_casf_write_coeff(struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	int id = crtc_state->scaler_state.scaler_id;
	int i;

	if (id == 0) {
		drm_WARN(display->drm, 0, "Second scaler not enabled\n");
		return;
	}

	intel_de_write_fw(display, GLK_PS_COEF_INDEX_SET(crtc->pipe, id, 0),
			  PS_COEF_INDEX_AUTO_INC);

	intel_de_write_fw(display, GLK_PS_COEF_INDEX_SET(crtc->pipe, id, 1),
			  PS_COEF_INDEX_AUTO_INC);

	for (i = 0; i < 17 * 7; i += 2) {
		u32 tmp;
		int t;

		t = casf_coeff_tap(i);
		tmp = casf_coeff(crtc_state, t);

		t = casf_coeff_tap(i + 1);
		tmp |= casf_coeff(crtc_state, t) << 16;

		intel_de_write_fw(display, GLK_PS_COEF_DATA_SET(crtc->pipe, id, 0),
				  tmp);
		intel_de_write_fw(display, GLK_PS_COEF_DATA_SET(crtc->pipe, id, 1),
				  tmp);
	}
}

void intel_casf_enable(struct intel_crtc_state *crtc_state)
{
	intel_casf_update_strength(crtc_state);

	intel_casf_write_coeff(crtc_state);

	skl_scaler_setup_casf(crtc_state);
}

bool intel_casf_needs_scaler(const struct intel_crtc_state *crtc_state)
{
	if (crtc_state->hw.casf_params.casf_enable)
		return true;

	return false;
}

int intel_casf_compute_config(struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);

	if (!HAS_CASF(display))
		return -EINVAL;

	if (crtc_state->uapi.sharpness_strength == 0) {
		crtc_state->hw.casf_params.casf_enable = false;
		crtc_state->hw.casf_params.strength = 0;
		return 0;
	}

	/* If panel fitting enabled casf cannot be enabled */
	if (crtc_state->pch_pfit.enabled)
		return -EINVAL;

	crtc_state->hw.casf_params.casf_enable = true;

	/*
	 * HW takes a value in form (1.0 + strength) in 4.4 fixed format.
	 * Strength is from 0.0-14.9375 ie from 0-239.
	 * User can give value from 0-255 but is clamped to 239.
	 * Ex. User gives 85 which is 5.3125 and adding 1.0 gives 6.3125.
	 * 6.3125 in 4.4 format is b01100101 which is equal to 101.
	 * Also 85 + 16 = 101.
	 */
	crtc_state->hw.casf_params.strength =
		 min(crtc_state->uapi.sharpness_strength, 0xEF) + 0x10;

	intel_casf_size_compute(crtc_state);

	intel_casf_scaler_compute_config(crtc_state);

	return 0;
}

static void convert_sharpness_coef_binary(struct scaler_filter_coeff *coeff,
					  u16 coefficient)
{
	if (coefficient < 25) {
		coeff->mantissa = (coefficient * 2048) / 100;
		coeff->exp = 3;
	} else if (coefficient < 50) {
		coeff->mantissa = (coefficient * 1024) / 100;
		coeff->exp = 2;
	} else if (coefficient < 100) {
		coeff->mantissa = (coefficient * 512) / 100;
		coeff->exp = 1;
	} else {
		coeff->mantissa = (coefficient * 256) / 100;
		coeff->exp = 0;
	}
}

void intel_casf_scaler_compute_config(struct intel_crtc_state *crtc_state)
{
	const u16 *filtercoeff;
	u16 filter_coeff[SCALER_FILTER_NUM_TAPS];
	u16 sumcoeff = 0;
	u8 i;

	if (crtc_state->hw.casf_params.win_size == 0)
		filtercoeff = filtercoeff_1;
	else if (crtc_state->hw.casf_params.win_size == 1)
		filtercoeff = filtercoeff_2;
	else
		filtercoeff = filtercoeff_3;

	for (i = 0; i < SCALER_FILTER_NUM_TAPS; i++)
		sumcoeff += *(filtercoeff + i);

	for (i = 0; i < SCALER_FILTER_NUM_TAPS; i++) {
		filter_coeff[i] = (*(filtercoeff + i) * 100 / sumcoeff);
		convert_sharpness_coef_binary(&crtc_state->hw.casf_params.coeff[i],
					      filter_coeff[i]);
	}
}

void intel_casf_disable(const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	intel_de_write(display, SHARPNESS_CTL(crtc->pipe), 0);
}
