// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/falloc.h>
#include <linux/guest_memfd.h>
#include <linux/pagemap.h>
#include <linux/set_memory.h>
#include <linux/wait.h>

#include "internal.h"

static DECLARE_WAIT_QUEUE_HEAD(safe_wait);

/**
 * struct guest_memfd_private - private per-folio data
 * @accessible: number of kernel users expecting folio to be accessible.
 *              When zero, the folio converts to being inaccessible.
 * @safe: number of "safe" references to the folio. Each reference is
 *        aware that the folio can be made (in)accessible at any time.
 */
struct guest_memfd_private {
	atomic_t accessible;
	atomic_t safe;
};

static inline int folio_set_direct_map_invalid_noflush(struct folio *folio)
{
	unsigned long i, nr = folio_nr_pages(folio);
	int r;

	for (i = 0; i < nr; i++) {
		struct page *page = folio_page(folio, i);

		r = set_direct_map_invalid_noflush(page);
		if (r)
			goto out_remap;
	}
	/**
	 * Currently no need to flush as hypervisor will also be flushing
	 * tlb when giving the folio to guest.
	 */

	return 0;
out_remap:
	for (; i > 0; i--) {
		struct page *page = folio_page(folio, i - 1);

		BUG_ON(set_direct_map_default_noflush(page));
	}

	return r;
}

static inline void folio_set_direct_map_default_noflush(struct folio *folio)
{
	unsigned long i, nr = folio_nr_pages(folio);

	for (i = 0; i < nr; i++) {
		struct page *page = folio_page(folio, i);

		BUG_ON(set_direct_map_default_noflush(page));
	}
}

static inline int base_safe_refs(struct folio *folio)
{
	/* 1 for filemap */
	return 1 + folio_nr_pages(folio);
}

/**
 * guest_memfd_grab_folio() -- grabs a folio from the guest memfd
 * @file: guest memfd file to grab from
 *        Caller must ensure file is a guest_memfd file
 * @index: the page index in the file
 * @flags: bitwise OR of guest_memfd_grab_flags
 *
 * If a folio is returned, the folio was successfully initialized or converted
 * to (in)accessible based on the GUEST_MEMFD_* flags. The folio is guaranteed
 * to be (in)accessible until the folio lock is relinquished. After folio
 * lock relinquished, ->prepare_inaccessible and ->prepare_accessible ops are
 * responsible for preventing transitioning between the states as
 * required.
 *
 * This function may return error even if the folio exists, in the event
 * the folio couldn't be made (in)accessible as requested in the flags.
 *
 * This function may sleep.
 *
 * The caller must call either guest_memfd_put_folio() or
 * guest_memfd_unsafe_folio().
 *
 * Returns:
 * A pointer to the grabbed folio on success, otherwise an error code.
 */
struct folio *guest_memfd_grab_folio(struct file *file, pgoff_t index, u32 flags)
{
	const bool accessible = flags & GUEST_MEMFD_GRAB_ACCESSIBLE;
	struct inode *inode = file_inode(file);
	struct guest_memfd_operations *ops = inode->i_private;
	struct guest_memfd_private *private;
	unsigned long gmem_flags;
	struct folio *folio;
	int r;

	/* TODO: Support huge pages. */
	folio = __filemap_get_folio(inode->i_mapping, index,
			FGP_LOCK | FGP_ACCESSED | FGP_CREAT | FGP_STABLE,
			mapping_gfp_mask(inode->i_mapping));
	if (IS_ERR(folio))
		return folio;

	if (folio_test_uptodate(folio)) {
		private = folio_get_private(folio);
		atomic_inc(&private->safe);
		if (accessible)
			r = guest_memfd_make_accessible(folio);
		else
			r = guest_memfd_make_inaccessible(folio);

		if (r) {
			atomic_dec(&private->safe);
			goto out_err;
		}

		wake_up_all(&safe_wait);
		return folio;
	}

	private = kmalloc(sizeof(*private), GFP_KERNEL);
	if (!private) {
		r = -ENOMEM;
		goto out_err;
	}

