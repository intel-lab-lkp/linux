// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 *
 */

#include "i915_reg.h"
#include "intel_de.h"
#include "intel_display_types.h"

#define COLOR_DEPTH_6BPC 6
#define COLOR_DEPTH_8BPC 8
#define COLOR_DEPTH_10BPC 10
#define COLOR_DEPTH_12BPC 12

#define COMPARE_VALUE_6_BPC 4
#define COMPARE_VALUE_8_BPC 16
#define COMPARE_VALUE_10_BPC 64
#define COMPARE_VALUE_12_BPC 256

#define COMPARE_VALUE_CALCULATION_FACTOR 12

/*
 * Checks the color format and compute the comapre value based on bpc.
 */
void intel_dark_screen_enable(struct intel_crtc_state *crtc_state)
{
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	u32 comparevalue;

	if (!crtc->dark_screen.enable)
		return;

	if (crtc_state->output_format != INTEL_OUTPUT_FORMAT_RGB)
		return;

	switch (crtc_state->pipe_bpp / 3) {
	case COLOR_DEPTH_6BPC:
		comparevalue = COMPARE_VALUE_6_BPC;
		break;
	case COLOR_DEPTH_8BPC:
		comparevalue = COMPARE_VALUE_8_BPC;
		break;
	case COLOR_DEPTH_10BPC:
		comparevalue = COMPARE_VALUE_10_BPC;
		break;
	case COLOR_DEPTH_12BPC:
		comparevalue = COMPARE_VALUE_12_BPC;
		break;
	default:
		drm_dbg(&dev_priv->drm,
			"Bpc value is incorrect:%d\n",
			crtc_state->pipe_bpp / 3);
		return;
	}

	comparevalue = comparevalue <<
		(COMPARE_VALUE_CALCULATION_FACTOR - crtc_state->pipe_bpp / 3);

	intel_de_write(dev_priv, DARK_SCREEN(cpu_transcoder),
		       DARK_SCREEN_ENABLE | DARK_SCREEN_DETECT |
		       DARK_SCREEN_DONE | DARK_SCREEN_COMPARE_VAL(comparevalue));

	intel_de_wait_for_set(dev_priv,
			      DARK_SCREEN(crtc->config->cpu_transcoder), DARK_SCREEN_DONE, 1);

	if (intel_de_read(dev_priv, DARK_SCREEN(crtc->config->cpu_transcoder)) &
			  DARK_SCREEN_DETECT) {
		drm_err(&dev_priv->drm,
			"Dark Screen detected:%c\n",
			pipe_name(crtc->pipe));
	}

	intel_de_rmw(dev_priv, DARK_SCREEN(crtc->config->cpu_transcoder), 1, DARK_SCREEN_DETECT |
		     DARK_SCREEN_DONE);
}

void intel_dark_screen_disable(struct intel_crtc_state *crtc_state)
{
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	intel_de_write(dev_priv, DARK_SCREEN(cpu_transcoder), 0);
}

static int intel_darkscreen_debugfs_status_get(void *data, u64 *val)
{
	struct intel_crtc *crtc = data;

	*val = crtc->dark_screen.enable;

	return 0;
}

static int intel_darkscreen_debugfs_status_set(void *data, u64 val)
{
	struct intel_crtc *crtc = data;
	struct intel_crtc_state *crtc_state;

	crtc->dark_screen.enable = val;

	crtc_state = to_intel_crtc_state(crtc->base.state);

	if (val)
		intel_dark_screen_enable(crtc_state);
	else
		intel_dark_screen_disable(crtc_state);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(intel_darkscreen_debugfs_status_fops,
			 intel_darkscreen_debugfs_status_get,
			 intel_darkscreen_debugfs_status_set, "%llu\n");

void intel_darkscreen_crtc_debugfs_add(struct intel_crtc *crtc)
{
	debugfs_create_file("i915_darkscreen_status", 0644, crtc->base.debugfs_entry,
			    crtc, &intel_darkscreen_debugfs_status_fops);
}
