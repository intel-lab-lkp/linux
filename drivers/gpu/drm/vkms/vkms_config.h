/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _VKMS_CONFIG_H_
#define _VKMS_CONFIG_H_

#include <linux/list.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include <drm/drm_connector.h>

#include "vkms_drv.h"

/**
 * struct vkms_config - General configuration for VKMS driver
 *
 * @dev_name: Name of the device
 * @planes: List of planes configured for the device
 * @crtcs: List of CRTCs configured for the device
 * @encoders: List of encoders configured for the device
 * @connectors: List of connectors configured for the device
 * @dev: Used to store the current VKMS device. Only set when the device is instantiated.
 */
struct vkms_config {
	const char *dev_name;
	struct list_head planes;
	struct list_head crtcs;
	struct list_head encoders;
	struct list_head connectors;
	struct vkms_device *dev;
};

/**
 * struct vkms_config_plane
 *
 * @link: Link to the others planes in vkms_config
 * @config: The vkms_config this plane belongs to
 * @name: Name of the plane
 * @type: Type of the plane. The creator of configuration needs to ensures that
 *        at least one primary plane is present.
 * @possible_crtcs: Array of CRTCs that can be used with this plane
 * @default_rotation: Default rotation that should be used by this plane
 * @supported_rotation: Rotation that this plane will support
 * @plane: Internal usage. This pointer should never be considered as valid.
 *         It can be used to store a temporary reference to a VKMS plane during
 *         device creation. This pointer is not managed by the configuration and
 *         must be managed by other means.
 * @default_color_encoding: Default color encoding that should be used by this plane
 * @supported_color_encodings: Color encoding that this plane will support
 * @default_color_range: Default color range that should be used by this plane
 * @supported_color_ranges: Color range that this plane will support
 * @supported_formats: List of supported formats
 * @supported_formats_count: Length of @supported_formats
 * @zpos_enabled: Enable or disable the zpos property
 * @zpos_mutable: Make the zpos property mutable or not (ignored if @zpos_enabled is false)
 * @zpos_initial: Initial value for zpos property (ignored if @zpos_enabled is false)
 * @zpos_min: Minimal value for zpos property (ignored if @zpos_enabled is false)
 * @zpos_max: Maximal value for zpos property (ignored if @zpos_enabled is false)
 */
struct vkms_config_plane {
	struct list_head link;
	struct vkms_config *config;

	const char *name;
	enum drm_plane_type type;
	unsigned int default_rotation;
	unsigned int supported_rotations;
	enum drm_color_encoding default_color_encoding;
	unsigned int supported_color_encodings;
	enum drm_color_range default_color_range;
	unsigned int supported_color_ranges;
	u32 *supported_formats;
	unsigned int supported_formats_count;
	struct xarray possible_crtcs;
	bool zpos_enabled;
	bool zpos_mutable;
	unsigned int zpos_initial;
	unsigned int zpos_min;
	unsigned int zpos_max;

	/* Internal usage */
	struct vkms_plane *plane;
};

/**
 * struct vkms_config_crtc
 *
 * @link: Link to the others CRTCs in vkms_config
 * @config: The vkms_config this CRTC belongs to
 * @writeback: If true, a writeback buffer can be attached to the CRTC
 * @crtc: Internal usage. This pointer should never be considered as valid.
 *        It can be used to store a temporary reference to a VKMS CRTC during
 *        device creation. This pointer is not managed by the configuration and
 *        must be managed by other means.
 */
struct vkms_config_crtc {
	struct list_head link;
	struct vkms_config *config;

	bool writeback;

	/* Internal usage */
	struct vkms_output *crtc;
};

/**
 * struct vkms_config_encoder
 *
 * @link: Link to the others encoders in vkms_config
 * @config: The vkms_config this CRTC belongs to
 * @possible_crtcs: Array of CRTCs that can be used with this encoder
 * @encoder: Internal usage. This pointer should never be considered as valid.
 *           It can be used to store a temporary reference to a VKMS encoder
 *           during device creation. This pointer is not managed by the
 *           configuration and must be managed by other means.
 */
struct vkms_config_encoder {
	struct list_head link;
	struct vkms_config *config;

	struct xarray possible_crtcs;

