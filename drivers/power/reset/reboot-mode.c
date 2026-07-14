// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 */

#define pr_fmt(fmt)	"reboot-mode: " fmt

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/slab.h>
#include <linux/string.h>

#define PREFIX "mode-"

struct mode_info {
	const char *mode;
	u32 magic[3];
	int count;
	struct list_head list;
};

struct reboot_mode_sysfs_data {
	struct device *reboot_mode_device;
	struct list_head head;
};

static inline void reboot_mode_release_list(struct list_head *head)
{
	struct mode_info *info;
	struct mode_info *next;

	list_for_each_entry_safe(info, next, head, list) {
		list_del(&info->list);
		kfree_const(info->mode);
		kfree(info);
	}
}

/**
 * reboot_mode_reset_predefined_modes - Remove all predefined reboot modes
 * @reboot: reboot mode driver
 *
 * Reset predefined reboot modes added via reboot_mode_add_predefined_modes().
 */
void reboot_mode_reset_predefined_modes(struct reboot_mode_driver *reboot)
{
	reboot_mode_release_list(&reboot->predefined_modes);
}
EXPORT_SYMBOL_GPL(reboot_mode_reset_predefined_modes);

static ssize_t reboot_modes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct reboot_mode_sysfs_data *priv;
	struct mode_info *sysfs_info;
	ssize_t size = 0;

	priv = dev_get_drvdata(dev);
	if (!priv)
		return -ENODATA;

	list_for_each_entry(sysfs_info, &priv->head, list)
		size += sysfs_emit_at(buf, size, "%s ", sysfs_info->mode);

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

static struct mode_info *get_reboot_mode_info(struct reboot_mode_driver *reboot, const char *cmd)
{
	const char *normal = "normal";
	struct mode_info *info;
	char cmd_[110];

	if (!cmd)
		cmd = normal;

	list_for_each_entry(info, &reboot->head, list)
		if (!strcmp(info->mode, cmd))
			return info;

	/* try to match again, replacing characters impossible in DT */
	if (strscpy(cmd_, cmd, sizeof(cmd_)) == -E2BIG)
		return NULL;

	strreplace(cmd_, ' ', '-');
	strreplace(cmd_, ',', '-');
	strreplace(cmd_, '/', '-');

	list_for_each_entry(info, &reboot->head, list)
		if (!strcmp(info->mode, cmd_))
			return info;

	return NULL;
}

static int reboot_mode_notify(struct notifier_block *this,
			      unsigned long mode, void *cmd)
{
	struct reboot_mode_driver *reboot;
	struct mode_info *info;

	reboot = container_of(this, struct reboot_mode_driver, reboot_notifier);
	info = get_reboot_mode_info(reboot, cmd);
	if (info && info->count > 0)
		reboot->write(reboot, info->magic, info->count);

	return NOTIFY_DONE;
}

/**
 * reboot_mode_driver_init - Initialize reboot-mode state
 * @reboot: reboot mode driver object to initialize
 * @dev: backing device
 * @write: write callback for programming magic
 *
 * This function must be called with a valid @dev and @write before calling
 * reboot_mode_register(), reboot_mode_add_predefined_modes(), or any other
 * reboot-mode framework API.
 */
void reboot_mode_driver_init(struct reboot_mode_driver *reboot,
			     struct device *dev,
			     int (*write)(struct reboot_mode_driver *reboot, u32 *magic, int count))
{
	memset(reboot, 0, sizeof(*reboot));
	reboot->dev = dev;
	reboot->write = write;
	INIT_LIST_HEAD(&reboot->head);
	INIT_LIST_HEAD(&reboot->predefined_modes);
}
EXPORT_SYMBOL_GPL(reboot_mode_driver_init);

static struct mode_info *reboot_mode_create_info(const char *mode, const u32 *magic, int count)
{
	struct mode_info *info;

	if (!mode || mode[0] == '\0') {
		pr_err("invalid mode name\n");
		return ERR_PTR(-EINVAL);
	}

