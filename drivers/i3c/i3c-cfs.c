// SPDX-License-Identifier: GPL-2.0
/*
 * Configfs to configure the I3C Slave
 *
 * Copyright (C) 2023 NXP
 * Author: Frank Li <Frank.Li@nxp.com>
 */

#include <linux/configfs.h>
#include <linux/module.h>
#include <linux/i3c/slave.h>
#include <linux/idr.h>
#include <linux/slab.h>

static DEFINE_MUTEX(functions_mutex);
static struct config_group *functions_group;
static struct config_group *controllers_group;

struct i3c_slave_func_group {
	struct config_group group;
	struct i3c_slave_func *func;
};

struct i3c_slave_ctrl_group {
	struct config_group group;
	struct i3c_slave_ctrl *ctrl;
};

static inline struct i3c_slave_func_group *to_i3c_slave_func_group(struct config_item *item)
{
	return container_of(to_config_group(item), struct i3c_slave_func_group, group);
}

static inline struct i3c_slave_ctrl_group *to_i3c_slave_ctrl_group(struct config_item *item)
{
	return container_of(to_config_group(item), struct i3c_slave_ctrl_group, group);
}

static int i3c_slave_ctrl_func_link(struct config_item *ctrl_cfg, struct config_item *func_cfg)
{
	struct i3c_slave_func_group *func_group = to_i3c_slave_func_group(func_cfg);
	struct i3c_slave_ctrl_group *ctrl_group = to_i3c_slave_ctrl_group(ctrl_cfg);
	struct i3c_slave_ctrl *ctrl = ctrl_group->ctrl;
	struct i3c_slave_func *func = func_group->func;
	int ret;

	ret = i3c_slave_ctrl_add_func(ctrl, func);
	if (ret)
		return ret;

	ret = i3c_slave_func_bind(func);
	if (ret) {
		i3c_slave_ctrl_remove_func(ctrl, func);
		return ret;
	}

	return 0;
}

static void i3c_slave_ctrl_func_unlink(struct config_item *ctrl_cfg, struct config_item *func_cfg)
{
	struct i3c_slave_func_group *func_group = to_i3c_slave_func_group(func_cfg->ci_parent);
	struct i3c_slave_ctrl_group *ctrl_group = to_i3c_slave_ctrl_group(ctrl_cfg);
	struct i3c_slave_ctrl *ctrl = ctrl_group->ctrl;
	struct i3c_slave_func *func = func_group->func;

	i3c_slave_func_unbind(func);
	i3c_slave_ctrl_remove_func(ctrl, func);
}

static ssize_t i3c_slave_ctrl_hotjoin_store(struct config_item *item, const char *page, size_t len)
{
	struct i3c_slave_ctrl_group *ctrl_group = to_i3c_slave_ctrl_group(item);
	struct i3c_slave_ctrl *ctrl;
	int ret;

	ctrl = ctrl_group->ctrl;

	ret = i3c_slave_ctrl_hotjoin(ctrl);
	if (ret) {
		dev_err(&ctrl->dev, "failed to hotjoin i3c slave controller\n");
		return -EINVAL;
	}

	return len;
}

static ssize_t i3c_slave_ctrl_hotjoin_show(struct config_item *item, char *page)
{
	return sysfs_emit(page, "%d\n", 0);
}

CONFIGFS_ATTR(i3c_slave_ctrl_, hotjoin);

static struct configfs_item_operations i3c_slave_ctrl_item_ops = {
	.allow_link     = i3c_slave_ctrl_func_link,
	.drop_link      = i3c_slave_ctrl_func_unlink,
};

static struct configfs_attribute *i3c_slave_ctrl_attrs[] = {
	&i3c_slave_ctrl_attr_hotjoin,
	NULL,
};

static const struct config_item_type i3c_slave_ctrl_type = {
	.ct_item_ops    = &i3c_slave_ctrl_item_ops,
	.ct_attrs	= i3c_slave_ctrl_attrs,
	.ct_owner       = THIS_MODULE,
};

/**
 * i3c_slave_cfs_add_ctrl_group() - add I3C slave controller group
 * @ctrl: I3C slave controller device
 *
 * Return: Pointer to struct config_group
 */
struct config_group *i3c_slave_cfs_add_ctrl_group(struct i3c_slave_ctrl *ctrl)
{
	struct i3c_slave_ctrl_group *ctrl_group;
	struct config_group *group;
	int ret;

