/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VKMS_CONNECTOR_H_
#define _VKMS_CONNECTOR_H_

#include "vkms_drv.h"
#include "vkms_config.h"

#define drm_connector_to_vkms_connector(target) \
	container_of(target, struct vkms_connector, base)

/**
 * struct vkms_connector - VKMS custom type wrapping around the DRM connector
 *
 * @drm: Base DRM connector
 */
struct vkms_connector {
	struct drm_connector base;
};

/**
 * vkms_connector_init_static() - Initialize a connector
 * @vkmsdev: VKMS device containing the connector
 *
 * Returns:
 * The connector or an error on failure.
 */
struct vkms_connector *vkms_connector_init_static(struct vkms_device *vkmsdev,
						  struct vkms_config_connector *connector_cfg);

/**
 * vkms_trigger_connector_hotplug() - Update the device's connectors status
 * @vkmsdev: VKMS device to update
 */
void vkms_trigger_connector_hotplug(struct vkms_device *vkmsdev);

/**
 * vkms_connector_hot_add() - Create a connector after the device is created
 * @vkmsdev: Device to hot-add the connector to
 * @connector_cfg: Connector's configuration
 *
 * Returns:
 * The connector or an error on failure.
 */
struct vkms_connector *vkms_connector_hot_add(struct vkms_device *vkmsdev,
					      struct vkms_config_connector *connector_cfg);

/**
 * vkms_connector_hot_remove() - Remove a connector after a device is created
 * @connector: The connector to hot-remove
 */
void vkms_connector_hot_remove(struct vkms_device *vkmsdev,
			       struct vkms_connector *connector);

/**
 * vkms_connector_hot_attach_encoder() - Attach a connector to a encoder after
 * the device is created.
 * @vkmsdev: Device containing the connector and the encoder
 * @connector: Connector to attach to @encoder
 * @encoder: Target encoder
 *
 * Returns:
 * 0 on success or an error on failure.
 */
int vkms_connector_hot_attach_encoder(struct vkms_device *vkmsdev,
				      struct vkms_connector *connector,
				      struct drm_encoder *encoder);

#endif /* _VKMS_CONNECTOR_H_ */
