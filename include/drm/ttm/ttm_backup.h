/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef _TTM_BACKUP_H_
#define _TTM_BACKUP_H_

#include <linux/mm_types.h>
#include <linux/shmem_fs.h>

struct ttm_backup;

/**
 * ttm_backup_handle_to_page_ptr() - Convert handle to struct page pointer
 * @handle: The handle to convert.
 *
 * Converts an opaque handle received from the
 * struct ttm_backoup_ops::backup_page() function to an (invalid)
 * struct page pointer suitable for a struct page array.
 *
 * Return: An (invalid) struct page pointer.
 */
static inline struct page *
ttm_backup_handle_to_page_ptr(unsigned long handle)
{
	return (struct page *)(handle << 1 | 1);
}

/**
 * ttm_backup_page_ptr_is_handle() - Whether a struct page pointer is a handle
 * @page: The struct page pointer to check.
 *
 * Return: true if the struct page pointer is a handld returned from
 * ttm_backup_handle_to_page_ptr(). False otherwise.
 */
static inline bool ttm_backup_page_ptr_is_handle(const struct page *page)
{
	return (unsigned long)page & 1;
}

/**
 * ttm_backup_page_ptr_to_handle() - Convert a struct page pointer to a handle
 * @page: The struct page pointer to convert
 *
 * Return: The handle that was previously used in
 * ttm_backup_handle_to_page_ptr() to obtain a struct page pointer, suitable
 * for use as argument in the struct ttm_backup_ops drop() or
 * copy_backed_up_page() functions.
 */
static inline unsigned long
ttm_backup_page_ptr_to_handle(const struct page *page)
{
	WARN_ON(!ttm_backup_page_ptr_is_handle(page));
	return (unsigned long)page >> 1;
}

/** struct ttm_backup_ops - A struct ttm_backup backend operations */
struct ttm_backup_ops {
	/**
	 * drop - release memory associated with a handle
	 * @backup: The struct backup pointer used to obtain the handle
	 * @handle: The handle obtained from the @backup_page function.
	 */
	void (*drop)(struct ttm_backup *backup, unsigned long handle);

	/**
	 * copy_backed_up_page - Copy the contents of a previously backed
	 * up page
	 * @backup: The struct backup pointer used to back up the page.
	 * @dst: The struct page to copy into.
	 * @handle: The handle returned when the page was backed up.
	 * @intr: Try to perform waits interruptable or at least killable.
	 *
	 * Return: 0 on success, Negative error code on failure, notably
	 * -EINTR if @intr was set to true and a signal is pending.
	 */
	int (*copy_backed_up_page)(struct ttm_backup *backup, struct page *dst,
				   unsigned long handle, bool intr);

	/**
	 * backup_page - Backup a page
	 * @backup: The struct backup pointer to use.
	 * @page: The page to back up.
	 * @writeback: Whether to perform immediate writeback of the page.
	 * This may have performance implications.
	 * @i: A unique integer for each page and each struct backup.
	 * This is a hint allowing the backup backend to avoid managing
	 * its address space separately.
	 * @page_gfp: The gfp value used when the page was allocated.
	 * This is used for accounting purposes.
	 * @alloc_gfp: The gpf to be used when the backend needs to allocaete
	 * memory.
	 *
	 * Return: A handle on success. 0 on failure.
	 * (This is following the swp_entry_t convention).
	 *
	 * Note: This function could be extended to back up a folio and
	 * backends would then split the folio internally if needed.
	 * Drawback is that the caller would then have to keep track of
	 */
	unsigned long (*backup_page)(struct ttm_backup *backup, struct page *page,
				     bool writeback, pgoff_t i, gfp_t page_gfp,
				     gfp_t alloc_gfp);
	/**
	 * fini - Free the struct backup resources after last use.
	 * @backup: Pointer to the struct backup whose resources to free.
	 *
	 * After a call to @fini, it's illegal to use the @backup pointer.
	 */
	void (*fini)(struct ttm_backup *backup);
};

/**
 * struct ttm_backup - Abstract a backup backend.
 * @ops: The operations as described above.
 *
 * The struct ttm_backup is intended to be subclassed by the
 * backend implementation.
 */
struct ttm_backup {
	const struct ttm_backup_ops *ops;
};

/**
 * ttm_backup_shmem_create() - Create a shmem-based struct backup.
 * @size: The maximum size (in bytes) to back up.
 *
 * Create a backup utilizing shmem objects.
 *
 * Return: A pointer to a struct ttm_backup on success,
 * an error pointer on error.
 */
struct ttm_backup *ttm_backup_shmem_create(loff_t size);

#endif