	ctrl_group = kzalloc(sizeof(*ctrl_group), GFP_KERNEL);
	if (!ctrl_group) {
		ret = -ENOMEM;
		goto err;
	}

	group = &ctrl_group->group;

	config_group_init_type_name(group, dev_name(&ctrl->dev), &i3c_slave_ctrl_type);
	ret = configfs_register_group(controllers_group, group);
	if (ret) {
		pr_err("failed to register configfs group for %s\n", dev_name(&ctrl->dev));
		goto err_register_group;
	}

	ctrl_group->ctrl = ctrl;

	return group;

err_register_group:
	kfree(ctrl_group);

err:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(i3c_slave_cfs_add_ctrl_group);

/**
 * i3c_slave_cfs_remove_ctrl_group() - remove I3C slave controller group
 * @group: the group to be removed
 */
void i3c_slave_cfs_remove_ctrl_group(struct config_group *group)
{
	struct i3c_slave_ctrl_group *ctrl_group;

	if (!group)
		return;

	ctrl_group = container_of(group, struct i3c_slave_ctrl_group, group);
	i3c_slave_ctrl_put(ctrl_group->ctrl);
	configfs_unregister_group(&ctrl_group->group);
	kfree(ctrl_group);
}
EXPORT_SYMBOL(i3c_slave_cfs_remove_ctrl_group);

#define I3C_SLAVE_ATTR_R(_name)                                                \
static ssize_t i3c_slave_func_##_name##_show(struct config_item *item, char *page)    \
{                                                                              \
	struct i3c_slave_func *func = to_i3c_slave_func_group(item)->func;                     \
	return sysfs_emit(page, "0x%04x\n", func->_name);               \
}

#define I3C_SLAVE_ATTR_W(_name, _u)                                            \
static ssize_t i3c_slave_func_##_name##_store(struct config_item *item,               \
				       const char *page, size_t len)           \
{                                                                              \
	_u val;                                                               \
	struct i3c_slave_func *func = to_i3c_slave_func_group(item)->func;                     \
	if (kstrto##_u(page, 0, &val) < 0)                                      \
		return -EINVAL;                                                \
	func->_name = val;                                              \
	return len;                                                            \
}

I3C_SLAVE_ATTR_R(vendor_id);
I3C_SLAVE_ATTR_W(vendor_id, u16);
CONFIGFS_ATTR(i3c_slave_func_, vendor_id);

I3C_SLAVE_ATTR_R(vendor_info);
I3C_SLAVE_ATTR_W(vendor_info, u16);
CONFIGFS_ATTR(i3c_slave_func_, vendor_info);

I3C_SLAVE_ATTR_R(part_id);
I3C_SLAVE_ATTR_W(part_id, u16);
CONFIGFS_ATTR(i3c_slave_func_, part_id);

I3C_SLAVE_ATTR_R(instance_id);
I3C_SLAVE_ATTR_W(instance_id, u8);
CONFIGFS_ATTR(i3c_slave_func_, instance_id);

I3C_SLAVE_ATTR_R(ext_id);
I3C_SLAVE_ATTR_W(ext_id, u16);
CONFIGFS_ATTR(i3c_slave_func_, ext_id);

I3C_SLAVE_ATTR_R(max_write_len);
I3C_SLAVE_ATTR_W(max_write_len, u16);
CONFIGFS_ATTR(i3c_slave_func_, max_write_len);

I3C_SLAVE_ATTR_R(max_read_len);
I3C_SLAVE_ATTR_W(max_read_len, u16);
CONFIGFS_ATTR(i3c_slave_func_, max_read_len);

I3C_SLAVE_ATTR_R(bcr);
I3C_SLAVE_ATTR_W(bcr, u8);
CONFIGFS_ATTR(i3c_slave_func_, bcr);

I3C_SLAVE_ATTR_R(dcr);
I3C_SLAVE_ATTR_W(dcr, u8);
CONFIGFS_ATTR(i3c_slave_func_, dcr);

static struct configfs_attribute *i3c_slave_func_attrs[] = {
	&i3c_slave_func_attr_vendor_id,
	&i3c_slave_func_attr_vendor_info,
	&i3c_slave_func_attr_part_id,
	&i3c_slave_func_attr_instance_id,
	&i3c_slave_func_attr_ext_id,
	&i3c_slave_func_attr_max_write_len,
	&i3c_slave_func_attr_max_read_len,
	&i3c_slave_func_attr_bcr,
	&i3c_slave_func_attr_dcr,
	NULL,
};

static const struct config_item_type i3c_slave_func_type = {
	.ct_attrs       = i3c_slave_func_attrs,
	.ct_owner       = THIS_MODULE,
};

static struct config_group *i3c_slave_func_make(struct config_group *group, const char *name)
{
	struct i3c_slave_func_group *func_group;
	struct i3c_slave_func *func;
	int err;

	func_group = kzalloc(sizeof(*func_group), GFP_KERNEL);
	if (!func_group)
		return ERR_PTR(-ENOMEM);

	config_group_init_type_name(&func_group->group, name, &i3c_slave_func_type);

	func = i3c_slave_func_create(group->cg_item.ci_name, name);
	if (IS_ERR(func)) {
		pr_err("failed to create i3c slave function device\n");
		err = -EINVAL;
		goto free_group;
	}

	func->group = &func_group->group;

	func_group->func = func;

	return &func_group->group;

free_group:
	kfree(func_group);

	return ERR_PTR(err);
}

static void i3c_slave_func_drop(struct config_group *group, struct config_item *item)
{
	config_item_put(item);
}

static struct configfs_group_operations i3c_slave_func_group_ops = {
	.make_group     = &i3c_slave_func_make,
	.drop_item      = &i3c_slave_func_drop,
};

static const struct config_item_type i3c_slave_func_group_type = {
	.ct_group_ops   = &i3c_slave_func_group_ops,
	.ct_owner       = THIS_MODULE,
};

/**
 * i3c_slave_cfs_add_func_group() - add I3C slave function group
 * @name: group name
 *
 * Return: Pointer to struct config_group
 */
struct config_group *i3c_slave_cfs_add_func_group(const char *name)
{
	struct config_group *group;

	group = configfs_register_default_group(functions_group, name,
						&i3c_slave_func_group_type);
	if (IS_ERR(group))
		pr_err("failed to register configfs group for %s function\n",
		       name);

	return group;
}
EXPORT_SYMBOL(i3c_slave_cfs_add_func_group);

/**
 * i3c_slave_cfs_remove_func_group() - add I3C slave function group
 * @group: group to be removed
 */
void i3c_slave_cfs_remove_func_group(struct config_group *group)
{
	if (IS_ERR_OR_NULL(group))
		return;

	configfs_unregister_default_group(group);
}
EXPORT_SYMBOL(i3c_slave_cfs_remove_func_group);

static const struct config_item_type i3c_slave_controllers_type = {
	.ct_owner       = THIS_MODULE,
};

static const struct config_item_type i3c_slave_functions_type = {
	.ct_owner       = THIS_MODULE,
};

static const struct config_item_type i3c_slave_type = {
	.ct_owner       = THIS_MODULE,
};

static struct configfs_subsystem i3c_slave_cfs_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "i3c_slave",
			.ci_type = &i3c_slave_type,
		},
	},
	.su_mutex = __MUTEX_INITIALIZER(i3c_slave_cfs_subsys.su_mutex),
};

