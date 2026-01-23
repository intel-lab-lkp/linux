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

	dev_err(ddev->dev, "No connector found for %s encoder (id %d)\n",
		encoder->name, encoder->base.id);

	return NULL;
}

static
int tilcdc_attach_bridge(struct drm_device *ddev, struct drm_bridge *bridge)
{
	struct tilcdc_drm_private *priv = ddev->dev_private;
	int ret;

	priv->encoder->possible_crtcs = BIT(0);

	ret = drm_bridge_attach(priv->encoder, bridge, NULL, 0);
	if (ret)
		return ret;

	priv->connector = tilcdc_encoder_find_connector(ddev, priv->encoder);
	if (!priv->connector)
		return -ENODEV;

	return 0;
}

int tilcdc_encoder_create(struct drm_device *ddev)
{
	struct tilcdc_drm_private *priv = ddev->dev_private;
	struct drm_bridge *bridge;
	struct drm_panel *panel;
	int ret;

	ret = drm_of_find_panel_or_bridge(ddev->dev->of_node, 0, 0,
					  &panel, &bridge);
	if (ret == -ENODEV)
		return 0;
	else if (ret)
		return ret;

	priv->encoder = devm_kzalloc(ddev->dev, sizeof(*priv->encoder), GFP_KERNEL);
	if (!priv->encoder)
		return -ENOMEM;

	ret = drm_simple_encoder_init(ddev, priv->encoder,
				      DRM_MODE_ENCODER_NONE);
	if (ret) {
		dev_err(ddev->dev, "drm_encoder_init() failed %d\n", ret);
		return ret;
	}

	if (panel) {
		bridge = devm_drm_panel_bridge_add_typed(ddev->dev, panel,
							 DRM_MODE_CONNECTOR_DPI);
		if (IS_ERR(bridge)) {
			ret = PTR_ERR(bridge);
			goto err_encoder_cleanup;
		}
	}

	ret = tilcdc_attach_bridge(ddev, bridge);
	if (ret)
		goto err_encoder_cleanup;

	return 0;

err_encoder_cleanup:
	drm_encoder_cleanup(priv->encoder);
	return ret;
}
