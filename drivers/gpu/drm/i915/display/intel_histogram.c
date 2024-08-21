// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <drm/drm_device.h>
#include <drm/drm_file.h>

#include "i915_reg.h"
#include "i915_drv.h"
#include "intel_display.h"
#include "intel_histogram.h"
#include "intel_display_types.h"
#include "intel_de.h"

#define HISTOGRAM_GUARDBAND_THRESHOLD_DEFAULT 300    // 3.0% of the pipe's current pixel count.
#define HISTOGRAM_GUARDBAND_PRECISION_FACTOR 10000   // Precision factor for threshold guardband.
#define HISTOGRAM_DEFAULT_GUARDBAND_DELAY 0x04

struct intel_histogram {
	struct drm_i915_private *i915;
	bool enable;
	bool can_enable;
	enum pipe pipe;
	u32 bindata[HISTOGRAM_BIN_COUNT];
};

int intel_histogram_atomic_check(struct intel_crtc *intel_crtc)
{
	struct intel_histogram *histogram = intel_crtc->histogram;

	/* TODO: Restrictions for enabling histogram */
	histogram->can_enable = true;

	return 0;
}

static void intel_histogram_enable_dithering(struct drm_i915_private *dev_priv,
					     enum pipe pipe)
{
	intel_de_rmw(dev_priv, PIPE_MISC(pipe), PIPE_MISC_DITHER_ENABLE,
		     PIPE_MISC_DITHER_ENABLE);
}

static int intel_histogram_enable(struct intel_crtc *intel_crtc)
{
	struct drm_i915_private *i915 = to_i915(intel_crtc->base.dev);
	struct intel_histogram *histogram = intel_crtc->histogram;
	int pipe = intel_crtc->pipe;
	u64 res;
	u32 gbandthreshold;

	if (!histogram)
		return -EINVAL;

	if (!histogram->can_enable) {
		return -EINVAL;
	}

	if (histogram->enable)
		return 0;

	/* Pipe Dithering should be enabled with GLOBAL_HIST */
	intel_histogram_enable_dithering(i915, pipe);

	/*
	 * enable DPST_CTL Histogram mode
	 * Clear DPST_CTL Bin Reg function select to TC
	 */
	intel_de_rmw(i915, DPST_CTL(pipe),
		     DPST_CTL_BIN_REG_FUNC_SEL | DPST_CTL_IE_HIST_EN |
		     DPST_CTL_HIST_MODE | DPST_CTL_IE_TABLE_VALUE_FORMAT,
		     DPST_CTL_BIN_REG_FUNC_TC | DPST_CTL_IE_HIST_EN |
		     DPST_CTL_HIST_MODE_HSV |
		     DPST_CTL_IE_TABLE_VALUE_FORMAT_1INT_9FRAC);

	/* Re-Visit: check if wait for one vblank is required */
	drm_crtc_wait_one_vblank(&intel_crtc->base);

	/* TODO: one time programming: Program GuardBand Threshold */
	res = (intel_crtc->config->hw.adjusted_mode.vtotal *
				intel_crtc->config->hw.adjusted_mode.htotal);
	gbandthreshold = (res *	HISTOGRAM_GUARDBAND_THRESHOLD_DEFAULT) /
				HISTOGRAM_GUARDBAND_PRECISION_FACTOR;

	/* Enable histogram interrupt mode */
	intel_de_rmw(i915, DPST_GUARD(pipe),
		     DPST_GUARD_THRESHOLD_GB_MASK |
		     DPST_GUARD_INTERRUPT_DELAY_MASK | DPST_GUARD_HIST_INT_EN,
		     DPST_GUARD_THRESHOLD_GB(gbandthreshold) |
		     DPST_GUARD_INTERRUPT_DELAY(HISTOGRAM_DEFAULT_GUARDBAND_DELAY) |
		     DPST_GUARD_HIST_INT_EN);

	/* Clear pending interrupts has to be done on separate write */
	intel_de_rmw(i915, DPST_GUARD(pipe),
		     DPST_GUARD_HIST_EVENT_STATUS, 1);

	histogram->enable = true;

	return 0;
}