	/* Internal usage */
	struct drm_encoder *encoder;
};

/**
 * struct vkms_config_connector
 *
 * @link: Link to the others connector in vkms_config
 * @type: Store the type of connector using DRM_MODE_CONNECTOR_* values
 * @config: The vkms_config this connector belongs to
 * @status: Status (connected, disconnected...) of the connector
 * @possible_encoders: Array of encoders that can be used with this connector
 * @connector: Internal usage. This pointer should never be considered as valid.
 *             It can be used to store a temporary reference to a VKMS connector
 *             during device creation. This pointer is not managed by the
 *             configuration and must be managed by other means.
 */
struct vkms_config_connector {
	struct list_head link;
	struct vkms_config *config;

	int type;
	enum drm_connector_status status;
	struct xarray possible_encoders;

	/* Internal usage */
	struct vkms_connector *connector;
};

/**
 * vkms_config_for_each_plane - Iterate over the vkms_config planes
 * @config: &struct vkms_config pointer
 * @plane_cfg: &struct vkms_config_plane pointer used as cursor
 */
#define vkms_config_for_each_plane(config, plane_cfg) \
	list_for_each_entry((plane_cfg), &(config)->planes, link)

/**
 * vkms_config_for_each_crtc - Iterate over the vkms_config CRTCs
 * @config: &struct vkms_config pointer
 * @crtc_cfg: &struct vkms_config_crtc pointer used as cursor
 */
#define vkms_config_for_each_crtc(config, crtc_cfg) \
	list_for_each_entry((crtc_cfg), &(config)->crtcs, link)

/**
 * vkms_config_for_each_encoder - Iterate over the vkms_config encoders
 * @config: &struct vkms_config pointer
 * @encoder_cfg: &struct vkms_config_encoder pointer used as cursor
 */
#define vkms_config_for_each_encoder(config, encoder_cfg) \
	list_for_each_entry((encoder_cfg), &(config)->encoders, link)

/**
 * vkms_config_for_each_connector - Iterate over the vkms_config connectors
 * @config: &struct vkms_config pointer
 * @connector_cfg: &struct vkms_config_connector pointer used as cursor
 */
#define vkms_config_for_each_connector(config, connector_cfg) \
	list_for_each_entry((connector_cfg), &(config)->connectors, link)

/**
 * vkms_config_plane_for_each_possible_crtc - Iterate over the vkms_config_plane
 * possible CRTCs
 * @plane_cfg: &struct vkms_config_plane pointer
 * @idx: Index of the cursor
 * @possible_crtc: &struct vkms_config_crtc pointer used as cursor
 */
#define vkms_config_plane_for_each_possible_crtc(plane_cfg, idx, possible_crtc) \
	xa_for_each(&(plane_cfg)->possible_crtcs, idx, (possible_crtc))

/**
 * vkms_config_encoder_for_each_possible_crtc - Iterate over the
 * vkms_config_encoder possible CRTCs
 * @encoder_cfg: &struct vkms_config_encoder pointer
 * @idx: Index of the cursor
 * @possible_crtc: &struct vkms_config_crtc pointer used as cursor
 */
#define vkms_config_encoder_for_each_possible_crtc(encoder_cfg, idx, possible_crtc) \
	xa_for_each(&(encoder_cfg)->possible_crtcs, idx, (possible_crtc))

/**
 * vkms_config_connector_for_each_possible_encoder - Iterate over the
 * vkms_config_connector possible encoders
 * @connector_cfg: &struct vkms_config_connector pointer
 * @idx: Index of the cursor
 * @possible_encoder: &struct vkms_config_encoder pointer used as cursor
 */
#define vkms_config_connector_for_each_possible_encoder(connector_cfg, idx, possible_encoder) \
	xa_for_each(&(connector_cfg)->possible_encoders, idx, (possible_encoder))

/**
 * vkms_config_create() - Create a new VKMS configuration
 * @dev_name: Name of the device
 *
 * Returns:
 * The new vkms_config or an error. Call vkms_config_destroy() to free the
 * returned configuration.
 */
struct vkms_config *vkms_config_create(const char *dev_name);

