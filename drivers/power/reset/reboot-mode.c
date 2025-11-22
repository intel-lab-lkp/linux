// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>

#define PREFIX "mode-"

static bool reboot_mode_class_registered;

struct mode_info {
	const char *mode;
	u32 magic;
	struct list_head list;
};

static ssize_t reboot_modes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct reboot_mode_driver *reboot;
	struct mode_info *info;
	ssize_t size = 0;

	reboot = container_of(dev, struct reboot_mode_driver, reboot_mode_device);
	if (!reboot)
		return -ENODATA;

	list_for_each_entry(info, &reboot->head, list)
		size += sysfs_emit_at(buf, size, "%s ", info->mode);

	if (!size)
		return -ENODATA;

	return size + sysfs_emit_at(buf, size - 1, "\n");
}
static DEVICE_ATTR_RO(reboot_modes);

static struct attribute *reboot_mode_attrs[] = {
	&dev_attr_reboot_modes.attr,
	NULL,
};
ATTRIBUTE_GROUPS(reboot_mode);

static const struct class reboot_mode_class = {
	.name = "reboot-mode",
	.dev_groups = reboot_mode_groups,
};

static void reboot_mode_device_release(struct device *dev)
{
    /* place holder to avoid warning on device_unregister. nothing to free */
}

static void reboot_mode_register_device(struct reboot_mode_driver *reboot)
{
	reboot->reboot_mode_device.class = &reboot_mode_class;
	reboot->reboot_mode_device.release = reboot_mode_device_release;
	dev_set_name(&reboot->reboot_mode_device, reboot->driver_name);
	if (!device_register(&reboot->reboot_mode_device))
		reboot->reboot_mode_device_registered = true;
	else
		reboot->reboot_mode_device_registered = false;
}

static unsigned int get_reboot_mode_magic(struct reboot_mode_driver *reboot,
					  const char *cmd)
{
	const char *normal = "normal";
	struct mode_info *info;
	char cmd_[110];

	if (!cmd)
		cmd = normal;

	list_for_each_entry(info, &reboot->head, list)
		if (!strcmp(info->mode, cmd))
			return info->magic;

	/* try to match again, replacing characters impossible in DT */
	if (strscpy(cmd_, cmd, sizeof(cmd_)) == -E2BIG)
		return 0;

	strreplace(cmd_, ' ', '-');
	strreplace(cmd_, ',', '-');
	strreplace(cmd_, '/', '-');

	list_for_each_entry(info, &reboot->head, list)
		if (!strcmp(info->mode, cmd_))
			return info->magic;

	return 0;
}

static int reboot_mode_notify(struct notifier_block *this,
			      unsigned long mode, void *cmd)
{
	struct reboot_mode_driver *reboot;
	unsigned int magic;

	reboot = container_of(this, struct reboot_mode_driver, reboot_notifier);
	magic = get_reboot_mode_magic(reboot, cmd);
	if (magic)
		reboot->write(reboot, magic);

	return NOTIFY_DONE;
}

/**
 * reboot_mode_register - register a reboot mode driver
 * @reboot: reboot mode driver
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int reboot_mode_register(struct reboot_mode_driver *reboot)
{
	struct mode_info *info;
	struct property *prop;
	struct device_node *np = reboot->dev->of_node;
	size_t len = strlen(PREFIX);
	int ret;

	INIT_LIST_HEAD(&reboot->head);

	if (reboot_mode_class_registered)
		reboot_mode_register_device(reboot);

	for_each_property_of_node(np, prop) {
		if (strncmp(prop->name, PREFIX, len))
			continue;

		info = devm_kzalloc(reboot->dev, sizeof(*info), GFP_KERNEL);
		if (!info) {
			ret = -ENOMEM;
			goto error;
		}

		if (of_property_read_u32(np, prop->name, &info->magic)) {
			dev_err(reboot->dev, "reboot mode %s without magic number\n",
				info->mode);
			devm_kfree(reboot->dev, info);
			continue;
		}

		info->mode = kstrdup_const(prop->name + len, GFP_KERNEL);
		if (!info->mode) {
			ret =  -ENOMEM;
			goto error;
		} else if (info->mode[0] == '\0') {
			kfree_const(info->mode);
			ret = -EINVAL;
			dev_err(reboot->dev, "invalid mode name(%s): too short!\n",
				prop->name);
			goto error;
		}

		list_add_tail(&info->list, &reboot->head);
	}

	reboot->reboot_notifier.notifier_call = reboot_mode_notify;
	register_reboot_notifier(&reboot->reboot_notifier);

	return 0;

error:
	list_for_each_entry(info, &reboot->head, list)
		kfree_const(info->mode);

	if (reboot->reboot_mode_device_registered) {
		device_unregister(&reboot->reboot_mode_device);
		reboot->reboot_mode_device_registered = false;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(reboot_mode_register);

/**
 * reboot_mode_unregister - unregister a reboot mode driver
 * @reboot: reboot mode driver
 */
int reboot_mode_unregister(struct reboot_mode_driver *reboot)
{
	struct mode_info *info;

	unregister_reboot_notifier(&reboot->reboot_notifier);

	list_for_each_entry(info, &reboot->head, list)
		kfree_const(info->mode);

	if (reboot->reboot_mode_device_registered) {
		device_unregister(&reboot->reboot_mode_device);
		reboot->reboot_mode_device_registered = false;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(reboot_mode_unregister);

static void devm_reboot_mode_release(struct device *dev, void *res)
{
	reboot_mode_unregister(*(struct reboot_mode_driver **)res);
}

/**
 * devm_reboot_mode_register() - resource managed reboot_mode_register()
 * @dev: device to associate this resource with
 * @reboot: reboot mode driver
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int devm_reboot_mode_register(struct device *dev,
			      struct reboot_mode_driver *reboot)
{
	struct reboot_mode_driver **dr;
	int rc;

	dr = devres_alloc(devm_reboot_mode_release, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	reboot->driver_name = reboot->dev->driver->name;
	rc = reboot_mode_register(reboot);
	if (rc) {
		devres_free(dr);
		return rc;
	}

	*dr = reboot;
	devres_add(dev, dr);

	return 0;
}
EXPORT_SYMBOL_GPL(devm_reboot_mode_register);

static int devm_reboot_mode_match(struct device *dev, void *res, void *data)
{
	struct reboot_mode_driver **p = res;

	if (WARN_ON(!p || !*p))
		return 0;

	return *p == data;
}

/**
 * devm_reboot_mode_unregister() - resource managed reboot_mode_unregister()
 * @dev: device to associate this resource with
 * @reboot: reboot mode driver
 */
void devm_reboot_mode_unregister(struct device *dev,
				 struct reboot_mode_driver *reboot)
{
	WARN_ON(devres_release(dev,
			       devm_reboot_mode_release,
			       devm_reboot_mode_match, reboot));
}
EXPORT_SYMBOL_GPL(devm_reboot_mode_unregister);

static int __init reboot_mode_init(void)
{
	if (class_register(&reboot_mode_class))
		reboot_mode_class_registered = false;
	else
		reboot_mode_class_registered = true;

	return 0;
}

static void __exit reboot_mode_exit(void)
{
	if (reboot_mode_class_registered)
		class_unregister(&reboot_mode_class);
}

#ifdef MODULE
module_init(reboot_mode_init);
module_exit(reboot_mode_exit);
#else
subsys_initcall(reboot_mode_init);
#endif

MODULE_AUTHOR("Andy Yan <andy.yan@rock-chips.com>");
MODULE_DESCRIPTION("System reboot mode core library");
MODULE_LICENSE("GPL v2");
