// SPDX-License-Identifier: MIT
/*
 * DRM Backlight Helpers
 * Copyright (c) 2014 David Herrmann
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/backlight.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <drm/drm_backlight.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_property.h>

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
 * This DRM backlight interface implements a generic per-connector LUMINANCE
 * property. The core is backend-agnostic: it does not talk to any hardware
 * itself, it only forwards luminance requests to a backend that a driver has
 * linked. The backend is described by a &struct drm_backlight_funcs. Today the
 * only backend is the backlight subsystem (&struct backlight_device), linked
 * with drm_backlight_link(), but other backends (DDC/CI, MIPI-DCS, ...) can be
 * added by providing a different set of operations without changing the core.
 *
 * A &struct drm_backlight is embedded in every &struct drm_connector and
 * initialized by the DRM core (drm_backlight_connector_init()); drivers do not
 * allocate it. Drivers link a backend once it is available by calling
 * drm_backlight_link(); this creates the connector's LUMINANCE property with
 * the backend's range. Passing NULL unlinks the backend. Hardware is only ever
 * touched from a workqueue, so slow backends never stall an atomic commit.
 */

static LIST_HEAD(drm_backlight_list);
static DEFINE_SPINLOCK(drm_backlight_lock);

/* caller must hold @drm_backlight_lock */
static bool __drm_backlight_is_linked(struct drm_backlight *b)
{
	lockdep_assert_held(&drm_backlight_lock);
	/* a backlight is live while it is on @drm_backlight_list */
	return !list_empty(&b->list);
}

/*
 * Return the linked &backlight_device if the current backend is the backlight
 * subsystem, or NULL otherwise. The legacy-sysfs takeover accounting only
 * applies to that backend.
 */
static const struct drm_backlight_funcs drm_backlight_bd_funcs;

static struct backlight_device *drm_backlight_bd(struct drm_backlight *b)
{
	if (b->funcs != &drm_backlight_bd_funcs)
		return NULL;
	return b->backend;
}

/* caller must hold @drm_backlight_lock */
static void __drm_backlight_schedule(struct drm_backlight *b)
{
	lockdep_assert_held(&drm_backlight_lock);
	if (__drm_backlight_is_linked(b))
		schedule_work(&b->work);
}

static void __drm_backlight_worker(struct work_struct *w)
{
	struct drm_backlight *b = container_of(w, struct drm_backlight, work);
	static char *ep[] = { "BACKLIGHT=1", NULL };
	const struct drm_backlight_funcs *funcs;
	bool send_uevent;
	unsigned int v;

	scoped_guard(spinlock, &drm_backlight_lock) {
		send_uevent = b->changed;
		b->changed = false;
		v = b->set_value;
		funcs = b->funcs;
	}

	/*
	 * The backend stays valid here: drm_backlight_do_unlink() clears
	 * @backend and drops its reference only after cancel_work_sync(), so an
	 * in-flight worker always sees a live backend.
	 */
	if (funcs && funcs->set_luminance)
		WARN_ON(funcs->set_luminance(b, v));

	if (send_uevent && b->connector->kdev)
		kobject_uevent_env(&b->connector->kdev->kobj, KOBJ_CHANGE, ep);
}

/* caller must hold @drm_backlight_lock */
static void __drm_backlight_set(struct drm_backlight *b, unsigned int v)
{
	unsigned int max = 0;
	bool can_disable = false;

	lockdep_assert_held(&drm_backlight_lock);

	if (!b->funcs || !b->funcs->get_range)
		return;

	b->funcs->get_range(b, &max, &can_disable);
	if (!max)
		return;

	/* clamp to the backend maximum */
	b->set_value = min(v, max);
	__drm_backlight_schedule(b);
}

/* caller must hold @drm_backlight_lock */
static void __drm_backlight_readback(struct drm_backlight *b, unsigned int v)
{
	struct drm_connector *connector = b->connector;
	unsigned int max = 0;
	bool can_disable = false;

	lockdep_assert_held(&drm_backlight_lock);

	if (!b->funcs || !b->funcs->get_range)
		return;

	b->funcs->get_range(b, &max, &can_disable);
	if (!max)
		return;

	/*
	 * Reflect a hardware-side brightness change (firmware hotkeys, or a
	 * legacy sysfs write while not inhibited) back into the connector's
	 * committed luminance so a read-back returns the real value.
	 */
	if (connector->state)
		connector->state->luminance = min(v, max);
}

/*
 * Create and attach the per-connector LUMINANCE property. Runs in process
 * context (driver register path) with no spinlock held, since it allocates.
 * The property's min/max are baked in for its lifetime, so it is never mutated
 * afterwards (which would race &drm_mode_getproperty_ioctl); it is freed by
 * drm_mode_config_cleanup().
 */
static int drm_backlight_create_property(struct drm_connector *connector,
					 unsigned int max, bool can_disable)
{
	struct drm_device *dev = connector->dev;
	struct drm_property *prop;
	unsigned int min = can_disable ? 0 : 1;

	prop = drm_property_create_range(dev, DRM_MODE_PROP_ATOMIC,
					 "LUMINANCE", min, max);
	if (!prop)
		return -ENOMEM;

	prop->is_luminance = true;
	connector->luminance_property = prop;
	drm_object_attach_property(&connector->base, prop, min);
	if (connector->state)
		connector->state->luminance = min;

	return 0;
}

