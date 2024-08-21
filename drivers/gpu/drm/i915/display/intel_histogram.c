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
	struct intel_crtc *crtc;
	struct delayed_work handle_histogram_int_work;
	bool enable;
	bool can_enable;
	u32 bindata[HISTOGRAM_BIN_COUNT];
};

static void intel_histogram_handle_int_work(struct work_struct *work)
{
	struct intel_histogram *histogram = container_of(work,
		struct intel_histogram, handle_histogram_int_work.work);
	struct drm_i915_private *i915 = histogram->i915;
	struct intel_crtc *intel_crtc = histogram->crtc;
	char *histogram_event[] = {"HISTOGRAM=1", NULL};
	u32 dpstbin;
	int i, try = 0;

	/* Wa: 14014889975 */
	if (IS_DISPLAY_VER(i915, 12, 13))
		intel_de_rmw(i915, DPST_CTL(intel_crtc->pipe),
			     DPST_CTL_RESTORE, 0);

	/*
	 * TODO: PSR to be exited while reading the Histogram data
	 * Set DPST_CTL Bin Reg function select to TC
	 * Set DPST_CTL Bin Register Index to 0
	 */
retry:
	intel_de_rmw(i915, DPST_CTL(intel_crtc->pipe),
		     DPST_CTL_BIN_REG_FUNC_SEL | DPST_CTL_BIN_REG_MASK, 0);
	for (i = 0; i < HISTOGRAM_BIN_COUNT; i++) {
		dpstbin = intel_de_read(i915, DPST_BIN(intel_crtc->pipe));
		if (dpstbin & DPST_BIN_BUSY) {
			/*
			 * If DPST_BIN busy bit is set, then set the
			 * DPST_CTL bin reg index to 0 and proceed
			 * from beginning.
			 */
			if (try++ >= 5) {
				drm_err(&i915->drm,
					"Histogram block is busy, failed to read\n");
				intel_de_rmw(i915, DPST_GUARD(intel_crtc->pipe),
					     DPST_GUARD_HIST_EVENT_STATUS, 1);
				return;
			}
			goto retry;
		}
		histogram->bindata[i] = dpstbin & DPST_BIN_DATA_MASK;
		drm_dbg_atomic(&i915->drm, "Histogram[%d]=%x\n",
			       i, histogram->bindata[i]);
	}

	drm_property_replace_global_blob(&i915->drm,
			&intel_crtc->config->histogram,
			sizeof(histogram->bindata),
			histogram->bindata, &intel_crtc->base.base,
			intel_crtc->histogram_property);

	/* Notify user for Histogram rediness */
	if (kobject_uevent_env(&i915->drm.primary->kdev->kobj, KOBJ_CHANGE,
			       histogram_event))
		drm_err(&i915->drm, "sending HISTOGRAM event failed\n");

	/* Wa: 14014889975 */
	if (IS_DISPLAY_VER(i915, 12, 13))
		/* Write the value read from DPST_CTL to DPST_CTL.Interrupt Delay Counter(bit 23:16) */
		intel_de_write(i915, DPST_CTL(intel_crtc->pipe), intel_de_read(i915,
			       DPST_CTL(intel_crtc->pipe)) | DPST_CTL_RESTORE);

	/* Enable histogram interrupt */
	intel_de_rmw(i915, DPST_GUARD(intel_crtc->pipe), DPST_GUARD_HIST_INT_EN,
		     DPST_GUARD_HIST_INT_EN);

	/* Clear histogram interrupt by setting histogram interrupt status bit*/
	intel_de_rmw(i915, DPST_GUARD(intel_crtc->pipe),
		     DPST_GUARD_HIST_EVENT_STATUS, 1);
}

void intel_histogram_irq_handler(struct drm_i915_private *i915, enum pipe pipe)
{
	struct intel_crtc *intel_crtc =
		to_intel_crtc(drm_crtc_from_index(&i915->drm, pipe));
	struct intel_histogram *histogram = intel_crtc->histogram;

	if (!histogram->enable) {
		drm_err(&i915->drm,
			"spurious interrupt, histogram not enabled\n");
		return;
	}

	queue_delayed_work(i915->unordered_wq,
			   &histogram->handle_histogram_int_work, 0);
}

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

	/* Wa: 14014889975 */
	if (IS_DISPLAY_VER(i915, 12, 13))
		/* Write the value read from DPST_CTL to DPST_CTL.Interrupt Delay Counter(bit 23:16) */
		intel_de_write(i915, DPST_CTL(intel_crtc->pipe), intel_de_read(i915,
			       DPST_CTL(intel_crtc->pipe)) | DPST_CTL_RESTORE);

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

	cancel_delayed_work(&histogram->handle_histogram_int_work);
	histogram->enable = false;
	intel_crtc->config->histogram_en = false;
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

	cancel_delayed_work_sync(&histogram->handle_histogram_int_work);
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
	histogram->crtc = intel_crtc;
	histogram->can_enable = false;

	INIT_DEFERRABLE_WORK(&histogram->handle_histogram_int_work,
			     intel_histogram_handle_int_work);

	histogram->i915 = i915;

	return 0;
}
