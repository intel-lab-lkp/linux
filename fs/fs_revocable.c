// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 Google LLC
 *
 * File operation replacement with Revocable
 */

#include <linux/cleanup.h>
#include <linux/fs_revocable.h>
#include <linux/poll.h>
#include <linux/revocable.h>

struct fops_replacement {
	struct file *filp;
	void *orig_private_data;
	const struct file_operations *orig_fops;
	struct file_operations fops;
	struct revocable *rev;
};

/*
 * Recover the private_data to its original one.
 */
static struct fops_replacement *_recover_private_data(struct file *filp)
{
	struct fops_replacement *fr = filp->private_data;

	filp->private_data = fr->orig_private_data;
	return fr;
}

/*
 * Replace the private_data to fops_replacement.
 */
static void _replace_private_data(struct fops_replacement *fr)
{
	fr->filp->private_data = fr;
}

DEFINE_CLASS(fops_replacement, struct fops_replacement *,
	     _replace_private_data(_T), _recover_private_data(filp),
	     struct file *filp)

static ssize_t fs_revocable_read(struct file *filp, char __user *buffer,
				 size_t length, loff_t *offset)
{
	void *any;
	CLASS(fops_replacement, fr)(filp);

	REVOCABLE_TRY_ACCESS_WITH(fr->rev, any);
	if (!any)
		return -ENODEV;

	return fr->orig_fops->read(filp, buffer, length, offset);
}

static __poll_t fs_revocable_poll(struct file *filp, poll_table *wait)
{
	void *any;
	CLASS(fops_replacement, fr)(filp);

	REVOCABLE_TRY_ACCESS_WITH(fr->rev, any);
	if (!any)
		return -ENODEV;

	return fr->orig_fops->poll(filp, wait);
}

static long fs_revocable_unlocked_ioctl(struct file *filp, unsigned int cmd,
					unsigned long arg)
{
	void *any;
	CLASS(fops_replacement, fr)(filp);

	REVOCABLE_TRY_ACCESS_WITH(fr->rev, any);
	if (!any)
		return -ENODEV;

	return fr->orig_fops->unlocked_ioctl(filp, cmd, arg);
}

static int fs_revocable_release(struct inode *inode, struct file *filp)
{
	struct fops_replacement *fr = _recover_private_data(filp);
	int ret = 0;
	void *any;

	filp->f_op = fr->orig_fops;

	if (!fr->orig_fops->release)
		goto leave;

	REVOCABLE_TRY_ACCESS_SCOPED(fr->rev, any) {
		if (!any) {
			ret = -ENODEV;
			goto leave;
		}

		ret = fr->orig_fops->release(inode, filp);
	}

leave:
	kfree(fr);
	return ret;
}

/**
 * fs_revocable_replace() - Replace the file operations to be revocable-aware.
 * @rp: The revocable resource provider.
 * @filp: The opening file.
 *
 * This replaces @filp->f_op to a set of wrappers.  The wrappers return -ENODEV
 * if the resource provided by @rp has been revoked.  Note that it doesn't
 * concern how the file operations access the resource but only care about if
 * the resource is still available.
 *
 * This should only be used after @filp->f_op->open().  It assumes the
 * @filp->private_data would be set only once in @filp->f_op->open() and wouldn't
 * update in subsequent file operations.
 */
int fs_revocable_replace(struct revocable_provider *rp, struct file *filp)
{
	struct fops_replacement *fr;

	fr = kzalloc(sizeof(*fr), GFP_KERNEL);
	if (!fr)
		return -ENOMEM;

	fr->rev = revocable_alloc(rp);
	if (!fr->rev)
		goto free_fr;

	fr->filp = filp;
	fr->orig_private_data = filp->private_data;
	fr->orig_fops = filp->f_op;

	memcpy(&fr->fops, filp->f_op, sizeof(fr->fops));
	fr->fops.release = fs_revocable_release;

	if (fr->fops.read)
		fr->fops.read = fs_revocable_read;
	if (fr->fops.poll)
		fr->fops.poll = fs_revocable_poll;
	if (fr->fops.unlocked_ioctl)
		fr->fops.unlocked_ioctl = fs_revocable_unlocked_ioctl;

	filp->f_op = &fr->fops;
	filp->private_data = fr;
	return 0;
free_fr:
	kfree(fr);
	if (filp->f_op->release)
		filp->f_op->release(filp->f_inode, filp);
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(fs_revocable_replace);
