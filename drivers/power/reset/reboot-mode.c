// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 */

#define pr_fmt(fmt)	"reboot-mode: " fmt

#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/slab.h>

#define PREFIX "mode-"

struct mode_info {
	const char *mode;
	u64 magic;
	struct list_head list;
};

static u64 get_reboot_mode_magic(struct reboot_mode_driver *reboot, const char *cmd)
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
	u64 magic;

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
	struct mode_info *next;
	struct property *prop;
	struct device_node *np = reboot->dev->of_node;
	size_t len = strlen(PREFIX);
	u32 magic_arg1;
	u32 magic_arg2;
	int ret;

	INIT_LIST_HEAD(&reboot->head);

	for_each_property_of_node(np, prop) {
		if (strncmp(prop->name, PREFIX, len))
			continue;

		if (of_property_read_u32(np, prop->name, &magic_arg1)) {
			pr_err("reboot mode %s without magic number\n", prop->name);
			continue;
		}
		/* Default magic_arg2 to zero */
		if (of_property_read_u32_index(np, prop->name, 1, &magic_arg2))
			magic_arg2 = 0;

		info = kzalloc(sizeof(*info), GFP_KERNEL);
		if (!info) {
			ret = -ENOMEM;
			goto error;
		}

		/**
		 * Format of u64 magic
		 *-------------------------------------------
		 *|    Higher 32 bit  |   Lower 32 bit      |
		 *|        arg2       |       arg1          |
		 *-------------------------------------------
		 */
		info->magic = FIELD_PREP(GENMASK_ULL(63, 32), magic_arg2) |
			      FIELD_PREP(GENMASK_ULL(31, 0), magic_arg1);
		info->mode = kstrdup_const(prop->name + len, GFP_KERNEL);
		if (!info->mode) {
			ret =  -ENOMEM;
			goto error;
		} else if (info->mode[0] == '\0') {
			kfree_const(info->mode);
			ret = -EINVAL;
			pr_err("invalid mode name(%s): too short!\n", prop->name);
			goto error;
		}

		list_add_tail(&info->list, &reboot->head);
	}

	reboot->reboot_notifier.notifier_call = reboot_mode_notify;
	register_reboot_notifier(&reboot->reboot_notifier);

	return 0;

error:
	kfree(info);
	list_for_each_entry_safe(info, next, &reboot->head, list) {
		list_del(&info->list);
		kfree_const(info->mode);
		kfree(info);
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
	struct mode_info *next;

	unregister_reboot_notifier(&reboot->reboot_notifier);

	list_for_each_entry_safe(info, next, &reboot->head, list) {
		list_del(&info->list);
		kfree_const(info->mode);
		kfree(info);
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

MODULE_AUTHOR("Andy Yan <andy.yan@rock-chips.com>");
MODULE_DESCRIPTION("System reboot mode core library");
MODULE_LICENSE("GPL v2");
