/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _HID_LAMPARRAY_H
#define _HID_LAMPARRAY_H

#include <linux/hid.h>
#include <linux/err.h>
#include <linux/types.h>

struct lamparray;

/*
 * Optional initial LED state for lamparray_register().
 * Used to define the initial state of a LampArray's LEDs.
 */
struct lamparray_init_state {
	u8 r;
	u8 g;
	u8 b;
	u8 brightness;
};

#if IS_ENABLED(CONFIG_HID_LAMPARRAY)

/**
 * lamparray_is_supported_device() - check whether a HID device supports LampArray
 * @hdev: HID device to inspect
 *
 * Check whether the given HID device exposes a Lighting/LampArray application
 * collection as defined by the HID Lighting specification.
 *
 * This helper can be used by HID drivers to determine whether LampArray
 * functionality should be enabled for a device.
 *
 * Return: %true if LampArray support is detected, %false otherwise.
 */
bool lamparray_is_supported_device(struct hid_device *hdev);

/**
 * lamparray_register() - initialize LampArray support for a HID device
 * @hdev: HID device
 * @led_init_state: Optional LED state at init specification
 *
 * Allocate and initialize internal LampArray state for the given HID device.
 * The function parses required HID reports and fields and registers the
 * associated miscdevice and sysfs attributes.
 *
 * Registers a multicolor LED class device to expose the LampArray functionality
 * via the LED subsystem. If specified, the desired initial LED state is
 * applied. If led_init_state is NULL, a default state is applied (all LEDs off).
 *
 * Return: pointer to a LampArray handle on success, or ERR_PTR() on failure.
 */
struct lamparray *lamparray_register(struct hid_device *hdev,
				     const struct lamparray_init_state *led_init_state);

/**
 * lamparray_unregister() - tear down LampArray support
 * @la: LampArray handle returned by lamparray_register()
 *
 * Remove all resources associated with a LampArray instance.
 *
 * This unregisters the LED class device (if present), removes the miscdevice
 * and sysfs interfaces and frees all internal state associated with @la.
 */
void lamparray_unregister(struct lamparray *la);

/**
 * lamparray_suspend() - blank all lamps ahead of sleep transition
 * @la: LampArray handle returned by lamparray_register()
 *
 * Writes zeroes to the rgb values only, keeping the brightness, unless the
 * use_leds_uapi sysfs attribute is 0, in which case, it will return early
 * before writing anything. The cached state is left untouched so
 * lamparray_resume() can restore it.
 */
void lamparray_suspend(struct lamparray *la);

/**
 * lamparray_resume() - restore host control and LampArray state
 * @la: LampArray handle returned by lamparray_register()
 *
 * Disables autonomous mode (in case device returns to firmware control after suspend)
 * and restores the cached state of the device. If the use_leds_uapi attribute is 0,
 * it will return early and prevent any unwanted writing.
 */
void lamparray_resume(struct lamparray *la);

#else /* !CONFIG_HID_LAMPARRAY */

static inline bool lamparray_is_supported_device(struct hid_device *hdev)
{
	return false;
}

static inline struct lamparray *
lamparray_register(struct hid_device *hdev,
		   const struct lamparray_init_state *led_init_state)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline void lamparray_unregister(struct lamparray *la)
{
}

static inline void lamparray_suspend(struct lamparray *la)
{
}

static inline void lamparray_resume(struct lamparray *la)
{
}

#endif /* CONFIG_HID_LAMPARRAY */

#endif /* _HID_LAMPARRAY_H */
