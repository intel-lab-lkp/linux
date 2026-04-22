// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>

#include "vkms_config.h"
#include "vkms_connector.h"

/**
 * vkms_connector_build_path_property() - Build the PATH property string for MST connectors
 * @connector: The connector to build the PATH property for
 * @connector_cfg: The connector configuration
 *
 * The PATH property format is:
 *     mst:<drm object ID of root connector>-<dash-separated list of port_id>
 * For nested MST connectors, this builds the full path like mst:45-2-3-4-2
 */
static void vkms_connector_build_path_property(struct vkms_connector *connector,
						const struct vkms_config_connector *connector_cfg)
{
	const struct vkms_config_connector *current_cfg = connector_cfg;
	const struct vkms_config_connector *root_cfg = NULL;
	struct vkms_connector *root_connector = NULL;
	char path[128]; /* Increased size for nested MST paths */
	int len = 0;
	u8 port_ids[16]; /* Max 16 levels of nesting */
	int port_count = 0;
	int i;

	if (!vkms_config_connector_get_parent(connector_cfg))
		return;

	while (current_cfg) {
		if (port_count < ARRAY_SIZE(port_ids))
			port_ids[port_count++] = current_cfg->port_id;

		if (!vkms_config_connector_get_parent(current_cfg)) {
			root_cfg = current_cfg;
			break;
		}

		current_cfg = vkms_config_connector_get_parent(current_cfg);
	}

	if (!root_cfg || !root_cfg->connector)
		return;

	root_connector = root_cfg->connector;

	len = snprintf(path, sizeof(path), "mst:%d", root_connector->base.base.id);

	for (i = port_count - 2; i >= 0; i--) {
		int added = snprintf(path + len, sizeof(path) - len,
				     "-%u", port_ids[i]);
		if (added < 0 || len + added >= sizeof(path))
			return;
		len += added;
	}

	drm_connector_set_path_property(&connector->base, path);
}

/**
 * vkms_connector_update_path_properties() - Update PATH properties for all connectors
 * @vkmsdev: VKMS device
 *
 * This should be called after all connectors are created to ensure parent connectors
 * have valid DRM object IDs.
 */
void vkms_connector_update_path_properties(struct vkms_device *vkmsdev)
{
	struct vkms_config_connector *connector_cfg;

	vkms_config_for_each_connector(vkmsdev->config, connector_cfg)
		if (connector_cfg->connector)
			vkms_connector_build_path_property(connector_cfg->connector, connector_cfg);
}

static enum drm_connector_status vkms_connector_detect(struct drm_connector *connector,
						       bool force)
{
	struct drm_device *dev = connector->dev;
	struct vkms_device *vkmsdev = drm_device_to_vkms_device(dev);
	struct vkms_connector *vkms_connector;
	enum drm_connector_status status;
	struct vkms_config_connector *connector_cfg;

	vkms_connector = drm_connector_to_vkms_connector(connector);

	/*
	 * The connector configuration might not exist if its configfs directory
	 * was deleted. Therefore, use the configuration if present or keep the
	 * current status if we can not access it anymore.
	 */
	status = connector->status;

	vkms_config_for_each_connector(vkmsdev->config, connector_cfg) {
		if (connector_cfg->connector == vkms_connector)
			status = vkms_config_connector_get_status(connector_cfg);
	}

	return status;
}