	folio_attach_private(folio, private);
	/*
	 * 1 for us
	 * 1 for unmapping from userspace
	 */
	atomic_set(&private->accessible, accessible ? 2 : 0);
	/*
	 * +1 for us
	 */
	atomic_set(&private->safe, 1 + base_safe_refs(folio));

	gmem_flags = (unsigned long)inode->i_mapping->i_private_data;

	/*
	 * Use the up-to-date flag to track whether or not the memory has been
	 * zeroed before being handed off to the guest.  There is no backing
	 * storage for the memory, so the folio will remain up-to-date until
	 * it's removed.
	 */
	if (accessible || (gmem_flags & GUEST_MEMFD_FLAG_CLEAR_INACCESSIBLE)) {
		unsigned long nr_pages = folio_nr_pages(folio);
		unsigned long i;

		for (i = 0; i < nr_pages; i++)
			clear_highpage(folio_page(folio, i));
	}

	if (accessible) {
		if (ops->prepare_accessible) {
			r = ops->prepare_accessible(inode, folio);
			if (r < 0)
				goto out_free;
		}
	} else {
		if (gmem_flags & GUEST_MEMFD_FLAG_REMOVE_DIRECT_MAP) {
			r = folio_set_direct_map_invalid_noflush(folio);
			if (r < 0)
				goto out_free;
		}

		if (ops->prepare_inaccessible) {
			r = ops->prepare_inaccessible(inode, folio);
			if (r < 0)
				goto out_free;
		}
	}

	folio_mark_uptodate(folio);
	/*
	 * Ignore accessed, referenced, and dirty flags.  The memory is
	 * unevictable and there is no storage to write back to.
	 */
	return folio;
out_free:
	kfree(private);
out_err:
	folio_unlock(folio);
	folio_put(folio);
	return ERR_PTR(r);
}
EXPORT_SYMBOL_GPL(guest_memfd_grab_folio);

/**
 * guest_memfd_put_folio() - Drop safe and accessible references to a folio
 * @folio: the folio to drop references to
 * @accessible_refs: number of accessible refs to drop, 0 if holding a
 *                   reference to an inaccessible folio.
 */
void guest_memfd_put_folio(struct folio *folio, unsigned int accessible_refs)
{
	struct guest_memfd_private *private = folio_get_private(folio);

	WARN_ON_ONCE(atomic_sub_return(accessible_refs, &private->accessible) < 0);
	atomic_dec(&private->safe);
	folio_put(folio);
	wake_up_all(&safe_wait);
}
EXPORT_SYMBOL_GPL(guest_memfd_put_folio);

/**
 * guest_memfd_unsafe_folio() - Demotes the current folio reference to "unsafe"
 * @folio: the folio to demote
 *
 * Decrements the number of safe references to this folio. The folio will not
 * transition to inaccessible until the folio_ref_count is also decremented.
 *
 * This function does not release the folio reference count.
 */
void guest_memfd_unsafe_folio(struct folio *folio)
{
	struct guest_memfd_private *private = folio_get_private(folio);

	atomic_dec(&private->safe);
	wake_up_all(&safe_wait);
}
EXPORT_SYMBOL_GPL(guest_memfd_unsafe_folio);

/**
 * guest_memfd_make_accessible() - Attempt to make the folio accessible to host
 * @folio: the folio to make accessible
 *
 * Makes the given folio accessible to the host. If the folio is currently
 * inaccessible, attempts to convert it to accessible. Otherwise, returns with
 * EBUSY.
 *
 * This function may sleep.
 */
