// SPDX-License-Identifier: GPL-2.0
/*
 * linux/drivers/char/misc.c
 *
 * Generic misc open routine by Johan Myreen
 *
 * Based on code from Linus
 *
 * Teemu Rantanen's Microsoft Busmouse support and Derrick Cole's
 *   changes incorporated into 0.97pl4
 *   by Peter Cervasio (pete%q106fm.uucp@wupost.wustl.edu) (08SEP92)
 *   See busmouse.c for particulars.
 *
 * Made things a lot mode modular - easy to compile in just one or two
 * of the misc drivers, as they are now completely independent. Linus.
 *
 * Support for loadable modules. 8-Sep-95 Philip Blundell <pjb27@cam.ac.uk>
 *
 * Fixed a failing symbol register to free the device registration
 *		Alan Cox <alan@lxorguk.ukuu.org.uk> 21-Jan-96
 *
 * Dynamic minors and /proc/mice by Alessandro Rubini. 26-Mar-96
 *
 * Renamed to misc and miscdevice to be more accurate. Alan Cox 26-Mar-96
 *
 * Handling of mouse minor numbers for kerneld:
 *  Idea by Jacques Gelinas <jack@solucorp.qc.ca>,
 *  adapted by Bjorn Ekwall <bj0rn@blox.se>
 *  corrected by Alan Cox <alan@lxorguk.ukuu.org.uk>
 *
 * Changes for kmod (from kerneld):
 *	Cyrus Durgin <cider@speakeasy.org>
 *
 * Added devfs support. Richard Gooch <rgooch@atnf.csiro.au>  10-Jan-1998
 */

#include <linux/module.h>

#include <linux/cleanup.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/srcu.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/tty.h>
#include <linux/kmod.h>
#include <linux/gfp.h>

/*
 * Head entry for the doubly linked miscdevice list
 */
static LIST_HEAD(misc_list);
static LIST_HEAD(misc_sync_ctx_list);
static DEFINE_MUTEX(misc_mtx);
DEFINE_STATIC_SRCU(misc_srcu);

/*
 * Assigned numbers.
 */
static DEFINE_IDA(misc_minors_ida);

static int misc_minor_alloc(int minor)
{
	int ret = 0;

	if (minor == MISC_DYNAMIC_MINOR) {
		/* allocate free id */
		ret = ida_alloc_range(&misc_minors_ida, MISC_DYNAMIC_MINOR + 1,
				      MINORMASK, GFP_KERNEL);
	} else {
		ret = ida_alloc_range(&misc_minors_ida, minor, minor, GFP_KERNEL);
	}
	return ret;
}

static void misc_minor_free(int minor)
{
	ida_free(&misc_minors_ida, minor);
}

#ifdef CONFIG_PROC_FS
static void *misc_seq_start(struct seq_file *seq, loff_t *pos)
{
	seq->private = (void *)(long)srcu_read_lock(&misc_srcu);
	return seq_list_start_rcu(&misc_list, *pos);
}

static void *misc_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	return seq_list_next_rcu(v, &misc_list, pos);
}

static void misc_seq_stop(struct seq_file *seq, void *v)
{
	srcu_read_unlock(&misc_srcu, (int)(long)seq->private);
}

static int misc_seq_show(struct seq_file *seq, void *v)
{
	const struct miscdevice *p = list_entry_rcu(v, struct miscdevice, list);

	seq_printf(seq, "%3i %s\n", p->minor, p->name ? p->name : "");
	return 0;
}


static const struct seq_operations misc_seq_ops = {
	.start = misc_seq_start,
	.next  = misc_seq_next,
	.stop  = misc_seq_stop,
	.show  = misc_seq_show,
};
#endif

static struct miscdevice *misc_find(int minor)
{
	struct miscdevice *iter;

	list_for_each_entry_srcu(iter, &misc_list, list,
				 srcu_read_lock_held(&misc_srcu)) {
		if (iter->minor == minor)
			return iter;
	}

	return NULL;
}

#define DEFINE_SYNC_FOPS(member, ret_type, PROTO, ARGS)			\
	static ret_type misc_sync_##member PROTO			\
	{								\
		struct miscdevice *c;					\
									\
		guard(srcu)(&misc_srcu);				\
									\
		c = misc_find(iminor(filp->f_inode));			\
		if (!c)							\
			return -ENODEV;					\
									\
		return c->fops->member ARGS;				\
	}

DEFINE_SYNC_FOPS(read, ssize_t,
	(struct file *filp, char __user *buf, size_t len, loff_t *off),
	(filp, buf, len, off))
