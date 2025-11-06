// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/dma-fence.h>

#include <drm/drm_crtc.h>
#include <drm/drm_vblank.h>
#include <drm/intel/display_parent_interface.h>

#include "i915_reg.h"
#include "intel_display_core.h"
#include "intel_display_irq.h"
#include "intel_display_rps.h"
#include "intel_display_types.h"

struct wait_rps_boost {
	struct wait_queue_entry wait;

	struct drm_crtc *crtc;
	struct dma_fence *fence;
};

static int do_rps_boost(struct wait_queue_entry *_wait,
			unsigned mode, int sync, void *key)
{
	struct wait_rps_boost *wait = container_of(_wait, typeof(*wait), wait);
	struct intel_display *display = to_intel_display(wait->crtc->dev);

	display->parent->rps->boost(wait->fence);

	dma_fence_put(wait->fence);

	drm_crtc_vblank_put(wait->crtc);

	list_del(&wait->wait.entry);
	kfree(wait);
	return 1;
}

void intel_display_rps_boost_after_vblank(struct drm_crtc *crtc,
					  struct dma_fence *fence)
{
	struct intel_display *display = to_intel_display(crtc->dev);
	struct wait_rps_boost *wait;

	if (!display->parent->rps)
		return;

	if (DISPLAY_VER(display) < 6)
		return;

	if (drm_crtc_vblank_get(crtc))
		return;

	wait = kmalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait) {
		drm_crtc_vblank_put(crtc);
		return;
	}

	wait->fence = dma_fence_get(fence);
	wait->crtc = crtc;

	wait->wait.func = do_rps_boost;
	wait->wait.flags = 0;

	add_wait_queue(drm_crtc_vblank_waitqueue(crtc), &wait->wait);
}

void intel_display_rps_mark_interactive(struct intel_display *display,
					struct intel_atomic_state *state,
					bool interactive)
{
	if (!display->parent->rps)
		return;

	if (state->rps_interactive == interactive)
		return;

	display->parent->rps->mark_interactive(display->drm, interactive);

	state->rps_interactive = interactive;
}

void ilk_display_rps_enable(struct intel_display *display)
{
	spin_lock(&display->irq.lock);
	ilk_enable_display_irq(display, DE_PCU_EVENT);
	spin_unlock(&display->irq.lock);
}

void ilk_display_rps_disable(struct intel_display *display)
{
	spin_lock(&display->irq.lock);
	ilk_disable_display_irq(display, DE_PCU_EVENT);
	spin_unlock(&display->irq.lock);
}

void ilk_display_rps_irq_handler(struct intel_display *display)
{
	/* We expect these to be non-NULL when running on ILK */
	display->parent->rps->ilk_irq_handler(display->drm);
}
