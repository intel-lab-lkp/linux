// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <drm/ttm/ttm_backup.h>
#include <linux/page-flags.h>

/**
 * struct ttm_backup_shmem - A shmem based ttm_backup subclass.
 * @backup: The base struct ttm_backup
 * @filp: The associated shmem object
 */
struct ttm_backup_shmem {
	struct ttm_backup backup;
	struct file *filp;
};

static struct ttm_backup_shmem *to_backup_shmem(struct ttm_backup *backup)
{
	return container_of(backup, struct ttm_backup_shmem, backup);
}

static void ttm_backup_shmem_drop(struct ttm_backup *backup, unsigned long handle)
{
	handle -= 1;
	shmem_truncate_range(file_inode(to_backup_shmem(backup)->filp), handle,
			     handle + 1);
}

static int ttm_backup_shmem_copy_page(struct ttm_backup *backup, struct page *dst,
				      unsigned long handle, bool killable)
{
	struct file *filp = to_backup_shmem(backup)->filp;
	struct address_space *mapping = filp->f_mapping;
	struct folio *from_folio;

	handle -= 1;
	from_folio = shmem_read_folio(mapping, handle);
	if (IS_ERR(from_folio))
		return PTR_ERR(from_folio);

	/* Note: Use drm_memcpy_from_wc? */
	copy_highpage(dst, folio_file_page(from_folio, handle));
	folio_put(from_folio);

	return 0;
}

static unsigned long
ttm_backup_shmem_backup_page(struct ttm_backup *backup, struct page *page,
			     bool writeback, pgoff_t i, gfp_t page_gfp,
			     gfp_t alloc_gfp)
{
	struct file *filp = to_backup_shmem(backup)->filp;
	struct address_space *mapping = filp->f_mapping;
	unsigned long handle = 0;
	struct folio *to_folio;
	int ret;

	to_folio = shmem_read_folio_gfp(mapping, i, alloc_gfp);
	if (IS_ERR(to_folio))
		return handle;

	folio_mark_accessed(to_folio);
	folio_lock(to_folio);
	folio_mark_dirty(to_folio);
	copy_highpage(folio_file_page(to_folio, i), page);
	handle = i + 1;

	if (writeback && !folio_mapped(to_folio) && folio_clear_dirty_for_io(to_folio)) {
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_NONE,
			.nr_to_write = SWAP_CLUSTER_MAX,
			.range_start = 0,
			.range_end = LLONG_MAX,
			.for_reclaim = 1,
		};
		folio_set_reclaim(to_folio);
		ret = mapping->a_ops->writepage(folio_page(to_folio, 0), &wbc);
		if (!folio_test_writeback(to_folio))
			folio_clear_reclaim(to_folio);
		/* If writepage succeeds, it unlocks the folio */
		if (ret)
			folio_unlock(to_folio);
	} else {
		folio_unlock(to_folio);
	}

	folio_put(to_folio);

	return handle;
}

static void ttm_backup_shmem_fini(struct ttm_backup *backup)
{
	struct ttm_backup_shmem *sbackup = to_backup_shmem(backup);

	fput(sbackup->filp);
	kfree(sbackup);
}

static const struct ttm_backup_ops ttm_backup_shmem_ops = {
	.drop = ttm_backup_shmem_drop,
	.copy_backed_up_page = ttm_backup_shmem_copy_page,
	.backup_page = ttm_backup_shmem_backup_page,
	.fini = ttm_backup_shmem_fini,
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
struct ttm_backup *ttm_backup_shmem_create(loff_t size)
{
	struct ttm_backup_shmem *sbackup =
		kzalloc(sizeof(*sbackup), GFP_KERNEL | __GFP_ACCOUNT);

	if (!sbackup)
		return ERR_PTR(-ENOMEM);

	sbackup->filp = shmem_file_setup("ttm shmem backup", size, 0);
	if (IS_ERR(sbackup->filp)) {
		kfree(sbackup);
		return ERR_CAST(sbackup->filp);
	}

	sbackup->backup.ops = &ttm_backup_shmem_ops;

	return &sbackup->backup;
}
EXPORT_SYMBOL_GPL(ttm_backup_shmem_create);
