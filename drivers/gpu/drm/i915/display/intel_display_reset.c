// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/dma-fence.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_print.h>

#include "intel_clock_gating.h"
#include "intel_cx0_phy.h"
#include "intel_display_core.h"
#include "intel_display_driver.h"
#include "intel_display_reset.h"
#include "intel_display_types.h"
#include "intel_display_utils.h"
#include "intel_hotplug.h"
#include "intel_pps.h"

static const char *intel_display_reset_fence_get_driver_name(struct dma_fence *fence)
{
	return "intel_display";
}

static const char *intel_display_reset_fence_get_timeline_name(struct dma_fence *fence)
{
	return "reset";
}

static const struct dma_fence_ops intel_display_reset_fence_ops = {
	.get_driver_name = intel_display_reset_fence_get_driver_name,
	.get_timeline_name = intel_display_reset_fence_get_timeline_name,
};

static void intel_display_reset_create(struct intel_display *display)
{
	struct dma_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return;

	dma_fence_init(fence, &intel_display_reset_fence_ops, NULL, 0, 0);

	display->reset.fence = fence;
}

struct dma_fence *intel_display_reset_fence_get(struct intel_display *display)
{
	struct dma_fence *fence;

	mutex_lock(&display->reset.mutex);

	if (!display->reset.fence)
		intel_display_reset_create(display);

	fence = display->reset.fence;
	if (fence)
		dma_fence_get(fence);

	mutex_unlock(&display->reset.mutex);

	return fence;
}

void intel_display_reset_fence_discard(struct intel_display *display)
{
	struct dma_fence *fence;

	mutex_lock(&display->reset.mutex);

	fence = display->reset.fence;
	if (fence)
		dma_fence_put(fence);

	display->reset.fence = NULL;

	mutex_unlock(&display->reset.mutex);
}

void intel_display_reset_fence_init(struct intel_display *display)
{
	mutex_init(&display->reset.mutex);
}

bool intel_display_reset_supported(struct intel_display *display)
{
	return HAS_DISPLAY(display);
}

bool intel_display_reset_test(struct intel_display *display)
{
	return HAS_DISPLAY(display) &&
		display->params.force_reset_modeset_test;
}

void intel_display_reset_prepare(struct intel_display *display)
{
	struct drm_modeset_acquire_ctx *ctx = &display->restore.reset_ctx;
	struct drm_atomic_state *state;
	struct dma_fence *reset_fence;
	int ret;

	reset_fence = intel_display_reset_fence_get(display);
	if (reset_fence) {
		dma_fence_signal(reset_fence);
		dma_fence_put(reset_fence);
	}

	/*
	 * Need mode_config.mutex so that we don't
	 * trample ongoing ->detect() and whatnot.
	 */
	mutex_lock(&display->drm->mode_config.mutex);
	drm_modeset_acquire_init(ctx, 0);
	while (1) {
		ret = drm_modeset_lock_all_ctx(display->drm, ctx);
		if (ret != -EDEADLK)
			break;

		drm_modeset_backoff(ctx);
	}
	/*
	 * Disabling the crtcs gracefully seems nicer. Also the
	 * g33 docs say we should at least disable all the planes.
	 */
	state = drm_atomic_helper_duplicate_state(display->drm, ctx);
	if (IS_ERR(state)) {
		ret = PTR_ERR(state);
		drm_err(display->drm, "Duplicating state failed with %i\n",
			ret);
		return;
	}

	ret = drm_atomic_helper_disable_all(display->drm, ctx);
	if (ret) {
		drm_err(display->drm, "Suspending crtc's failed with %i\n",
			ret);
		drm_atomic_state_put(state);
		return;
	}

	display->restore.modeset_state = state;
	state->acquire_ctx = ctx;
}

void intel_display_reset_finish(struct intel_display *display, bool test_only)
{
	struct drm_modeset_acquire_ctx *ctx = &display->restore.reset_ctx;
	struct drm_atomic_state *state;
	int ret;

	state = fetch_and_zero(&display->restore.modeset_state);
	if (!state)
		goto unlock;

	/* reset doesn't touch the display */
	if (test_only) {
		/* for testing only restore the display */
		ret = drm_atomic_helper_commit_duplicated_state(state, ctx);
		if (ret) {
			drm_WARN_ON(display->drm, ret == -EDEADLK);
			drm_err(display->drm,
				"Restoring old state failed with %i\n", ret);
		}
	} else {
		/*
		 * The display has been reset as well,
		 * so need a full re-initialization.
		 */
		intel_pps_unlock_regs_wa(display);
		intel_display_driver_init_hw(display);
		intel_clock_gating_init(display->drm);
		intel_cx0_pll_power_save_wa(display);
		intel_hpd_init(display);

		ret = __intel_display_driver_resume(display, state, ctx);
		if (ret)
			drm_err(display->drm,
				"Restoring old state failed with %i\n", ret);

		intel_hpd_poll_disable(display);
	}

	drm_atomic_state_put(state);
unlock:
	intel_display_reset_fence_discard(display);

	drm_modeset_drop_locks(ctx);
	drm_modeset_acquire_fini(ctx);
	mutex_unlock(&display->drm->mode_config.mutex);
}
