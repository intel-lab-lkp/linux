// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023 Google Inc.
 */

#include <linux/dev_printk.h>

#include <drm/drm_panel.h>
#include <drm/drm_panel_helper.h>

/**
 * drm_panel_helper_shutdown - helper for panels to use at shutdown time
 * @panel: DRM panel
 *
 * Panels may call this function unconditionally at shutdown time to ensure
 * that they are disabled and unprepared if necessary.
 *
 * As part of this function:
 * - The backlight will be turned off, if it was on.
 * - Any panel followers will be power sequenced.
 */
void drm_panel_helper_shutdown(struct drm_panel *panel)
{
	int ret;

	if (panel->enabled) {
		ret = drm_panel_disable(panel);
		if (ret)
			dev_warn(panel->dev, "Error disabling panel %d\n", ret);
	}
	if (panel->prepared) {
		ret = drm_panel_unprepare(panel);
		if (ret)
			dev_warn(panel->dev, "Error unpreparing panel %d\n", ret);
	}
}
EXPORT_SYMBOL_GPL(drm_panel_helper_shutdown);
