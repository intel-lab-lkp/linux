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
#define HISTOGRAM_BIN_READ_RETRY_COUNT 5

struct intel_histogram {
	struct intel_crtc *crtc;
	struct delayed_work work;
	bool enable;
	bool can_enable;
	u32 bin_data[HISTOGRAM_BIN_COUNT];
};

static bool intel_histogram_get_data(struct intel_crtc *intel_crtc)
{
	struct intel_display *display = to_intel_display(intel_crtc);
	struct intel_histogram *histogram = intel_crtc->histogram;
	int index;
	u32 dpstbin;

	for (index = 0; index < ARRAY_SIZE(histogram->bin_data); index++) {
		dpstbin = intel_de_read(display, DPST_BIN(intel_crtc->pipe));
		if (!(dpstbin & DPST_BIN_BUSY)) {
			histogram->bin_data[index] = dpstbin & DPST_BIN_DATA_MASK;
		} else
			return false;
	}
	return true;
}

static void intel_histogram_handle_int_work(struct work_struct *work)
{
	struct intel_histogram *histogram = container_of(work,
		struct intel_histogram, work.work);
	struct intel_crtc *intel_crtc = histogram->crtc;
	struct intel_display *display = to_intel_display(intel_crtc);
	char event[] = "HISTOGRAM=1", pipe_id[21];
	char *histogram_event[] = { event, pipe_id, NULL };
	int retry;

	snprintf(pipe_id, sizeof(pipe_id),
		 "PIPE=%u", intel_crtc->base.base.id);

	/*
	 * TODO: PSR to be exited while reading the Histogram data
	 * Set DPST_CTL Bin Reg function select to TC
	 * Set DPST_CTL Bin Register Index to 0
	 */
	intel_de_rmw(display, DPST_CTL(intel_crtc->pipe),
		     DPST_CTL_BIN_REG_FUNC_SEL | DPST_CTL_BIN_REG_MASK, 0);
	for (retry = 0; retry < HISTOGRAM_BIN_READ_RETRY_COUNT; retry++) {
		if (intel_histogram_get_data(intel_crtc)) {
			drm_property_replace_global_blob(display->drm,
				&intel_crtc->config->histogram.histogram,
				sizeof(histogram->bin_data),
				histogram->bin_data, &intel_crtc->base.base,
				intel_crtc->histogram_property);
			/* Notify user for Histogram rediness */
			if (kobject_uevent_env(&display->drm->primary->kdev->kobj,
					       KOBJ_CHANGE, histogram_event))
				drm_err(display->drm,
					"sending HISTOGRAM event failed\n");
			break;
		}
	}
	if (retry >= HISTOGRAM_BIN_READ_RETRY_COUNT) {
		drm_err(display->drm, "Histogram bin read failed with max retry\n");
		return;
	}

	/* Enable histogram interrupt */
	intel_de_rmw(display, DPST_GUARD(intel_crtc->pipe), DPST_GUARD_HIST_INT_EN,
		     DPST_GUARD_HIST_INT_EN);

	/* Clear histogram interrupt by setting histogram interrupt status bit*/
	intel_de_rmw(display, DPST_GUARD(intel_crtc->pipe),
		     DPST_GUARD_HIST_EVENT_STATUS, 1);
}

void intel_histogram_irq_handler(struct intel_display *display, enum pipe pipe)
{
	struct intel_crtc *intel_crtc =
		to_intel_crtc(drm_crtc_from_index(display->drm, pipe));
	struct intel_histogram *histogram = intel_crtc->histogram;
	struct drm_i915_private *i915 = to_i915(intel_crtc->base.dev);

	if (!histogram->enable) {
		drm_err(display->drm,
			"spurious interrupt, histogram not enabled\n");
		return;
	}

	queue_delayed_work(i915->unordered_wq,
			   &histogram->work, 0);
}

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
		     DPST_GUARD_INTERRUPT_DELAY(0x04) |
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

	cancel_delayed_work(&histogram->work);
	histogram->enable = false;
	intel_crtc->config->histogram.histogram_enable = false;
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

	cancel_delayed_work_sync(&histogram->work);
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

	INIT_DEFERRABLE_WORK(&histogram->work,
			     intel_histogram_handle_int_work);

	return 0;
}