	info = kzalloc_obj(*info, GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	info->mode = kstrdup_const(mode, GFP_KERNEL);
	if (!info->mode) {
		kfree(info);
		return ERR_PTR(-ENOMEM);
	}

	if (!memchr_inv(magic, 0, count * sizeof(u32))) {
		pr_debug("reboot mode %s with zero magic values\n", mode);
		info->count = -1;
	} else {
		memcpy(info->magic, magic, count * sizeof(u32));
		info->count = count;
	}

	return info;
}

static int reboot_mode_create_device(struct reboot_mode_driver *reboot)
{
	struct reboot_mode_sysfs_data *priv;
	struct mode_info *sysfs_info;
	struct mode_info *info;
	int ret;

	priv = kzalloc_obj(*priv, GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	INIT_LIST_HEAD(&priv->head);

	list_for_each_entry(info, &reboot->head, list) {
		sysfs_info = kzalloc_obj(*sysfs_info, GFP_KERNEL);
		if (!sysfs_info) {
			ret = -ENOMEM;
			goto error;
		}

		sysfs_info->mode = kstrdup_const(info->mode, GFP_KERNEL);
		if (!sysfs_info->mode) {
			kfree(sysfs_info);
			ret = -ENOMEM;
			goto error;
		}

		list_add_tail(&sysfs_info->list, &priv->head);
	}

	priv->reboot_mode_device = device_create(&reboot_mode_class, NULL, 0,
						 (void *)priv, "%s",
						 reboot->dev->driver->name);
	if (IS_ERR(priv->reboot_mode_device)) {
		ret = PTR_ERR(priv->reboot_mode_device);
		goto error;
	}

	return 0;

error:
	reboot_mode_release_list(&priv->head);
	kfree(priv);
	return ret;
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
	u32 magic[3];
	int count;
	int ret;

	if (reboot->reboot_notifier.notifier_call == reboot_mode_notify)
		return -EBUSY;

	INIT_LIST_HEAD(&reboot->head);

	if (!np)
		goto predefined_modes;

	for_each_property_of_node(np, prop) {
		memset(magic, 0, sizeof(magic));
		if (strncmp(prop->name, PREFIX, len))
			continue;

		count = device_property_count_u32(reboot->dev, prop->name);

		if (count <= 0 || count > ARRAY_SIZE(magic) ||
		    device_property_read_u32_array(reboot->dev, prop->name, magic, count)) {
			pr_debug("reboot mode %s without magic number\n", prop->name);
			continue;
		}

		info = reboot_mode_create_info(prop->name + len, magic, count);
		if (IS_ERR(info)) {
			ret = PTR_ERR(info);
			goto error;
		}

		list_add_tail(&info->list, &reboot->head);
	}

predefined_modes:
	list_splice_tail_init(&reboot->predefined_modes, &reboot->head);
	reboot->reboot_notifier.notifier_call = reboot_mode_notify;
	register_reboot_notifier(&reboot->reboot_notifier);

	ret = reboot_mode_create_device(reboot);
	if (ret)
		goto error;

	return 0;

error:
	reboot_mode_unregister(reboot);
	return ret;
}
EXPORT_SYMBOL_GPL(reboot_mode_register);

static int reboot_mode_match_by_name(struct device *dev, const void *data)
{
	const char *name = data;

	if (!dev || !data)
		return 0;

	return dev_name(dev) && strcmp(dev_name(dev), name) == 0;
}

static inline void reboot_mode_unregister_device(struct reboot_mode_driver *reboot)
{
	struct reboot_mode_sysfs_data *priv;
	struct device *reboot_mode_device;

	reboot_mode_device = class_find_device(&reboot_mode_class, NULL, reboot->dev->driver->name,
					       reboot_mode_match_by_name);

	if (!reboot_mode_device)
		return;

	priv = dev_get_drvdata(reboot_mode_device);
	device_unregister(reboot_mode_device);

	if (!priv)
		return;

	reboot_mode_release_list(&priv->head);
	kfree(priv);
}

/**
 * reboot_mode_unregister - unregister a reboot mode driver
 * @reboot: reboot mode driver
 */
int reboot_mode_unregister(struct reboot_mode_driver *reboot)
{
	unregister_reboot_notifier(&reboot->reboot_notifier);
	reboot->reboot_notifier.notifier_call = NULL;
	reboot_mode_unregister_device(reboot);

	reboot_mode_release_list(&reboot->head);
	reboot_mode_release_list(&reboot->predefined_modes);

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
	if (!dr) {
		reboot_mode_reset_predefined_modes(reboot);
		return -ENOMEM;
	}

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

/**
 * reboot_mode_add_predefined_modes - Add predefined reboot modes
 * @reboot: reboot mode driver
 * @modes: array of predefined reboot mode entries
 * @count: number of entries in @modes
 *
 * Add predefined reboot modes before registration.
 *
 * The entire list is discarded if any mode entry is invalid. An entry
 * with a zero or negative magic count, a NULL mode string, or a mode
 * string containing spaces or "\n" is considered invalid.
 *
 * Predefined modes are cleared if registration fails.
 * Call reboot_mode_reset_predefined_modes() if registration is not
 * performed after adding predefined modes.
 *
 * @reboot must be initialized with reboot_mode_driver_init() before calling
 * this function.
 *
 * Returns: 0 on success,
 *	    -EINVAL if invalid entry is found in list,
 *	    -EBUSY if called after reboot_mode_register() or if predefined modes
 *	    are already set, and, -ENOMEM on allocation failures.
 */
int reboot_mode_add_predefined_modes(struct reboot_mode_driver *reboot,
				     const struct reboot_mode_entry *modes,
				     size_t count)
{
	struct mode_info *info;
	int ret;
	size_t i;

	if (reboot->reboot_notifier.notifier_call == reboot_mode_notify ||
	    !list_empty(&reboot->predefined_modes))
		return -EBUSY;

	if (!modes || !count)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		if (modes[i].name && strpbrk(modes[i].name, "\n ")) {
			ret = -EINVAL;
			goto error;
		}

		if (modes[i].count <= 0 || modes[i].count > ARRAY_SIZE(modes[i].magic)) {
			ret = -EINVAL;
			goto error;
		}

		info = reboot_mode_create_info(modes[i].name, modes[i].magic, modes[i].count);
		if (IS_ERR(info)) {
			ret = PTR_ERR(info);
			goto error;
		}

		list_add_tail(&info->list, &reboot->predefined_modes);
	}

	return 0;

error:
	reboot_mode_release_list(&reboot->predefined_modes);
	return ret;
}
EXPORT_SYMBOL_GPL(reboot_mode_add_predefined_modes);

static int __init reboot_mode_init(void)
{
	return class_register(&reboot_mode_class);
}

static void __exit reboot_mode_exit(void)
{
	class_unregister(&reboot_mode_class);
}

subsys_initcall(reboot_mode_init);
module_exit(reboot_mode_exit);

MODULE_AUTHOR("Andy Yan <andy.yan@rock-chips.com>");
MODULE_DESCRIPTION("System reboot mode core library");
MODULE_LICENSE("GPL v2");
