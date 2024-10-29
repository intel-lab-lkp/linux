// SPDX-License-Identifier: GPL-2.0
#include <linux/fanotify.h>
#include <linux/module.h>

#include "fanotify.h"

extern struct srcu_struct fsnotify_mark_srcu;

static DEFINE_SPINLOCK(fp_list_lock);
static LIST_HEAD(fp_list);

static struct fanotify_fastpath_ops *fanotify_fastpath_find(const char *name)
{
	struct fanotify_fastpath_ops *ops;

	list_for_each_entry(ops, &fp_list, list) {
		if (!strcmp(ops->name, name))
			return ops;
	}
	return NULL;
}


/*
 * fanotify_fastpath_register - Register a new fastpath handler.
 *
 * Add a fastpath handler to the fp_list. These fastpath handlers are
 * available for all users in the system.
 *
 * @ops:	pointer to fanotify_fastpath_ops to add.
 *
 * Returns:
 *	0	- on success;
 *	-EEXIST	- fastpath handler of the same name already exists.
 */
int fanotify_fastpath_register(struct fanotify_fastpath_ops *ops)
{
	spin_lock(&fp_list_lock);
	if (fanotify_fastpath_find(ops->name)) {
		/* cannot register two handlers with the same name */
		spin_unlock(&fp_list_lock);
		return -EEXIST;
	}
	list_add_tail(&ops->list, &fp_list);
	spin_unlock(&fp_list_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(fanotify_fastpath_register);

/*
 * fanotify_fastpath_unregister - Unregister a new fastpath handler.
 *
 * Remove a fastpath handler from fp_list.
 *
 * @ops:	pointer to fanotify_fastpath_ops to remove.
 */
void fanotify_fastpath_unregister(struct fanotify_fastpath_ops *ops)
{
	spin_lock(&fp_list_lock);
	list_del_init(&ops->list);
	spin_unlock(&fp_list_lock);
}
EXPORT_SYMBOL_GPL(fanotify_fastpath_unregister);

/*
 * fanotify_fastpath_add - Add a fastpath handler to fsnotify_group.
 *
 * Add a fastpath handler from fp_list to a fsnotify_group.
 *
 * @group:	fsnotify_group that will have add
 * @argp:	fanotify_fastpath_args that specifies the fastpath handler
 *		and the init arguments of the fastpath handler.
 *
 * Returns:
 *	0	- on success;
 *	-EEXIST	- fastpath handler of the same name already exists.
 */
int fanotify_fastpath_add(struct fsnotify_group *group,
			  struct fanotify_fastpath_args __user *argp)
{
	struct fanotify_fastpath_hook *fp_hook;
	struct fanotify_fastpath_ops *fp_ops;
	struct fanotify_fastpath_args args;
	int ret = 0;

	ret = copy_from_user(&args, argp, sizeof(args));
	if (ret)
		return -EFAULT;

	if (args.version != 1 || args.flags || args.init_args_len > FAN_FP_ARGS_MAX)
		return -EINVAL;

	args.name[FAN_FP_NAME_MAX - 1] = '\0';

	fsnotify_group_lock(group);

	if (rcu_access_pointer(group->fanotify_data.fp_hook)) {
		fsnotify_group_unlock(group);
		return -EBUSY;
	}

	fp_hook = kzalloc(sizeof(*fp_hook), GFP_KERNEL);
	if (!fp_hook) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock(&fp_list_lock);
	fp_ops = fanotify_fastpath_find(args.name);
	if (!fp_ops || !try_module_get(fp_ops->owner)) {
		spin_unlock(&fp_list_lock);
		ret = -ENOENT;
		goto err_free_hook;
	}
	spin_unlock(&fp_list_lock);

	if (fp_ops->fp_init) {
		char *init_args = NULL;

		if (args.init_args_len) {
			init_args = strndup_user(u64_to_user_ptr(args.init_args),
						 args.init_args_len);
			if (IS_ERR(init_args)) {
				ret = PTR_ERR(init_args);
				if (ret == -EINVAL)
					ret = -E2BIG;
				goto err_module_put;
			}
		}
		ret = fp_ops->fp_init(fp_hook, init_args);
		kfree(init_args);
		if (ret)
			goto err_module_put;
	}
	fp_hook->ops = fp_ops;
	rcu_assign_pointer(group->fanotify_data.fp_hook, fp_hook);

out:
	fsnotify_group_unlock(group);
	return ret;

err_module_put:
	module_put(fp_ops->owner);
err_free_hook:
	kfree(fp_hook);
	goto out;
}

void fanotify_fastpath_hook_free(struct fanotify_fastpath_hook *fp_hook)
{
	if (fp_hook->ops->fp_free)
		fp_hook->ops->fp_free(fp_hook);

	module_put(fp_hook->ops->owner);
}

void fanotify_fastpath_del(struct fsnotify_group *group)
{
	struct fanotify_fastpath_hook *fp_hook;

	fsnotify_group_lock(group);
	fp_hook = group->fanotify_data.fp_hook;
	if (!fp_hook)
		goto out;

	rcu_assign_pointer(group->fanotify_data.fp_hook, NULL);
	fanotify_fastpath_hook_free(fp_hook);

out:
	fsnotify_group_unlock(group);
}