static void drm_backlight_do_unlink(struct drm_backlight *b)
{
	struct backlight_device *bd;
	unsigned int clients;

	/*
	 * Stop new work first, but leave @backend in place so an in-flight
	 * worker keeps a valid backend to operate on. Capture the linked
	 * backlight_device (if any) while @funcs is still set.
	 */
	scoped_guard(spinlock, &drm_backlight_lock) {
		if (!b->funcs)
			return;
		bd = drm_backlight_bd(b);
		clients = b->luminance_clients;
		b->funcs = NULL;
		list_del_init(&b->list);
	}

	cancel_work_sync(&b->work);

	scoped_guard(spinlock, &drm_backlight_lock) {
		if (clients && bd)
			atomic_sub(clients, &bd->drm_takeover);
		b->backend = NULL;
	}

	backlight_device_unref(bd);
}

/**
 * drm_backlight_connector_init - initialize a connector's embedded backlight
 * @connector: connector to initialize
 *
 * Called by the DRM core from drm_connector_init(). Drivers never call this.
 */
void drm_backlight_connector_init(struct drm_connector *connector)
{
	struct drm_backlight *b = &connector->backlight;

	b->connector = connector;
	INIT_LIST_HEAD(&b->list);
	INIT_WORK(&b->work, __drm_backlight_worker);
}

/**
 * drm_backlight_connector_cleanup - tear down a connector's embedded backlight
 * @connector: connector being cleaned up
 *
 * Called by the DRM core from drm_connector_cleanup(). The LUMINANCE property
 * itself is freed by drm_mode_config_cleanup().
 */
void drm_backlight_connector_cleanup(struct drm_connector *connector)
{
	struct drm_backlight *b = &connector->backlight;

	WARN_ON(__drm_backlight_is_linked(b));
	WARN_ON(b->funcs);
	WARN_ON(b->luminance_clients);
}

/**
 * drm_backlight_unregister - unlink a connector's backlight on unregister
 * @connector: connector being unregistered
 *
 * Called by the DRM core from drm_connector_unregister() as a safety net in
 * case a driver did not unlink its backend itself.
 */
void drm_backlight_unregister(struct drm_connector *connector)
{
	drm_backlight_do_unlink(&connector->backlight);
}

/**
 * drm_backlight_link - link a backlight device to a connector
 * @connector: connector to modify
 * @bd: backlight device to link, or NULL to unlink
 *
 * Establish the link between a connector's LUMINANCE property and a registered
 * backlight_device. On the first link the connector's LUMINANCE property is
 * created with the backend's range. Passing NULL unlinks any linked device.
 *
 * User-space cannot create or modify this link.
 *
 * Returns: 0 on success, or a negative error code if the property could not be
 * created (in which case no backend is linked).
 */
int drm_backlight_link(struct drm_connector *connector,
		       struct backlight_device *bd)
{
	struct drm_backlight *b = &connector->backlight;
	unsigned int max;
	int ret;

	if (!bd) {
		drm_backlight_do_unlink(b);
		return 0;
	}

	/* Retarget: drop any previously linked backend first. */
	if (b->funcs)
		drm_backlight_do_unlink(b);

	max = bd->props.max_brightness;
	if (max && !connector->luminance_property) {
		ret = drm_backlight_create_property(connector, max, false);
		if (ret)
			return ret;
	}

	scoped_guard(spinlock, &drm_backlight_lock) {
		b->funcs = &drm_backlight_bd_funcs;
		b->backend = bd;
		backlight_device_ref(bd);
		list_add(&b->list, &drm_backlight_list);
		if (b->luminance_clients)
			atomic_add(b->luminance_clients, &bd->drm_takeover);
		__drm_backlight_readback(b, bd->props.brightness);
		b->changed = true;
		__drm_backlight_set(b, bd->props.brightness);
	}

	return 0;
}
EXPORT_SYMBOL(drm_backlight_link);

/**
 * drm_backlight_get_device - get the backlight_device linked to a connector
 * @connector: connector to query
 *
 * Returns the &backlight_device linked to @connector, or NULL if no backlight
 * subsystem device is linked.
 */
struct backlight_device *drm_backlight_get_device(struct drm_connector *connector)
{
	guard(spinlock)(&drm_backlight_lock);
	return drm_backlight_bd(&connector->backlight);
}
EXPORT_SYMBOL(drm_backlight_get_device);

/**
 * drm_backlight_inhibit_legacy - disable legacy sysfs control of the backend
 * @connector: connector whose backlight should be inhibited
 *
 * Record that one more luminance-aware DRM client has taken over this
 * connector's backlight. While any clients are recorded, writes to the linked
 * backlight_device's legacy ``brightness`` sysfs attribute return ``-EBUSY``.
 * The takeover follows the linked device if the link changes.
 *
 * Calls must be balanced with drm_backlight_uninhibit_legacy().
 */
