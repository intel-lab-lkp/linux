// SPDX-License-Identifier: GPL-2.0

#include <linux/io.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>

#include "yhgch_drm_drv.h"
#include "yhgch_drm_regs.h"

static int yhgch_connector_get_modes(struct drm_connector *connector)
{
	int count;
	const struct drm_edid *drm_edid;

	drm_edid = drm_edid_read(connector);
	if (drm_edid) {
		drm_edid_connector_update(connector, drm_edid);
		count =  drm_edid_connector_add_modes(connector);
	} else {
		drm_edid_connector_update(connector, NULL);
		count = drm_add_modes_noedid(connector,
					     connector->dev->mode_config.max_width,
					     connector->dev->mode_config.max_height);
		drm_set_preferred_mode(connector, 1024, 768);
	}
	drm_edid_free(drm_edid);
	return count;
}

static int yhgch_connector_helper_detect_from_ddc(struct drm_connector *connector,
						  struct drm_modeset_acquire_ctx *ctx,
						  bool force)
{
	if (drm_connector_helper_detect_from_ddc(connector, ctx, force)
			!= connector_status_connected) {
		drm_dbg_kms(connector->dev, "ddc detect failed, force connect\n");
	}
	return connector_status_connected;
}

static const struct drm_connector_helper_funcs
	yhgch_connector_helper_funcs = {
	.get_modes = yhgch_connector_get_modes,
	.detect_ctx = yhgch_connector_helper_detect_from_ddc,
};

static const struct drm_connector_funcs yhgch_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void yhgch_encoder_enable(struct drm_encoder *encoder)
{
	u32 reg;
	struct drm_device *dev = encoder->dev;
	struct yhgch_drm_private *priv = to_yhgch_drm_private(dev);

	reg = readl(priv->mmio + YHGCH_DISPLAY_CONTROL_HISILE);
	reg |= YHGCH_DISPLAY_CONTROL_FPVDDEN(1);
	reg |= YHGCH_DISPLAY_CONTROL_PANELDATE(1);
	reg |= YHGCH_DISPLAY_CONTROL_FPEN(1);
	reg |= YHGCH_DISPLAY_CONTROL_VBIASEN(1);
	writel(reg, priv->mmio + YHGCH_DISPLAY_CONTROL_HISILE);
}

static void yhgch_encoder_disable(struct drm_encoder *encoder)
{
	u32 reg = 0, regmask = 0;
	struct drm_device *dev = encoder->dev;
	struct yhgch_drm_private *priv = to_yhgch_drm_private(dev);

	reg = readl(priv->mmio + YHGCH_DISPLAY_CONTROL_HISILE);
	regmask |= YHGCH_DISPLAY_CONTROL_FPVDDEN(1);
	regmask |= YHGCH_DISPLAY_CONTROL_PANELDATE(1);
	regmask |= YHGCH_DISPLAY_CONTROL_FPEN(1);
	regmask |= YHGCH_DISPLAY_CONTROL_VBIASEN(1);
	reg &= ~regmask;
	writel(reg, priv->mmio + YHGCH_DISPLAY_CONTROL_HISILE);
}

static const struct drm_encoder_helper_funcs yhgch_encoder_helper_funcs = {
	.enable = yhgch_encoder_enable,
	.disable = yhgch_encoder_disable,
};

int yhgch_vdac_init(struct yhgch_drm_private *priv)
{
	struct drm_device *dev = &priv->dev;
	struct drm_encoder *encoder = &priv->encoder;
	struct drm_crtc *crtc = &priv->crtc;
	struct drm_connector *connector = &priv->connector;
	struct i2c_adapter *ddc;
	int ret;

	ddc = yhgch_ddc_create(priv);
	if (IS_ERR(ddc)) {
		ret = PTR_ERR(ddc);
		drm_err(dev, "failed to create ddc: %d\n", ret);
		return ret;
	}

	encoder->possible_crtcs = drm_crtc_mask(crtc);
	ret = drmm_encoder_init(dev, encoder, NULL, DRM_MODE_ENCODER_DAC, NULL);
	if (ret) {
		drm_err(dev, "failed to init encoder: %d\n", ret);
		return ret;
	}

	drm_encoder_helper_add(encoder, &yhgch_encoder_helper_funcs);

	ret = drm_connector_init_with_ddc(dev, connector,
					  &yhgch_connector_funcs,
					  DRM_MODE_CONNECTOR_VGA,
					  ddc);
	if (ret) {
		drm_err(dev, "failed to init connector: %d\n", ret);
		return ret;
	}
	drm_connector_helper_add(connector, &yhgch_connector_helper_funcs);
	connector->polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	drm_connector_attach_encoder(connector, encoder);

	return 0;
}
