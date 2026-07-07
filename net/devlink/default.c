// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include <linux/init.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "devl_internal.h"

static char *devlink_default_esw_mode_param;
static bool devlink_default_esw_mode_match_all;
static bool devlink_default_esw_mode_enabled;
static enum devlink_eswitch_mode devlink_default_esw_mode;
static LIST_HEAD(devlink_default_esw_mode_nodes);
static struct workqueue_struct *devlink_default_esw_mode_wq;

struct devlink_default_esw_mode_node {
	struct list_head list;
	char *bus_name;
	char *dev_name;
};

static int __init
devlink_default_esw_mode_to_value(const char *str,
				  enum devlink_eswitch_mode *mode)
{
	if (!strcmp(str, "legacy")) {
		*mode = DEVLINK_ESWITCH_MODE_LEGACY;
		return 0;
	}
	if (!strcmp(str, "switchdev")) {
		*mode = DEVLINK_ESWITCH_MODE_SWITCHDEV;
		return 0;
	}
	if (!strcmp(str, "switchdev_inactive")) {
		*mode = DEVLINK_ESWITCH_MODE_SWITCHDEV_INACTIVE;
		return 0;
	}

	return -EINVAL;
}

static int __init
devlink_default_esw_mode_handle_parse(char *handle, char **bus_name,
				      char **dev_name)
{
	char *slash;
	char *p;

	if (!*handle)
		return -EINVAL;

	for (p = handle; *p; p++) {
		if (*p == '*' || *p == '=')
			return -EINVAL;
	}

	slash = strchr(handle, '/');
	if (!slash || slash == handle || !slash[1])
		return -EINVAL;
	if (strchr(slash + 1, '/'))
		return -EINVAL;

	*slash = '\0';

	*bus_name = handle;
	*dev_name = slash + 1;
	return 0;
}

static struct devlink_default_esw_mode_node *
devlink_default_esw_mode_node_find(const char *bus_name, const char *dev_name)
{
	struct devlink_default_esw_mode_node *node;

	list_for_each_entry(node, &devlink_default_esw_mode_nodes, list) {
		if (!strcmp(node->bus_name, bus_name) &&
		    !strcmp(node->dev_name, dev_name))
			return node;
	}

	return NULL;
}

static int __init
devlink_default_esw_mode_node_add(const char *bus_name, const char *dev_name)
{
	struct devlink_default_esw_mode_node *node;

	if (devlink_default_esw_mode_node_find(bus_name, dev_name))
		return -EEXIST;

	node = kzalloc_obj(*node);
	if (!node)
		return -ENOMEM;

	INIT_LIST_HEAD(&node->list);
	node->bus_name = kstrdup(bus_name, GFP_KERNEL);
	node->dev_name = kstrdup(dev_name, GFP_KERNEL);
	if (!node->bus_name || !node->dev_name) {
		kfree(node->bus_name);
		kfree(node->dev_name);
		kfree(node);
		return -ENOMEM;
	}

	list_add_tail(&node->list, &devlink_default_esw_mode_nodes);
	return 0;
}

static int __init devlink_default_esw_mode_handles_parse(char *handles)
{
	char *handle;
	int err;

	if (!strcmp(handles, "*")) {
		devlink_default_esw_mode_match_all = true;
		return 0;
	}

	while ((handle = strsep(&handles, ",")) != NULL) {
		char *bus_name;
		char *dev_name;

		err = devlink_default_esw_mode_handle_parse(handle, &bus_name,
							    &dev_name);
		if (err)
			return err;

		err = devlink_default_esw_mode_node_add(bus_name, dev_name);
		if (err)
			return err;
	}

	return 0;
}

static void __init
devlink_default_esw_mode_node_free(struct devlink_default_esw_mode_node *node)
{
	kfree(node->bus_name);
	kfree(node->dev_name);
	kfree(node);
}

static void __init devlink_default_esw_mode_nodes_clear(void)
{
	struct devlink_default_esw_mode_node *node_tmp;
	struct devlink_default_esw_mode_node *node;

	list_for_each_entry_safe(node, node_tmp,
				 &devlink_default_esw_mode_nodes, list) {
		list_del(&node->list);
		devlink_default_esw_mode_node_free(node);
	}

	devlink_default_esw_mode_match_all = false;
	devlink_default_esw_mode_enabled = false;
}

static int __init devlink_default_esw_mode_parse(char *str)
{
	enum devlink_eswitch_mode esw_mode;
	char *separator;
	char *handles;
	char *mode;
	int err;

	if (!*str)
		return -EINVAL;

	separator = strrchr(str, '=');
	if (!separator || separator == str || !separator[1])
		return -EINVAL;

	*separator = '\0';
	handles = str;
	mode = separator + 1;

	err = devlink_default_esw_mode_to_value(mode, &esw_mode);
	if (err)
		return err;

	err = devlink_default_esw_mode_handles_parse(handles);
	if (err) {
		devlink_default_esw_mode_nodes_clear();
	} else {
		devlink_default_esw_mode = esw_mode;
		devlink_default_esw_mode_enabled = true;
	}

	return err;
}

