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

void intel_casf_update_strength(struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);

	intel_de_rmw(display, SHARPNESS_CTL(crtc->pipe), 0,
		     FILTER_STRENGTH(crtc_state->hw.casf_params.strength));
}

static void intel_casf_compute_win_size(struct intel_crtc_state *crtc_state)
{
	const struct drm_display_mode *mode = &crtc_state->hw.adjusted_mode;
	u16 total_pixels = mode->hdisplay * mode->vdisplay;

	if (total_pixels <= MAX_PIXELS_FOR_3_TAP_FILTER)
		crtc_state->hw.casf_params.win_size = SHARPNESS_FILTER_SIZE_3X3;
	else if (total_pixels <= MAX_PIXELS_FOR_5_TAP_FILTER)
		crtc_state->hw.casf_params.win_size = SHARPNESS_FILTER_SIZE_5X5;
	else
		crtc_state->hw.casf_params.win_size = SHARPNESS_FILTER_SIZE_7X7;
}

int intel_casf_compute_config(struct intel_crtc_state *crtc_state)
{
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

	intel_casf_compute_win_size(crtc_state);

	return 0;
}
