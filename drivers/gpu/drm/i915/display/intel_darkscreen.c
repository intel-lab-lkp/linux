// SPDX-License-Identifier: MIT
/*
 * Copyright © 2018 Intel Corporation
 *
 * Author: Nemesa Garg <nemesa.garg@intel.com>
 */
#include "i915_reg.h"
#include "intel_display_types.h"
#include "intel_de.h"

/*
 * Dark screen detection check if all pixels
 * in a frame are less than or equal to compare
 * value.Check the color format and compute the
 * compare value based on bpc.
 */
void dark_screen_enable(struct intel_crtc_state *crtc_state)
{
	u32 comparevalue;
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	if (!crtc->dark_screen.enable)
		return;

	if (crtc_state->output_format != INTEL_OUTPUT_FORMAT_RGB)
		return;
	drm_err(&dev_priv->drm,
		"RGB format is not present%c\n",
		pipe_name(crtc->pipe));

	switch (crtc_state->pipe_bpp / 3) {
	case DD_COLOR_DEPTH_6BPC:
		comparevalue = DARKSCREEN_COMPARE_VALUE_LIMITED_RANGE_6_BPC;
		break;
	case DD_COLOR_DEPTH_8BPC:
		comparevalue = DARKSCREEN_COMPARE_VALUE_LIMITED_RANGE_8_BPC;
		break;
	case DD_COLOR_DEPTH_10BPC:
		comparevalue = DARKSCREEN_COMPARE_VALUE_LIMITED_RANGE_10_BPC;
		break;
	case DD_COLOR_DEPTH_12BPC:
		comparevalue = DARKSCREEN_COMPARE_VALUE_LIMITED_RANGE_12_BPC;
		break;
	default:
		break;
	}

	comparevalue = comparevalue <<
		(DARKSCREEN_PROGRAMMED_COMPARE_VALUE_CALCULATION_FACTOR - crtc->dark_screen.bpc);

	intel_de_write(dev_priv, DARK_SCREEN(cpu_transcoder),
		       DARK_SCREEN_ENABLE | DARK_SCREEN_DETECT |
		       DARK_SCREEN_DONE | DARK_SCREEN_COMPARE_VAL(comparevalue));

	intel_de_wait_for_set(dev_priv,
			      DARK_SCREEN(crtc->config->cpu_transcoder), DARK_SCREEN_DONE, 1);

	if (intel_de_read(dev_priv, DARK_SCREEN(crtc->config->cpu_transcoder)) &
			  DARK_SCREEN_DETECT) {
		drm_err(&dev_priv->drm,
			"Dark Screen detected %c\n",
			pipe_name(crtc->pipe));
	}

	intel_de_rmw(dev_priv, DARK_SCREEN(crtc->config->cpu_transcoder), 0, DARK_SCREEN_DETECT |
		       DARK_SCREEN_DONE);
}