/**
 * vkms_config_default_create() - Create the configuration for the default device
 * @enable_cursor: Create or not a cursor plane
 * @enable_writeback: Create or not a writeback connector
 * @enable_overlay: Create or not overlay planes
 *
 * Returns:
 * The default vkms_config or an error. Call vkms_config_destroy() to free the
 * returned configuration.
 */
struct vkms_config *vkms_config_default_create(bool enable_cursor,
					       bool enable_writeback,
					       bool enable_overlay);

/**
 * vkms_config_destroy() - Free a VKMS configuration
 * @config: vkms_config to free
 */
void vkms_config_destroy(struct vkms_config *config);

/**
 * vkms_config_get_device_name() - Return the name of the device
 * @config: Configuration to get the device name from
 *
 * Returns:
 * The device name. Only valid while @config is valid.
 */
static inline const char *
vkms_config_get_device_name(struct vkms_config *config)
{
	return config->dev_name;
}

/**
 * vkms_config_get_num_crtcs() - Return the number of CRTCs in the configuration
 * @config: Configuration to get the number of CRTCs from
 */
static inline size_t vkms_config_get_num_crtcs(struct vkms_config *config)
{
	return list_count_nodes(&config->crtcs);
}

/**
 * vkms_config_is_valid() - Validate a configuration
 * @config: Configuration to validate
 *
 * Returns:
 * Whether the configuration is valid or not.
 * For example, a configuration without primary planes is not valid.
 */
bool vkms_config_is_valid(const struct vkms_config *config);

/**
 * vkms_config_register_debugfs() - Register a debugfs file to show the device's
 * configuration
 * @vkms_device: Device to register
 */
void vkms_config_register_debugfs(struct vkms_device *vkms_device);

/**
 * vkms_config_create_plane() - Add a new plane configuration
 * @config: Configuration to add the plane to
 *
 * Returns:
 * The new plane configuration or an error. Call vkms_config_destroy_plane() to
 * free the returned plane configuration.
 */
struct vkms_config_plane *vkms_config_create_plane(struct vkms_config *config);

/**
 * vkms_config_destroy_plane() - Remove and free a plane configuration
 * @plane_cfg: Plane configuration to destroy
 */
void vkms_config_destroy_plane(struct vkms_config_plane *plane_cfg);

/**
 * vkms_config_plane_type() - Return the plane type
 * @plane_cfg: Plane to get the type from
 */
static inline enum drm_plane_type
vkms_config_plane_get_type(struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->type;
}

/**
 * vkms_config_plane_set_type() - Set the plane type
 * @plane_cfg: Plane to set the type to
 * @type: New plane type
 */
static inline void
vkms_config_plane_set_type(struct vkms_config_plane *plane_cfg,
			   enum drm_plane_type type)
{
	plane_cfg->type = type;
}

/**
 * vkms_config_connector_get_type() - Return the connector type
 * @connector_cfg: Connector to get the type from
 */
static inline int
vkms_config_connector_get_type(const struct vkms_config_connector *connector_cfg)
{
	return connector_cfg->type;
}

/**
 * vkms_config_connector_set_type() - Set the connector type
 * @connector_cfg: Connector to set the type to
 * @type: New connector type
 */
static inline void
vkms_config_connector_set_type(struct vkms_config_connector *connector_cfg,
			       int type)
{
	connector_cfg->type = type;
}

/*
 * vkms_config_plane_get_default_rotation() - Get the default rotation for a plane
 * @plane_cfg: Plane to get the default rotation from
 *
 * Returns:
 * The default rotation for the plane.
 */
static inline unsigned int
vkms_config_plane_get_default_rotation(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->default_rotation;
}

/**
 * vkms_config_plane_set_default_rotation() - Set the default rotation for a plane
 * @plane_cfg: Plane to set the default rotation to
 * @default_rotation: New default rotation for the plane
 */
static inline void
vkms_config_plane_set_default_rotation(struct vkms_config_plane *plane_cfg,
				       unsigned int default_rotation)
{
	plane_cfg->default_rotation = default_rotation;
}

/**
 * vkms_config_plane_get_supported_rotations() - Get the supported rotations for a plane
 * @plane_cfg: Plane to get the supported rotations from
 *
 * Returns:
 * The supported rotations for the plane.
 */
static inline unsigned int
vkms_config_plane_get_supported_rotations(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->supported_rotations;
}

