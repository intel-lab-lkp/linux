// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/io.h>

#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>

#include "hibmc_drm_drv.h"
#include "dp/dp_kapi.h"

static int hibmc_dp_connector_get_modes(struct drm_connector *connector)
{
	int count;

	count = drm_add_modes_noedid(connector, connector->dev->mode_config.max_width,
				     connector->dev->mode_config.max_height);
	drm_set_preferred_mode(connector, 800, 600); /* default 800x600 */

	return count;
}

static const struct drm_connector_helper_funcs hibmc_dp_conn_helper_funcs = {
	.get_modes = hibmc_dp_connector_get_modes,
};

static const struct drm_connector_funcs hibmc_dp_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void dp_mode_cfg(struct drm_device *dev, struct dp_mode *dp_mode,
			struct drm_display_mode *mode)
{
	dp_mode->field_rate = drm_mode_vrefresh(mode);
	dp_mode->pixel_clock = mode->clock / 1000; /* 1000: khz to hz */

	dp_mode->h_total = mode->htotal;
	dp_mode->h_active = mode->hdisplay;
	dp_mode->h_blank = mode->htotal - mode->hdisplay;
	dp_mode->h_front = mode->hsync_start - mode->hdisplay;
	dp_mode->h_sync = mode->hsync_end - mode->hsync_start;
	dp_mode->h_back = mode->htotal - mode->hsync_end;

	dp_mode->v_total = mode->vtotal;
	dp_mode->v_active = mode->vdisplay;
	dp_mode->v_blank = mode->vtotal - mode->vdisplay;
	dp_mode->v_front = mode->vsync_start - mode->vdisplay;
	dp_mode->v_sync = mode->vsync_end - mode->vsync_start;
	dp_mode->v_back = mode->vtotal - mode->vsync_end;

	if (mode->flags & DRM_MODE_FLAG_PHSYNC) {
		drm_info(dev, "horizontal sync polarity: positive\n");
		dp_mode->h_pol = 1;
	} else if (mode->flags & DRM_MODE_FLAG_NHSYNC) {
		drm_info(dev, "horizontal sync polarity: negative\n");
		dp_mode->h_pol = 0;
	} else {
		drm_err(dev, "horizontal sync polarity: unknown or not set\n");
	}

	if (mode->flags & DRM_MODE_FLAG_PVSYNC) {
		drm_info(dev, "vertical sync polarity: positive\n");
		dp_mode->v_pol = 1;
	} else if (mode->flags & DRM_MODE_FLAG_NVSYNC) {
		drm_info(dev, "vertical sync polarity: negative\n");
		dp_mode->v_pol = 0;
	} else {
		drm_err(dev, "vertical sync polarity: unknown or not set\n");
	}
}

static int dp_prepare(struct hibmc_dp *dp, struct drm_display_mode *mode)
{
	struct dp_mode dp_mode = {0};
	int ret;

	hibmc_dp_display_en(dp, false);

	dp_mode_cfg(dp->drm_dev, &dp_mode, mode);
	ret = hibmc_dp_mode_set(dp, &dp_mode);
	if (ret)
		drm_err(dp->drm_dev, "hibmc dp mode set failed: %d\n", ret);

	return ret;
}

static void dp_enable(struct hibmc_dp *dp)
{
	hibmc_dp_display_en(dp, true);
}

static void dp_disable(struct hibmc_dp *dp)
{
	hibmc_dp_display_en(dp, false);
}

static int hibmc_dp_hw_init(struct hibmc_drm_private *priv)
{
	int ret;

	ret = hibmc_dp_kapi_init(&priv->dp);
	if (ret)
		return ret;

	hibmc_dp_display_en(&priv->dp, false);

	return 0;
}

static void hibmc_dp_hw_uninit(struct hibmc_drm_private *priv)
{
	hibmc_dp_kapi_uninit(&priv->dp);
}

static void hibmc_dp_encoder_enable(struct drm_encoder *drm_encoder,
				    struct drm_atomic_state *state)
{
	struct hibmc_dp *dp = container_of(drm_encoder, struct hibmc_dp, encoder);
	struct drm_display_mode *mode = &drm_encoder->crtc->state->mode;

	if (dp_prepare(dp, mode))
		return;

	dp_enable(dp);
}

static void hibmc_dp_encoder_disable(struct drm_encoder *drm_encoder,
				     struct drm_atomic_state *state)
{
	struct hibmc_dp *dp = container_of(drm_encoder, struct hibmc_dp, encoder);

	dp_disable(dp);
}

static const struct drm_encoder_helper_funcs hibmc_dp_encoder_helper_funcs = {
	.atomic_enable = hibmc_dp_encoder_enable,
	.atomic_disable = hibmc_dp_encoder_disable,
};

void hibmc_dp_uninit(struct hibmc_drm_private *priv)
{
	hibmc_dp_hw_uninit(priv);
}

int hibmc_dp_init(struct hibmc_drm_private *priv)
{
	struct drm_device *dev = &priv->dev;
	struct drm_crtc *crtc = &priv->crtc;
	struct hibmc_dp *dp = &priv->dp;
	struct drm_connector *connector = &dp->connector;
	struct drm_encoder *encoder = &dp->encoder;
	int ret;

	dp->mmio = priv->mmio;
	dp->drm_dev = dev;

	ret = hibmc_dp_hw_init(priv);
	if (ret) {
		drm_err(dev, "dp hw init failed: %d\n", ret);
		return ret;
	}

	encoder->possible_crtcs = drm_crtc_mask(crtc);
	ret = drm_simple_encoder_init(dev, encoder, DRM_MODE_ENCODER_TMDS);
	if (ret) {
		drm_err(dev, "init dp encoder failed: %d\n", ret);
		goto err_init;
	}

	drm_encoder_helper_add(encoder, &hibmc_dp_encoder_helper_funcs);

	ret = drm_connector_init(dev, connector, &hibmc_dp_conn_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret) {
		drm_err(dev, "init dp connector failed: %d\n", ret);
		goto err_init;
	}

	drm_connector_helper_add(connector, &hibmc_dp_conn_helper_funcs);

	drm_connector_attach_encoder(connector, encoder);

	return 0;

err_init:
	hibmc_dp_hw_uninit(priv);

	return ret;
}
