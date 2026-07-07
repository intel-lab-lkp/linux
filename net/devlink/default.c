// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "devl_internal.h"

static char *devlink_default_esw_mode_param;
static bool devlink_default_esw_mode_match_all;
static enum devlink_eswitch_mode devlink_default_esw_mode;
static LIST_HEAD(devlink_default_esw_mode_nodes);

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
	if (err)
		devlink_default_esw_mode_nodes_clear();
	else
		devlink_default_esw_mode = esw_mode;

	return err;
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

	return 0;
}

void __init devlink_default_esw_mode_cleanup(void)
{
	devlink_default_esw_mode_nodes_clear();
}