void drm_backlight_inhibit_legacy(struct drm_connector *connector)
{
	struct drm_backlight *b = &connector->backlight;
	struct backlight_device *bd;

	guard(spinlock)(&drm_backlight_lock);
	b->luminance_clients++;
	bd = drm_backlight_bd(b);
	if (bd)
		atomic_inc(&bd->drm_takeover);
}
EXPORT_SYMBOL(drm_backlight_inhibit_legacy);

/**
 * drm_backlight_uninhibit_legacy - re-enable legacy sysfs control
 * @connector: connector to uninhibit
 *
 * Balances a previous drm_backlight_inhibit_legacy() call.
 */
void drm_backlight_uninhibit_legacy(struct drm_connector *connector)
{
	struct drm_backlight *b = &connector->backlight;
	struct backlight_device *bd;

	guard(spinlock)(&drm_backlight_lock);
	if (WARN_ON(b->luminance_clients == 0))
		return;
	b->luminance_clients--;
	bd = drm_backlight_bd(b);
	if (bd)
		atomic_dec(&bd->drm_takeover);
}
EXPORT_SYMBOL(drm_backlight_uninhibit_legacy);

/**
 * drm_backlight_inhibit_legacy_all - inhibit legacy sysfs on every connector
 * @dev: DRM device whose connectors should be inhibited
 *
 * Used when a client declares it is luminance-aware via
 * DRM_CLIENT_CAP_LUMINANCE.
 */
void drm_backlight_inhibit_legacy_all(struct drm_device *dev)
{
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;

	drm_connector_list_iter_begin(dev, &iter);
	drm_for_each_connector_iter(connector, &iter)
		drm_backlight_inhibit_legacy(connector);
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
		drm_backlight_uninhibit_legacy(connector);
	drm_connector_list_iter_end(&iter);
}
EXPORT_SYMBOL(drm_backlight_uninhibit_legacy_all);

/**
 * drm_backlight_set_luminance - request a luminance change on a connector
 * @connector: connector to update
 * @value: luminance value to apply
 *
 * Clamp @value to the backend range and schedule the hardware update. Safe to
 * call from an atomic commit tail: the actual hardware access happens later
 * from a workqueue.
 */
void drm_backlight_set_luminance(struct drm_connector *connector,
				 unsigned int value)
{
	guard(spinlock)(&drm_backlight_lock);
	__drm_backlight_set(&connector->backlight, value);
}
EXPORT_SYMBOL(drm_backlight_set_luminance);

/* backlight_device backend ------------------------------------------------- */

static int drm_backlight_bd_set_luminance(struct drm_backlight *b,
					  unsigned int value)
{
	struct backlight_device *bd = b->backend;
	int rc;

	rc = backlight_set_brightness(bd, value, BACKLIGHT_UPDATE_DRM);
	if (rc)
		backlight_set_brightness(bd, U16_MAX, BACKLIGHT_UPDATE_DRM);

	return rc;
}

static int drm_backlight_bd_get_luminance(struct drm_backlight *b,
					  unsigned int *value)
{
	struct backlight_device *bd = b->backend;

	if (!bd)
		return -ENODEV;
	*value = bd->props.brightness;

	return 0;
}

static void drm_backlight_bd_get_range(struct drm_backlight *b,
				       unsigned int *max, bool *can_disable)
{
	struct backlight_device *bd = b->backend;

	*max = bd ? bd->props.max_brightness : 0;
	/*
	 * A generic backlight_device gives no guarantee that a value of 0 turns
	 * the panel fully off, so keep 0 reserved for the DPMS-off sentinel and
	 * expose a 1..max range.
	 */
	*can_disable = false;
}

static const struct drm_backlight_funcs drm_backlight_bd_funcs = {
	.set_luminance = drm_backlight_bd_set_luminance,
	.get_luminance = drm_backlight_bd_get_luminance,
	.get_range = drm_backlight_bd_get_range,
};

static int drm_backlight_notify(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct backlight_device *bd = data;
	struct drm_backlight *b;

	switch (event) {
	case BACKLIGHT_UNREGISTERED:
		/*
		 * Unlink every connector using @bd. drm_backlight_do_unlink()
		 * sleeps (cancel_work_sync()), so it cannot run under the list
		 * spinlock; re-scan for the next match after each unlink.
		 */
		for (;;) {
			struct drm_backlight *found = NULL;

			scoped_guard(spinlock, &drm_backlight_lock) {
				list_for_each_entry(b, &drm_backlight_list, list) {
					if (drm_backlight_bd(b) == bd) {
						found = b;
						break;
					}
				}
			}
			if (!found)
				break;
			drm_backlight_do_unlink(found);
		}
		break;
	case BACKLIGHT_BRIGHTNESS_CHANGED:
		scoped_guard(spinlock, &drm_backlight_lock) {
			list_for_each_entry(b, &drm_backlight_list, list)
				if (drm_backlight_bd(b) == bd)
					__drm_backlight_readback(b, bd->props.brightness);
		}
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
