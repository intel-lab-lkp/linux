// SPDX-License-Identifier: GPL-2.0+

#include "vkms_drv.h"
#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>

static const struct drm_connector_funcs vkms_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int vkms_conn_get_modes(struct drm_connector *connector)
{
	int count;

	count = drm_add_modes_noedid(connector, XRES_MAX, YRES_MAX);
	drm_set_preferred_mode(connector, XRES_DEF, YRES_DEF);

	return count;
}

static const struct drm_connector_helper_funcs vkms_conn_helper_funcs = {
	.get_modes    = vkms_conn_get_modes,
};

static int vkms_add_overlay_plane(struct vkms_device *vkmsdev, int index,
				  struct drm_crtc *crtc)
{
	struct vkms_plane *overlay;

	overlay = vkms_plane_init(vkmsdev, DRM_PLANE_TYPE_OVERLAY, index);
	if (IS_ERR(overlay))
		return PTR_ERR(overlay);

	if (!overlay->base.possible_crtcs)
		overlay->base.possible_crtcs = drm_crtc_mask(crtc);

	return 0;
}

int vkms_output_init(struct vkms_device *vkmsdev, int index)
{
	struct vkms_output *output = &vkmsdev->output;
	struct drm_device *dev = &vkmsdev->drm;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	struct drm_crtc *crtc = &output->crtc;
	struct vkms_plane *primary, *cursor = NULL;
	int ret;
	int writeback;
	unsigned int n;

	primary = vkms_plane_init(vkmsdev, DRM_PLANE_TYPE_PRIMARY, index);
	if (IS_ERR(primary))
		return PTR_ERR(primary);

	if (vkmsdev->config->overlay) {
		for (n = 0; n < NUM_OVERLAY_PLANES; n++) {
			ret = vkms_add_overlay_plane(vkmsdev, index, crtc);
			if (ret)
				return ret;
		}
	}

	if (vkmsdev->config->cursor) {
		cursor = vkms_plane_init(vkmsdev, DRM_PLANE_TYPE_CURSOR, index);
		if (IS_ERR(cursor))
			return PTR_ERR(cursor);
	}

	ret = vkms_crtc_init(dev, crtc, &primary->base, &cursor->base);
	if (ret)
		return ret;

	connector = drmm_kzalloc(dev, sizeof(*connector), GFP_KERNEL);
	if (!connector) {
		DRM_ERROR("Failed to allocate connector\n");
		ret = -ENOMEM;
		goto err_connector;
	}

	ret = drmm_connector_init(dev, connector, &vkms_connector_funcs,
				  DRM_MODE_CONNECTOR_VIRTUAL, NULL);
	if (ret) {
		DRM_ERROR("Failed to init connector\n");
		goto err_connector;
	}

	drm_connector_helper_add(connector, &vkms_conn_helper_funcs);

	encoder = drmm_kzalloc(dev, sizeof(*encoder), GFP_KERNEL);
	if (!encoder) {
		DRM_ERROR("Failed to allocate encoder\n");
		ret = -ENOMEM;
		goto err_connector;
	}

	ret = drmm_encoder_init(dev, encoder, NULL, DRM_MODE_ENCODER_VIRTUAL, NULL);
	if (ret) {
		DRM_ERROR("Failed to init encoder\n");
		goto err_connector;
	}
	encoder->possible_crtcs = 1;

	ret = drm_connector_attach_encoder(connector, encoder);
	if (ret) {
		DRM_ERROR("Failed to attach connector to encoder\n");
		return ret;
	}

	if (vkmsdev->config->writeback) {
		writeback = vkms_enable_writeback_connector(vkmsdev);
		if (writeback)
			DRM_ERROR("Failed to init writeback connector\n");
	}

	drm_mode_config_reset(dev);

	return 0;

err_connector:
	drm_crtc_cleanup(crtc);
	return ret;
}
