// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>

#include <linux/export.h>

/**
 * __drm_atomic_helper_hdmi_connector_reset() - Initializes all @drm_hdmi_connector_state resources
 * @hdmi_connector: the connector this state refers to
 * @new_hdmi_state: the HDMI connector state to initialize
 *
 * Initializes all relevant resources from a @drm_hdmi_connector_state
 * without actually allocating it. This is useful for drivers that
 * subclass @drm_hdmi_connector_state.
 */
void __drm_atomic_helper_hdmi_connector_reset(struct drm_hdmi_connector *hdmi_connector,
					      struct drm_hdmi_connector_state *new_hdmi_state)
{
	struct drm_connector *connector = &hdmi_connector->base;

	__drm_atomic_helper_connector_reset(connector, &new_hdmi_state->base);

	new_hdmi_state->base.max_bpc = 8;
	new_hdmi_state->base.max_requested_bpc = 8;
	new_hdmi_state->output_bpc = 8;
	new_hdmi_state->broadcast_rgb = DRM_HDMI_BROADCAST_RGB_AUTO;
}
EXPORT_SYMBOL(__drm_atomic_helper_hdmi_connector_reset);

/**
 * drm_atomic_helper_hdmi_connector_reset() - Create a @drm_hdmi_connector_state object
 * @connector: the parent connector
 *
 * This helper is meant to be the default &drm_connector_funcs.reset
 * implementation for @drm_hdmi_connector that don't subclass
 * @drm_hdmi_connector_state.
 */
void drm_atomic_helper_hdmi_connector_reset(struct drm_connector *connector)
{
	struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	struct drm_connector_state *old_state = connector->state;
	struct drm_hdmi_connector_state *old_hdmi_state =
		connector_state_to_hdmi_connector_state(old_state);
	struct drm_hdmi_connector_state *new_hdmi_state;

	if (old_state)
		__drm_atomic_helper_connector_destroy_state(old_state);

	kfree(old_hdmi_state);

	new_hdmi_state = kzalloc(sizeof(*new_hdmi_state), GFP_KERNEL);
	if (!new_hdmi_state)
		return;

	__drm_atomic_helper_hdmi_connector_reset(hdmi_connector, new_hdmi_state);
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_reset);

/**
 * __drm_atomic_helper_hdmi_connector_duplicate_state() - Copies all @drm_hdmi_connector_state resources
 * @hdmi_connector: the connector this state refers to
 * @new_hdmi_state: the HDMI connector state to copy to
 *
 * Copies all relevant resources from a @drm_hdmi_connector_state to a
 * new one without actually allocating it. This is useful for drivers
 * that subclass @drm_hdmi_connector_state.
 */
void
__drm_atomic_helper_hdmi_connector_duplicate_state(struct drm_hdmi_connector *hdmi_connector,
						   struct drm_hdmi_connector_state *new_hdmi_state)
{
	struct drm_connector *connector = &hdmi_connector->base;
	struct drm_connector_state *old_state = connector->state;
	struct drm_hdmi_connector_state *old_hdmi_state =
		connector_state_to_hdmi_connector_state(old_state);

	new_hdmi_state->output_bpc = old_hdmi_state->output_bpc;
	new_hdmi_state->broadcast_rgb = old_hdmi_state->broadcast_rgb;
	__drm_atomic_helper_connector_duplicate_state(connector, &new_hdmi_state->base);
}
EXPORT_SYMBOL(__drm_atomic_helper_hdmi_connector_duplicate_state);

/**
 * drm_atomic_helper_hdmi_connector_duplicate_state() - Duplicate a @drm_hdmi_connector_state object
 * @connector: the parent connector this state refers to
 *
 * This helper is meant to be the default
 * &drm_connector_funcs.atomic_duplicate_state implementation for
 * @drm_hdmi_connector that don't subclass @drm_hdmi_connector_state.
 */
struct drm_connector_state *
drm_atomic_helper_hdmi_connector_duplicate_state(struct drm_connector *connector)
{
	struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	struct drm_hdmi_connector_state *new_hdmi_state;

	new_hdmi_state = kzalloc(sizeof(*new_hdmi_state), GFP_KERNEL);
	if (!new_hdmi_state)
		return NULL;

	__drm_atomic_helper_hdmi_connector_duplicate_state(hdmi_connector, new_hdmi_state);

	return &new_hdmi_state->base;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_duplicate_state);

/**
 * __drm_atomic_helper_hdmi_connector_destroy_state() - Releases all @drm_hdmi_connector_state resources
 * @hdmi_state: the HDMI connector state to release
 *
 * Release all resources stored in @drm_hdmi_connector_state without
 * actually freeing it. This is useful for drivers that subclass
 * @drm_hdmi_connector_state.
 */
void __drm_atomic_helper_hdmi_connector_destroy_state(struct drm_hdmi_connector_state *hdmi_state)
{
	__drm_atomic_helper_connector_destroy_state(&hdmi_state->base);
}
EXPORT_SYMBOL(__drm_atomic_helper_hdmi_connector_destroy_state);

