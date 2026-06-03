// SPDX-License-Identifier: GPL-2.0
/*
 * memfd_tripwire - Memory file descriptor with write notification support.
 *
 * Creates an anonymous memory-backed file descriptor that supports polling for
 * detecting writes to its mappings.
 *
 * Theory of operation:
 *  1.  memfd_tripwire() to create a new instance
 *  2.  Pass the fd / mapping to the (possibly out-of-process) producer
 *  3a. The producer uses the memory region as normal memory. Writes stick, but
 *      generate a notification as a side effect.
 *  3b. The consumer polls the file descriptor:
 *       a. POLLIN is observed when the memory region gets written
 *       b. Call ioctl(MEMFD_TRIPWIRE_ACK) to restore write protection
 *       c. Inspect memory contents and react as appropriate
 */

#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/folio_batch.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/poll.h>
#include <linux/pseudo_fs.h>
#include <linux/rmap.h>
#include <linux/slab.h>
#include <linux/syscalls.h>

#include <uapi/linux/magic.h>
#include <uapi/linux/memfd_tripwire.h>

struct tripwire_info {
	atomic_t dirty;
	wait_queue_head_t wqh;
};

static struct tripwire_info *to_tripwire_info(struct file *file)
{
	return file->private_data;
}

static vm_fault_t tripwire_fault(struct vm_fault *vmf)
{
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	pgoff_t offset = vmf->pgoff;
	gfp_t gfp = vmf->gfp_mask;
	struct folio *folio;
	int err;

	if (((loff_t)offset << PAGE_SHIFT) >= i_size_read(inode))
		return vmf_error(-EINVAL);

retry:
	folio = filemap_lock_folio(mapping, offset);
	if (IS_ERR(folio)) {
		folio = folio_alloc(gfp | __GFP_ZERO, 0);
		if (!folio)
			return VM_FAULT_OOM;

		__folio_mark_uptodate(folio);
		err = filemap_add_folio(mapping, folio, offset, gfp);
		if (unlikely(err)) {
			folio_put(folio);
			if (err == -EEXIST)
				goto retry;
			return vmf_error(err);
		}
	}

	vmf->page = folio_file_page(folio, offset);
	return VM_FAULT_LOCKED;
}

static vm_fault_t tripwire_page_mkwrite(struct vm_fault *vmf)
{
	struct tripwire_info *info = to_tripwire_info(vmf->vma->vm_file);
	struct folio *folio = page_folio(vmf->page);

	/*
	 * Note that this might be racing with a concurrent ACK. We need to
	 * guarantee that the dirty flag and page protection state remains
	 * consistent and that no notifications are lost.
	 *
	 * The actual update to mark the PTE writable only happens after this
	 * function completes, so the window between here and PTE update must
	 * be protected against concurrent modifications by the ACK code path.
	 * Taking the folio lock before notifying consumers conveniently
	 * makes sure that ACKs can only complete after our PTE updates go
	 * through, preventing a situation where an interleaved ACK clears the
	 * dirty flag but we're still going ahead to mark the PTE writable.
	 */
	folio_lock(folio);

	if (atomic_cmpxchg(&info->dirty, 0, 1) == 0)
		wake_up_poll(&info->wqh, EPOLLIN);

	if (folio->mapping != vmf->vma->vm_file->f_mapping) {
		folio_unlock(folio);
		return VM_FAULT_NOPAGE;
	}

	return VM_FAULT_LOCKED;
}

static const struct vm_operations_struct tripwire_vm_ops = {
	.fault = tripwire_fault,
	.page_mkwrite = tripwire_page_mkwrite,
};

static int tripwire_mmap_prepare(struct vm_area_desc *desc)
{
	file_accessed(desc->file);
	desc->vm_ops = &tripwire_vm_ops;
	return 0;
}

static __poll_t tripwire_poll(struct file *file, poll_table *wait)
{
	struct tripwire_info *info = to_tripwire_info(file);
	__poll_t mask = 0;

	poll_wait(file, &info->wqh, wait);

	if (atomic_read(&info->dirty))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static void tripwire_ack(struct file *file)
{
	struct tripwire_info *info = to_tripwire_info(file);
	struct address_space *mapping = file->f_mapping;
	struct folio_batch fbatch;
	pgoff_t index = 0;
	int i;

	/*
	 * Note that this flag update is not protected by the folio locks taken
	 * below. Hence a concurrent writer might sneak in and switch back to
	 * dirty state. That's OK though, since it merely results in a spurious
	 * notification.
	 */
	atomic_set(&info->dirty, 0);

	folio_batch_init(&fbatch);
	while (filemap_get_folios(mapping, &index, ~0UL, &fbatch)) {
		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			folio_lock(fbatch.folios[i]);
			folio_mkclean(fbatch.folios[i]);
			folio_unlock(fbatch.folios[i]);
		}
		folio_batch_release(&fbatch);
		cond_resched();
	}
}

static long tripwire_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	switch (cmd) {
	case MEMFD_TRIPWIRE_ACK:
		if (arg != 0)
			return -EINVAL;
		tripwire_ack(file);
		return 0;
	default:
		return -ENOTTY;
	}
}

static int tripwire_release(struct inode *inode, struct file *file)
{
	kfree(to_tripwire_info(file));
	return 0;
}

static const struct file_operations tripwire_fops = {
	.release = tripwire_release,
	.mmap_prepare = tripwire_mmap_prepare,
	.poll = tripwire_poll,
	.unlocked_ioctl = tripwire_ioctl,
};

static const struct address_space_operations tripwire_aops = {
	.dirty_folio = noop_dirty_folio,
};

static int tripwire_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			    struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	unsigned int ia_valid = iattr->ia_valid;
	int ret;

	filemap_invalidate_lock(inode->i_mapping);

	/* Allowing size to change only once avoids the need to unmap here. */
	if ((ia_valid & ATTR_SIZE) && inode->i_size)
		ret = -EINVAL;
	else
		ret = simple_setattr(idmap, dentry, iattr);

	filemap_invalidate_unlock(inode->i_mapping);

	return ret;
}

static const struct inode_operations tripwire_iops = {
	.setattr = tripwire_setattr,
};

SYSCALL_DEFINE1(memfd_tripwire, unsigned int, flags)
{
	struct tripwire_info *info;
	struct inode *inode;
	struct file *file;

	if (flags != 0)
		return -EINVAL;

	info = kzalloc_obj(struct tripwire_info);
	if (!info)
		return -ENOMEM;

	init_waitqueue_head(&info->wqh);

	file = anon_inode_create_getfile("memfd_tripwire", &tripwire_fops, info,
					 O_RDWR, NULL);
	if (IS_ERR(file)) {
		kfree(info);
		return PTR_ERR(file);
	}

	inode = file_inode(file);
	inode->i_op = &tripwire_iops;
	inode->i_mapping->a_ops = &tripwire_aops;
	inode->i_mode |= S_IFREG;
	inode->i_size = 0;

	return FD_ADD(O_CLOEXEC, file);
}