int guest_memfd_make_accessible(struct folio *folio)
{
	struct guest_memfd_private *private = folio_get_private(folio);
	struct inode *inode = folio_inode(folio);
	struct guest_memfd_operations *ops = inode->i_private;
	unsigned long gmem_flags;
	int r;

	/*
	 * If we already know the folio is accessible, then no need to do
	 * anything else.
	 */
	if (atomic_inc_not_zero(&private->accessible))
		return 0;

	r = wait_event_timeout(safe_wait,
			       folio_ref_count(folio) == atomic_read(&private->safe),
			       msecs_to_jiffies(10));
	if (!r)
		return -EBUSY;

	gmem_flags = (unsigned long)inode->i_mapping->i_private_data;
	if (gmem_flags & GUEST_MEMFD_FLAG_REMOVE_DIRECT_MAP)
		folio_set_direct_map_default_noflush(folio);

	if (ops->prepare_accessible) {
		r = ops->prepare_accessible(inode, folio);
		if (r)
			return r;
	}

	atomic_inc(&private->accessible);
	return 0;
}
EXPORT_SYMBOL_GPL(guest_memfd_make_accessible);

/**
 * guest_memfd_make_inaccessible() - Attempt to make the folio inaccessible
 * @folio: the folio to make inaccessible
 *
 * Makes the given folio inaccessible to the host. IF the folio is currently
 * accessible, attempt so convert it to inaccessible. Otherwise, returns with
 * EBUSY.
 *
 * Conversion to inaccessible is allowed when ->accessible decrements to zero,
 * the folio safe counter == folio reference counter, the folio is unmapped
 * from host, and ->prepare_inaccessible returns it's ready to do so.
 *
 * This function may sleep.
 */
int guest_memfd_make_inaccessible(struct folio *folio)
{
	struct guest_memfd_private *private = folio_get_private(folio);
	struct inode *inode = folio_inode(folio);
	struct guest_memfd_operations *ops = inode->i_private;
	unsigned long gmem_flags;
	int r;

	r = atomic_dec_if_positive(&private->accessible);
	if (r < 0)
		return 0;
	else if (r > 0)
		return -EBUSY;

	unmap_mapping_folio(folio);

	r = wait_event_timeout(safe_wait,
			       folio_ref_count(folio) == atomic_read(&private->safe),
			       msecs_to_jiffies(10));
	if (!r) {
		r = -EBUSY;
		goto err;
	}

	gmem_flags = (unsigned long)inode->i_mapping->i_private_data;
	if (gmem_flags & GUEST_MEMFD_FLAG_REMOVE_DIRECT_MAP) {
		r = folio_set_direct_map_invalid_noflush(folio);
		if (r)
			goto err;
	}

	if (ops->prepare_inaccessible) {
		r = ops->prepare_inaccessible(inode, folio);
		if (r)
			goto err;
	}

	return 0;
err:
	atomic_inc(&private->accessible);
	return r;
}
EXPORT_SYMBOL_GPL(guest_memfd_make_inaccessible);

static vm_fault_t gmem_fault(struct vm_fault *vmf)
{
	struct file *file = vmf->vma->vm_file;
	struct guest_memfd_private *private;
	struct folio *folio;

	folio = guest_memfd_grab_folio(file, vmf->pgoff, GUEST_MEMFD_GRAB_ACCESSIBLE);
	if (IS_ERR(folio))
		return VM_FAULT_SIGBUS;

	vmf->page = folio_page(folio, vmf->pgoff - folio_index(folio));

	/**
	 * Drop the safe and accessible references, the folio refcount will
	 * be preserved and unmap_mapping_folio() will decrement the
	 * refcount when converting to inaccessible.
	 */
	private = folio_get_private(folio);
	atomic_dec(&private->accessible);
	atomic_dec(&private->safe);

	return VM_FAULT_LOCKED;
}

static const struct vm_operations_struct gmem_vm_ops = {
	.fault = gmem_fault,
};

static int gmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	const struct guest_memfd_operations *ops = file_inode(file)->i_private;

	if (!ops->prepare_accessible)
		return -EPERM;

	/* No support for private mappings to avoid COW.  */
	if ((vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) !=
	    (VM_SHARED | VM_MAYSHARE))
		return -EINVAL;

	file_accessed(file);
	vma->vm_ops = &gmem_vm_ops;
	return 0;
}

