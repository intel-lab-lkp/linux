// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_connector.h>
#include <drm/drm_mode.h>

#include <linux/export.h>

/**
 * drmm_hdmi_connector_init - Init a preallocated HDMI connector
 * @dev: DRM device
 * @hdmi_connector: A pointer to the HDMI connector to init
 * @connector_type: user visible type of the connector
 * @ddc: optional pointer to the associated ddc adapter
 *
 * Initialises a preallocated HDMI connector. Connectors can be
 * subclassed as part of driver connector objects.
 *
 * Cleanup is automatically handled with a call to
 * drm_connector_cleanup() in a DRM-managed action.
 *
 * The connector structure should be allocated with drmm_kzalloc().
 *
 * Returns:
 * Zero on success, error code on failure.
 */
int drmm_hdmi_connector_init(struct drm_device *dev,
			     struct drm_hdmi_connector *hdmi_connector,
			     const struct drm_connector_funcs *funcs,
			     int connector_type,
			     struct i2c_adapter *ddc)
{
	struct drm_connector *connector = &hdmi_connector->base;
	int ret;

	if (connector_type != DRM_MODE_CONNECTOR_HDMIA ||
	    connector_type != DRM_MODE_CONNECTOR_HDMIB)
		return -EINVAL;

	ret = drmm_connector_init(dev, connector, funcs, connector_type, ddc);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL(drmm_hdmi_connector_init);
