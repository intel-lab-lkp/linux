// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2024 Amazon.com, Inc. or its affiliates. All rights reserved.
 * Author: Roman Kagan <rkagan@amazon.de>
 *
 * test driver for proclocal memory allocator
 */

#include <linux/compat.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/file.h>
#include <linux/secretmem.h>

struct proclocal_test_alloc {
	u64 size;
	u64 ptr;
};

#define PROCLOCAL_TEST_ALLOC _IOWR('A', 0x10, struct proclocal_test_alloc)

#define BOUNCE_BUF_SIZE PAGE_SIZE

struct proclocal_test {
	struct secretmem_area *area;
	size_t size;
	void *bounce;
};

static int proclocal_test_open(struct inode *inode, struct file *f)
{
	struct proclocal_test *plt;

	plt = kzalloc(sizeof(*plt), GFP_KERNEL);
	if (!plt)
		return -ENOMEM;

	plt->bounce = kmalloc(BOUNCE_BUF_SIZE, GFP_KERNEL);
	if (!plt->bounce) {
		kfree(plt);
		return -ENOMEM;
	}

	f->f_mode |= FMODE_UNSIGNED_OFFSET;
	f->private_data = plt;
	return 0;
}

static int proclocal_test_release(struct inode *inode, struct file *f)
{
	struct proclocal_test *plt = f->private_data;
	if (plt->area)
		secretmem_release_pages(plt->area);
	kfree(plt->bounce);
	kfree(plt);
	return 0;
}

static ssize_t proclocal_test_read(struct file *f, char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct proclocal_test *plt = f->private_data;
	const void *p = (const void *)*ppos;
	ssize_t ret = -EFAULT;

	if (p + count < p)
		return -EINVAL;

	while (count) {
		size_t chunk = min_t(size_t, count, BOUNCE_BUF_SIZE);
		size_t left;

		/*
		 * copy_to_user() disables superuser checks, so need to copy to
		 * bounce buffer first to test the access
		 */
		memcpy(plt->bounce, p, chunk);

		left = copy_to_user(buf, plt->bounce, chunk);
		if (left == chunk)
			goto out;
		chunk -= left;

		buf += chunk;
		p += chunk;
		count -= chunk;
	}

	ret = p - (const void *)*ppos;
	*ppos = (loff_t)p;
out:
	return ret;
}

static ssize_t proclocal_test_write(struct file *f, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct proclocal_test *plt = f->private_data;
	void *p = (void *)*ppos;
	ssize_t ret = -EFAULT;

	if (p + count < p)
		return -EINVAL;

	while (count) {
		size_t chunk = min_t(size_t, count, BOUNCE_BUF_SIZE);
		size_t left;

		/*
		 * copy_from_user() disables superuser checks, so need to copy
		 * to bounce buffer first to test the access
		 */
		left = copy_from_user(plt->bounce, buf, chunk);
		if (left == chunk)
			goto out;
		chunk -= left;

		memcpy(p, plt->bounce, chunk);

		buf += chunk;
		p += chunk;
		count -= chunk;
	}

	ret = p - (void *)*ppos;
	*ppos = (loff_t)p;
out:
	return ret;
}

static long proclocal_test_alloc(struct proclocal_test *plt,
				 void __user *argp)
{
	struct proclocal_test_alloc pta;
	unsigned long pages_needed;

	if (plt->size)
		return -EEXIST;

	if (copy_from_user(&pta, argp, sizeof(pta)))
		return -EFAULT;

	if (!pta.size)
		return -EINVAL;

	pages_needed = (pta.size + PAGE_SIZE - 1) / PAGE_SIZE;
	plt->area = secretmem_allocate_pages(fls(pages_needed - 1));
	if (!plt->area)
		return -ENOMEM;

	plt->size = pta.size;

	pta.ptr = (u64)plt->area->ptr;
	if (copy_to_user(argp, &pta, sizeof(pta)))
		goto err;

	return 0;
err:
	secretmem_release_pages(plt->area);
	plt->area = NULL;
	plt->size = 0;
	return -EFAULT;
}

static long proclocal_test_ioctl(struct file *f, unsigned int ioctl,
				 unsigned long arg)
{
	struct proclocal_test *plt = f->private_data;
	void __user *argp = (void __user *)arg;

	switch (ioctl) {
	case PROCLOCAL_TEST_ALLOC:
		return proclocal_test_alloc(plt, argp);
	default:
		return -EINVAL;
	}
}

static const struct file_operations proclocal_test_fops = {
	.owner          = THIS_MODULE,
	.release        = proclocal_test_release,
	.unlocked_ioctl = proclocal_test_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.open           = proclocal_test_open,
	.read           = proclocal_test_read,
	.write          = proclocal_test_write,
	.llseek		= no_seek_end_llseek,
};

static struct miscdevice proclocal_test_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "proclocal-test",
	.fops  = &proclocal_test_fops,
};
module_misc_device(proclocal_test_misc);

MODULE_VERSION("0.0.1");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Roman Kagan");
MODULE_DESCRIPTION("Test driver for proclocal allocator");