DEFINE_SYNC_FOPS(unlocked_ioctl, long,
		 (struct file *filp, unsigned int cmd, unsigned long arg),
		 (filp, cmd, arg))
DEFINE_SYNC_FOPS(compat_ioctl, long,
		 (struct file *filp, unsigned int cmd, unsigned long arg),
		 (filp, cmd, arg))

static void misc_sync_ctx_release(struct kref *kref)
{
	struct miscdevice_sync_ctx *ctx = container_of(kref, typeof(*ctx), kref);

	misc_minor_free(ctx->minor);
	scoped_guard(mutex, &misc_mtx)
		list_del_rcu(&ctx->list);
	synchronize_srcu(&misc_srcu);
	kfree(ctx);
}

static int misc_sync_release(struct inode *inode, struct file *filp)
{
	int minor = iminor(filp->f_inode);
	struct miscdevice *c;
	struct miscdevice_sync_ctx *iter, *ctx = NULL;

	scoped_guard(srcu, &misc_srcu) {
		c = misc_find(minor);
		if (c) {
			/* The miscdevice is still registered. */
			ctx = c->sync_ctx;
		} else {
			/* The miscdeivce is unregistered.  Search in the list. */
			list_for_each_entry_srcu(iter, &misc_sync_ctx_list,
					list, srcu_read_lock_held(&misc_srcu)) {
				if (iter->minor == minor) {
					ctx = iter;
					break;
				}
			}
			if (!ctx) {
				pr_err("Cannot find miscdevice_sync_ctx\n");
				return -ENOENT;
			}
		}
	}

	/* Restore it so that the corresponding fops_put() works. */
	filp->f_op = ctx->orig_fops;
	kref_put(&ctx->kref, misc_sync_ctx_release);

	/* Call to the original .release() if any. */
	if (filp->f_op->release)
		return filp->f_op->release(inode, filp);
	return 0;
}

static int misc_open(struct inode *inode, struct file *file)
{
	int minor = iminor(inode);
	struct miscdevice *c = NULL;
	int err = -ENODEV;
	const struct file_operations *new_fops = NULL;
	int idx;

	idx = srcu_read_lock(&misc_srcu);

	c = misc_find(minor);
	if (c)
		new_fops = fops_get(c->fops);

	/* Only request module for fixed minor code */
	if (!new_fops && minor < MISC_DYNAMIC_MINOR) {
		srcu_read_unlock(&misc_srcu, idx);
		request_module("char-major-%d-%d", MISC_MAJOR, minor);
		idx = srcu_read_lock(&misc_srcu);

		c = misc_find(minor);
		if (c)
			new_fops = fops_get(c->fops);
	}

	if (!new_fops)
		goto fail;

	/*
	 * Place the miscdevice in the file's
	 * private_data so it can be used by the
	 * file operations, including f_op->open below
	 */
	file->private_data = c;

	err = 0;
	replace_fops(file, new_fops);
	if (c->sync_ctx) {
		file->f_op = &c->sync_ctx->fops;
		kref_get(&c->sync_ctx->kref);
	}
	if (file->f_op->open)
		err = file->f_op->open(inode, file);
fail:
	srcu_read_unlock(&misc_srcu, idx);
	return err;
}

static char *misc_devnode(const struct device *dev, umode_t *mode)
{
	const struct miscdevice *c = dev_get_drvdata(dev);

	if (mode && c->mode)
		*mode = c->mode;
	if (c->nodename)
		return kstrdup(c->nodename, GFP_KERNEL);
	return NULL;
}

static const struct class misc_class = {
	.name		= "misc",
	.devnode	= misc_devnode,
};

static const struct file_operations misc_fops = {
	.owner		= THIS_MODULE,
	.open		= misc_open,
	.llseek		= noop_llseek,
};

/**
 *	misc_register	-	register a miscellaneous device
 *	@misc: device structure
 *
 *	Register a miscellaneous device with the kernel. If the minor
 *	number is set to %MISC_DYNAMIC_MINOR a minor number is assigned
 *	and placed in the minor field of the structure. For other cases
 *	the minor number requested is used.
 *
 *	The structure passed is linked into the kernel and may not be
 *	destroyed until it has been unregistered. By default, an open()
 *	syscall to the device sets file->private_data to point to the
 *	structure. Drivers don't need open in fops for this.
 *
 *	A zero is returned on success and a negative errno code for
 *	failure.
 */

