// SPDX-License-Identifier: MIT
/*
 * DRM Backlight Helpers
 * Copyright (c) 2014 David Herrmann
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/backlight.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <drm/drm_backlight.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_mode_config.h>

/**
 * DOC: Backlight Devices
 *
 * Backlight devices have always been managed as a separate subsystem,
 * independent of DRM. They are usually controlled via separate hardware
 * interfaces than the display controller, so the split works out fine.
 * However, backlight brightness is a property of a display, and thus a
 * property of a DRM connector. We already manage DPMS states via connector
 * properties, so it is natural to keep brightness control at the same place.
 *
 * This DRM backlight interface implements generic backlight properties on
 * connectors. It does not handle any hardware backends but simply forwards
 * the requests to a linked backlight device. The links between connectors and
 * backlight devices are established by DRM drivers; user-space cannot create
 * or modify these links. A 'change' uevent is sent whenever the brightness is
 * updated.
 *
 * Drivers have to call drm_backlight_alloc() after allocating a connector via
 * drm_connector_init(). This will automatically add a backlight device to the
 * given connector. Drivers must then link a hardware backlight by calling
 * drm_backlight_link() with the registered backlight_device. If no link is
 * established, the DRM backlight property reports an empty range and
 * brightness changes are no-ops.
 */

struct drm_backlight {
	struct list_head list;
	struct drm_connector *connector;
	struct backlight_device *link;
	/*
	 * Number of luminance-aware DRM clients that have taken over this
	 * connector's backlight. While > 0, legacy sysfs writes to the
	 * linked backlight_device return -EBUSY. Protected by
	 * drm_backlight_lock.
	 */
	unsigned int luminance_clients;
};

static LIST_HEAD(drm_backlight_list);
static DEFINE_SPINLOCK(drm_backlight_lock);

/* caller must hold @drm_backlight_lock */
static bool __drm_backlight_is_registered(struct drm_backlight *b)
{
	lockdep_assert_held(&drm_backlight_lock);
	/* a device is live if it is linked to @drm_backlight_list */
	return !list_empty(&b->list);
}

/* caller must hold @drm_backlight_lock */
static void __drm_backlight_real_changed(struct drm_backlight *b, uint64_t v)
{
	unsigned int max, set;

	lockdep_assert_held(&drm_backlight_lock);

	if (!b->link)
		return;

	max = b->link->props.max_brightness;
	if (max < 1)
		return;

	set = v;
	if (set >= max)
		set = max;
}

/**
 * __drm_backlight_update_prop_range - update the luminance property range
 * @b: backlight device
 *
 * Updates the luminance property range based on the linked backlight device's
 * max_brightness. If no device is linked, sets range to 0-0 to indicate
 * unavailability.
 */
static void __drm_backlight_update_prop_range(struct drm_backlight *b)
{
	struct drm_device *dev = b->connector->dev;
	struct drm_property *prop = dev->mode_config.luminance_property;
	unsigned int max = 0;

	lockdep_assert_held(&drm_backlight_lock);

	if (b->link && b->link->props.max_brightness > 0)
		max = b->link->props.max_brightness;

	/* Update property range to match hardware capabilities.
	 * Range of 0-0 indicates no backing device.
	 * Range of 1-max for normal operation (0 reserved for display off).
	 */
	if (prop->values[1] != max) {
		prop->values[0] = max ? 1 : 0;
		prop->values[1] = max;
	}
}

/* caller must hold @drm_backlight_lock */
static bool __drm_backlight_link(struct drm_backlight *b,
				 struct backlight_device *bd)
{
	if (bd == b->link)
		return false;

	backlight_device_unref(b->link);
	b->link = bd;
	backlight_device_ref(b->link);
	if (bd)
		__drm_backlight_real_changed(b, bd->props.brightness);
	__drm_backlight_update_prop_range(b);

	return true;
}

/**
 * drm_backlight_alloc - add backlight capability to a connector
 * @connector: connector to add backlight to
 *
 * This allocates a new DRM-backlight device and attaches it to @connector.
 * This *must* be called before registering the connector. The backlight
 * device will be automatically registered in sync with the connector. It will
 * also get removed once the connector is removed.
 *
 * No hardware backlight is linked by default. Drivers must call
 * drm_backlight_link() to associate a registered backlight_device with the
 * connector. User-space cannot create or modify this link.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int drm_backlight_alloc(struct drm_connector *connector)
{
	struct drm_mode_config *config = &connector->dev->mode_config;
	struct drm_backlight *b;

	b = kzalloc_obj(*b, GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	INIT_LIST_HEAD(&b->list);
	b->connector = connector;
	connector->backlight = b;

	drm_object_attach_property(&connector->base,
				   config->luminance_property, 0);

	return 0;
}
EXPORT_SYMBOL(drm_backlight_alloc);

void drm_backlight_free(struct drm_connector *connector)
{
	struct drm_backlight *b = connector->backlight;

	if (!b)
		return;

	WARN_ON(__drm_backlight_is_registered(b));
	WARN_ON(b->link);

	kfree(b);
	connector->backlight = NULL;
}
EXPORT_SYMBOL(drm_backlight_free);

void drm_backlight_register(struct drm_backlight *b)
{
	if (!b)
		return;

	WARN_ON(__drm_backlight_is_registered(b));

	guard(spinlock)(&drm_backlight_lock);
	list_add(&b->list, &drm_backlight_list);
}
EXPORT_SYMBOL(drm_backlight_register);

void drm_backlight_unregister(struct drm_backlight *b)
{
	if (!b)
		return;

	WARN_ON(!__drm_backlight_is_registered(b));

	scoped_guard(spinlock, &drm_backlight_lock) {
		list_del_init(&b->list);
		__drm_backlight_link(b, NULL);
	}
}
EXPORT_SYMBOL(drm_backlight_unregister);

/**
 * drm_backlight_link - link a backlight device to a DRM backlight
 * @b: DRM backlight to modify
 * @bd: backlight device to link, or NULL to unlink
 *
 * Establish the link between a DRM connector's backlight property and a
 * registered backlight_device. Drivers must call this with the
 * backlight_device they registered for the connector. Passing NULL unlinks
 * any previously linked device.
 *
 * The caller is responsible for ensuring @bd remains valid until either it
 * is unlinked via drm_backlight_link(b, NULL) or the connector is
 * unregistered.
 *
 * Whenever a hardware backlight is linked or unlinked, a uevent with
 * "BACKLIGHT=1" is generated on the connector.
 */
