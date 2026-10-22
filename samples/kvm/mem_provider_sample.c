// SPDX-License-Identifier: GPL-2.0
/*
 * mem_provider_sample - a sample memory provider.
 *
 * The module owns a region of physical memory and lends it through provider
 * files (include/linux/mem_provider.h).  A VMM passes the same file to
 * KVM_CREATE_GUEST_MEMFD, for the guest, and to IOMMU_IOAS_MAP_FILE, for the
 * guest's devices.  The module calls neither KVM nor iommufd: it answers
 * "what is page N" and revokes a range when the answer changes.
 *
 * The control device /dev/mem_provider_sample creates provider files.
 * NEW_CHILD carves part of the region into a child file. A page can also
 * be taken away. See mem_provider_sample.h.
 *
 * Confidential VMs are not supported.
 */

#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/mem_provider.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "mem_provider_sample.h"

/*
 * One provider file.
 *
 * Lock order: mps_root.lock, then mps_info.lock, then the consumers' locks
 * (taken by their revoke callbacks).  get_page() takes no lock, so a
 * consumer can call it from its fault paths and its revoke callback.
 */
struct mps_info {
	struct mem_provider_file mpf;
	struct mutex lock;		/* the bitmaps */

	unsigned long base_pfn;
	unsigned long npages;
	struct page *cma_pages;		/* from alloc_contig_pages(), or NULL */

	unsigned long *absent;		/* taken away by CTL_SET_PRESENT */

	/*
	 * A child of the root: its first page in the root, and whether it
	 * was carved out from the root (is_child).
	 */
	unsigned long root_index;
	bool is_child;
	struct list_head root_link;	/* mps_root.children, mps_root.lock */
};

/*
 * The root region, owned by the control device.  Changes of ownership run
 * under @lock, which is taken before any child's lock.
 */
static struct mps_root {
	struct mutex lock;		/* everything below */
	unsigned long base_pfn;
	unsigned long npages;
	struct page *cma_pages;
	unsigned long *allocated;	/* pages allocated to a child */
	struct list_head children;
} mps_root;

/* Does @info have page @index now?  Called with or without info->lock. */
static bool mps_page_present(struct mps_info *info, unsigned long index)
{
	return !test_bit(index, info->absent);
}

/*
 * The length of the run from @index of pages with the same state as @index:
 * present.
 */
static unsigned long mps_run(struct mps_info *info, unsigned long index)
{
	unsigned long end = info->npages;

	end = min(end, find_next_bit(info->absent, info->npages, index + 1));
	return end - index;
}

/* ---- Provider operations ------------------------------------------------ */

static struct mem_provider_file *
mps_mp_attach(struct file *file, loff_t size)
{
	struct mps_info *info = file->private_data;

	if (size > (loff_t)info->npages << PAGE_SHIFT)
		return ERR_PTR(-EINVAL);
	return &info->mpf;
}

static void mps_mp_detach(struct mem_provider_file *mpf)
{
}

/*
 * Reads the bitmaps without info->lock.  A change runs under the lock and
 * revokes the range afterwards, so an answer that a change makes out of date
 * is dropped by the consumer.
 */
static int mps_mp_get_page(struct mem_provider_file *mpf, pgoff_t index,
			   unsigned long *pfn, int *max_order, u32 *attrs)
{
	struct mps_info *info = container_of(mpf, struct mps_info, mpf);
	unsigned long run;

	if (index >= info->npages)
		return -EINVAL;

	if (!mps_page_present(info, index))
		return -EFAULT;

	/* The aligned block around @index must be one run. */
	run = mps_run(info, index);
	while (*max_order) {
		unsigned long first = ALIGN_DOWN(index, 1UL << *max_order);

		if (first + (1UL << *max_order) <= index + run &&
		    (first == index || mps_page_present(info, first)) &&
		    mps_run(info, first) >= 1UL << *max_order)
			break;
		(*max_order)--;
	}

	*pfn = info->base_pfn + index;
	return 0;
}

static const struct mem_provider_ops mps_mp_ops = {
	.attach		= mps_mp_attach,
	.detach		= mps_mp_detach,
	.get_page	= mps_mp_get_page,
};