int misc_register(struct miscdevice *misc)
{
	dev_t dev;
	bool is_dynamic = (misc->minor == MISC_DYNAMIC_MINOR);

	if (misc->minor > MISC_DYNAMIC_MINOR) {
		pr_err("Invalid fixed minor %d for miscdevice '%s'\n",
		       misc->minor, misc->name);
		return -EINVAL;
	}

	INIT_LIST_HEAD(&misc->list);

	guard(mutex)(&misc_mtx);

	if (is_dynamic) {
		int i = misc_minor_alloc(misc->minor);

		if (i < 0)
			return -EBUSY;
		misc->minor = i;
	} else {
		int i;

		scoped_guard(srcu, &misc_srcu) {
			if (misc_find(misc->minor))
				return -EBUSY;
		}

		i = misc_minor_alloc(misc->minor);
		if (i < 0)
			return -EBUSY;
	}

	dev = MKDEV(MISC_MAJOR, misc->minor);

	misc->this_device =
		device_create_with_groups(&misc_class, misc->parent, dev,
					  misc, misc->groups, "%s", misc->name);
	if (IS_ERR(misc->this_device)) {
		misc_minor_free(misc->minor);
		if (is_dynamic) {
			misc->minor = MISC_DYNAMIC_MINOR;
		}
		return PTR_ERR(misc->this_device);
	}

	/*
	 * Add it to the front, so that later devices can "override"
	 * earlier defaults
	 */
	list_add_rcu(&misc->list, &misc_list);
	return 0;
}
EXPORT_SYMBOL(misc_register);

/**
 *	misc_deregister - unregister a miscellaneous device
 *	@misc: device to unregister
 *
 *	Unregister a miscellaneous device that was previously
 *	successfully registered with misc_register().
 */

void misc_deregister(struct miscdevice *misc)
{
	scoped_guard(mutex, &misc_mtx)
		list_del_rcu(&misc->list);
	synchronize_srcu(&misc_srcu);
	INIT_LIST_HEAD(&misc->list);

	device_destroy(&misc_class, MKDEV(MISC_MAJOR, misc->minor));

	/* Defer to free the minor number for sync fops */
	if (!misc->sync_ctx) {
		misc_minor_free(misc->minor);
	} else {
		scoped_guard(mutex, &misc_mtx)
			list_add_rcu(&misc->sync_ctx->list,
				     &misc_sync_ctx_list);
		kref_put(&misc->sync_ctx->kref, misc_sync_ctx_release);
	}

	if (misc->minor > MISC_DYNAMIC_MINOR)
		misc->minor = MISC_DYNAMIC_MINOR;
}
EXPORT_SYMBOL(misc_deregister);

int misc_sync_register(struct miscdevice *misc)
{
	struct miscdevice_sync_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ret = misc_register(misc);
	if (ret) {
		kfree(ctx);
		return ret;
	}

	ctx->minor = misc->minor;
	kref_init(&ctx->kref);
	ctx->orig_fops = misc->fops;
	INIT_LIST_HEAD(&ctx->list);

	/* Use any fops as default in case the misc sync doesn't support them. */
	memcpy(&ctx->fops, misc->fops, sizeof(struct file_operations));

	/* Override fops that support sync. */
	if (misc->fops->read)
		ctx->fops.read = misc_sync_read;
	if (misc->fops->unlocked_ioctl)
		ctx->fops.unlocked_ioctl = misc_sync_unlocked_ioctl;
	if (misc->fops->compat_ioctl)
		ctx->fops.compat_ioctl = misc_sync_compat_ioctl;

	/* .release() is used to drop the reference to the sync context. */
	ctx->fops.release = misc_sync_release;

	misc->sync_ctx = ctx;
	return 0;
}
EXPORT_SYMBOL(misc_sync_register);

static int __init misc_init(void)
{
	int err;
	struct proc_dir_entry *misc_proc_file;

	misc_proc_file = proc_create_seq("misc", 0, NULL, &misc_seq_ops);
	err = class_register(&misc_class);
	if (err)
		goto fail_remove;

	err = __register_chrdev(MISC_MAJOR, 0, MINORMASK + 1, "misc", &misc_fops);
	if (err < 0)
		goto fail_printk;
	return 0;

fail_printk:
	pr_err("unable to get major %d for misc devices\n", MISC_MAJOR);
	class_unregister(&misc_class);
fail_remove:
	if (misc_proc_file)
		remove_proc_entry("misc", NULL);
	return err;
}
subsys_initcall(misc_init);
