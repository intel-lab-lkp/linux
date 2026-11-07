// SPDX-License-Identifier: GPL-2.0
/*
 * memory_provider - a sample memory provider.
 *
 * The module provides physical memory through provider files.
 *
 * The control device /dev/memory_provider creates provider files.
 * Each file allocates its own contiguous memory from alloc_contig_pages().
 * Pages can be dynamically marked present or absent.
 * See memory_provider.h.
 *
 * Confidential VMs are not supported.
 */

#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "memory_provider.h"

/*
 * One provider file.
 */
struct mp_info {
	unsigned long base_pfn;
	unsigned long npages;
	struct page *cma_pages;		/* from alloc_contig_pages() */

	struct mutex lock;		/* the bitmaps */
	unsigned long *absent;		/* taken away by MP_SET_PRESENT */
	unsigned long *readonly;	/* marked read-only by MP_SET_READONLY */
};

/* Byte range [offset, offset + len) of @info as page indices. */
static int mp_range(struct mp_info *info, u64 offset, u64 len,
		    unsigned long *start, unsigned long *end)
{
	if (!len || !PAGE_ALIGNED(offset) || !PAGE_ALIGNED(len))
		return -EINVAL;

	*start = offset >> PAGE_SHIFT;
	*end = *start + (len >> PAGE_SHIFT);

	if (*end > info->npages || *end < *start)
		return -EINVAL;

	return 0;
}

static void mp_set_readonly(struct mp_info *info, unsigned long start,
			    unsigned long end, bool readonly)
{
	mutex_lock(&info->lock);

	if (readonly)
		bitmap_set(info->readonly, start, end - start);
	else
		bitmap_clear(info->readonly, start, end - start);

	mutex_unlock(&info->lock);
}

static void mp_set_present(struct mp_info *info, unsigned long start,
			   unsigned long end, bool present)
{
	mutex_lock(&info->lock);

	if (present)
		bitmap_clear(info->absent, start, end - start);
	else
		bitmap_set(info->absent, start, end - start);

	mutex_unlock(&info->lock);
}

static int mp_file_release(struct inode *inode, struct file *file)
{
	struct mp_info *info = file->private_data;

	if (info->cma_pages)
		free_contig_range(info->base_pfn, info->npages);
	bitmap_free(info->readonly);
	bitmap_free(info->absent);
	kfree(info);
	return 0;
}

static long mp_file_set_readonly(struct mp_info *info, void __user *uarg)
{
	struct mp_set_readonly sr;
	unsigned long start, end;
	int ret;

	if (copy_from_user(&sr, uarg, sizeof(sr)))
		return -EFAULT;
	if (sr.value > 1 || sr.pad)
		return -EINVAL;

	ret = mp_range(info, sr.offset, sr.len, &start, &end);
	if (!ret)
		mp_set_readonly(info, start, end, sr.value);

	return ret;
}

static long mp_file_set_present(struct mp_info *info, void __user *uarg)
{
	struct mp_set_present sp;
	unsigned long start, end;
	int ret;

	if (copy_from_user(&sp, uarg, sizeof(sp)))
		return -EFAULT;
	if (sp.value > 1 || sp.pad)
		return -EINVAL;

	ret = mp_range(info, sp.offset, sp.len, &start, &end);
	if (!ret)
		mp_set_present(info, start, end, sp.value);

	return ret;
}

static long mp_file_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct mp_info *info = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case MP_SET_PRESENT:
		return mp_file_set_present(info, uarg);
	case MP_SET_READONLY:
		return mp_file_set_readonly(info, uarg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations mp_file_fops = {
	.owner		= THIS_MODULE,
	.release	= mp_file_release,
	.unlocked_ioctl	= mp_file_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static long mp_ctl_new_file(void __user *uarg)
{
	struct mp_new_file nf;
	struct mp_info *info;
	struct page *pages;
	struct file *file;
	unsigned long npages;
	int fd, ret;

	if (copy_from_user(&nf, uarg, sizeof(nf)))
		return -EFAULT;
	if (nf.flags || nf.pad)
		return -EINVAL;
	if (!nf.size || !PAGE_ALIGNED(nf.size))
		return -EINVAL;

	npages = nf.size >> PAGE_SHIFT;
	if (npages > KMALLOC_MAX_SIZE * BITS_PER_BYTE)
		return -EINVAL;

	info = kzalloc_obj(*info);
	if (!info)
		return -ENOMEM;

	info->absent = bitmap_zalloc(npages, GFP_KERNEL);
	if (!info->absent) {
		ret = -ENOMEM;
		goto err_free_info;
	}

	info->readonly = bitmap_zalloc(npages, GFP_KERNEL);
	if (!info->readonly) {
		ret = -ENOMEM;
		goto err_free_absent;
	}

	pages = alloc_contig_pages(npages, GFP_KERNEL | __GFP_ZERO,
				   numa_node_id(), NULL);
	if (!pages) {
		ret = -ENOMEM;
		goto err_free_readonly;
	}

	info->cma_pages = pages;
	info->base_pfn = page_to_pfn(pages);
	info->npages = npages;
	mutex_init(&info->lock);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto err_free_pages;
	}

	file = anon_inode_getfile("[memory-provider]", &mp_file_fops,
				  info, O_RDWR);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		ret = PTR_ERR(file);
		goto err_free_pages;
	}

	fd_install(fd, file);
	return fd;

err_free_pages:
	free_contig_range(page_to_pfn(pages), npages);
err_free_readonly:
	bitmap_free(info->readonly);
err_free_absent:
	bitmap_free(info->absent);
err_free_info:
	kfree(info);
	return ret;
}

static long mp_ctl_ioctl(struct file *file, unsigned int cmd,
			 unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case MP_NEW_FILE:
		return mp_ctl_new_file(uarg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations mp_ctl_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= mp_ctl_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice mp_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "memory_provider",
	.fops	= &mp_ctl_fops,
};

static int __init mp_init(void)
{
	return misc_register(&mp_dev);
}
module_init(mp_init);

static void __exit mp_exit(void)
{
	misc_deregister(&mp_dev);
}
module_exit(mp_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sample memory provider for guest_memfd and iommufd");
