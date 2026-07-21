// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <drm/ttm/ttm_backup.h>

#include <linux/export.h>
#include <linux/swap.h>

#include "ttm_pool_internal.h"

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
 * ttm_backup_backup_folio() - Backup a folio
 * @backup: The struct backup pointer to use.
 * @folio: The folio to back up.
 * @order: The allocation order of @folio.  Since TTM allocates higher-order
 *         pages without __GFP_COMP, folio_nr_pages(@folio) would always
 *         return 1; the caller must pass the true order explicitly.
 * @writeback: Whether to perform immediate writeback of the folio's pages.
 * This may have performance implications.
 * @idx: A unique integer for the first page of the folio and each struct backup.
 * This allows the backup implementation to avoid managing
 * its address space separately.
 * @folio_gfp: The gfp value used when the folio was allocated.
 * Currently unused.
 * @alloc_gfp: The gfp to be used when allocating memory.
 * @nr_pages_backed: Output. On a successful return, set to the number of
 * pages actually backed up, which may be less than (1 << @order)
 * if an -ENOMEM was encountered mid-folio.
 *
 * Context: If called from reclaim context, the caller needs to
 * assert that the shrinker gfp has __GFP_FS set, to avoid
 * deadlocking on lock_page(). If @writeback is set to true and
 * called from reclaim context, the caller also needs to assert
 * that the shrinker gfp has __GFP_IO set, since without it,
 * we're not allowed to start backup IO.
 *
 * Return: A handle for the first backed-up page on success (handles for
 * subsequent pages follow sequentially). -ENOMEM if no pages could be backed
 * up. Any other negative error code if a non-ENOMEM failure occurred; in that
 * case any pages backed up so far are truncated before returning.
 */
s64
ttm_backup_backup_folio(struct file *backup, struct folio *folio,
			unsigned int order, bool writeback, pgoff_t idx,
			gfp_t folio_gfp, gfp_t alloc_gfp,
			pgoff_t *nr_pages_backed)
{
	unsigned int backup_order = order;
	int err;

	/*
	 * Fault injection: back up only the first half of the folio to
	 * simulate a mid-compound OOM. The caller sees *nr_pages_backed
	 * < (1 << order) on success and drives its reactive-split path
	 * exactly as it would on a real short return. order == 0 cannot
	 * be shrunk further, so injection is skipped in that case.
	 */
	if (IS_ENABLED(CONFIG_FAULT_INJECTION) && order &&
	    ttm_backup_fault_inject_folio())
		backup_order = order - 1;

	err = shmem_backup_folio(folio, backup, idx, alloc_gfp, backup_order,
				 nr_pages_backed, writeback);

	if (!err || (err == -ENOMEM && *nr_pages_backed))
		return ttm_backup_shmem_idx_to_handle(idx);

	return err;
}

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
