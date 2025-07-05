// SPDX-License-Identifier: GPL-2.0-or-later

/* Firmware attributes class helper module */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/kdev_t.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include "firmware_attributes_class.h"

const struct class firmware_attributes_class = {
	.name = "firmware-attributes",
};
EXPORT_SYMBOL_GPL(firmware_attributes_class);

static void fwat_device_release(struct device *dev)
{
	struct fwat_device *fadev = to_fwat_device(dev);

	kfree(fadev);
}

/**
 * fwat_device_register - Create and register a firmware-attributes class
 *			  device
 * @parent: Parent device
 * @name: Name of the class device
 * @drvdata: Drvdata of the class device
 * @groups: Extra groups for the "attributes" directory
 *
 * Return: pointer to the new fwat_device on success, ERR_PTR on failure
 */
struct fwat_device *
fwat_device_register(struct device *parent, const char *name, void *drvdata,
		     const struct attribute_group **groups)
{
	struct fwat_device *fadev;
	int ret;

	if (!parent || !name)
		return ERR_PTR(-EINVAL);

	fadev = kzalloc(sizeof(*fadev), GFP_KERNEL);
	if (!fadev)
		return ERR_PTR(-ENOMEM);

	fadev->groups = groups;
	fadev->dev.class = &firmware_attributes_class;
	fadev->dev.parent = parent;
	fadev->dev.release = fwat_device_release;
	dev_set_drvdata(&fadev->dev, drvdata);
	ret = dev_set_name(&fadev->dev, "%s", name);
	if (ret) {
		kfree(fadev);
		return ERR_PTR(ret);
	}
	ret = device_register(&fadev->dev);
	if (ret)
		return ERR_PTR(ret);

	fadev->attrs_kset = kset_create_and_add("attributes", NULL, &fadev->dev.kobj);
	if (!fadev->attrs_kset) {
		ret = -ENOMEM;
		goto out_device_unregister;
	}

	ret = sysfs_create_groups(&fadev->attrs_kset->kobj, groups);
	if (ret)
		goto out_kset_unregister;

	return fadev;

out_kset_unregister:
	kset_unregister(fadev->attrs_kset);

out_device_unregister:
	device_unregister(&fadev->dev);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(fwat_device_register);

void fwat_device_unregister(struct fwat_device *fadev)
{
	if (!fadev)
		return;

	sysfs_remove_groups(&fadev->attrs_kset->kobj, fadev->groups);
	kset_unregister(fadev->attrs_kset);
	device_unregister(&fadev->dev);
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
 * @groups: Extra groups for the class device (Optional)
 *
 * Device managed version of fwat_device_register().
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