/**
 * vkms_config_plane_set_supported_rotations() - Set the supported rotations for a plane
 * @plane_cfg: Plane to set the supported rotations to
 * @supported_rotations: New supported rotations for the plane
 */
static inline void
vkms_config_plane_set_supported_rotations(struct vkms_config_plane *plane_cfg,
					  unsigned int supported_rotations)
{
	plane_cfg->supported_rotations = supported_rotations;
}

#if IS_ENABLED(CONFIG_KUNIT)
bool vkms_config_valid_plane_rotation(const struct vkms_config *config,
				      const struct vkms_config_plane *plane_cfg);
#endif

/**
 * vkms_config_plane_get_default_color_encoding() - Get the default color encoding for a plane
 * @plane_cfg: Plane to get the default color encoding from
 *
 * Returns:
 * The default color encoding for the plane
 */
static inline enum drm_color_encoding
vkms_config_plane_get_default_color_encoding(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->default_color_encoding;
}

/**
 * vkms_config_plane_set_default_color_encoding() - Set the default color encoding for a plane
 * @plane_cfg: Plane to set the default color encoding to
 * @default_color_encoding: New default color encoding for the plane
 */
static inline void
vkms_config_plane_set_default_color_encoding(struct vkms_config_plane *plane_cfg,
					     enum drm_color_encoding default_color_encoding)
{
	plane_cfg->default_color_encoding = default_color_encoding;
}

/**
 * vkms_config_plane_get_supported_color_encodings() - Get the supported color encodings for a plane
 * @plane_cfg: Plane to get the supported color encodings from
 *
 * Returns:
 * The supported color encodings for the plane. Each set bit correspond to a value of enum
 * drm_color_encoding: BIT(DRM_COLOR_YCBCR_BT601) | BIT(DRM_COLOR_YCBCR_BT709) means that
 * DRM_COLOR_YCBCR_BT601 and DRM_COLOR_YCBCR_BT709 are supported.
 */
static inline unsigned int
vkms_config_plane_get_supported_color_encodings(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->supported_color_encodings;
}

/**
 * vkms_config_plane_set_supported_color_encodings() - Set the supported color encodings for a plane
 * @plane_cfg: Plane to set the supported color encodings to
 * @supported_color_encodings: New supported color encodings for the plane. Each set bit corresponds
 *                            to a value of enum drm_color_encoding:
 *                            BIT(DRM_COLOR_YCBCR_BT601) | BIT(DRM_COLOR_YCBCR_BT709) means that
 *                            DRM_COLOR_YCBCR_BT601 and DRM_COLOR_YCBCR_BT709 are supported.
 */
static inline void
vkms_config_plane_set_supported_color_encodings(struct vkms_config_plane *plane_cfg,
						unsigned int supported_color_encodings)
{
	plane_cfg->supported_color_encodings = supported_color_encodings;
}

#if IS_ENABLED(CONFIG_KUNIT)
bool vkms_config_valid_plane_color_encoding(const struct vkms_config *config,
					    const struct vkms_config_plane *plane_cfg);
#endif

static inline enum drm_color_range
vkms_config_plane_get_default_color_range(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->default_color_range;
}

static inline void
vkms_config_plane_set_default_color_range(struct vkms_config_plane *plane_cfg,
					  enum drm_color_range default_color_range)
{
	plane_cfg->default_color_range = default_color_range;
}

static inline unsigned int
vkms_config_plane_get_supported_color_ranges(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->supported_color_ranges;
}

static inline void
vkms_config_plane_set_supported_color_ranges(struct vkms_config_plane *plane_cfg,
					     unsigned int supported_color_ranges)
{
	plane_cfg->supported_color_ranges = supported_color_ranges;
}

#if IS_ENABLED(CONFIG_KUNIT)
bool vkms_config_valid_plane_color_range(const struct vkms_config *config,
					 const struct vkms_config_plane *plane_cfg);
#endif

static inline u32 *
vkms_config_plane_get_supported_formats(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->supported_formats;
}

static inline unsigned int
vkms_config_plane_get_supported_formats_count(struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->supported_formats_count;
}

