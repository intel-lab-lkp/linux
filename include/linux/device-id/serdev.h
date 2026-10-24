/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_DEVICE_ID_SERDEV_H
#define LINUX_DEVICE_ID_SERDEV_H

#ifdef __KERNEL__
typedef unsigned long kernel_ulong_t;
#endif

/* serdev */

#define SERDEV_NAME_SIZE	40
#define SERDEV_MODULE_PREFIX	"serial:"

struct serdev_device_id {
	char name[SERDEV_NAME_SIZE];
	kernel_ulong_t driver_data;	/* Data private to the driver */
};

#endif /* ifndef LINUX_DEVICE_ID_SERDEV_H */