/* Tell the consumers that pages [start, end) of @info changed. */
static void mps_revoke(struct mps_info *info, unsigned long start,
		       unsigned long end)
{
	if (end > start)
		mem_provider_revoke(&info->mpf, (loff_t)start << PAGE_SHIFT,
				    (loff_t)(end - start) << PAGE_SHIFT);
}

/* ---- Provider file ------------------------------------------------------ */

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
	if (present)
		bitmap_clear(info->absent, start, end - start);
	else
		bitmap_set(info->absent, start, end - start);
	mps_revoke(info, start, end);
	mutex_unlock(&info->lock);
}

static long mps_get_stats(struct mps_info *info, void __user *uarg)
{
	struct mps_stats st = {};

	mutex_lock(&info->lock);
	st.region_offset = (u64)info->root_index << PAGE_SHIFT;
	st.region_len = (u64)info->npages << PAGE_SHIFT;
	st.absent_pages = bitmap_weight(info->absent, info->npages);
	mutex_unlock(&info->lock);

	return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;
}

/*
 * A child fd is read-only to its holder: only GET_STATS.  The owner changes a
 * child's pages through the control device (MPS_CTL_SET_PRESENT), so a VMM
 * that holds a child cannot.
 */
static long mps_file_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct mps_info *info = file->private_data;
	void __user *uarg = (void __user *)arg;

	if (cmd == MPS_GET_STATS)
		return mps_get_stats(info, uarg);
	return -ENOTTY;
}

/* Every consumer has detached: each one holds a reference to the file. */
static int mps_file_release(struct inode *inode, struct file *file)
{
	struct mps_info *info = file->private_data;

	/* A child gives the pages it has back to the root. */
	if (info->is_child) {
		mutex_lock(&mps_root.lock);
		list_del(&info->root_link);
		bitmap_clear(mps_root.allocated, info->root_index, info->npages);
		mutex_unlock(&mps_root.lock);
	}
	if (info->cma_pages)
		free_contig_range(info->base_pfn, info->npages);
	bitmap_free(info->absent);
	kfree(info);
	return 0;
}

static const struct mem_provider_fops mps_file_fops = {
	.fops = {
		.owner		= THIS_MODULE,
		.fop_flags	= FOP_MEM_PROVIDER,
		.release	= mps_file_release,
		.unlocked_ioctl	= mps_file_ioctl,
		.compat_ioctl	= compat_ptr_ioctl,
	},
	.ops = &mps_mp_ops,
};

/*
 * Make a provider file over [base_pfn, base_pfn + npages).  The fd is
 * reserved but not installed, so that the caller can finish setting up
 * @info before another thread can reach it.
 */
static int mps_new_file(unsigned long base_pfn, unsigned long npages,
			u32 flags, struct mps_info **infop,
			 struct file **filep)
{
	struct mps_info *info;
	struct file *file;
	int fd, ret;

	info = kzalloc_obj(*info);
	if (!info)
		return -ENOMEM;
	mem_provider_file_init(&info->mpf);
	info->base_pfn = base_pfn;
	info->npages = npages;
	info->absent = bitmap_zalloc(npages, GFP_KERNEL);
	if (!info->absent) {
		ret = -ENOMEM;
		goto err_free;
	}
	mutex_init(&info->lock);
	INIT_LIST_HEAD(&info->root_link);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto err_free;
	}
	file = anon_inode_getfile("[mem-provider-sample]", &mps_file_fops.fops,
				  info, O_RDWR);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		ret = PTR_ERR(file);
		goto err_free;
	}
	*infop = info;
	*filep = file;
	return fd;

err_free:
	bitmap_free(info->absent);
	kfree(info);
	return ret;
}

static struct page *mps_alloc_region(unsigned long npages)
{
	struct page *pages;

	pages = alloc_contig_pages(npages, GFP_KERNEL | __GFP_ZERO,
				   numa_node_id(), NULL);
	return pages;
}

/* ---- Root and children -------------------------------------------------- */

/* Create the root on the first NEW_CHILD of @size. */
static int mps_root_ensure(u64 size)
{
	struct page *pages;
	unsigned long npages;

	lockdep_assert_held(&mps_root.lock);
	if (mps_root.npages)
		return 0;

	if (!size || !PAGE_ALIGNED(size))
		return -EINVAL;
	npages = size >> PAGE_SHIFT;
	pages = mps_alloc_region(npages);
	if (!pages)
		return -ENOMEM;
	mps_root.base_pfn = page_to_pfn(pages);
	mps_root.allocated = bitmap_zalloc(npages, GFP_KERNEL);
	if (!mps_root.allocated) {
		free_contig_range(page_to_pfn(pages), npages);
		return -ENOMEM;
	}
	mps_root.cma_pages = pages;
	mps_root.npages = npages;
	return 0;
}