static bool devlink_default_esw_mode_match(struct devlink *devlink)
{
	const char *bus_name = devlink_bus_name(devlink);
	const char *dev_name = devlink_dev_name(devlink);
	struct devlink_default_esw_mode_node *node;

	if (devlink_default_esw_mode_match_all)
		return true;

	node = devlink_default_esw_mode_node_find(bus_name, dev_name);
	return !!node;
}

void devlink_default_esw_mode_apply_locked(struct devlink *devlink)
{
	const struct devlink_ops *ops = devlink->ops;
	int err;

	devl_assert_locked(devlink);

	if (!devlink_default_esw_mode_match(devlink))
		return;

	if (!ops->eswitch_mode_set) {
		if (!devlink_default_esw_mode_match_all)
			devl_warn(devlink,
				  "devlink_eswitch_mode= selected this device but eswitch mode setting is not supported\n");
		return;
	}

	err = devlink_eswitch_mode_set(devlink, devlink_default_esw_mode, NULL);
	if (err)
		devl_warn(devlink,
			  "Couldn't apply default eswitch mode, err %d\n",
			  err);
}

void devlink_default_esw_mode_queue_apply_work(struct devlink *devlink)
{
	devl_assert_locked(devlink);

	if (!devlink_default_esw_mode_enabled || !devlink_default_esw_mode_wq)
		return;
	if (!devlink->default_esw_mode_apply_pending ||
	    !__devl_is_registered(devlink))
		return;
	if (!devlink_try_get(devlink))
		return;
	if (!queue_work(devlink_default_esw_mode_wq,
			&devlink->default_esw_mode_apply_work))
		devlink_put(devlink);
}

static void devlink_default_esw_mode_apply_work(struct work_struct *work)
{
	struct devlink *devlink;

	devlink = container_of(work, struct devlink,
			       default_esw_mode_apply_work);

	devl_lock(devlink);

	if (devl_is_registered(devlink) &&
	    devlink->default_esw_mode_apply_pending) {
		devlink_default_esw_mode_apply_locked(devlink);
		devlink->default_esw_mode_apply_pending = false;
	}

	devl_unlock(devlink);
	devlink_put(devlink);
}

void devlink_default_esw_mode_instance_init(struct devlink *devlink)
{
	INIT_WORK(&devlink->default_esw_mode_apply_work,
		  devlink_default_esw_mode_apply_work);
	devlink->default_esw_mode_apply_pending = true;
}

void devlink_default_esw_mode_apply_pending_clear(struct devlink *devlink)
{
	devl_assert_locked(devlink);

	devlink->default_esw_mode_apply_pending = false;
}

/**
 * devl_apply_default_esw_mode - Apply devlink eswitch mode boot default
 * @devlink: devlink
 *
 * Apply the devlink eswitch mode selected by the devlink_eswitch_mode=
 * kernel command line parameter, if any matches @devlink.
 *
 * The caller must hold the devlink instance lock.
 */
void devl_apply_default_esw_mode(struct devlink *devlink)
{
	devl_assert_locked(devlink);

	devlink->default_esw_mode_apply_pending = false;
	devlink_default_esw_mode_apply_locked(devlink);
}
EXPORT_SYMBOL_GPL(devl_apply_default_esw_mode);

void devlink_default_esw_mode_instance_cleanup(struct devlink *devlink)
{
	if (cancel_work_sync(&devlink->default_esw_mode_apply_work))
		devlink_put(devlink);
}

static int __init devlink_default_esw_mode_setup(char *str)
{
	devlink_default_esw_mode_param = str;
	return 1;
}
__setup("devlink_eswitch_mode=", devlink_default_esw_mode_setup);

int __init devlink_default_esw_mode_init(void)
{
	char *def;
	int err;

	if (!devlink_default_esw_mode_param)
		return 0;

	def = kstrdup(devlink_default_esw_mode_param, GFP_KERNEL);
	if (!def) {
		devlink_default_esw_mode_param = NULL;
		pr_warn("devlink: devlink_eswitch_mode parameter ignored, failed to allocate memory\n");
		return 0;
	}

	err = devlink_default_esw_mode_parse(def);
	kfree(def);
	if (err == -EEXIST) {
		devlink_default_esw_mode_param = NULL;
		pr_warn("devlink: duplicate eswitch mode handles ignored\n");
		return 0;
	} else if (err == -EINVAL) {
		devlink_default_esw_mode_param = NULL;
		pr_warn("devlink: invalid devlink_eswitch_mode parameter ignored\n");
		return 0;
	} else if (err == -ENOMEM) {
		devlink_default_esw_mode_param = NULL;
		pr_warn("devlink: devlink_eswitch_mode parameter ignored, failed to allocate memory\n");
		return 0;
	} else if (err) {
		return err;
	}

	devlink_default_esw_mode_wq = alloc_workqueue("devlink_default_esw_mode",
						      WQ_UNBOUND | WQ_MEM_RECLAIM,
						      0);
	if (!devlink_default_esw_mode_wq) {
		devlink_default_esw_mode_param = NULL;
		devlink_default_esw_mode_nodes_clear();
		pr_warn("devlink: devlink_eswitch_mode parameter ignored, failed to allocate workqueue\n");
	}

	return 0;
}

void __init devlink_default_esw_mode_cleanup(void)
{
	if (devlink_default_esw_mode_wq)
		destroy_workqueue(devlink_default_esw_mode_wq);
	devlink_default_esw_mode_nodes_clear();
}