/** vkms_config_plane_add_format - Add a format to the list of supported format of a plane
 *
 * The passed drm_format can already be present in the list. This may fail if the allocation of a
 * bigger array fails.
 *
 * @plane_cfg: Plane to add the format to
 * @drm_format: Format to add to this plane
 *
 * Returns: 0 on success, -ENOMEM if array allocation fails, -EINVAL if the format is not supported
 * by VKMS
 */
int __must_check vkms_config_plane_add_format(struct vkms_config_plane *plane_cfg, u32 drm_format);

/**
 * vkms_config_plane_add_all_formats - Helper to quickly add all the supported formats
 * @plane_cfg: Plane to add the formats to
 *
 * Returns: 0 on success, -ENOMEM if array allocation fails, -EINVAL if the format is not supported
 * by VKMS
 */
int __must_check vkms_config_plane_add_all_formats(struct vkms_config_plane *plane_cfg);

/**
 * vkms_config_plane_remove_format - Remove a specific format from a plane
 * @plane_cfg: Plane to remove the format to
 * @drm_format: Format to remove
 */
void vkms_config_plane_remove_format(struct vkms_config_plane *plane_cfg, u32 drm_format);

/**
 * vkms_config_plane_remove_all_formats - Remove all formast from a plane
 * @plane_cfg: Plane to remove the formats from
 */
void vkms_config_plane_remove_all_formats(struct vkms_config_plane *plane_cfg);

/**
 * vkms_config_plane_set_name() - Set the plane name
 * @plane_cfg: Plane to set the name to
 * @name: New plane name. The name is copied.
 */
static inline void
vkms_config_plane_set_name(struct vkms_config_plane *plane_cfg,
			   const char *name)
{
	if (plane_cfg->name)
		kfree_const(plane_cfg->name);
	plane_cfg->name = kstrdup_const(name, GFP_KERNEL);
}

/**
 * vkms_config_plane_get_name - Get the plane name
 * @plane_cfg: Plane to get the name from
 */
static inline const char *
vkms_config_plane_get_name(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->name;
}

/**
 * vkms_config_plane_set_zpos_enabled() - Enable or disable zpos property for a plane
 * @plane_cfg: Plane configuration to modify
 * @zpos_enabled: Whether to enable the zpos property
 */
static inline
void vkms_config_plane_set_zpos_enabled(struct vkms_config_plane *plane_cfg,
					bool zpos_enabled)
{
	plane_cfg->zpos_enabled = zpos_enabled;
}

/**
 * vkms_config_plane_set_zpos_mutable() - Set whether zpos property is mutable
 * @plane_cfg: Plane configuration to modify
 * @zpos_mutable: Whether the zpos property should be mutable
 */
static inline
void vkms_config_plane_set_zpos_mutable(struct vkms_config_plane *plane_cfg,
					bool zpos_mutable)
{
	plane_cfg->zpos_mutable = zpos_mutable;
}

/**
 * vkms_config_plane_set_zpos_initial() - Set the initial zpos value
 * @plane_cfg: Plane configuration to modify
 * @zpos_initial: Initial zpos value
 */
static inline
void vkms_config_plane_set_zpos_initial(struct vkms_config_plane *plane_cfg,
					unsigned int zpos_initial)
{
	plane_cfg->zpos_initial = zpos_initial;
}

/**
 * vkms_config_plane_set_zpos_min() - Set the minimum zpos value
 * @plane_cfg: Plane configuration to modify
 * @zpos_min: Minimum zpos value
 */
static inline
void vkms_config_plane_set_zpos_min(struct vkms_config_plane *plane_cfg,
				    unsigned int zpos_min)
{
	plane_cfg->zpos_min = zpos_min;
}

/**
 * vkms_config_plane_set_zpos_max() - Set the maximum zpos value
 * @plane_cfg: Plane configuration to modify
 * @zpos_max: Maximum zpos value
 *
 * Sets the maximum allowed value for the zpos property. This setting is
 * ignored if zpos is disabled.
 */
static inline
void vkms_config_plane_set_zpos_max(struct vkms_config_plane *plane_cfg,
				    unsigned int zpos_max)
{
	plane_cfg->zpos_max = zpos_max;
}

/**
 * vkms_config_plane_get_zpos_enabled() - Check if zpos property is enabled
 * @plane_cfg: Plane configuration to check
 *
 * Returns:
 * True if the zpos property is enabled for this plane, false otherwise.
 */
