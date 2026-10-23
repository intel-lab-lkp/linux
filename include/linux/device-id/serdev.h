/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_DEVICE_ID_SERDEV_H
#define LINUX_DEVICE_ID_SERDEV_H

/* serdev */

/* Large enough to hold the KBUILD_MODNAME of any serdev client driver */
#define SERDEV_NAME_SIZE	40
#define SERDEV_MODULE_PREFIX	"serial:"

struct serdev_device_id {
	char name[SERDEV_NAME_SIZE];
	unsigned long driver_data;	/* Data private to the driver */
};

#endif /* ifndef LINUX_DEVICE_ID_SERDEV_H */
