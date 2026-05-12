// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <drm/ttm/ttm_backup.h>

#include <linux/export.h>
#include <linux/swap.h>

/*
 * Need to map shmem indices to handle since a handle value
 * of 0 means error, following the swp_entry_t convention.
 */
static unsigned long ttm_backup_shmem_idx_to_handle(pgoff_t idx)
{
	return (unsigned long)idx + 1;
}

static pgoff_t ttm_backup_handle_to_shmem_idx(pgoff_t handle)
{
	return handle - 1;
}

/**
 * ttm_backup_drop() - release memory associated with a handle
 * @backup: The struct backup pointer used to obtain the handle
 * @handle: The handle obtained from the @backup_page function.
 */
void ttm_backup_drop(struct file *backup, pgoff_t handle)
{
	loff_t start = ttm_backup_handle_to_shmem_idx(handle);

	start <<= PAGE_SHIFT;
	shmem_truncate_range(file_inode(backup), start,
			     start + PAGE_SIZE - 1);
}

/**
 * ttm_backup_copy_page() - Copy the contents of a previously backed
 * up page
 * @backup: The struct backup pointer used to back up the page.
 * @dst: The struct page to copy into.
 * @handle: The handle returned when the page was backed up.
 * @intr: Try to perform waits interruptible or at least killable.
 * @additional_gfp: GFP mask to add to the default GFP mask if any.
 *
 * Return: 0 on success, Negative error code on failure, notably
 * -EINTR if @intr was set to true and a signal is pending.
 */
int ttm_backup_copy_page(struct file *backup, struct page *dst,
			 pgoff_t handle, bool intr, gfp_t additional_gfp)
{
	struct address_space *mapping = backup->f_mapping;
	struct folio *from_folio;
	pgoff_t idx = ttm_backup_handle_to_shmem_idx(handle);

	from_folio = shmem_read_folio_gfp(mapping, idx, mapping_gfp_mask(mapping)
					  | additional_gfp);
	if (IS_ERR(from_folio))
		return PTR_ERR(from_folio);

	copy_highpage(dst, folio_file_page(from_folio, idx));
	folio_put(from_folio);

	return 0;
}

/**
 * ttm_backup_insert_folio() - Zero-copy insert of an isolated folio into backup.
 * @backup: The struct backup pointer to use.
 * @folio: The folio to insert. Must be isolated (not on LRU), unlocked,
 *         have exactly one reference (the caller's), and have no page-table
 *         mappings.  The folio must not be swapbacked or in the swapcache,
 *         and folio->private must have been cleared by the caller.
 * @order: The allocation order of @folio.  If @order > 0 and @folio is not
 *         already a large folio, it is promoted to a compound folio of this
 *         order (see shmem_insert_folio()).  split_page() must NOT have been
 *         called; tail-page refcounts must be 0.
 * @writeback: Whether to attempt immediate writeback to swap after insertion.
 *             Best-effort; failure is silently ignored.
 * @idx: Page-cache index within @backup.  Must be aligned to (1 << @order).
 * @folio_gfp: The gfp value used when the folio was allocated.
 *             Used for memory-cgroup charging.
 *
 * Context: May be called from reclaim context.  If @writeback is true, the
 * caller must assert that the shrinker gfp has __GFP_IO set.
 *
 * The folio is transferred zero-copy into the shmem page cache.  On success
 * the caller should release their reference with folio_put() and track the
 * handle for later recovery via ttm_backup_copy_page() and release via
 * ttm_backup_drop().  Handles for sub-pages of a compound folio follow
 * sequentially: handle + j addresses sub-page j.
 *
 * Return: A positive handle on success. Negative error code on failure;
 *         the folio is returned to its original non-compound state and the
 *         caller retains ownership.
 */
s64
ttm_backup_insert_folio(struct file *backup, struct folio *folio,
			unsigned int order, bool writeback, pgoff_t idx,
			gfp_t folio_gfp)
{
	int ret;

	WARN_ON_ONCE(folio_get_private(folio));
	ret = shmem_insert_folio(backup, folio, order, idx, writeback, folio_gfp);
	if (ret)
		return ret;

	return ttm_backup_shmem_idx_to_handle(idx);
}
EXPORT_SYMBOL_GPL(ttm_backup_insert_folio);

/**
 * ttm_backup_fini() - Free the struct backup resources after last use.
 * @backup: Pointer to the struct backup whose resources to free.
 *
 * After a call to this function, it's illegal to use the @backup pointer.
 */
void ttm_backup_fini(struct file *backup)
{
	fput(backup);
}

/**
 * ttm_backup_bytes_avail() - Report the approximate number of bytes of backup space
 * left for backup.
 *
 * This function is intended also for driver use to indicate whether a
 * backup attempt is meaningful.
 *
 * Return: An approximate size of backup space available.
 */
u64 ttm_backup_bytes_avail(void)
{
	/*
	 * The idea behind backing up to shmem is that shmem objects may
	 * eventually be swapped out. So no point swapping out if there
	 * is no or low swap-space available. But the accuracy of this
	 * number also depends on shmem actually swapping out backed-up
	 * shmem objects without too much buffering.
	 */
	return (u64)get_nr_swap_pages() << PAGE_SHIFT;
}
EXPORT_SYMBOL_GPL(ttm_backup_bytes_avail);

/**
 * ttm_backup_shmem_create() - Create a shmem-based struct backup.
 * @size: The maximum size (in bytes) to back up.
 *
 * Create a backup utilizing shmem objects.
 *
 * Return: A pointer to a struct file on success,
 * an error pointer on error.
 */
struct file *ttm_backup_shmem_create(loff_t size)
{
	return shmem_file_setup("ttm shmem backup", size,
				EMPTY_VMA_FLAGS);
}
