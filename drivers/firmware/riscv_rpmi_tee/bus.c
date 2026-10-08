// SPDX-License-Identifier: GPL-2.0-only
/*
 * RISC-V RPMI TEE bus
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmi_tee.h>
#include <linux/slab.h>

#define RPMI_TEE_UEVENT_MODALIAS_FMT	"rpmi_tee:%pUb"

static DEFINE_IDA(rpmi_tee_bus_id);

static int rpmi_tee_device_match(struct device *dev,
				 const struct device_driver *drv)
{
	const struct rpmi_tee_device_id *id_table;
	struct rpmi_tee_device *rdev = to_rpmi_tee_dev(dev);

	id_table = to_rpmi_tee_drv(drv)->id_table;
	if (!id_table)
		return 0;

	while (!uuid_is_null(&id_table->uuid)) {
		if (uuid_equal(&rdev->uuid, &id_table->uuid))
			return 1;
		id_table++;
	}

	return 0;
}

static int rpmi_tee_device_probe(struct device *dev)
{
	struct rpmi_tee_driver *rdrv = to_rpmi_tee_drv(dev->driver);

	return rdrv->probe(to_rpmi_tee_dev(dev));
}

static void rpmi_tee_device_remove(struct device *dev)
{
	struct rpmi_tee_driver *rdrv = to_rpmi_tee_drv(dev->driver);

	if (rdrv->remove)
		rdrv->remove(to_rpmi_tee_dev(dev));
}

static int rpmi_tee_device_uevent(const struct device *dev,
				  struct kobj_uevent_env *env)
{
	const struct rpmi_tee_device *rdev = to_rpmi_tee_dev(dev);

	return add_uevent_var(env, "MODALIAS=" RPMI_TEE_UEVENT_MODALIAS_FMT,
			      &rdev->uuid);
}

static ssize_t endpoint_id_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct rpmi_tee_device *rdev = to_rpmi_tee_dev(dev);

	return sysfs_emit(buf, "0x%x\n", rdev->endpoint_id);
}
static DEVICE_ATTR_RO(endpoint_id);

static ssize_t uuid_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct rpmi_tee_device *rdev = to_rpmi_tee_dev(dev);

	return sysfs_emit(buf, "%pUb\n", &rdev->uuid);
}
static DEVICE_ATTR_RO(uuid);

static ssize_t modalias_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct rpmi_tee_device *rdev = to_rpmi_tee_dev(dev);

	return sysfs_emit(buf, RPMI_TEE_UEVENT_MODALIAS_FMT, &rdev->uuid);
}
static DEVICE_ATTR_RO(modalias);

static struct attribute *rpmi_tee_device_attrs[] = {
	&dev_attr_endpoint_id.attr,
	&dev_attr_uuid.attr,
	&dev_attr_modalias.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rpmi_tee_device);

const struct bus_type rpmi_tee_bus_type = {
	.name		= "rpmi_tee",
	.match		= rpmi_tee_device_match,
	.probe		= rpmi_tee_device_probe,
	.remove		= rpmi_tee_device_remove,
	.uevent		= rpmi_tee_device_uevent,
	.dev_groups	= rpmi_tee_device_groups,
};
EXPORT_SYMBOL_GPL(rpmi_tee_bus_type);

int rpmi_tee_driver_register(struct rpmi_tee_driver *driver,
			     struct module *owner, const char *mod_name)
{
	if (!driver->probe || !driver->id_table)
		return -EINVAL;

	driver->driver.bus = &rpmi_tee_bus_type;
	driver->driver.name = driver->name;
	driver->driver.owner = owner;
	driver->driver.mod_name = mod_name;

	return driver_register(&driver->driver);
}
EXPORT_SYMBOL_GPL(rpmi_tee_driver_register);

void rpmi_tee_driver_unregister(struct rpmi_tee_driver *driver)
{
	driver_unregister(&driver->driver);
}
EXPORT_SYMBOL_GPL(rpmi_tee_driver_unregister);

static void rpmi_tee_device_release(struct device *dev)
{
	struct rpmi_tee_device *rdev = to_rpmi_tee_dev(dev);

	ida_free(&rpmi_tee_bus_id, rdev->id);
	kfree(rdev);
}

struct rpmi_tee_device *
rpmi_tee_device_register(const uuid_t *uuid, u32 endpoint_id,
			 const struct rpmi_tee_ops *ops, struct device *parent)
{
	struct rpmi_tee_device *rdev;
	int id;
	int ret;

	if (!uuid || !ops)
		return ERR_PTR(-EINVAL);

	id = ida_alloc_min(&rpmi_tee_bus_id, 1, GFP_KERNEL);
	if (id < 0)
		return ERR_PTR(id);

	rdev = kzalloc_obj(*rdev, GFP_KERNEL);
	if (!rdev) {
		ida_free(&rpmi_tee_bus_id, id);
		return ERR_PTR(-ENOMEM);
	}

	rdev->dev.parent = parent;
	rdev->dev.bus = &rpmi_tee_bus_type;
	rdev->dev.release = rpmi_tee_device_release;
	dev_set_name(&rdev->dev, "rpmi-tee-%d", id);

	rdev->id = id;
	rdev->endpoint_id = endpoint_id;
	rdev->ops = ops;
	uuid_copy(&rdev->uuid, uuid);

	ret = device_register(&rdev->dev);
	if (ret) {
		put_device(&rdev->dev);
		return ERR_PTR(ret);
	}

	return rdev;
}
EXPORT_SYMBOL_GPL(rpmi_tee_device_register);

void rpmi_tee_device_unregister(struct rpmi_tee_device *rdev)
{
	if (rdev)
		device_unregister(&rdev->dev);
}
EXPORT_SYMBOL_GPL(rpmi_tee_device_unregister);

static int __init rpmi_tee_bus_init(void)
{
	return bus_register(&rpmi_tee_bus_type);
}

subsys_initcall(rpmi_tee_bus_init);

static void __exit rpmi_tee_bus_exit(void)
{
	bus_unregister(&rpmi_tee_bus_type);
	ida_destroy(&rpmi_tee_bus_id);
}

module_exit(rpmi_tee_bus_exit);

MODULE_DESCRIPTION("RISC-V RPMI TEE bus");
MODULE_LICENSE("GPL");
