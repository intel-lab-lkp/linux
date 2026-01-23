// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Texas Instruments
 * Author: Jyri Sarha <jsarha@ti.com>
 */

#include <linux/of_graph.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_of.h>
#include <drm/drm_simple_kms_helper.h>

#include "tilcdc_drv.h"
#include "tilcdc_encoder.h"

static
struct drm_connector *tilcdc_encoder_find_connector(struct drm_device *ddev,
						    struct drm_encoder *encoder)
{
	struct drm_connector *connector;

	list_for_each_entry(connector, &ddev->mode_config.connector_list, head) {
		if (drm_connector_has_possible_encoder(connector, encoder))
			return connector;
	}

	drm_err(ddev, "No connector found for %s encoder (id %d)\n",
		encoder->name, encoder->base.id);

	return NULL;
}

static
int tilcdc_attach_bridge(struct drm_device *ddev, struct drm_bridge *bridge)
{
	struct tilcdc_drm_private *priv = ddev_to_tilcdc_priv(ddev);
	int ret;

	priv->encoder->base.possible_crtcs = BIT(0);

	ret = drm_bridge_attach(&priv->encoder->base, bridge, NULL, 0);
	if (ret)
		return ret;

	priv->connector = tilcdc_encoder_find_connector(ddev, &priv->encoder->base);
	if (!priv->connector)
		return -ENODEV;

	return 0;
}

int tilcdc_encoder_create(struct drm_device *ddev)
{
	struct tilcdc_drm_private *priv = ddev_to_tilcdc_priv(ddev);
	struct tilcdc_encoder *encoder;
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	int ret;

	ret = drm_of_find_panel_or_bridge(ddev->dev->of_node, 0, 0,
					  &panel, &bridge);
	if (ret == -ENODEV)
		return 0;
	else if (ret)
		return ret;

	encoder = drmm_simple_encoder_alloc(ddev, struct tilcdc_encoder,
					    base, DRM_MODE_ENCODER_NONE);
	if (IS_ERR(encoder)) {
		drm_err(ddev, "drm_encoder_init() failed %pe\n", encoder);
		return PTR_ERR(encoder);
	}
	priv->encoder = encoder;

	if (panel) {
		bridge = devm_drm_panel_bridge_add_typed(ddev->dev, panel,
							 DRM_MODE_CONNECTOR_DPI);
		if (IS_ERR(bridge))
			return PTR_ERR(bridge);
	}

	return tilcdc_attach_bridge(ddev, bridge);
}
