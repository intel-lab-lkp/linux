// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_vblank.h>

#include "i915_reg.h"
#include "i915_drv.h"
#include "intel_de.h"
#include "intel_display.h"
#include "intel_display_types.h"
#include "intel_histogram.h"
#include "intel_histogram_regs.h"

/* 3.0% of the pipe's current pixel count, hw does x4 */
#define HISTOGRAM_GUARDBAND_THRESHOLD_DEFAULT 300
/* Precision factor for threshold guardband */
#define HISTOGRAM_GUARDBAND_PRECISION_FACTOR 10000
#define HISTOGRAM_DEFAULT_GUARDBAND_DELAY 0x04

struct intel_histogram {
	struct intel_crtc *crtc;
	struct delayed_work work;
	bool enable;
	bool can_enable;
	u32 bin_data[HISTOGRAM_BIN_COUNT];
};

int intel_histogram_atomic_check(struct intel_crtc *intel_crtc)
{
	struct intel_histogram *histogram = intel_crtc->histogram;

	/* TODO: Restrictions for enabling histogram */
	histogram->can_enable = true;

	return 0;
}

static int intel_histogram_enable(struct intel_crtc *intel_crtc)
{
	struct intel_display *display = to_intel_display(intel_crtc);
	struct intel_histogram *histogram = intel_crtc->histogram;
	int pipe = intel_crtc->pipe;
	u64 res;
	u32 gbandthreshold;

	if (!histogram)
		return -EINVAL;

	if (!histogram->can_enable)
		return -EINVAL;

	if (histogram->enable)
		return 0;

	 /* enable histogram, clear DPST_CTL bin reg func select to TC */
	intel_de_rmw(display, DPST_CTL(pipe),
		     DPST_CTL_BIN_REG_FUNC_SEL | DPST_CTL_IE_HIST_EN |
		     DPST_CTL_HIST_MODE | DPST_CTL_IE_TABLE_VALUE_FORMAT |
		     DPST_CTL_ENHANCEMENT_MODE_MASK | DPST_CTL_IE_MODI_TABLE_EN,
		     DPST_CTL_BIN_REG_FUNC_TC | DPST_CTL_IE_HIST_EN |
		     DPST_CTL_HIST_MODE_HSV |
		     DPST_CTL_IE_TABLE_VALUE_FORMAT_1INT_9FRAC |
		     DPST_CTL_EN_MULTIPLICATIVE | DPST_CTL_IE_MODI_TABLE_EN);

	/* Re-Visit: check if wait for one vblank is required */
	drm_crtc_wait_one_vblank(&intel_crtc->base);

	/* TODO: Program GuardBand Threshold: To be moved to modeset path */
	res = (intel_crtc->config->hw.adjusted_mode.vtotal *
	       intel_crtc->config->hw.adjusted_mode.htotal);

	gbandthreshold = (res *	HISTOGRAM_GUARDBAND_THRESHOLD_DEFAULT) /
			  HISTOGRAM_GUARDBAND_PRECISION_FACTOR;

	/* Enable histogram interrupt mode */
	intel_de_rmw(display, DPST_GUARD(pipe),
		     DPST_GUARD_THRESHOLD_GB_MASK |
		     DPST_GUARD_INTERRUPT_DELAY_MASK | DPST_GUARD_HIST_INT_EN,
		     DPST_GUARD_THRESHOLD_GB(gbandthreshold) |
		     DPST_GUARD_INTERRUPT_DELAY(HISTOGRAM_DEFAULT_GUARDBAND_DELAY) |
		     DPST_GUARD_HIST_INT_EN);

	/* Clear pending interrupts has to be done on separate write */
	intel_de_rmw(display, DPST_GUARD(pipe),
		     DPST_GUARD_HIST_EVENT_STATUS, 1);

	histogram->enable = true;

	return 0;
}

static void intel_histogram_disable(struct intel_crtc *intel_crtc)
{
	struct intel_display *display = to_intel_display(intel_crtc);
	struct intel_histogram *histogram = intel_crtc->histogram;
	int pipe = intel_crtc->pipe;

	if (!histogram)
		return;

	/* If already disabled return */
	if (histogram->enable)
		return;

	/* Clear pending interrupts and disable interrupts */
	intel_de_rmw(display, DPST_GUARD(pipe),
		     DPST_GUARD_HIST_INT_EN | DPST_GUARD_HIST_EVENT_STATUS, 0);

	/* disable DPST_CTL Histogram mode */
	intel_de_rmw(display, DPST_CTL(pipe),
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
	struct intel_display *display = to_intel_display(intel_crtc);
	int pipe = intel_crtc->pipe;
	int i = 0;

	if (!histogram)
		return -EINVAL;

	if (!histogram->enable) {
		drm_err(display->drm, "histogram not enabled");
		return -EINVAL;
	}

	if (!data) {
		drm_err(display->drm, "enhancement LUT data is NULL");
		return -EINVAL;
	}

	/* Set DPST_CTL Bin Reg function select to IE & wait for a vblabk */
	intel_de_rmw(display, DPST_CTL(pipe),
		     DPST_CTL_BIN_REG_FUNC_SEL, DPST_CTL_BIN_REG_FUNC_IE);

	drm_crtc_wait_one_vblank(&intel_crtc->base);

	 /* Set DPST_CTL Bin Register Index to 0 */
	intel_de_rmw(display, DPST_CTL(pipe),
		     DPST_CTL_BIN_REG_MASK, DPST_CTL_BIN_REG_CLEAR);

	for (i = 0; i < HISTOGRAM_IET_LENGTH; i++) {
		intel_de_rmw(display, DPST_BIN(pipe),
			     DPST_BIN_DATA_MASK, data[i]);
		drm_dbg_atomic(display->drm, "iet_lut[%d]=%x\n", i, data[i]);
	}

	return 0;
}

void intel_histogram_finish(struct intel_crtc *intel_crtc)
{
	struct intel_histogram *histogram = intel_crtc->histogram;

	kfree(histogram);
}

int intel_histogram_init(struct intel_crtc *intel_crtc)
{
	struct intel_histogram *histogram;

	/* Allocate histogram internal struct */
	histogram = kzalloc(sizeof(*histogram), GFP_KERNEL);
	if (!histogram) {
		return -ENOMEM;
	}

	intel_crtc->histogram = histogram;
	histogram->crtc = intel_crtc;
	histogram->can_enable = false;

	return 0;
}