static long gmem_punch_hole(struct file *file, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	const struct guest_memfd_operations *ops = inode->i_private;
	pgoff_t start = offset >> PAGE_SHIFT;
	unsigned long nr = len >> PAGE_SHIFT;
	long ret;

	/*
	 * Bindings must be stable across invalidation to ensure the start+end
	 * are balanced.
	 */
	filemap_invalidate_lock(inode->i_mapping);

	ret = ops->invalidate_begin(inode, start, nr);
	if (ret)
		goto out;

	truncate_inode_pages_range(inode->i_mapping, offset, offset + len - 1);

	if (ops->invalidate_end)
		ops->invalidate_end(inode, start, nr);

out:
	filemap_invalidate_unlock(inode->i_mapping);

	return 0;
}

static long gmem_allocate(struct file *file, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	pgoff_t start, index, end;
	int r;

	/* Dedicated guest is immutable by default. */
	if (offset + len > i_size_read(inode))
		return -EINVAL;

	filemap_invalidate_lock_shared(mapping);

	start = offset >> PAGE_SHIFT;
	end = (offset + len) >> PAGE_SHIFT;

	r = 0;
	for (index = start; index < end;) {
		struct folio *folio;

		if (signal_pending(current)) {
			r = -EINTR;
			break;
		}

		folio = guest_memfd_grab_folio(file, index, 0);
		if (!folio) {
			r = -ENOMEM;
			break;
		}

		index = folio_next_index(folio);

		folio_unlock(folio);
		guest_memfd_put_folio(folio, 0);

		/* 64-bit only, wrapping the index should be impossible. */
		if (WARN_ON_ONCE(!index))
			break;

		cond_resched();
	}

	filemap_invalidate_unlock_shared(mapping);

	return r;
}

