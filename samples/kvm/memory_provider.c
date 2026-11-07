// SPDX-License-Identifier: GPL-2.0
/*
 * memory_provider - a sample memory provider.
 *
 * The module provides physical memory through provider files.
 *
 * The control device /dev/memory_provider creates provider files.
 * Each file allocates its own contiguous memory from alloc_contig_pages().
 * See memory_provider.h.
 *
 * Confidential VMs are not supported.
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
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
};

static int mp_file_release(struct inode *inode, struct file *file)
{
	struct mp_info *info = file->private_data;

	if (info->cma_pages)
		free_contig_range(info->base_pfn, info->npages);
	kfree(info);
	return 0;
}

static const struct file_operations mp_file_fops = {
	.owner		= THIS_MODULE,
	.release	= mp_file_release,
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

	info = kzalloc_obj(*info);
	if (!info)
		return -ENOMEM;

	pages = alloc_contig_pages(npages, GFP_KERNEL | __GFP_ZERO,
				   numa_node_id(), NULL);
	if (!pages) {
		ret = -ENOMEM;
		goto err_free_info;
	}

	info->cma_pages = pages;
	info->base_pfn = page_to_pfn(pages);
	info->npages = npages;

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
