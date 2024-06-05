/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2024 Google Inc.
 *
 * This header is a temporary solution and is intended to be included
 * directly by panel-edp.c and panel-simple.c.
 *
 * This header is needed because panel-edp and panel-simple are used by a
 * wide variety of DRM drivers and it's hard to know for sure if all of the
 * DRM drivers used by those panel drivers are properly calling
 * drm_atomic_helper_shutdown() or drm_helper_force_disable_all() at
 * shutdown/remove time.
 *
 * The plan for this header file:
 * - Land it and hope that the warning print will encourage DRM drivers to
 *   get fixed.
 * - Eventually move to a WARN() splat for extra encouragement.
 * - Assume that everyone has been fixed and remove this header file.
 */

#ifndef __PANEL_DRM_SHUTDOWN_CHECK_H__
#define __PANEL_DRM_SHUTDOWN_CHECK_H__

#include <drm/drm_bridge.h>
#include <drm/drm_drv.h>

/*
 * This is a list of all DRM drivers that appear to properly call
 * drm_atomic_helper_shutdown() or drm_helper_force_disable_all() at
 * shutdown and remove time.
 *
 * We can't detect this dynamically and are stuck with a list because the panel
 * driver's shutdown() call might be called _before_ the DRM driver's
 * shutdown() call.
 *
 * NOTE: no verification has been done to confirm that the below drivers
 * are actually _used_ with panel-simple or panel-edp, only that these drivers
 * appear to be shutting down properly. It doesn't hurt to have extra drivers
 * listed here as long as the list doesn't contain any drivers that are
 * missing the shutdown calls.
 */
static const char * const drm_drivers_that_shutdown[] = {
	"armada-drm",
	"aspeed-gfx-drm",
	"ast",
	"atmel-hlcdc",
	"bochs-drm",
	"cirrus",
	"exynos",
	"fsl-dcu-drm",
	"gm12u320",
	"gud",
	"hdlcd",
	"hibmc",
	"hx8357d",
	"hyperv_drm",
	"ili9163",
	"ili9225",
	"ili9341",
	"ili9486",
	"imx-dcss",
	"imx-drm",
	"imx-lcdc",
	"imx-lcdif",
	"ingenic-drm",
	"kirin",
	"komeda",
	"logicvc-drm",
	"loongson",
	"mali-dp",
	"mcde",
	"meson",
	"mgag200",
	"mi0283qt",
	"msm",
	"mxsfb-drm",
	"omapdrm",
	"panel-mipi-dbi",
	"pl111",
	"qxl",
	"rcar-du",
	"repaper",
	"rockchip",
	"rzg2l-du",
	"ssd130x",
	"st7586",
	"st7735r",
	"sti",
	"stm",
	"sun4i-drm",
	"tidss",
	"tilcdc",
	"tve200",
	"vboxvideo",
	"zynqmp-dpsub",
	""
};

static void panel_shutdown_if_drm_driver_needs_fixing(struct drm_panel *panel)
{
	struct drm_bridge *bridge;
	const struct drm_driver *driver;
	const char * const *driver_name;

	/*
	 * Look for a bridge that shares the DT node of this panel. That only
	 * works if we've been linked up with a panel_bridge.
	 */
	bridge = of_drm_find_bridge(panel->dev->of_node);
	if (bridge && bridge->dev && bridge->dev->driver) {
		/*
		 * If the DRM driver for the bridge is known to be fine then
		 * we're done.
		 */
		driver = bridge->dev->driver;
		for (driver_name = drm_drivers_that_shutdown; *driver_name; driver_name++) {
			if (strcmp(*driver_name, driver->name) == 0)
				return;
		}

		/*
		 * If you see the message below then:
		 * 1. Make sure your DRM driver is properly calling
		 *    drm_atomic_helper_shutdown() or drm_helper_force_disable_all()
		 *    at shutdown time.
		 * 2. Add your driver to the list.
		 */
		dev_warn(panel->dev,
			 "DRM driver appears buggy; manually disable/unprepare\n");
	} else {
		/*
		 * If you see the message below then your setup needs to
		 * be moved to using a panel_bridge. This often happens
		 * by calling devm_drm_of_get_bridge(). Having a panel without
		 * an associated panel_bridge is deprecated.
		 */
		dev_warn(panel->dev,
			 "Can't't find DRM driver; manually disable/unprepare\n");
	}

	/*
	 * If we don't know if a DRM driver is properly shutting things down
	 * then we'll manually call the disable/unprepare. This is always a
	 * safe thing to do (in that it won't cause you to crash), but it
	 * does generate a warning.
	 */
	drm_panel_disable(panel);
	drm_panel_unprepare(panel);
}

#endif
