/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _HID_LAMPARRAY_H
#define _HID_LAMPARRAY_H

#include <linux/types.h>
#include <linux/leds.h>
#include <linux/err.h>
#include <linux/errno.h>

struct hid_device;
struct lamparray;

/*
 * Optional initial LED state for lamparray_register().
 * Used to define the initial state of a LampArray's LEDs.
 */
struct lamparray_init_state {
	u8 r;
	u8 g;
	u8 b;
	enum led_brightness brightness;
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
 * If enabled, a multicolor LED class device is also registered to expose the
 * LampArray functionality via the LED subsystem. If specified, the desired
 * initial LED state is applied. If led_init_state is NULL, a default state is
 * applied.
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

#endif /* CONFIG_HID_LAMPARRAY */

#endif /* _HID_LAMPARRAY_H */
