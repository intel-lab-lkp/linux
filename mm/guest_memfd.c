// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/anon_inodes.h>
#include <linux/falloc.h>
#include <linux/guest_memfd.h>
#include <linux/pagemap.h>
#include <linux/set_memory.h>

static inline int guest_memfd_folio_private(struct folio *folio)
{
	unsigned long nr_pages = folio_nr_pages(folio);
	unsigned long i;
	int r;

	for (i = 0; i < nr_pages; i++) {
		struct page *page = folio_page(folio, i);

		r = set_direct_map_invalid_noflush(page);
		if (r < 0)
			goto out_remap;
	}

	folio_set_private(folio);
	return 0;
out_remap:
	for (; i > 0; i--) {
		struct page *page = folio_page(folio, i - 1);

		BUG_ON(set_direct_map_default_noflush(page));
	}
	return r;
}

static inline void guest_memfd_folio_clear_private(struct folio *folio)
{
	unsigned long start = (unsigned long)folio_address(folio);
	unsigned long nr = folio_nr_pages(folio);
	unsigned long i;

	if (!folio_test_private(folio))
		return;

	for (i = 0; i < nr; i++) {
		struct page *page = folio_page(folio, i);

		BUG_ON(set_direct_map_default_noflush(page));
	}
	flush_tlb_kernel_range(start, start + folio_size(folio));

	folio_clear_private(folio);
}

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

	/*
	 * Use the up-to-date flag to track whether or not the memory has been
	 * zeroed before being handed off to the guest.  There is no backing
	 * storage for the memory, so the folio will remain up-to-date until
	 * it's removed.
	 */
	if ((flags & GUEST_MEMFD_GRAB_UPTODATE) &&
	    !folio_test_uptodate(folio)) {
		unsigned long nr_pages = folio_nr_pages(folio);
		unsigned long i;

		for (i = 0; i < nr_pages; i++)
			clear_highpage(folio_page(folio, i));

		folio_mark_uptodate(folio);
	}

	if (flags & GUEST_MEMFD_PREPARE && ops->prepare) {
		r = ops->prepare(inode, index, folio);
		if (r < 0)
			goto out_err;
	}

	if (gmem_flags & GUEST_MEMFD_FLAG_NO_DIRECT_MAP) {
		r = guest_memfd_folio_private(folio);
		if (r)
			goto out_err;
	}

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

		folio = guest_memfd_grab_folio(file, index,
					       GUEST_MEMFD_GRAB_UPTODATE |
						       GUEST_MEMFD_PREPARE);
		if (!folio) {
			r = -ENOMEM;
			break;
		}

		index = folio_next_index(folio);

		folio_unlock(folio);
		folio_put(folio);

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

static struct file_operations gmem_fops = {
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

	guest_memfd_folio_clear_private(folio);

	return true;
}

static void gmem_invalidate_folio(struct folio *folio, size_t offset, size_t len)
{
	/* not yet supported */
	BUG_ON(offset || len != folio_size(folio));

	BUG_ON(!gmem_release_folio(folio, 0));
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

	if (flags & ~GUEST_MEMFD_FLAG_NO_DIRECT_MAP)
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

bool is_guest_memfd(struct file *file, const struct guest_memfd_operations *ops)
{
	if (file->f_op != &gmem_fops)
		return false;

	struct inode *inode = file_inode(file);
	struct guest_memfd_operations *gops = inode->i_private;

	return ops == gops;
}
EXPORT_SYMBOL_GPL(is_guest_memfd);
