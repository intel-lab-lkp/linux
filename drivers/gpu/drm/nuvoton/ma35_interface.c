// SPDX-License-Identifier: GPL-2.0+
/*
 * Nuvoton DRM driver
 *
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <a0987203069@gmail.com>
 */

#include <linux/types.h>
#include <linux/clk.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "ma35_drm.h"

#define ma35_encoder(e) \
	container_of(e, struct ma35_interface, drm_encoder)
#define ma35_connector(c) \
	container_of(c, struct ma35_interface, drm_connector)

static void ma35_encoder_mode_set(struct drm_encoder *encoder,
	struct drm_crtc_state *crtc_state,
	struct drm_connector_state *conn_state)
{
	struct drm_device *drm_dev = encoder->dev;
	struct ma35_drm *priv = ma35_drm(drm_dev);
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;
	int result;

	clk_set_rate(priv->dcupclk, adjusted_mode->clock * 1000);
	result = DIV_ROUND_UP(clk_get_rate(priv->dcupclk), 1000);
	drm_dbg(drm_dev, "Pixel clock: %d kHz; request : %d kHz\n", result, adjusted_mode->clock);
}

static int ma35_encoder_atomic_check(struct drm_encoder *encoder,
					struct drm_crtc_state *crtc_state,
					struct drm_connector_state *conn_state)
{
	struct ma35_interface *interface = ma35_encoder(encoder);
	struct drm_display_info *display_info = &conn_state->connector->display_info;

	interface->bus_flags = display_info->bus_flags;

	return 0;
}

static const struct drm_encoder_helper_funcs ma35_encoder_helper_funcs = {
	.atomic_mode_set	= ma35_encoder_mode_set,
	.atomic_check		= ma35_encoder_atomic_check,
};

static const struct drm_connector_funcs ma35_connector_funcs = {
	.reset			= drm_atomic_helper_connector_reset,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static int ma35_connector_get_modes(struct drm_connector *drm_connector)
{
	struct ma35_drm *priv = ma35_drm(drm_connector->dev);
	struct drm_device *drm_dev = &priv->drm_dev;
	struct drm_mode_config *mode_config = &drm_dev->mode_config;
	struct ma35_interface *interface = ma35_connector(drm_connector);
	int count;

	if (!interface->drm_panel) {
		/* Use the default modes */
		count = drm_add_modes_noedid(drm_connector,
				mode_config->max_width, mode_config->max_height);
		drm_set_preferred_mode(drm_connector,
				mode_config->max_width, mode_config->max_height);

		return count;
	} else {
		return drm_panel_get_modes(interface->drm_panel, drm_connector);
	}
}

static const struct drm_connector_helper_funcs ma35_connector_helper_funcs = {
	.get_modes		= ma35_connector_get_modes,
};

static void ma35_encoder_attach_crtc(struct ma35_drm *priv)
{
	uint32_t possible_crtcs = drm_crtc_mask(&priv->crtc->drm_crtc);

	priv->interface->drm_encoder.possible_crtcs = possible_crtcs;
}

static int ma35_bridge_try_attach(struct ma35_drm *priv, struct ma35_interface *interface)
{
	struct drm_device *drm_dev = &priv->drm_dev;
	struct device *dev = drm_dev->dev;
	struct device_node *of_node = dev->of_node;
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	int ret;

	ret = drm_of_find_panel_or_bridge(of_node, 0, 0, &panel, &bridge);

	if (ret) {
		drm_info(drm_dev, "No panel or bridge found\n");
		return ret;
	}

	if (panel) {
		bridge = drm_panel_bridge_add_typed(panel, DRM_MODE_CONNECTOR_DPI);
		if (IS_ERR(bridge))
			return PTR_ERR(bridge);
	}

	interface->drm_panel = panel;
	interface->drm_bridge = bridge;

	ret = drm_bridge_attach(&interface->drm_encoder, bridge,
				NULL, 0);
	if (ret) {
		drm_err(drm_dev, "Failed to attach bridge to encoder\n");
		return ret;
	}

	return 0;
}

int ma35_interface_init(struct ma35_drm *priv)
{
	struct ma35_interface *interface;
	struct drm_device *drm_dev = &priv->drm_dev;
	struct drm_encoder *drm_encoder;
	int ret = 0;

	/* encoder */
	interface = drmm_simple_encoder_alloc(drm_dev,
					struct ma35_interface, drm_encoder, DRM_MODE_ENCODER_DPI);
	if (!interface) {
		drm_err(drm_dev, "Failed to initialize encoder\n");
		goto error_early;
	}
	priv->interface = interface;
	drm_encoder = &interface->drm_encoder;
	drm_encoder_helper_add(drm_encoder,
			       &ma35_encoder_helper_funcs);

	/* attach encoder to crtc */
	ma35_encoder_attach_crtc(priv);

	/* attach bridge to encoder if found one in device tree */
	ret = ma35_bridge_try_attach(priv, interface);
	if (!ret)
		return 0;

	/* fallback to raw dpi connector */
	ret = drmm_connector_init(drm_dev, &interface->drm_connector,
					&ma35_connector_funcs,
					DRM_MODE_CONNECTOR_DPI,
					NULL);
	if (ret) {
		drm_err(drm_dev, "Failed to initialize connector\n");
		goto error_encoder;
	}
	drm_connector_helper_add(&interface->drm_connector,
						&ma35_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&interface->drm_connector,
						drm_encoder);
	if (ret) {
		drm_err(drm_dev,
			"Failed to attach connector to encoder\n");
		goto error_encoder;
	}

	return ret;

error_encoder:
	drm_encoder_cleanup(drm_encoder);

error_early:
	return ret;
}