/**
 * drm_atomic_helper_hdmi_connector_destroy_state() - Destroy a @drm_hdmi_connector_state object
 * @connector: the parent connector this state refers to
 * @state: the parent connector state to destroy
 *
 * Destroys an HDMI connector state previously created by
 * &drm_atomic_helper_hdmi_connector_reset() or
 * &drm_atomic_helper_hdmi_connector_duplicate_state().
 *
 * This helper is meant to be the default
 * &drm_connector_funcs.atomic_destroy_state implementation for
 * @drm_hdmi_connector that don't subclass @drm_hdmi_connector_state.
 */
void drm_atomic_helper_hdmi_connector_destroy_state(struct drm_connector *connector,
						    struct drm_connector_state *state)
{
	struct drm_hdmi_connector_state *hdmi_state =
		connector_state_to_hdmi_connector_state(state);

	__drm_atomic_helper_hdmi_connector_destroy_state(hdmi_state);
	kfree(hdmi_state);
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_destroy_state);

/**
 * drm_atomic_helper_hdmi_connector_get_property() - Reads out HDMI connector properties
 * @connector: the parent connector this state refers to
 * @state: the parent connector state to destroy
 * @property: Property instance being queried
 * @val: Raw value of the property to read into
 *
 * Read out a @drm_connector_state property value.
 *
 * This helper is meant to be the default
 * &drm_connector_funcs.atomic_get_property implementation for
 * @drm_hdmi_connector.
 */