static void intel_histogram_disable(struct intel_crtc *intel_crtc)
{
	struct drm_i915_private *i915 = to_i915(intel_crtc->base.dev);
	struct intel_histogram *histogram = intel_crtc->histogram;
	int pipe = intel_crtc->pipe;

	if (!histogram)
		return;

	/* Pipe Dithering should be enabled with GLOBAL_HIST */
	intel_histogram_enable_dithering(i915, pipe);

	/* Clear pending interrupts and disable interrupts */
	intel_de_rmw(i915, DPST_GUARD(pipe),
		     DPST_GUARD_HIST_INT_EN | DPST_GUARD_HIST_EVENT_STATUS, 0);

	/* disable DPST_CTL Histogram mode */
	intel_de_rmw(i915, DPST_CTL(pipe),
		     DPST_CTL_IE_HIST_EN, 0);

	histogram->enable = false;
}

int intel_histogram_update(struct intel_crtc *intel_crtc, bool enable)
{
	if (enable)
		return intel_histogram_enable(intel_crtc);

	intel_histogram_disable(intel_crtc);
	return 0;
}

int intel_histogram_set_iet_lut(struct intel_crtc *intel_crtc, u32 *data)
{
	struct intel_histogram *histogram = intel_crtc->histogram;
	struct drm_i915_private *i915 = to_i915(intel_crtc->base.dev);
	int pipe = intel_crtc->pipe;
	int i = 0;

	if (!histogram)
		return -EINVAL;

	if (!histogram->enable) {
		drm_err(&i915->drm, "histogram not enabled");
		return -EINVAL;
	}

	if (!data) {
		drm_err(&i915->drm, "enhancement LUT data is NULL");
		return -EINVAL;
	}

	/*
	 * Set DPST_CTL Bin Reg function select to IE
	 * Set DPST_CTL Bin Register Index to 0
	 */
	intel_de_rmw(i915, DPST_CTL(pipe),
		     DPST_CTL_BIN_REG_FUNC_SEL | DPST_CTL_BIN_REG_MASK,
		     DPST_CTL_BIN_REG_FUNC_IE | DPST_CTL_BIN_REG_CLEAR);

	for (i = 0; i < HISTOGRAM_IET_LENGTH; i++) {
		intel_de_rmw(i915, DPST_BIN(pipe),
			     DPST_BIN_DATA_MASK, data[i]);
		drm_dbg_atomic(&i915->drm, "iet_lut[%d]=%x\n", i, data[i]);
	}

	intel_de_rmw(i915, DPST_CTL(pipe),
		     DPST_CTL_ENHANCEMENT_MODE_MASK | DPST_CTL_IE_MODI_TABLE_EN,
		     DPST_CTL_EN_MULTIPLICATIVE | DPST_CTL_IE_MODI_TABLE_EN);

	/* Once IE is applied, change DPST CTL to TC */
	intel_de_rmw(i915, DPST_CTL(pipe),
		     DPST_CTL_BIN_REG_FUNC_SEL, DPST_CTL_BIN_REG_FUNC_TC);

	return 0;
}

void intel_histogram_deinit(struct intel_crtc *intel_crtc)
{
	struct intel_histogram *histogram = intel_crtc->histogram;

	kfree(histogram);
}

int intel_histogram_init(struct intel_crtc *intel_crtc)
{
	struct drm_i915_private *i915 = to_i915(intel_crtc->base.dev);
	struct intel_histogram *histogram;

	/* Allocate histogram internal struct */
	histogram = kzalloc(sizeof(*histogram), GFP_KERNEL);
	if (!histogram) {
		return -ENOMEM;
	}

	intel_crtc->histogram = histogram;
	histogram->pipe = intel_crtc->pipe;
	histogram->can_enable = false;

	histogram->i915 = i915;

	return 0;
}
