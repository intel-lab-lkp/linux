// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/anon_inodes.h>
#include <linux/falloc.h>
#include <linux/guest_memfd.h>
#include <linux/pagemap.h>

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
	unsigned long gmem_flags = (unsigned long)file->private_data;
	struct inode *inode = file_inode(file);
	struct guest_memfd_operations *ops = inode->i_private;
	struct folio *folio;
	int r;

	/* TODO: Support huge pages. */
	folio = filemap_grab_folio(inode->i_mapping, index);
	if (IS_ERR(folio))
		return folio;

	if (folio_test_uptodate(folio))
		return folio;

	folio_wait_stable(folio);

	/*
	 * Use the up-to-date flag to track whether or not the memory has been
	 * zeroed before being handed off to the guest.  There is no backing
	 * storage for the memory, so the folio will remain up-to-date until
	 * it's removed.
	 */
	if (gmem_flags & GUEST_MEMFD_FLAG_CLEAR_INACCESSIBLE) {
		unsigned long nr_pages = folio_nr_pages(folio);
		unsigned long i;

		for (i = 0; i < nr_pages; i++)
			clear_highpage(folio_page(folio, i));

	}

	if (ops->prepare_inaccessible) {
		r = ops->prepare_inaccessible(inode, folio);
		if (r < 0)
			goto out_err;
	}

	folio_mark_uptodate(folio);
	/*
	 * Ignore accessed, referenced, and dirty flags.  The memory is
	 * unevictable and there is no storage to write back to.
	 */
	return folio;
out_err:
	folio_unlock(folio);
	folio_put(folio);
	return ERR_PTR(r);
}
EXPORT_SYMBOL_GPL(guest_memfd_grab_folio);

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
	int ret;

	filemap_invalidate_lock_shared(mapping);

	ret = ops->invalidate_begin(inode, offset, nr);
	if (!ret && ops->invalidate_end)
		ops->invalidate_end(inode, offset, nr);

	filemap_invalidate_unlock_shared(mapping);

	return ret;
}

static bool gmem_release_folio(struct folio *folio, gfp_t gfp)
{
	struct inode *inode = folio_inode(folio);
	struct guest_memfd_operations *ops = inode->i_private;
	off_t offset = folio->index;
	size_t nr = folio_nr_pages(folio);
	int ret;

	ret = ops->invalidate_begin(inode, offset, nr);
	if (ret)
		return false;
	if (ops->invalidate_end)
		ops->invalidate_end(inode, offset, nr);

	return true;
}

static const struct address_space_operations gmem_aops = {
	.dirty_folio = noop_dirty_folio,
	.migrate_folio = gmem_migrate_folio,
	.error_remove_folio = gmem_error_folio,
	.release_folio = gmem_release_folio,
};

static inline bool guest_memfd_check_ops(const struct guest_memfd_operations *ops)
{
	return ops->invalidate_begin && ops->release;
}

static inline unsigned long guest_memfd_valid_flags(void)
{
	return GUEST_MEMFD_FLAG_CLEAR_INACCESSIBLE;
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
	file = anon_inode_create_getfile(name, &gmem_fops, (void *)flags,
					 O_RDWR, NULL);
	if (IS_ERR(file))
		return file;

	file->f_flags |= O_LARGEFILE;

	inode = file_inode(file);
	WARN_ON(file->f_mapping != inode->i_mapping);

	inode->i_private = (void *)ops; /* discards const qualifier */
	inode->i_mapping->a_ops = &gmem_aops;
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