static inline
bool vkms_config_plane_get_zpos_enabled(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->zpos_enabled;
}

/**
 * vkms_config_plane_get_zpos_mutable() - Check if zpos property is mutable
 * @plane_cfg: Plane configuration to check
 *
 * Returns:
 * True if the zpos property is mutable for this plane, false otherwise.
 * Returns false if zpos is disabled.
 */
static inline
bool vkms_config_plane_get_zpos_mutable(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->zpos_mutable;
}

/**
 * vkms_config_plane_get_zpos_initial() - Get the initial zpos value
 * @plane_cfg: Plane configuration to check
 *
 * Returns:
 * The initial zpos value for this plane. The return value is undefined if
 * zpos is disabled.
 */
static inline
unsigned int vkms_config_plane_get_zpos_initial(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->zpos_initial;
}

/**
 * vkms_config_plane_get_zpos_min() - Get the minimum zpos value
 * @plane_cfg: Plane configuration to check
 *
 * Returns:
 * The minimum allowed zpos value for this plane. The return value is undefined
 * if zpos is disabled.
 */
static inline
unsigned int vkms_config_plane_get_zpos_min(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->zpos_min;
}

/**
 * vkms_config_plane_get_zpos_max() - Get the maximum zpos value
 * @plane_cfg: Plane configuration to check
 *
 * Returns:
 * The maximum allowed zpos value for this plane. The return value is undefined
 * if zpos is disabled.
 */
static inline
unsigned int vkms_config_plane_get_zpos_max(const struct vkms_config_plane *plane_cfg)
{
	return plane_cfg->zpos_max;
}

#if IS_ENABLED(CONFIG_KUNIT)
bool vkms_config_valid_plane_zpos(const struct vkms_config *config,
				  const struct vkms_config_plane *plane_cfg);
#endif

/**
 * vkms_config_plane_attach_crtc - Attach a plane to a CRTC
 * @plane_cfg: Plane to attach
 * @crtc_cfg: CRTC to attach @plane_cfg to
 */
int __must_check vkms_config_plane_attach_crtc(struct vkms_config_plane *plane_cfg,
					       struct vkms_config_crtc *crtc_cfg);

/**
 * vkms_config_plane_detach_crtc - Detach a plane from a CRTC
 * @plane_cfg: Plane to detach
 * @crtc_cfg: CRTC to detach @plane_cfg from
 */
void vkms_config_plane_detach_crtc(struct vkms_config_plane *plane_cfg,
				   struct vkms_config_crtc *crtc_cfg);

/**
 * vkms_config_create_crtc() - Add a new CRTC configuration
 * @config: Configuration to add the CRTC to
 *
 * Returns:
 * The new CRTC configuration or an error. Call vkms_config_destroy_crtc() to
 * free the returned CRTC configuration.
 */
struct vkms_config_crtc *vkms_config_create_crtc(struct vkms_config *config);

/**
 * vkms_config_destroy_crtc() - Remove and free a CRTC configuration
 * @config: Configuration to remove the CRTC from
 * @crtc_cfg: CRTC configuration to destroy
 */
void vkms_config_destroy_crtc(struct vkms_config *config,
			      struct vkms_config_crtc *crtc_cfg);

/**
 * vkms_config_crtc_get_writeback() - If a writeback connector will be created
 * @crtc_cfg: CRTC with or without a writeback connector
 */
static inline bool
vkms_config_crtc_get_writeback(struct vkms_config_crtc *crtc_cfg)
{
	return crtc_cfg->writeback;
}

/**
 * vkms_config_crtc_set_writeback() - If a writeback connector will be created
 * @crtc_cfg: Target CRTC
 * @writeback: Enable or disable the writeback connector
 */
static inline void
vkms_config_crtc_set_writeback(struct vkms_config_crtc *crtc_cfg,
			       bool writeback)
{
	crtc_cfg->writeback = writeback;
}

/**
 * vkms_config_crtc_primary_plane() - Return the primary plane for a CRTC
 * @config: Configuration containing the CRTC
 * @crtc_config: Target CRTC
 *
 * Note that, if multiple primary planes are found, the first one is returned.
 * In this case, the configuration will be invalid. See vkms_config_is_valid().
 *
 * Returns:
 * The primary plane or NULL if none is assigned yet.
 */
