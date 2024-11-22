// SPDX-License-Identifier: GPL-2.0
#include <linux/fanotify.h>
#include <linux/kobject.h>
#include <linux/module.h>

#include "fanotify.h"

extern struct srcu_struct fsnotify_mark_srcu;

static DEFINE_SPINLOCK(filter_list_lock);
static LIST_HEAD(filter_list);

static struct kobject *fan_filter_root_kobj;

static struct {
	enum fanotify_filter_flags flag;
	const char *name;
} fanotify_filter_flags_names[] = {
	{
		.flag = FAN_FILTER_F_SYS_ADMIN_ONLY,
		.name = "SYS_ADMIN_ONLY",
	}
};

static ssize_t flags_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	struct fanotify_filter_ops *ops;
	ssize_t len = 0;
	int i;

	ops = container_of(kobj, struct fanotify_filter_ops, kobj);
	for (i = 0; i < ARRAY_SIZE(fanotify_filter_flags_names); i++) {
		if (ops->flags & fanotify_filter_flags_names[i].flag) {
			len += sysfs_emit_at(buf, len, "%s%s", len ? " " : "",
					     fanotify_filter_flags_names[i].name);
		}
	}
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t desc_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	struct fanotify_filter_ops *ops;

	ops = container_of(kobj, struct fanotify_filter_ops, kobj);

	return sysfs_emit(buf, "%s\n", ops->desc ?: "N/A");
}

static ssize_t init_args_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct fanotify_filter_ops *ops;

	ops = container_of(kobj, struct fanotify_filter_ops, kobj);

	return sysfs_emit(buf, "%s\n", ops->init_args ?: "N/A");
}

static struct kobj_attribute flags_kobj_attr = __ATTR_RO(flags);
static struct kobj_attribute desc_kobj_attr = __ATTR_RO(desc);
static struct kobj_attribute init_args_kobj_attr = __ATTR_RO(init_args);

static struct attribute *fan_filter_attrs[] = {
	&flags_kobj_attr.attr,
	&desc_kobj_attr.attr,
	&init_args_kobj_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fan_filter);

static void fan_filter_kobj_release(struct kobject *kobj)
{
}

static const struct kobj_type fan_filter_ktype = {
	.release = fan_filter_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = fan_filter_groups,
};

static struct fanotify_filter_ops *fanotify_filter_find(const char *name)
{
	struct fanotify_filter_ops *ops;

	list_for_each_entry(ops, &filter_list, list) {
		if (!strcmp(ops->name, name))
			return ops;
	}
	return NULL;
}

static void __fanotify_filter_unregister(struct fanotify_filter_ops *ops)
{
	spin_lock(&filter_list_lock);
	list_del_init(&ops->list);
	spin_unlock(&filter_list_lock);
}

/*
 * fanotify_filter_register - Register a new filter.
 *
 * Add a filter to the filter_list. These filter are
 * available for all users in the system.
 *
 * @ops:	pointer to fanotify_filter_ops to add.
 *
 * Returns:
 *	0	- on success;
 *	-EEXIST	- filter of the same name already exists.
 *	-ENODEV	- fanotify filter was not properly initialized.
 */