int drm_atomic_helper_hdmi_connector_get_property(struct drm_connector *connector,
						  const struct drm_connector_state *state,
						  struct drm_property *property,
						  uint64_t *val)
{
	const struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	const struct drm_hdmi_connector_state *hdmi_state =
		connector_state_to_hdmi_connector_state(state);
	struct drm_device *drm = connector->dev;

	if (property == hdmi_connector->broadcast_rgb_property) {
		*val = hdmi_state->broadcast_rgb;
	} else {
		drm_dbg(drm, "Unknown property [PROP:%d:%s]\n",
			property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_get_property);

/**
 * drm_atomic_helper_hdmi_connector_set_property() - Decodes HDMI connector properties
 * @connector: the parent connector this state refers to
 * @state: the parent connector state to destroy
 * @property: Property instance being queried
 * @val: Raw value of the property to decode
 *
 * Decodes a property into an @drm_connector_state.
 *
 * This helper is meant to be the default
 * &drm_connector_funcs.atomic_set_property implementation for
 * @drm_hdmi_connector.
 */
int drm_atomic_helper_hdmi_connector_set_property(struct drm_connector *connector,
						  struct drm_connector_state *state,
						  struct drm_property *property,
						  uint64_t val)
{
	const struct drm_hdmi_connector *hdmi_connector =
		connector_to_hdmi_connector(connector);
	struct drm_hdmi_connector_state *hdmi_state =
		connector_state_to_hdmi_connector_state(state);
	struct drm_device *drm = connector->dev;

	if (property == hdmi_connector->broadcast_rgb_property) {
		hdmi_state->broadcast_rgb = val;
	} else {
		drm_dbg(drm, "Unknown property [PROP:%d:%s]\n",
			property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_set_property);

static const struct drm_display_mode *
connector_state_get_adjusted_mode(const struct drm_connector_state *state)
{
	struct drm_crtc *crtc = state->crtc;
	struct drm_crtc_state *crtc_state = crtc->state;

	return &crtc_state->adjusted_mode;
}

/**
 * drm_atomic_helper_hdmi_connector_atomic_check() - Helper to check HDMI connector atomic state
 * @connector: the parent connector this state refers to
 * @state: the parent connector state to check
 *
 * Provides a default connector state check handler for HDMI connectors.
 * Checks that a desired connector update is valid, and updates various
 * fields of derived state.
 *
 * Drivers that subclass @drm_hdmi_connector_state may still wish to
 * call this function to avoid duplication of error checking code.
 *
 * RETURNS:
 * Zero on success, or an errno code otherwise.
 */
int drm_atomic_helper_hdmi_connector_atomic_check(struct drm_connector *connector,
						  struct drm_atomic_state *state)
{
	struct drm_connector_state *old_state =
		drm_atomic_get_old_connector_state(state, connector);
	struct drm_hdmi_connector_state *old_hdmi_state =
		connector_state_to_hdmi_connector_state(old_state);
	struct drm_connector_state *new_state =
		drm_atomic_get_new_connector_state(state, connector);
	struct drm_hdmi_connector_state *new_hdmi_state =
		connector_state_to_hdmi_connector_state(new_state);

	if (old_hdmi_state->broadcast_rgb != new_hdmi_state->broadcast_rgb ||
	    old_hdmi_state->output_bpc != new_hdmi_state->output_bpc) {
		struct drm_crtc *crtc = new_state->crtc;
		struct drm_crtc_state *crtc_state;

		crtc_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		crtc_state->mode_changed = true;
	}

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_atomic_check);

/**
 * drm_atomic_helper_hdmi_connector_is_full_range() - Checks whether a state uses Full-Range RGB
 * @hdmi_connector: the HDMI connector this state refers to
 * @hdmi_state: the HDMI connector state to check
 *
 * RETURNS:
 * True if @hdmi_state requires a Full range RGB output, False otherwise
 */
bool
drm_atomic_helper_hdmi_connector_is_full_range(const struct drm_hdmi_connector *hdmi_connector,
					       const struct drm_hdmi_connector_state *hdmi_state)
{
	const struct drm_connector *connector = &hdmi_connector->base;
	const struct drm_connector_state *conn_state = &hdmi_state->base;
	const struct drm_display_mode *mode =
		connector_state_get_adjusted_mode(conn_state);
	const struct drm_display_info *display = &connector->display_info;

	if (hdmi_state->broadcast_rgb == DRM_HDMI_BROADCAST_RGB_FULL)
		return true;

	if (hdmi_state->broadcast_rgb == DRM_HDMI_BROADCAST_RGB_LIMITED)
		return false;

	if (!display->is_hdmi)
		return true;

	return drm_default_rgb_quant_range(mode);
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_is_full_range);

static const struct drm_prop_enum_list broadcast_rgb_names[] = {
	{ DRM_HDMI_BROADCAST_RGB_AUTO, "Automatic" },
	{ DRM_HDMI_BROADCAST_RGB_FULL, "Full" },
	{ DRM_HDMI_BROADCAST_RGB_LIMITED, "Limited 16:235" },
};

/*
 * drm_hdmi_connector_get_broadcast_rgb_name - Return a string for HDMI connector RGB broadcast selection
 * @broadcast_rgb: Broadcast RGB selection to compute name of
 *
 * Returns: the name of the Broadcast RGB selection, or NULL if the type
 * is not valid.
 */
const char *
drm_hdmi_connector_get_broadcast_rgb_name(enum drm_hdmi_broadcast_rgb broadcast_rgb)
{
	if (broadcast_rgb > DRM_HDMI_BROADCAST_RGB_LIMITED)
		return NULL;

	return broadcast_rgb_names[broadcast_rgb].name;
}
EXPORT_SYMBOL(drm_hdmi_connector_get_broadcast_rgb_name);

/**
 * drm_atomic_helper_hdmi_connector_print_state - Prints a @drm_hdmi_connector_state
 * @p: output printer
 * @state: Connector state to print
 *
 * Default implementation of @drm_connector_funcs.atomic_print_state for
 * a @drm_hdmi_connector_state.
 */
void drm_atomic_helper_hdmi_connector_print_state(struct drm_printer *p,
						  const struct drm_connector_state *state)
{
	const struct drm_hdmi_connector_state *hdmi_state =
		connector_state_to_hdmi_connector_state(state);

	drm_printf(p, "\tbroadcast_rgb=%s\n",
		   drm_hdmi_connector_get_broadcast_rgb_name(hdmi_state->broadcast_rgb));
	drm_printf(p, "\toutput_bpc=%u\n", hdmi_state->output_bpc);
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_print_state);

/**
 * drmm_hdmi_connector_init - Init a preallocated HDMI connector
 * @dev: DRM device
 * @hdmi_connector: A pointer to the HDMI connector to init
 * @connector_type: user visible type of the connector
 * @ddc: optional pointer to the associated ddc adapter
 * @max_bpc: Maximum bits per char the HDMI connector supports
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
			     struct i2c_adapter *ddc,
			     unsigned int max_bpc)
{
	struct drm_connector *connector = &hdmi_connector->base;
	struct drm_property *prop;
	int ret;

	if (connector_type != DRM_MODE_CONNECTOR_HDMIA ||
	    connector_type != DRM_MODE_CONNECTOR_HDMIB)
		return -EINVAL;

	ret = drmm_connector_init(dev, connector, funcs, connector_type, ddc);
	if (ret)
		return ret;

	prop = hdmi_connector->broadcast_rgb_property;
	if (!prop) {
		prop = drm_property_create_enum(dev, DRM_MODE_PROP_ENUM,
						"Broadcast RGB",
						broadcast_rgb_names,
						ARRAY_SIZE(broadcast_rgb_names));
		if (!prop)
			return -EINVAL;

		hdmi_connector->broadcast_rgb_property = prop;
	}

	drm_object_attach_property(&connector->base, prop,
				   DRM_HDMI_BROADCAST_RGB_AUTO);

	if (max_bpc) {
		if (!(max_bpc == 8 || max_bpc == 10 || max_bpc == 12))
			return -EINVAL;

		drm_connector_attach_hdr_output_metadata_property(connector);
		drm_connector_attach_max_bpc_property(connector, 8, max_bpc);
		hdmi_connector->max_bpc = max_bpc;
	}

	return 0;
}
EXPORT_SYMBOL(drmm_hdmi_connector_init);
