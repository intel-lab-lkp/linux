// SPDX-License-Identifier: GPL-2.0-or-later

/* Firmware attributes class helper module */

#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include "firmware_attributes_class.h"

const struct class firmware_attributes_class = {
	.name = "firmware-attributes",
};
EXPORT_SYMBOL_GPL(firmware_attributes_class);

static ssize_t fwat_attrs_kobj_show(struct kobject *kobj, struct attribute *attr,
				    char *buf)
{
	const struct fwat_attribute *fattr = to_fwat_attribute(attr);
	struct fwat_device *fadev = to_fwat_device(kobj);

	if (!fattr->show)
		return -ENOENT;

	return fattr->show(fadev->dev, fattr, buf);
}

static ssize_t fwat_attrs_kobj_store(struct kobject *kobj, struct attribute *attr,
				     const char *buf, size_t count)
{
	const struct fwat_attribute *fattr = to_fwat_attribute(attr);
	struct fwat_device *fadev = to_fwat_device(kobj);

	if (!fattr->store)
		return -ENOENT;

	return fattr->store(fadev->dev, fattr, buf, count);
}

static const struct sysfs_ops fwat_attrs_kobj_ops = {
	.show	= fwat_attrs_kobj_show,
	.store	= fwat_attrs_kobj_store,
};

static void fwat_attrs_kobj_release(struct kobject *kobj)
{
	struct fwat_device *fadev = to_fwat_device(kobj);

	kfree(fadev);
}

static const struct kobj_type fwat_attrs_ktype = {
	.sysfs_ops	= &fwat_attrs_kobj_ops,
	.release	= fwat_attrs_kobj_release,
};

/**
 * fwat_device_register - Create and register a firmware-attributes class
 *			  device
 * @parent: Parent device
 * @name: Name of the class device
 * @data: Drvdata of the class device
 * @groups: Sysfs groups for the custom `fwat_attrs_ktype` kobj_type
 *
 * NOTE: @groups are attached to the .attrs_kobj of the new fwat_device which
 * has a custom ktype, which makes use of `struct fwat_attribute` to embed
 * attributes.
 *
 * Return: pointer to the new fwat_device on success, ERR_PTR on failure
 */
struct fwat_device *
fwat_device_register(struct device *parent, const char *name, void *data,
		     const struct attribute_group **groups)
{
	struct fwat_device *fadev;
	struct device *dev;
	int ret;

	if (!parent || !name)
		return ERR_PTR(-EINVAL);

	fadev = kzalloc(sizeof(*fadev), GFP_KERNEL);
	if (!fadev)
		return ERR_PTR(-ENOMEM);

	dev = device_create(&firmware_attributes_class, parent, MKDEV(0, 0),
			    data, "%s", name);
	if (IS_ERR(dev)) {
		kfree(fadev);
		return ERR_CAST(dev);
	}

	ret = kobject_init_and_add(&fadev->attrs_kobj, &fwat_attrs_ktype, &dev->kobj,
				   "attributes");
	if (ret)
		goto out_kobj_put;

	if (groups) {
		ret = sysfs_create_groups(&fadev->attrs_kobj, groups);
		if (ret)
			goto out_kobj_unregister;
	}

	fadev->dev = dev;
	fadev->groups = groups;

	kobject_uevent(&fadev->attrs_kobj, KOBJ_ADD);

	return fadev;

out_kobj_unregister:
	kobject_del(&fadev->attrs_kobj);

out_kobj_put:
	kobject_put(&fadev->attrs_kobj);
	device_unregister(dev);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(fwat_device_register);

void fwat_device_unregister(struct fwat_device *fwadev)
{
	if (fwadev->groups)
		sysfs_remove_groups(&fwadev->attrs_kobj, fwadev->groups);
	kobject_del(&fwadev->attrs_kobj);
	kobject_put(&fwadev->attrs_kobj);
	device_unregister(fwadev->dev);
}
EXPORT_SYMBOL_GPL(fwat_device_unregister);

static void devm_fwat_device_release(void *data)
{
	struct fwat_device *fadev = data;

	fwat_device_unregister(fadev);
}

/**
 * devm_fwat_device_register - Create and register a firmware-attributes class
 *			       device
 * @parent: Parent device
 * @name: Name of the class device
 * @data: Drvdata of the class device
 * @groups: Sysfs groups for the custom `fwat_attrs_ktype` kobj_type
 *
 * Device managed version of fwat_device_register().
 *
 * NOTE: @groups are attached to the .attrs_kobj of the new fwat_device which
 * has a custom ktype, which makes use of `struct fwat_attribute` to embed
 * attributes.
 *
 * Return: pointer to the new fwat_device on success, ERR_PTR on failure
 */
struct fwat_device *
devm_fwat_device_register(struct device *parent, const char *name, void *data,
			  const struct attribute_group **groups)
{
	struct fwat_device *fadev;
	int ret;

	fadev = fwat_device_register(parent, name, data, groups);
	if (IS_ERR(fadev))
		return fadev;

	ret = devm_add_action_or_reset(parent, devm_fwat_device_release, fadev);
	if (ret)
		return ERR_PTR(ret);

	return fadev;
}
EXPORT_SYMBOL_GPL(devm_fwat_device_register);

static __init int fw_attributes_class_init(void)
{
	return class_register(&firmware_attributes_class);
}
module_init(fw_attributes_class_init);

static __exit void fw_attributes_class_exit(void)
{
	class_unregister(&firmware_attributes_class);
}
module_exit(fw_attributes_class_exit);

MODULE_AUTHOR("Mark Pearson <markpearson@lenovo.com>");
MODULE_AUTHOR("Thomas Weißschuh <linux@weissschuh.net>");
MODULE_AUTHOR("Kurt Borja <kuurtb@gmail.com>");
MODULE_DESCRIPTION("Firmware attributes class helper module");
MODULE_LICENSE("GPL");
