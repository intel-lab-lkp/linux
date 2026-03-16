/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_EXTCON_INTERNAL_H__
#define __LINUX_EXTCON_INTERNAL_H__

#include <linux/extcon-provider.h>

/**
 * struct extcon_dev - An extcon device represents one external connector.
 * @name:		The name of this extcon device. Parent device name is
 *			used if NULL.
 * @supported_cable:	Array of supported cable names ending with EXTCON_NONE.
 *			If supported_cable is NULL, cable name related APIs
 *			are disabled.
 * @dev:		Device of this extcon.
 * @id:			Unique device ID of this extcon.
 * @state:		Attach/detach state of this extcon. Do not provide at
 *			register-time.
 * @nh_all:		Notifier for the state change events for all supported
 *			external connectors from this extcon.
 * @nh:			Notifier for the state change events from this extcon
 * @entry:		To support list of extcon devices so that users can
 *			search for extcon devices based on the extcon name.
 * @lock:		Protects device state and serialises device registration
 * @max_supported:	Internal value to store the number of cables.
 * @extcon_dev_type:	Device_type struct to provide attribute_groups
 *			customized for each extcon device.
 * @cables:		Sysfs subdirectories. Each represents one cable.
 *
 * In most cases, users only need to provide "User initializing data" of
 * this struct when registering an extcon. In some exceptional cases,
 * optional callbacks may be needed. However, the values in "internal data"
 * are overwritten by register function.
 */
struct extcon_dev {
	/* Optional user initializing data */
	const char *name;
	const unsigned int *supported_cable;

	/* Internal data. Please do not set. */
	struct device dev;
	unsigned int id;
	struct raw_notifier_head nh_all;
	struct raw_notifier_head *nh;
	struct list_head entry;
	int max_supported;
	spinlock_t lock;	/* could be called by irq handler */
	u32 state;

	/* /sys/class/extcon/.../cable.n/... */
	struct device_type extcon_dev_type;
	struct extcon_cable *cables;
};

#endif /* __LINUX_EXTCON_INTERNAL_H__ */