static long gmem_fallocate(struct file *file, int mode, loff_t offset,
			   loff_t len)
{
	int ret;

	if (!(mode & FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		return -EOPNOTSUPP;

	if (!PAGE_ALIGNED(offset) || !PAGE_ALIGNED(len))
		return -EINVAL;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		ret = gmem_punch_hole(file, offset, len);
	else
		ret = gmem_allocate(file, offset, len);

	if (!ret)
		file_modified(file);
	return ret;
}

static int gmem_release(struct inode *inode, struct file *file)
{
	struct guest_memfd_operations *ops = inode->i_private;

	return ops->release(inode);
}

static const struct file_operations gmem_fops = {
	.open = generic_file_open,
	.llseek = generic_file_llseek,
	.mmap = gmem_mmap,
	.release = gmem_release,
	.fallocate = gmem_fallocate,
	.owner = THIS_MODULE,
};

static int gmem_migrate_folio(struct address_space *mapping, struct folio *dst,
			      struct folio *src, enum migrate_mode mode)
{
	WARN_ON_ONCE(1);
	return -EINVAL;
}

static int gmem_error_folio(struct address_space *mapping, struct folio *folio)
{
	struct inode *inode = mapping->host;
	struct guest_memfd_operations *ops = inode->i_private;
	off_t offset = folio->index;
	size_t nr = folio_nr_pages(folio);
	unsigned long gmem_flags;
	int ret;

	filemap_invalidate_lock_shared(mapping);

	ret = ops->invalidate_begin(inode, offset, nr);
	if (!ret && ops->invalidate_end)
		ops->invalidate_end(inode, offset, nr);

	filemap_invalidate_unlock_shared(mapping);

	gmem_flags = (unsigned long)inode->i_mapping->i_private_data;
	if (gmem_flags & GUEST_MEMFD_FLAG_REMOVE_DIRECT_MAP)
		folio_set_direct_map_default_noflush(folio);

	return ret;
}

static bool gmem_release_folio(struct folio *folio, gfp_t gfp)
{
	struct guest_memfd_private *private = folio_get_private(folio);
	struct inode *inode = folio_inode(folio);
	struct guest_memfd_operations *ops = inode->i_private;
	off_t offset = folio->index;
	size_t nr = folio_nr_pages(folio);
	unsigned long val, expected, gmem_flags;
	int ret;

	ret = ops->invalidate_begin(inode, offset, nr);
	if (ret)
		return false;
	if (ops->invalidate_end)
		ops->invalidate_end(inode, offset, nr);

	gmem_flags = (unsigned long)inode->i_mapping->i_private_data;
	if (gmem_flags & GUEST_MEMFD_FLAG_REMOVE_DIRECT_MAP)
		folio_set_direct_map_default_noflush(folio);

	expected = base_safe_refs(folio);
	val = atomic_read(&private->safe);
	WARN_ONCE(val != expected, "folio[%x] safe ref: %d != expected %d\n",
		  folio_index(folio), val, expected);

	folio_detach_private(folio);
	kfree(private);

	return true;
}

static void gmem_invalidate_folio(struct folio *folio, size_t offset, size_t len)
{
	WARN_ON_ONCE(offset != 0);
	WARN_ON_ONCE(len != folio_size(folio));

	if (offset == 0 && len == folio_size(folio))
		filemap_release_folio(folio, 0);
}

static const struct address_space_operations gmem_aops = {
	.dirty_folio = noop_dirty_folio,
	.migrate_folio = gmem_migrate_folio,
	.error_remove_folio = gmem_error_folio,
	.release_folio = gmem_release_folio,
	.invalidate_folio = gmem_invalidate_folio,
};

static inline bool guest_memfd_check_ops(const struct guest_memfd_operations *ops)
{
	return ops->invalidate_begin && ops->release;
}

static inline unsigned long guest_memfd_valid_flags(void)
{
	unsigned long flags = GUEST_MEMFD_FLAG_CLEAR_INACCESSIBLE;

#ifdef CONFIG_ARCH_HAS_SET_DIRECT_MAP
	if (can_set_direct_map())
		flags |= GUEST_MEMFD_FLAG_REMOVE_DIRECT_MAP;
#endif

	return flags;
}

/**
 * guest_memfd_alloc() - Create a guest_memfd file
 * @name: the name of the new file
 * @ops: operations table for the guest_memfd file
 * @size: the size of the file
 * @flags: flags controlling behavior of the file
 *
 * Creates a new guest_memfd file.
 */
struct file *guest_memfd_alloc(const char *name,
			       const struct guest_memfd_operations *ops,
			       loff_t size, unsigned long flags)
{
	struct inode *inode;
	struct file *file;

	if (size <= 0 || !PAGE_ALIGNED(size))
		return ERR_PTR(-EINVAL);

	if (!guest_memfd_check_ops(ops))
		return ERR_PTR(-EINVAL);

	if (flags & ~guest_memfd_valid_flags())
		return ERR_PTR(-EINVAL);

	/*
	 * Use the so called "secure" variant, which creates a unique inode
	 * instead of reusing a single inode.  Each guest_memfd instance needs
	 * its own inode to track the size, flags, etc.
	 */
	file = anon_inode_create_getfile(name, &gmem_fops, NULL, O_RDWR, NULL);
	if (IS_ERR(file))
		return file;

	file->f_flags |= O_LARGEFILE;

	inode = file_inode(file);
	WARN_ON(file->f_mapping != inode->i_mapping);

	inode->i_private = (void *)ops; /* discards const qualifier */
	inode->i_mapping->a_ops = &gmem_aops;
	inode->i_mapping->i_private_data = (void *)flags;
	inode->i_mode |= S_IFREG;
	inode->i_size = size;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_inaccessible(inode->i_mapping);
	/* Unmovable mappings are supposed to be marked unevictable as well. */
	WARN_ON_ONCE(!mapping_unevictable(inode->i_mapping));

	return file;
}
EXPORT_SYMBOL_GPL(guest_memfd_alloc);

/**
 * is_guest_memfd() - Returns true if the struct file is a guest_memfd
 * @file: the file to check
 * @ops: the expected operations table
 */
bool is_guest_memfd(struct file *file, const struct guest_memfd_operations *ops)
{
	if (file->f_op != &gmem_fops)
		return false;

	struct inode *inode = file_inode(file);
	struct guest_memfd_operations *gops = inode->i_private;

	return ops == gops;
}
EXPORT_SYMBOL_GPL(is_guest_memfd);