static int __init i3c_slave_cfs_init(void)
{
	int ret;
	struct config_group *root = &i3c_slave_cfs_subsys.su_group;

	config_group_init(root);

	ret = configfs_register_subsystem(&i3c_slave_cfs_subsys);
	if (ret) {
		pr_err("Error %d while registering subsystem %s\n",
		       ret, root->cg_item.ci_namebuf);
		goto err;
	}

	functions_group = configfs_register_default_group(root, "functions",
							  &i3c_slave_functions_type);
	if (IS_ERR(functions_group)) {
		ret = PTR_ERR(functions_group);
		pr_err("Error %d while registering functions group\n",
		       ret);
		goto err_functions_group;
	}

	controllers_group =
		configfs_register_default_group(root, "controllers",
						&i3c_slave_controllers_type);
	if (IS_ERR(controllers_group)) {
		ret = PTR_ERR(controllers_group);
		pr_err("Error %d while registering controllers group\n",
		       ret);
		goto err_controllers_group;
	}

	return 0;

err_controllers_group:
	configfs_unregister_default_group(functions_group);

err_functions_group:
	configfs_unregister_subsystem(&i3c_slave_cfs_subsys);

err:
	return ret;
}
module_init(i3c_slave_cfs_init);

MODULE_DESCRIPTION("I3C FUNC CONFIGFS");
MODULE_AUTHOR("Frank Li <Frank.Li@nxp.com>");