int fanotify_filter_register(struct fanotify_filter_ops *ops)
{
	int ret;

	if (!fan_filter_root_kobj)
		return -ENODEV;

	spin_lock(&filter_list_lock);
	if (fanotify_filter_find(ops->name)) {
		/* cannot register two filters with the same name */
		spin_unlock(&filter_list_lock);
		return -EEXIST;
	}
	list_add_tail(&ops->list, &filter_list);
	spin_unlock(&filter_list_lock);


	kobject_init(&ops->kobj, &fan_filter_ktype);
	ret = kobject_add(&ops->kobj, fan_filter_root_kobj, "%s", ops->name);
	if (ret) {
		__fanotify_filter_unregister(ops);
		return ret;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(fanotify_filter_register);

/*
 * fanotify_filter_unregister - Unregister a new filter.
 *
 * Remove a filter from filter_list.
 *
 * @ops:	pointer to fanotify_filter_ops to remove.
 */
void fanotify_filter_unregister(struct fanotify_filter_ops *ops)
{
	kobject_put(&ops->kobj);
	__fanotify_filter_unregister(ops);
}
EXPORT_SYMBOL_GPL(fanotify_filter_unregister);

/*
 * fanotify_filter_add - Add a filter to fsnotify_group.
 *
 * Add a filter from filter_list to a fsnotify_group.
 *
 * @group:	fsnotify_group that will have add
 * @argp:	fanotify_filter_args that specifies the filter
 *		and the init arguments of the filter.
 *
 * Returns:
 *	0	- on success;
 *	-EEXIST	- filter of the same name already exists.
 */
int fanotify_filter_add(struct fsnotify_group *group,
			struct fanotify_filter_args __user *argp)
{
	struct fanotify_filter_hook *filter_hook;
	struct fanotify_filter_ops *filter_ops;
	struct fanotify_filter_args args;
	void *init_args = NULL;
	int ret = 0;

	ret = copy_from_user(&args, argp, sizeof(args));
	if (ret)
		return -EFAULT;

	if (args.init_args_size > FAN_FILTER_ARGS_MAX)
		return -EINVAL;

	args.name[FAN_FILTER_NAME_MAX - 1] = '\0';

	fsnotify_group_lock(group);

	if (rcu_access_pointer(group->fanotify_data.filter_hook)) {
		fsnotify_group_unlock(group);
		return -EBUSY;
	}

	filter_hook = kzalloc(sizeof(*filter_hook), GFP_KERNEL);
	if (!filter_hook) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock(&filter_list_lock);
	filter_ops = fanotify_filter_find(args.name);
	if (!filter_ops || !try_module_get(filter_ops->owner)) {
		spin_unlock(&filter_list_lock);
		ret = -ENOENT;
		goto err_free_hook;
	}
	spin_unlock(&filter_list_lock);

	if (!capable(CAP_SYS_ADMIN) && (filter_ops->flags & FAN_FILTER_F_SYS_ADMIN_ONLY)) {
		ret = -EPERM;
		goto err_module_put;
	}

	if (filter_ops->filter_init) {
		if (args.init_args_size != filter_ops->init_args_size) {
			ret = -EINVAL;
			goto err_module_put;
		}
		if (args.init_args_size) {
			init_args = kzalloc(args.init_args_size, GFP_KERNEL);
			if (!init_args) {
				ret = -ENOMEM;
				goto err_module_put;
			}
			if (copy_from_user(init_args, (void __user *)args.init_args,
					   args.init_args_size)) {
				ret = -EFAULT;
				goto err_free_args;
			}

		}
		ret = filter_ops->filter_init(group, filter_hook, init_args);
		if (ret)
			goto err_free_args;
		kfree(init_args);
	}
	filter_hook->ops = filter_ops;
	rcu_assign_pointer(group->fanotify_data.filter_hook, filter_hook);

out:
	fsnotify_group_unlock(group);
	return ret;

err_free_args:
	kfree(init_args);
err_module_put:
	module_put(filter_ops->owner);
err_free_hook:
	kfree(filter_hook);
	goto out;
}

void fanotify_filter_hook_free(struct fanotify_filter_hook *filter_hook)
{
	if (filter_hook->ops->filter_free)
		filter_hook->ops->filter_free(filter_hook);

	module_put(filter_hook->ops->owner);
	kfree(filter_hook);
}

/*
 * fanotify_filter_del - Delete a filter from fsnotify_group.
 */
void fanotify_filter_del(struct fsnotify_group *group)
{
	struct fanotify_filter_hook *filter_hook;

	fsnotify_group_lock(group);
	filter_hook = group->fanotify_data.filter_hook;
	if (!filter_hook)
		goto out;

	rcu_assign_pointer(group->fanotify_data.filter_hook, NULL);
	fanotify_filter_hook_free(filter_hook);

out:
	fsnotify_group_unlock(group);
}

static int __init fanotify_filter_init(void)
{
	fan_filter_root_kobj = kobject_create_and_add("fanotify_filter", kernel_kobj);
	if (!fan_filter_root_kobj)
		return -ENOMEM;
	return 0;
}
device_initcall(fanotify_filter_init);
