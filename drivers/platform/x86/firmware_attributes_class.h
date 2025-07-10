/* SPDX-License-Identifier: GPL-2.0 */

/* Firmware attributes class helper module */

#ifndef FW_ATTR_CLASS_H
#define FW_ATTR_CLASS_H

#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

extern const struct class firmware_attributes_class;

/**
 * struct fwat_device - The firmware-attributes device
 * @dev: The class device.
 * @attrs_kobj: The "attributes" root kobject.
 * @groups: Sysfs groups attached to the @attrs_kobj.
 */
struct fwat_device {
	struct device dev;
	struct kset *attrs_kset;
	const struct attribute_group **groups;
};

#define to_fwat_device(_d)	container_of_const(_d, struct fwat_device, dev)

struct fwat_device * __must_check
fwat_device_register(struct device *parent, const char *name, void *drvdata,
		     const struct attribute_group **groups);

void fwat_device_unregister(struct fwat_device *fwadev);

struct fwat_device * __must_check
devm_fwat_device_register(struct device *parent, const char *name, void *data,
			  const struct attribute_group **groups);

#endif /* FW_ATTR_CLASS_H */