void drm_backlight_link(struct drm_backlight *b, struct backlight_device *bd)
{
	if (!b)
		return;

	guard(spinlock)(&drm_backlight_lock);
	__drm_backlight_link(b, bd);
}
EXPORT_SYMBOL(drm_backlight_link);

/**
 * drm_backlight_get_device - get the backlight_device linked to a DRM backlight
 * @b: DRM backlight
 *
 * Returns the &backlight_device linked to @b, or NULL if no device is linked
 * or @b is NULL. The caller must hold the appropriate lock to prevent the
 * link from changing while the pointer is in use.
 */
struct backlight_device *drm_backlight_get_device(struct drm_backlight *b)
{
	if (!b)
		return NULL;

	guard(spinlock)(&drm_backlight_lock);
	return b->link;
}
EXPORT_SYMBOL(drm_backlight_get_device);

/**
 * drm_backlight_inhibit_legacy - disable legacy sysfs control of the linked bd
 * @b: DRM backlight to inhibit
 *
 * Record that one more luminance-aware DRM client has taken over @b. While
 * any clients are recorded, writes to the linked backlight_device's legacy
 * ``brightness`` sysfs attribute return ``-EBUSY``. The takeover follows
 * @b->link if the link changes.
 *
 * Calls must be balanced with drm_backlight_uninhibit_legacy().
 */
void drm_backlight_inhibit_legacy(struct drm_backlight *b)
{
	if (!b)
		return;
}
EXPORT_SYMBOL(drm_backlight_inhibit_legacy);

/**
 * drm_backlight_uninhibit_legacy - re-enable legacy sysfs control
 * @b: DRM backlight to uninhibit
 *
 * Balances a previous drm_backlight_inhibit_legacy() call. When the last
 * luminance-aware client goes away, legacy sysfs writes are allowed again.
 */
void drm_backlight_uninhibit_legacy(struct drm_backlight *b)
{
	if (!b)
		return;
}
EXPORT_SYMBOL(drm_backlight_uninhibit_legacy);

/**
 * drm_backlight_inhibit_legacy_all - inhibit legacy sysfs on every connector
 * @dev: DRM device whose connectors should be inhibited
 *
 * Walks all connectors on @dev and calls drm_backlight_inhibit_legacy() on
 * each connector that has a DRM backlight attached. Used when a client
 * declares it is luminance-aware via DRM_CLIENT_CAP_LUMINANCE.
 */
void drm_backlight_inhibit_legacy_all(struct drm_device *dev)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter)
		drm_backlight_inhibit_legacy(connector->backlight);
	drm_connector_list_iter_end(&iter);
}
EXPORT_SYMBOL(drm_backlight_inhibit_legacy_all);

/**
 * drm_backlight_uninhibit_legacy_all - reverse drm_backlight_inhibit_legacy_all()
 * @dev: DRM device whose connectors should be uninhibited
 */
void drm_backlight_uninhibit_legacy_all(struct drm_device *dev)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter)
		drm_backlight_uninhibit_legacy(connector->backlight);
	drm_connector_list_iter_end(&iter);
}
EXPORT_SYMBOL(drm_backlight_uninhibit_legacy_all);

void drm_backlight_set_luminance(struct drm_backlight *b, unsigned int value)
{
	guard(spinlock)(&drm_backlight_lock);
	__drm_backlight_real_changed(b, value);
}
EXPORT_SYMBOL(drm_backlight_set_luminance);

static int drm_backlight_notify(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct backlight_device *bd = data;
	struct drm_backlight *b;

	guard(spinlock)(&drm_backlight_lock);

	switch (event) {
	case BACKLIGHT_UNREGISTERED:
		list_for_each_entry(b, &drm_backlight_list, list)
			if (b->link == bd)
				__drm_backlight_link(b, NULL);

		break;
	case BACKLIGHT_BRIGHTNESS_CHANGED:
		/* Update DRM property value when hardware backlight changes */
		list_for_each_entry(b, &drm_backlight_list, list)
			if (b->link == bd)
				__drm_backlight_real_changed(b, bd->props.brightness);

		break;
	}

	return 0;
}

static struct notifier_block drm_backlight_notifier = {
	.notifier_call = drm_backlight_notify,
};

int drm_backlight_init(void)
{
	return backlight_register_notifier(&drm_backlight_notifier);
}

void drm_backlight_exit(void)
{
	backlight_unregister_notifier(&drm_backlight_notifier);
}