struct vkms_config_plane *vkms_config_crtc_primary_plane(const struct vkms_config *config,
							 struct vkms_config_crtc *crtc_cfg);

/**
 * vkms_config_crtc_cursor_plane() - Return the cursor plane for a CRTC
 * @config: Configuration containing the CRTC
 * @crtc_config: Target CRTC
 *
 * Note that, if multiple cursor planes are found, the first one is returned.
 * In this case, the configuration will be invalid. See vkms_config_is_valid().
 *
 * Returns:
 * The cursor plane or NULL if none is assigned yet.
 */
struct vkms_config_plane *vkms_config_crtc_cursor_plane(const struct vkms_config *config,
							struct vkms_config_crtc *crtc_cfg);

/**
 * vkms_config_create_encoder() - Add a new encoder configuration
 * @config: Configuration to add the encoder to
 *
 * Returns:
 * The new encoder configuration or an error. Call vkms_config_destroy_encoder()
 * to free the returned encoder configuration.
 */
struct vkms_config_encoder *vkms_config_create_encoder(struct vkms_config *config);

/**
 * vkms_config_destroy_encoder() - Remove and free a encoder configuration
 * @config: Configuration to remove the encoder from
 * @encoder_cfg: Encoder configuration to destroy
 */
void vkms_config_destroy_encoder(struct vkms_config *config,
				 struct vkms_config_encoder *encoder_cfg);

/**
 * vkms_config_encoder_attach_crtc - Attach a encoder to a CRTC
 * @encoder_cfg: Encoder to attach
 * @crtc_cfg: CRTC to attach @encoder_cfg to
 */
int __must_check vkms_config_encoder_attach_crtc(struct vkms_config_encoder *encoder_cfg,
						 struct vkms_config_crtc *crtc_cfg);

/**
 * vkms_config_encoder_detach_crtc - Detach a encoder from a CRTC
 * @encoder_cfg: Encoder to detach
 * @crtc_cfg: CRTC to detach @encoder_cfg from
 */
void vkms_config_encoder_detach_crtc(struct vkms_config_encoder *encoder_cfg,
				     struct vkms_config_crtc *crtc_cfg);

/**
 * vkms_config_create_connector() - Add a new connector configuration
 * @config: Configuration to add the connector to
 *
 * Returns:
 * The new connector configuration or an error. Call
 * vkms_config_destroy_connector() to free the returned connector configuration.
 */
struct vkms_config_connector *vkms_config_create_connector(struct vkms_config *config);

/**
 * vkms_config_destroy_connector() - Remove and free a connector configuration
 * @connector_cfg: Connector configuration to destroy
 */
void vkms_config_destroy_connector(struct vkms_config_connector *connector_cfg);

/**
 * vkms_config_connector_attach_encoder - Attach a connector to an encoder
 * @connector_cfg: Connector to attach
 * @encoder_cfg: Encoder to attach @connector_cfg to
 */
int __must_check vkms_config_connector_attach_encoder(struct vkms_config_connector *connector_cfg,
						      struct vkms_config_encoder *encoder_cfg);

/**
 * vkms_config_connector_detach_encoder - Detach a connector from an encoder
 * @connector_cfg: Connector to detach
 * @encoder_cfg: Encoder to detach @connector_cfg from
 */
void vkms_config_connector_detach_encoder(struct vkms_config_connector *connector_cfg,
					  struct vkms_config_encoder *encoder_cfg);

/**
 * vkms_config_connector_get_status() - Return the status of the connector
 * @connector_cfg: Connector to get the status from
 */
static inline enum drm_connector_status
vkms_config_connector_get_status(struct vkms_config_connector *connector_cfg)
{
	return connector_cfg->status;
}

/**
 * vkms_config_connector_set_status() - Set the status of the connector
 * @connector_cfg: Connector to set the status to
 * @status: New connector status
 */
static inline void
vkms_config_connector_set_status(struct vkms_config_connector *connector_cfg,
				 enum drm_connector_status status)
{
	connector_cfg->status = status;
}

#endif /* _VKMS_CONFIG_H_ */
