// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
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

static void intel_darkscreen_detect(struct intel_crtc *crtc);

static u32 intel_darkscreen_get_comp_val(struct drm_i915_private *i915, int bpc)
{
	u32 compare_value = 0;

	switch (bpc) {
	case COLOR_DEPTH_6BPC:
		compare_value = COMPARE_VALUE_6_BPC;
		break;
	case COLOR_DEPTH_8BPC:
		compare_value = COMPARE_VALUE_8_BPC;
		break;
	case COLOR_DEPTH_10BPC:
		compare_value = COMPARE_VALUE_10_BPC;
		break;
	case COLOR_DEPTH_12BPC:
		compare_value = COMPARE_VALUE_12_BPC;
		break;
	default:
		drm_dbg(&i915->drm, "Bpc value is incorrect:%d\n", bpc);
		return -EINVAL;
	}

	compare_value = compare_value << (COMPARE_VALUE_CALCULATION_FACTOR - bpc);
	return DARK_SCREEN_COMPARE_VAL(compare_value);
}

static void intel_darkscreen_work_fn(struct work_struct *work)
{
	struct intel_darkscreen *dark_screen =
		container_of(work, typeof(*dark_screen), darkscreen_detect_work);

	if (!dark_screen->enable)
		intel_darkscreen_enable(dark_screen->crtc);

	intel_darkscreen_detect(dark_screen->crtc);
}

void intel_darkscreen_schedule_work(struct intel_crtc *crtc)
{
	struct drm_i915_private *i915 = to_i915(crtc->base.dev);
	struct intel_darkscreen *dark_screen = &crtc->dark_screen;

	dark_screen->crtc = crtc;
	queue_work(i915->unordered_wq, &dark_screen->darkscreen_detect_work);
}

void intel_darkscreen_setup(struct intel_crtc *crtc)
{
	struct intel_darkscreen *dark_screen;

	dark_screen = &crtc->dark_screen;
	dark_screen = kzalloc(sizeof(*dark_screen), GFP_KERNEL);
	if (!dark_screen)
		return;
	dark_screen->enable = false;

	INIT_WORK(&dark_screen->darkscreen_detect_work, intel_darkscreen_work_fn);
}

/*
 * Check the color format and compute the compare value based on bpc.
 */
int intel_darkscreen_enable(struct intel_crtc *crtc)
{
	struct intel_crtc_state *crtc_state = crtc->config;
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	int bpc = crtc_state->pipe_bpp / 3;
	u32 val;

	if (!crtc->dark_screen.enable)
		return 0;

	if (crtc_state->output_format != INTEL_OUTPUT_FORMAT_RGB) {
		drm_dbg_kms(&dev_priv->drm,
			    "YUV format not supported:%c for darkscreen detection\n",
			    pipe_name(crtc->pipe));
		return -EPROTO;
	}

	val = intel_darkscreen_get_comp_val(dev_priv, bpc);
	val |= DARK_SCREEN_ENABLE;
	intel_de_write(dev_priv, DARK_SCREEN(cpu_transcoder), val);
	crtc->dark_screen.enable = true;

	return 0;
}

static void intel_darkscreen_detect(struct intel_crtc *crtc)
{
	struct intel_crtc_state *crtc_state = crtc->config;
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);
	unsigned int frame_time_in_us;
	u32 val = 0;

	val |= DARK_SCREEN_DETECT | DARK_SCREEN_DONE;
	intel_de_rmw(dev_priv, DARK_SCREEN(crtc->config->cpu_transcoder), 0, val);

	frame_time_in_us = (1000 / drm_mode_vrefresh(&crtc_state->hw.adjusted_mode)) * 2;
	intel_de_wait_for_set(dev_priv, DARK_SCREEN(crtc->config->cpu_transcoder),
			      DARK_SCREEN_DONE, frame_time_in_us);

	if (intel_de_read(dev_priv, DARK_SCREEN(crtc->config->cpu_transcoder)) &
	    DARK_SCREEN_DETECT) {
		drm_dbg_kms(&dev_priv->drm, "Dark screen detected:%c\n", pipe_name(crtc->pipe));
	}
}

void intel_darkscreen_disable(struct intel_crtc *crtc)
{
	struct intel_crtc_state *crtc_state = crtc->config;
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	struct drm_i915_private *dev_priv = to_i915(crtc->base.dev);

	intel_de_write(dev_priv, DARK_SCREEN(cpu_transcoder), 0);
}