static const struct drm_connector_funcs vkms_connector_funcs = {
	.detect = vkms_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int vkms_connector_read_block(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct vkms_config_connector *config = context;
	unsigned int edid_len;
	const u8 *edid = vkms_config_connector_get_edid(config, &edid_len);

	if (block * len + len > edid_len)
		return 1;
	memcpy(buf, &edid[block * len], len);
	return 0;
}

static int vkms_conn_get_modes(struct drm_connector *connector)
{
	struct vkms_connector *vkms_connector = drm_connector_to_vkms_connector(connector);
	const struct drm_edid *drm_edid = NULL;
	int count = 0;
	struct vkms_config_connector *context = NULL;
	struct drm_device *dev = connector->dev;
	struct vkms_device *vkmsdev = drm_device_to_vkms_device(dev);
	struct vkms_config_connector *connector_cfg;

	vkms_config_for_each_connector(vkmsdev->config, connector_cfg) {
		if (connector_cfg->connector == vkms_connector) {
			context = connector_cfg;
			break;
		}
	}
	if (context) {
		if (vkms_config_connector_get_edid_enabled(context)) {
			drm_edid = drm_edid_read_custom(connector,
							vkms_connector_read_block, context);

			/*
			 * Unconditionally update the connector. If the EDID was read
			 * successfully, fill in the connector information derived from the
			 * EDID. Otherwise, if the EDID is NULL, clear the connector
			 * information.
			 */
			drm_edid_connector_update(connector, drm_edid);

			count = drm_edid_connector_add_modes(connector);

			drm_edid_free(drm_edid);
		} else {
			count = drm_add_modes_noedid(connector, XRES_MAX, YRES_MAX);
			drm_set_preferred_mode(connector, XRES_DEF, YRES_DEF);
		}
	}

	return count;
}

static struct drm_encoder *vkms_conn_best_encoder(struct drm_connector *connector)
{
	struct drm_encoder *encoder;

	drm_connector_for_each_possible_encoder(connector, encoder)
		return encoder;

	return NULL;
}

static const struct drm_connector_helper_funcs vkms_conn_helper_funcs = {
	.get_modes    = vkms_conn_get_modes,
	.best_encoder = vkms_conn_best_encoder,
};


/**
 * vkms_connector_init - Common initialization for all vkms connectors
 *
 * @connector - Already allocated connector
 * @connector_cfg - Configuration to apply
 *
 * Returns: 0 on success, errno on error;
 */
static int __must_check vkms_connector_init(struct vkms_connector *connector,
					    struct vkms_config_connector *connector_cfg)
{
	int ret = 0;

	if (vkms_config_connector_get_supported_colorspaces(connector_cfg)) {
		if (connector_cfg->type == DRM_MODE_CONNECTOR_HDMIA) {
			ret = drm_mode_create_hdmi_colorspace_property(&connector->base,
								       vkms_config_connector_get_supported_colorspaces(connector_cfg));
			if (ret)
				return ret;
		} else if (connector_cfg->type == DRM_MODE_CONNECTOR_DisplayPort ||
			   connector_cfg->type == DRM_MODE_CONNECTOR_eDP) {
			ret = drm_mode_create_dp_colorspace_property(&connector->base,
								     vkms_config_connector_get_supported_colorspaces(connector_cfg));
			if (ret)
				return ret;
		}

		if (connector_cfg->type == DRM_MODE_CONNECTOR_HDMIA ||
		    connector_cfg->type == DRM_MODE_CONNECTOR_DisplayPort ||
		    connector_cfg->type == DRM_MODE_CONNECTOR_eDP) {
			ret = drm_connector_attach_colorspace_property(&connector->base);
			if (ret) {
				drm_property_destroy(connector->base.dev, connector->base.colorspace_property);
				return ret;
			}
			drm_connector_attach_hdr_output_metadata_property(&connector->base);
		}
	}

	return 0;
}

struct vkms_connector *vkms_connector_init_static(struct vkms_device *vkmsdev,
						  struct vkms_config_connector *connector_cfg)
{
	struct drm_device *dev = &vkmsdev->drm;
	struct vkms_connector *connector;
	int ret;

	connector = drmm_kzalloc(dev, sizeof(*connector), GFP_KERNEL);
	if (!connector)
		return ERR_PTR(-ENOMEM);

	ret = drmm_connector_init(dev, &connector->base, &vkms_connector_funcs,
				  vkms_config_connector_get_type(connector_cfg), NULL);
	if (ret)
		return ERR_PTR(ret);

	ret = vkms_connector_init(connector, connector_cfg);
	if (ret)
		return ERR_PTR(ret);

	drm_connector_helper_add(&connector->base, &vkms_conn_helper_funcs);

	if (vkms_config_connector_get_edid_enabled(connector_cfg))
		drm_connector_attach_edid_property(&connector->base);

	return connector;
}

static void vkms_connector_dynamic_destroy(struct drm_connector *connector)
{
	struct vkms_connector *vkms_connector;

	drm_connector_cleanup(connector);

	vkms_connector = drm_connector_to_vkms_connector(connector);
	kfree(vkms_connector);
}

static const struct drm_connector_funcs vkms_dynamic_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.destroy = vkms_connector_dynamic_destroy,
	.detect = vkms_connector_detect,
};

void vkms_trigger_connector_hotplug(struct vkms_device *vkmsdev)
{
	struct drm_device *dev = &vkmsdev->drm;

	drm_kms_helper_hotplug_event(dev);
}

struct vkms_connector *vkms_connector_hot_add(struct vkms_device *vkmsdev,
					      struct vkms_config_connector *connector_cfg)
{
	struct vkms_config_encoder *encoder_cfg;
	struct vkms_connector __free(kfree) * connector = NULL;
	int ret;
	unsigned long idx = 0;

	connector = kzalloc_obj(*connector, GFP_KERNEL);
	if (IS_ERR(connector))
		return connector;
	ret = drm_connector_dynamic_init(&vkmsdev->drm,
					 &connector->base,
					 &vkms_dynamic_connector_funcs,
					 connector_cfg->type,
					 NULL);
	if (ret)
		return ERR_PTR(ret);
	drm_connector_helper_add(&connector->base, &vkms_conn_helper_funcs);

	vkms_config_connector_for_each_possible_encoder(connector_cfg, idx, encoder_cfg) {
		ret = drm_connector_attach_encoder(&connector->base,
						   encoder_cfg->encoder);
		if (ret)
			return ERR_PTR(ret);
	}

	drm_atomic_helper_connector_reset(&connector->base);

	ret = vkms_connector_init(connector, connector_cfg);
	if (ret)
		return ERR_PTR(ret);

	vkms_connector_build_path_property(connector, connector_cfg);

	ret = drm_connector_dynamic_register(&connector->base);
	if (ret) {
		if (connector_cfg->type == DRM_MODE_CONNECTOR_HDMIA ||
		    connector_cfg->type == DRM_MODE_CONNECTOR_DisplayPort ||
		    connector_cfg->type == DRM_MODE_CONNECTOR_eDP) {
			drm_property_destroy(connector->base.dev,
					     connector->base.colorspace_property);
		}
		return ERR_PTR(ret);
	}

	return_ptr(connector);
}

void vkms_connector_hot_remove(struct vkms_device *vkmsdev,
			       struct vkms_connector *connector)
{
	drm_connector_unregister(&connector->base);
	drm_mode_config_reset(&vkmsdev->drm);
	drm_connector_put(&connector->base);
}

int vkms_connector_hot_attach_encoder(struct vkms_device *vkmsdev,
				      struct vkms_connector *connector,
				      struct drm_encoder *encoder)
{
	int ret;

	ret = drm_connector_attach_encoder(&connector->base, encoder);
	if (ret)
		return ret;

	drm_mode_config_reset(&vkmsdev->drm);

	return ret;
}
