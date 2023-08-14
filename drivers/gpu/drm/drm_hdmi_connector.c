// SPDX-License-Identifier: GPL-2.0+

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_mode.h>

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
}
EXPORT_SYMBOL(drm_atomic_helper_hdmi_connector_print_state);

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