static void mps_root_teardown(void)
{
	if (!mps_root.npages)
		return;
	WARN_ON(!list_empty(&mps_root.children));
	if (mps_root.cma_pages)
		free_contig_range(mps_root.base_pfn, mps_root.npages);
	bitmap_free(mps_root.allocated);
}

/* A page-aligned byte range of the root as page indices [first, last). */
static int mps_root_range(u64 offset, u64 length, unsigned long *first,
			  unsigned long *last)
{
	if (!length || !PAGE_ALIGNED(offset) || !PAGE_ALIGNED(length))
		return -EINVAL;
	*first = offset >> PAGE_SHIFT;
	*last = *first + (length >> PAGE_SHIFT);
	if (*last <= *first || *last > mps_root.npages)
		return -EINVAL;
	return 0;
}

/* Return a referenced child file made by NEW_CHILD. */
static struct file *mps_get_child(int fd, struct mps_info **infop)
{
	struct file *file = fget(fd);

	if (!file)
		return ERR_PTR(-EBADF);
	if (file->f_op != &mps_file_fops.fops ||
	    !((struct mps_info *)file->private_data)->is_child) {
		fput(file);
		return ERR_PTR(-EINVAL);
	}
	*infop = file->private_data;
	return file;
}

static long mps_ctl_new_child(void __user *uarg)
{
	struct mps_new_child nc;
	unsigned long first, last;
	struct mps_info *info;
	struct file *file;
	int fd, ret;

	if (copy_from_user(&nc, uarg, sizeof(nc)))
		return -EFAULT;
	if (nc.flags || nc.pad)
		return -EINVAL;

	mutex_lock(&mps_root.lock);
	ret = mps_root_ensure(nc.offset + nc.len);
	if (ret)
		goto out_unlock;
	ret = mps_root_range(nc.offset, nc.len, &first, &last);
	if (ret)
		goto out_unlock;

	if (find_next_bit(mps_root.allocated, last, first) < last) {
		ret = -EBUSY;
		goto out_unlock;
	}

	fd = mps_new_file(mps_root.base_pfn + first, last - first, nc.flags,
			  &info, &file);
	if (fd < 0) {
		ret = fd;
		goto out_unlock;
	}

	bitmap_set(mps_root.allocated, first, last - first);
	info->is_child = true;
	info->root_index = first;
	list_add(&info->root_link, &mps_root.children);
	mutex_unlock(&mps_root.lock);
	fd_install(fd, file);
	return fd;

out_unlock:
	mutex_unlock(&mps_root.lock);
	return ret;
}

/*
 * CTL_SET_PRESENT changes a child's pages, by the owner on the control device.
 * A VMM that holds the child fd cannot, so it cannot undo them.
 */
static long mps_ctl_set_present(void __user *uarg)
{
	struct mps_ctl_range cr;
	unsigned long start, end;
	struct mps_info *info;
	struct file *file;
	int ret;

	if (copy_from_user(&cr, uarg, sizeof(cr)))
		return -EFAULT;
	if (cr.value > 1)
		return -EINVAL;
	file = mps_get_child(cr.fd, &info);
	if (IS_ERR(file))
		return PTR_ERR(file);
	ret = mps_range(info, cr.offset, cr.len, &start, &end);
	if (!ret)
		mps_set_present(info, start, end, cr.value);
	fput(file);
	return ret;
}

static long mps_ctl_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case MPS_NEW_CHILD:
		return mps_ctl_new_child(uarg);
	case MPS_CTL_SET_PRESENT:
		return mps_ctl_set_present(uarg);
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
	int ret;

	mutex_init(&mps_root.lock);
	INIT_LIST_HEAD(&mps_root.children);

	ret = misc_register(&mps_dev);
	if (ret)
		return ret;
	return 0;
}
module_init(mps_init);

static void __exit mps_exit(void)
{
	misc_deregister(&mps_dev);
	mps_root_teardown();
}
module_exit(mps_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sample memory provider for guest_memfd and iommufd");
