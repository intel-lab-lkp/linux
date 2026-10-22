// SPDX-License-Identifier: GPL-2.0
/*
 * mem_provider_sample - a sample memory provider.
 *
 * The module provides physical memory through provider files.
 *
 * The control device /dev/mem_provider_sample creates provider files.
 * Each file allocates its own contiguous memory from alloc_contig_pages().
 * Pages can be dynamically marked present or absent.
 * See mem_provider_sample.h.
 *
 * Confidential VMs are not supported.
 */

#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/guest_memfd.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "mem_provider_sample.h"

/*
 * One provider file.
 */
struct mps_info {
	struct mutex lock;		/* the bitmaps */

	unsigned long base_pfn;
	unsigned long npages;
	struct page *cma_pages;		/* from alloc_contig_pages() */

	unsigned long *absent;		/* taken away by MPS_SET_PRESENT */

	struct inode *gmem_inode;
};

static void mps_zero_range(struct mps_info *info, pgoff_t index,
			    size_t nr_pages)
{
	size_t i;

	if (index >= info->npages)
		return;

	nr_pages = min_t(size_t, nr_pages, info->npages - index);
	for (i = 0; i < nr_pages; i++)
		clear_highpage(info->cma_pages + index + i);
}

static void mps_gmem_put_pfn_range(void *provider, pgoff_t index,
				   size_t nr_pages)
{
	struct file *resource_file = provider;

	mps_zero_range(resource_file->private_data, index, nr_pages);
}

/* Byte range [offset, offset + len) of @info as page indices. */
static int mps_range(struct mps_info *info, u64 offset, u64 len,
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

static void mps_set_present(struct mps_info *info, unsigned long start,
			    unsigned long end, bool present)
{
	mutex_lock(&info->lock);
	if (present) {
		bitmap_clear(info->absent, start, end - start);
	} else {
		bitmap_set(info->absent, start, end - start);
		if (info->gmem_inode) {
			unsigned long pfn = info->base_pfn + start;

			kvm_gmem_revoke(info->gmem_inode, start, end - start, &pfn);
			mps_zero_range(info, start, end - start);
		}
	}
	mutex_unlock(&info->lock);
}

static long mps_get_stats(struct mps_info *info, void __user *uarg)
{
	struct mps_stats st = {};

	mutex_lock(&info->lock);
	st.size = (u64)info->npages << PAGE_SHIFT;
	st.absent_pages = bitmap_weight(info->absent, info->npages);
	mutex_unlock(&info->lock);

	return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;
}

static int mps_file_release(struct inode *inode, struct file *file)
{
	struct mps_info *info = file->private_data;

	if (info->cma_pages)
		free_contig_range(info->base_pfn, info->npages);
	bitmap_free(info->absent);
	kfree(info);
	return 0;
}

static long mps_file_set_present(struct mps_info *info, void __user *uarg)
{
	struct mps_set_present sp;
	unsigned long start, end;
	int ret;

	if (copy_from_user(&sp, uarg, sizeof(sp)))
		return -EFAULT;
	if (sp.value > 1 || sp.pad)
		return -EINVAL;
	ret = mps_range(info, sp.offset, sp.len, &start, &end);
	if (!ret)
		mps_set_present(info, start, end, sp.value);
	return ret;
}

static long mps_file_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct mps_info *info = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case MPS_GET_STATS:
		return mps_get_stats(info, uarg);
	case MPS_SET_PRESENT:
		return mps_file_set_present(info, uarg);
	default:
		return -ENOTTY;
	}
}

static void *mps_gmem_attach(struct inode *gmem_inode,
			     struct file *resource_file)
{
	struct mps_info *info = resource_file->private_data;

	mutex_lock(&info->lock);
	if (info->gmem_inode) {
		mutex_unlock(&info->lock);
		return ERR_PTR(-EBUSY);
	}
	info->gmem_inode = gmem_inode;
	mutex_unlock(&info->lock);

	return get_file(resource_file);
}

static void mps_gmem_release(void *provider)
{
	struct file *resource_file = provider;
	struct mps_info *info = resource_file->private_data;

	mutex_lock(&info->lock);
	info->gmem_inode = NULL;
	mutex_unlock(&info->lock);

	fput(resource_file);
}

static int mps_gmem_get_pfn(void *provider, pgoff_t index, unsigned long *pfn,
			    int *max_order)
{
	struct file *resource_file = provider;
	struct mps_info *info = resource_file->private_data;

	if (index >= info->npages)
		return -EINVAL;

	if (test_bit(index, info->absent))
		return -EFAULT;

	*pfn = info->base_pfn + index;
	if (max_order)
		*max_order = 0;

	return 0;
}

static const struct guest_memfd_provider_operations mps_gmem_provider_ops = {
	.attach		= mps_gmem_attach,
	.release	= mps_gmem_release,
	.get_pfn	= mps_gmem_get_pfn,
	.put_pfn_range	= mps_gmem_put_pfn_range,
};

static const struct file_operations mps_file_fops = {
	.owner		= THIS_MODULE,
	.release	= mps_file_release,
	.unlocked_ioctl	= mps_file_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.gmem_provider_ops = &mps_gmem_provider_ops,
};

static long mps_ctl_new_file(void __user *uarg)
{
	struct mps_new_file nf;
	struct mps_info *info;
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

	pages = alloc_contig_pages(npages, GFP_KERNEL | __GFP_ZERO,
				   numa_node_id(), NULL);
	if (!pages) {
		ret = -ENOMEM;
		goto err_free_absent;
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

	file = anon_inode_getfile("[mem-provider-sample]", &mps_file_fops,
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
err_free_absent:
	bitmap_free(info->absent);
err_free_info:
	kfree(info);
	return ret;
}

static long mps_ctl_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case MPS_NEW_FILE:
		return mps_ctl_new_file(uarg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations mps_ctl_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= mps_ctl_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice mps_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "mem_provider_sample",
	.fops	= &mps_ctl_fops,
};

static int __init mps_init(void)
{
	return misc_register(&mps_dev);
}
module_init(mps_init);

static void __exit mps_exit(void)
{
	misc_deregister(&mps_dev);
}
module_exit(mps_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sample memory provider for guest_memfd and iommufd");
