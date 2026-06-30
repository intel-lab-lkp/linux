// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019-2022 Bootlin
 * Author: Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 */

#include <linux/types.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include "logicvc_crtc.h"
#include "logicvc_drm.h"
#include "logicvc_interface.h"
#include "logicvc_regs.h"

#define logicvc_interface_from_drm_encoder(c) \
	container_of(c, struct logicvc_interface, drm_encoder)
#define logicvc_interface_from_drm_connector(c) \
	container_of(c, struct logicvc_interface, drm_connector)

static void logicvc_encoder_enable(struct drm_encoder *drm_encoder)
{
	struct logicvc_drm *logicvc = logicvc_drm(drm_encoder->dev);
	struct logicvc_interface *interface =
		logicvc_interface_from_drm_encoder(drm_encoder);
	int idx;

	if (!drm_dev_enter(drm_encoder->dev, &idx))
		return;

	regmap_update_bits(logicvc->regmap, LOGICVC_POWER_CTRL_REG,
			   LOGICVC_POWER_CTRL_VIDEO_ENABLE,
			   LOGICVC_POWER_CTRL_VIDEO_ENABLE);

	if (interface->drm_panel) {
		drm_panel_prepare(interface->drm_panel);
		drm_panel_enable(interface->drm_panel);
	}

	drm_dev_exit(idx);
}

static void logicvc_encoder_disable(struct drm_encoder *drm_encoder)
{
	struct logicvc_interface *interface =
		logicvc_interface_from_drm_encoder(drm_encoder);
	int idx;

	if (!drm_dev_enter(drm_encoder->dev, &idx))
		return;

	if (interface->drm_panel) {
		drm_panel_disable(interface->drm_panel);
		drm_panel_unprepare(interface->drm_panel);
	}

	drm_dev_exit(idx);
}

static const struct drm_encoder_helper_funcs logicvc_encoder_helper_funcs = {
	.enable			= logicvc_encoder_enable,
	.disable		= logicvc_encoder_disable,
};

static int logicvc_connector_get_modes(struct drm_connector *drm_connector)
{
	struct logicvc_interface *interface =
		logicvc_interface_from_drm_connector(drm_connector);

	if (interface->drm_panel)
		return drm_panel_get_modes(interface->drm_panel, drm_connector);

	WARN_ONCE(1, "Retrieving modes from a native connector is not implemented.");

	return 0;
}

static const struct drm_connector_helper_funcs logicvc_connector_helper_funcs = {
	.get_modes		= logicvc_connector_get_modes,
};

static const struct drm_connector_funcs logicvc_connector_funcs = {
	.reset			= drm_atomic_helper_connector_reset,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static int logicvc_interface_encoder_type(struct logicvc_drm *logicvc)
{
	switch (logicvc->config.display_interface) {
	case LOGICVC_DISPLAY_INTERFACE_LVDS_4BITS:
	case LOGICVC_DISPLAY_INTERFACE_LVDS_4BITS_CAMERA:
	case LOGICVC_DISPLAY_INTERFACE_LVDS_3BITS:
		return DRM_MODE_ENCODER_LVDS;
	case LOGICVC_DISPLAY_INTERFACE_DVI:
		return DRM_MODE_ENCODER_TMDS;
	case LOGICVC_DISPLAY_INTERFACE_RGB:
		return DRM_MODE_ENCODER_DPI;
	default:
		return DRM_MODE_ENCODER_NONE;
	}
}

static int logicvc_interface_connector_type(struct logicvc_drm *logicvc)
{
	switch (logicvc->config.display_interface) {
	case LOGICVC_DISPLAY_INTERFACE_LVDS_4BITS:
	case LOGICVC_DISPLAY_INTERFACE_LVDS_4BITS_CAMERA:
	case LOGICVC_DISPLAY_INTERFACE_LVDS_3BITS:
		return DRM_MODE_CONNECTOR_LVDS;
	case LOGICVC_DISPLAY_INTERFACE_DVI:
		return DRM_MODE_CONNECTOR_DVID;
	case LOGICVC_DISPLAY_INTERFACE_RGB:
		return DRM_MODE_CONNECTOR_DPI;
	default:
		return DRM_MODE_CONNECTOR_Unknown;
	}
}

static bool logicvc_interface_native_connector(struct logicvc_drm *logicvc)
{
	switch (logicvc->config.display_interface) {
	case LOGICVC_DISPLAY_INTERFACE_DVI:
		return true;
	default:
		return false;
	}
}

void logicvc_interface_attach_crtc(struct logicvc_drm *logicvc)
{
	uint32_t possible_crtcs = drm_crtc_mask(&logicvc->crtc->drm_crtc);

	logicvc->interface->drm_encoder.possible_crtcs = possible_crtcs;
}

int logicvc_interface_init(struct logicvc_drm *logicvc)
{
	struct logicvc_interface *interface;
	struct drm_device *drm_dev = &logicvc->drm_dev;
	struct device *dev = drm_dev->dev;
	struct device_node *of_node = dev->of_node;
	int encoder_type = logicvc_interface_encoder_type(logicvc);
	int connector_type = logicvc_interface_connector_type(logicvc);
	bool native_connector = logicvc_interface_native_connector(logicvc);
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	int ret;

	ret = drm_of_find_panel_or_bridge(of_node, 0, 0, &panel,
					  &bridge);
	if (ret == -EPROBE_DEFER)
		return ret;

	interface = drmm_encoder_alloc(drm_dev, struct logicvc_interface, drm_encoder,
				       NULL, encoder_type, NULL);
	if (IS_ERR(interface)) {
		drm_err(drm_dev, "Failed to initialize encoder\n");
		return PTR_ERR(interface);
	}

	interface->drm_panel = panel;
	interface->drm_bridge = bridge;

	drm_encoder_helper_add(&interface->drm_encoder,
			       &logicvc_encoder_helper_funcs);

	if (native_connector || interface->drm_panel) {
		ret = drmm_connector_init(drm_dev, &interface->drm_connector,
					  &logicvc_connector_funcs,
					  connector_type, NULL);
		if (ret) {
			drm_err(drm_dev, "Failed to initialize connector\n");
			return ret;
		}

		drm_connector_helper_add(&interface->drm_connector,
					 &logicvc_connector_helper_funcs);

		ret = drm_connector_attach_encoder(&interface->drm_connector,
						   &interface->drm_encoder);
		if (ret) {
			drm_err(drm_dev,
				"Failed to attach connector to encoder\n");
			return ret;
		}
	}

	if (interface->drm_bridge) {
		ret = drm_bridge_attach(&interface->drm_encoder,
					interface->drm_bridge, NULL, 0);
		if (ret) {
			drm_err(drm_dev,
				"Failed to attach bridge to encoder\n");
			return ret;
		}
	}

	logicvc->interface = interface;

	return 0;
}
