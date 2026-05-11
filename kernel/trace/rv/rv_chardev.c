// SPDX-License-Identifier: GPL-2.0

#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/rv.h>
#include <uapi/linux/rv.h>

#include "rv.h"

static_assert(MAX_RV_MONITOR_NAME_SIZE == RV_MONITOR_NAME_MAX,
	      "RV internal and UAPI monitor name size constants must match");

struct rv_fd_priv {
	const struct rv_chardev_ops	*ops;
	void				*monitor_priv;
};

struct rv_chardev_entry {
	char			name[MAX_RV_MONITOR_NAME_SIZE];
	const struct rv_chardev_ops *ops;
	struct list_head	list;
};

/* Protected by rv_interface_lock (from rv.h / rv.c). */
static LIST_HEAD(rv_chardev_list);

/**
 * rv_chardev_register_monitor - expose a monitor via /dev/rv
 * @name: Monitor name, must match the rv_monitor .name field.
 * @ops:  Callbacks providing bind / ioctl / release.
 *
 * Returns 0 on success, -EINVAL if @name is too long, -EEXIST if @name is
 * already registered, -ENOMEM on OOM.
 */
int rv_chardev_register_monitor(const char *name,
				const struct rv_chardev_ops *ops)
{
	struct rv_chardev_entry *e, *existing;

	if (strlen(name) >= MAX_RV_MONITOR_NAME_SIZE)
		return -EINVAL;

	e = kmalloc_obj(*e, GFP_KERNEL);
	if (!e)
		return -ENOMEM;

	strscpy(e->name, name, sizeof(e->name));
	e->ops = ops;

	guard(mutex)(&rv_interface_lock);
	list_for_each_entry(existing, &rv_chardev_list, list) {
		if (strcmp(existing->name, name) == 0) {
			kfree(e);
			return -EEXIST;
		}
	}
	list_add_tail(&e->list, &rv_chardev_list);
	return 0;
}
EXPORT_SYMBOL_GPL(rv_chardev_register_monitor);

/**
 * rv_chardev_unregister_monitor - remove a monitor from the /dev/rv registry
 * @name: Monitor name previously passed to rv_chardev_register_monitor().
 *
 * Existing bound fds remain valid; their ops pointer is stable until the
 * fd is closed.  The caller must ensure no new binds to this monitor can
 * succeed after unregistration — typically by unregistering before unloading
 * the module that provides the ops.
 */
void rv_chardev_unregister_monitor(const char *name)
{
	struct rv_chardev_entry *e, *tmp;

	guard(mutex)(&rv_interface_lock);
	list_for_each_entry_safe(e, tmp, &rv_chardev_list, list) {
		if (strcmp(e->name, name) == 0) {
			list_del(&e->list);
			kfree(e);
			return;
		}
	}
}
EXPORT_SYMBOL_GPL(rv_chardev_unregister_monitor);

static int rv_dev_open(struct inode *inode, struct file *file)
{
	struct rv_fd_priv *fp;

	fp = kzalloc_obj(*fp, GFP_KERNEL);
	if (!fp)
		return -ENOMEM;

	file->private_data = fp;
	return 0;
}

static int rv_dev_release(struct inode *inode, struct file *file)
{
	struct rv_fd_priv *fp = file->private_data;

	if (fp->ops) {
		fp->ops->release(fp->monitor_priv);
		module_put(fp->ops->owner);
	}
	kfree(fp);
	return 0;
}

static int rv_bind_monitor(struct rv_fd_priv *fp, const char __user *uarg)
{
	const struct rv_chardev_ops *ops = NULL;
	struct rv_bind_args args;
	void *priv;

	if (fp->ops)
		return -EBUSY;

	if (copy_from_user(&args, uarg, sizeof(args)))
		return -EFAULT;

	args.monitor_name[RV_MONITOR_NAME_MAX - 1] = '\0';

	/*
	 * Pin the owning module while the list entry is still valid under
	 * rv_interface_lock, preventing a concurrent rmmod from completing
	 * between lookup and reference acquisition.  bind() may sleep
	 * (GFP_KERNEL inside), so it runs after the lock is dropped.
	 */
	scoped_guard(mutex, &rv_interface_lock) {
		struct rv_chardev_entry *e;

		list_for_each_entry(e, &rv_chardev_list, list) {
			if (strcmp(e->name, args.monitor_name) != 0)
				continue;
			if (!try_module_get(e->ops->owner))
				return -ENODEV;
			ops = e->ops;
			break;
		}
	}

	if (!ops)
		return -ENOENT;

	priv = ops->bind();
	if (IS_ERR(priv)) {
		module_put(ops->owner);
		return PTR_ERR(priv);
	}

	fp->ops = ops;
	fp->monitor_priv = priv;
	return 0;
}

static long rv_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct rv_fd_priv *fp = file->private_data;

	if (cmd == RV_IOCTL_BIND_MONITOR)
		return rv_bind_monitor(fp, (const char __user *)arg);

	if (!fp->ops)
		return -ENXIO;

	return fp->ops->ioctl(fp->monitor_priv, cmd, arg);
}

static __poll_t rv_dev_poll(struct file *file, poll_table *wait)
{
	struct rv_fd_priv *fp = file->private_data;

	if (!fp->ops || !fp->ops->poll)
		return 0;

	return fp->ops->poll(fp->monitor_priv, file, wait);
}

static const struct file_operations rv_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= rv_dev_open,
	.release	= rv_dev_release,
	.unlocked_ioctl	= rv_dev_ioctl,
	.compat_ioctl	= rv_dev_ioctl,
	.poll		= rv_dev_poll,
};

static struct miscdevice rv_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "rv",
	.fops	= &rv_dev_fops,
};

int __init rv_chardev_init(void)
{
	return misc_register(&rv_miscdev);
}
