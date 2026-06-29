// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Fusion-io  All rights reserved.
 * Copyright (C) 2012 Intel Corp. All rights reserved.
 */

#include <linux/sched.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/hash.h>
#include <linux/list_sort.h>
#include <linux/mm.h>
#include <linux/async_tx.h>
#include "messages.h"
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "raid56.h"
#include "async-thread.h"
#include "file-item.h"
#include "btrfs_inode.h"

/* set when additional merges to this rbio are not allowed */
#define RBIO_RMW_LOCKED_BIT	1

/*
 * set when this rbio is sitting in the hash, but it is just a cache
 * of past RMW
 */
#define RBIO_CACHE_BIT		2

/*
 * set when it is safe to trust the stripe_pages for caching
 */
#define RBIO_CACHE_READY_BIT	3

#define RBIO_CACHE_SIZE 1024

#define BTRFS_STRIPE_HASH_TABLE_BITS				11

static void dump_bioc(const struct btrfs_fs_info *fs_info, const struct btrfs_io_context *bioc)
{
	if (unlikely(!bioc)) {
		btrfs_crit(fs_info, "bioc=NULL");
		return;
	}
	btrfs_crit(fs_info,
"bioc logical=%llu full_stripe=%llu size=%llu map_type=0x%llx mirror=%u replace_nr_stripes=%u replace_stripe_src=%d num_stripes=%u",
		bioc->logical, bioc->full_stripe_logical, bioc->size,
		bioc->map_type, bioc->mirror_num, bioc->replace_nr_stripes,
		bioc->replace_stripe_src, bioc->num_stripes);
	for (int i = 0; i < bioc->num_stripes; i++) {
		btrfs_crit(fs_info, "    nr=%d devid=%llu physical=%llu",
			   i, bioc->stripes[i].dev->devid,
			   bioc->stripes[i].physical);
	}
}

static void btrfs_dump_rbio(const struct btrfs_fs_info *fs_info,
			    const struct btrfs_raid_bio *rbio)
{
	if (!IS_ENABLED(CONFIG_BTRFS_ASSERT))
		return;

	dump_bioc(fs_info, rbio->bioc);
	btrfs_crit(fs_info,
"rbio flags=0x%lx nr_sectors=%u nr_data=%u real_stripes=%u stripe_nsectors=%u sector_nsteps=%u scrubp=%u dbitmap=0x%lx",
		rbio->flags, rbio->nr_sectors, rbio->nr_data,
		rbio->real_stripes, rbio->stripe_nsectors,
		rbio->sector_nsteps, rbio->scrubp, rbio->dbitmap);
}

#define ASSERT_RBIO(expr, rbio)						\
({									\
	if (IS_ENABLED(CONFIG_BTRFS_ASSERT) && unlikely(!(expr))) {	\
		const struct btrfs_fs_info *__fs_info = (rbio)->bioc ?	\
					(rbio)->bioc->fs_info : NULL;	\
									\
		btrfs_dump_rbio(__fs_info, (rbio));			\
	}								\
	ASSERT((expr));							\
})

#define ASSERT_RBIO_STRIPE(expr, rbio, stripe_nr)			\
({									\
	if (IS_ENABLED(CONFIG_BTRFS_ASSERT) && unlikely(!(expr))) {	\
		const struct btrfs_fs_info *__fs_info = (rbio)->bioc ?	\
					(rbio)->bioc->fs_info : NULL;	\
									\
		btrfs_dump_rbio(__fs_info, (rbio));			\
		btrfs_crit(__fs_info, "stripe_nr=%d", (stripe_nr));	\
	}								\
	ASSERT((expr));							\
})

#define ASSERT_RBIO_SECTOR(expr, rbio, sector_nr)			\
({									\
	if (IS_ENABLED(CONFIG_BTRFS_ASSERT) && unlikely(!(expr))) {	\
		const struct btrfs_fs_info *__fs_info = (rbio)->bioc ?	\
					(rbio)->bioc->fs_info : NULL;	\
									\
		btrfs_dump_rbio(__fs_info, (rbio));			\
		btrfs_crit(__fs_info, "sector_nr=%d", (sector_nr));	\
	}								\
	ASSERT((expr));							\
})

#define ASSERT_RBIO_LOGICAL(expr, rbio, logical)			\
({									\
	if (IS_ENABLED(CONFIG_BTRFS_ASSERT) && unlikely(!(expr))) {	\
		const struct btrfs_fs_info *__fs_info = (rbio)->bioc ?	\
					(rbio)->bioc->fs_info : NULL;	\
									\
		btrfs_dump_rbio(__fs_info, (rbio));			\
		btrfs_crit(__fs_info, "logical=%llu", (logical));		\
	}								\
	ASSERT((expr));							\
})

/* Used by the raid56 code to lock stripes for read/modify/write */
struct btrfs_stripe_hash {
	struct list_head hash_list;
	spinlock_t lock;
};

/* Used by the raid56 code to lock stripes for read/modify/write */
struct btrfs_stripe_hash_table {
	struct list_head stripe_cache;
	spinlock_t cache_lock;
	int cache_size;
	struct btrfs_stripe_hash table[];
};

/*
 * The PFN may still be valid, but our paddrs should always be block size
 * aligned, thus such -1 paddr is definitely not a valid one.
 */
#define INVALID_PADDR	(~(phys_addr_t)0)

static void rmw_rbio_work(struct work_struct *work);
static void rmw_rbio_work_locked(struct work_struct *work);
static void index_rbio_pages(struct btrfs_raid_bio *rbio);
static int alloc_rbio_pages(struct btrfs_raid_bio *rbio);

static int finish_parity_scrub(struct btrfs_raid_bio *rbio);
static void scrub_rbio_work_locked(struct work_struct *work);

static void free_raid_bio_pointers(struct btrfs_raid_bio *rbio)
{
	bitmap_free(rbio->error_bitmap);
	bitmap_free(rbio->stripe_uptodate_bitmap);
	kfree(rbio->stripe_pages);
	kfree(rbio->bio_paddrs);
	kfree(rbio->stripe_paddrs);
}

static void free_raid_bio(struct btrfs_raid_bio *rbio)
{
	int i;

	if (!refcount_dec_and_test(&rbio->refs))
		return;

	WARN_ON(!list_empty(&rbio->stripe_cache));
	WARN_ON(!list_empty(&rbio->hash_list));
	WARN_ON(!bio_list_empty(&rbio->bio_list));

	for (i = 0; i < rbio->nr_pages; i++) {
		if (rbio->stripe_pages[i]) {
			__free_page(rbio->stripe_pages[i]);
			rbio->stripe_pages[i] = NULL;
		}
	}

	btrfs_put_bioc(rbio->bioc);
	free_raid_bio_pointers(rbio);
	kfree(rbio);
}

static void start_async_work(struct btrfs_raid_bio *rbio, work_func_t work_func)
{
	INIT_WORK(&rbio->work, work_func);
	queue_work(rbio->bioc->fs_info->rmw_workers, &rbio->work);
}

/*
 * the stripe hash table is used for locking, and to collect
 * bios in hopes of making a full stripe
 */
int btrfs_alloc_stripe_hash_table(struct btrfs_fs_info *info)
{
	struct btrfs_stripe_hash_table *table;
	struct btrfs_stripe_hash_table *x;
	struct btrfs_stripe_hash *cur;
	struct btrfs_stripe_hash *h;
	unsigned int num_entries = 1U << BTRFS_STRIPE_HASH_TABLE_BITS;

	if (info->stripe_hash_table)
		return 0;

	/*
	 * The table is large, starting with order 4 and can go as high as
	 * order 7 in case lock debugging is turned on.
	 *
	 * Try harder to allocate and fallback to vmalloc to lower the chance
	 * of a failing mount.
	 */
	table = kvzalloc_flex(*table, table, num_entries);
	if (!table)
		return -ENOMEM;

	spin_lock_init(&table->cache_lock);
	INIT_LIST_HEAD(&table->stripe_cache);

	h = table->table;

	for (unsigned int i = 0; i < num_entries; i++) {
		cur = h + i;
		INIT_LIST_HEAD(&cur->hash_list);
		spin_lock_init(&cur->lock);
	}

	x = cmpxchg(&info->stripe_hash_table, NULL, table);
	kvfree(x);
	return 0;
}

static void memcpy_from_bio_to_stripe(struct btrfs_raid_bio *rbio, unsigned int sector_nr)
{
	const u32 step = min(rbio->bioc->fs_info->sectorsize, PAGE_SIZE);
	struct dma_async_tx_descriptor *tx = NULL;

	ASSERT(sector_nr < rbio->nr_sectors);
	for (int i = 0; i < rbio->sector_nsteps; i++) {
		unsigned int index = sector_nr * rbio->sector_nsteps + i;
		phys_addr_t dst = rbio->stripe_paddrs[index];
		phys_addr_t src = rbio->bio_paddrs[index];
		struct async_submit_ctl submit;

		ASSERT(dst != INVALID_PADDR);
		ASSERT(src != INVALID_PADDR);

		init_async_submit(&submit, ASYNC_TX_FENCE, tx, NULL, NULL, NULL);
		tx = async_memcpy(phys_to_page(dst), phys_to_page(src),
				  offset_in_page(dst), offset_in_page(src),
				  step, &submit);
	}
	/*
	 * All steps are chained via ASYNC_TX_FENCE.  issue_pending and
	 * dma_wait_for_async_tx on the last descriptor walk the entire
	 * chain, so earlier descriptors are submitted and waited on too.
	 * If every step completed synchronously, tx is NULL and no wait
	 * is needed.
	 */
	if (tx) {
		async_tx_issue_pending(tx);
		dma_wait_for_async_tx(tx);
	}
}

/*
 * caching an rbio means to copy anything from the
 * bio_sectors array into the stripe_pages array.  We
 * use the page uptodate bit in the stripe cache array
 * to indicate if it has valid data
 *
 * once the caching is done, we set the cache ready
 * bit.
 */
static void cache_rbio_pages(struct btrfs_raid_bio *rbio)
{
	int i;
	int ret;

	ret = alloc_rbio_pages(rbio);
	if (ret)
		return;

	for (i = 0; i < rbio->nr_sectors; i++) {
		/* Some range not covered by bio (partial write), skip it */
		if (rbio->bio_paddrs[i * rbio->sector_nsteps] == INVALID_PADDR) {
			/*
			 * Even if the sector is not covered by bio, if it is
			 * a data sector it should still be uptodate as it is
			 * read from disk.
			 */
			if (i < rbio->nr_data * rbio->stripe_nsectors)
				ASSERT(test_bit(i, rbio->stripe_uptodate_bitmap));
			continue;
		}

		memcpy_from_bio_to_stripe(rbio, i);
		set_bit(i, rbio->stripe_uptodate_bitmap);
	}
	set_bit(RBIO_CACHE_READY_BIT, &rbio->flags);
}

/*
 * we hash on the first logical address of the stripe
 */
static int rbio_bucket(struct btrfs_raid_bio *rbio)
{
	u64 num = rbio->bioc->full_stripe_logical;

	/*
	 * we shift down quite a bit.  We're using byte
	 * addressing, and most of the lower bits are zeros.
	 * This tends to upset hash_64, and it consistently
	 * returns just one or two different values.
	 *
	 * shifting off the lower bits fixes things.
	 */
	return hash_64(num >> 16, BTRFS_STRIPE_HASH_TABLE_BITS);
}

/* Get the sector number of the first sector covered by @page_nr. */
static u32 page_nr_to_sector_nr(struct btrfs_raid_bio *rbio, unsigned int page_nr)
{
	u32 sector_nr;

	ASSERT(page_nr < rbio->nr_pages);

	sector_nr = (page_nr << PAGE_SHIFT) >> rbio->bioc->fs_info->sectorsize_bits;
	ASSERT(sector_nr < rbio->nr_sectors);
	return sector_nr;
}

/*
 * Get the number of sectors covered by @page_nr.
 *
 * For bs > ps cases, the result will always be 1.
 * For bs <= ps cases, the result will be ps / bs.
 */
static u32 page_nr_to_num_sectors(struct btrfs_raid_bio *rbio, unsigned int page_nr)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	u32 nr_sectors;

	ASSERT(page_nr < rbio->nr_pages);

	nr_sectors = round_up(PAGE_SIZE, fs_info->sectorsize) >> fs_info->sectorsize_bits;
	ASSERT(nr_sectors > 0);
	return nr_sectors;
}

static __maybe_unused bool full_page_sectors_uptodate(struct btrfs_raid_bio *rbio,
						      unsigned int page_nr)
{
	const u32 sector_nr = page_nr_to_sector_nr(rbio, page_nr);
	const u32 nr_bits = page_nr_to_num_sectors(rbio, page_nr);
	int i;

	ASSERT(page_nr < rbio->nr_pages);
	ASSERT(sector_nr + nr_bits < rbio->nr_sectors);

	for (i = sector_nr; i < sector_nr + nr_bits; i++) {
		if (!test_bit(i, rbio->stripe_uptodate_bitmap))
			return false;
	}
	return true;
}

/*
 * Update the stripe_sectors[] array to use correct page and pgoff
 *
 * Should be called every time any page pointer in stripes_pages[] got modified.
 */
static void index_stripe_sectors(struct btrfs_raid_bio *rbio)
{
	const u32 step = min(rbio->bioc->fs_info->sectorsize, PAGE_SIZE);
	u32 offset;
	int i;

	for (i = 0, offset = 0; i < rbio->nr_sectors * rbio->sector_nsteps;
	     i++, offset += step) {
		int page_index = offset >> PAGE_SHIFT;

		ASSERT(page_index < rbio->nr_pages);
		if (!rbio->stripe_pages[page_index])
			continue;

		rbio->stripe_paddrs[i] = page_to_phys(rbio->stripe_pages[page_index]) +
					 offset_in_page(offset);
	}
}

static void steal_rbio_page(struct btrfs_raid_bio *src,
			    struct btrfs_raid_bio *dest, int page_nr)
{
	const u32 sector_nr = page_nr_to_sector_nr(src, page_nr);
	const u32 nr_bits = page_nr_to_num_sectors(src, page_nr);

	ASSERT(page_nr < src->nr_pages);
	ASSERT(sector_nr + nr_bits < src->nr_sectors);

	if (dest->stripe_pages[page_nr])
		__free_page(dest->stripe_pages[page_nr]);
	dest->stripe_pages[page_nr] = src->stripe_pages[page_nr];
	src->stripe_pages[page_nr] = NULL;

	/* Also update the stripe_uptodate_bitmap bits. */
	bitmap_set(dest->stripe_uptodate_bitmap, sector_nr, nr_bits);
}

static bool is_data_stripe_page(struct btrfs_raid_bio *rbio, int page_nr)
{
	const int sector_nr = page_nr_to_sector_nr(rbio, page_nr);

	/*
	 * We have ensured PAGE_SIZE is aligned with sectorsize, thus
	 * we won't have a page which is half data half parity.
	 *
	 * Thus if the first sector of the page belongs to data stripes, then
	 * the full page belongs to data stripes.
	 */
	return (sector_nr < rbio->nr_data * rbio->stripe_nsectors);
}

/*
 * Stealing an rbio means taking all the uptodate pages from the stripe array
 * in the source rbio and putting them into the destination rbio.
 *
 * This will also update the involved stripe_sectors[] which are referring to
 * the old pages.
 */
static void steal_rbio(struct btrfs_raid_bio *src, struct btrfs_raid_bio *dest)
{
	int i;

	if (!test_bit(RBIO_CACHE_READY_BIT, &src->flags))
		return;

	for (i = 0; i < dest->nr_pages; i++) {
		struct page *p = src->stripe_pages[i];

		/*
		 * We don't need to steal P/Q pages as they will always be
		 * regenerated for RMW or full write anyway.
		 */
		if (!is_data_stripe_page(src, i))
			continue;

		/*
		 * If @src already has RBIO_CACHE_READY_BIT, it should have
		 * all data stripe pages present and uptodate.
		 */
		ASSERT(p);
		ASSERT(full_page_sectors_uptodate(src, i));
		steal_rbio_page(src, dest, i);
	}
	index_stripe_sectors(dest);
	index_stripe_sectors(src);
}

/*
 * merging means we take the bio_list from the victim and
 * splice it into the destination.  The victim should
 * be discarded afterwards.
 *
 * must be called with dest->rbio_list_lock held
 */
static void merge_rbio(struct btrfs_raid_bio *dest,
		       struct btrfs_raid_bio *victim)
{
	bio_list_merge_init(&dest->bio_list, &victim->bio_list);
	dest->bio_list_bytes += victim->bio_list_bytes;
	/* Also inherit the bitmaps from @victim. */
	bitmap_or(&dest->dbitmap, &victim->dbitmap, &dest->dbitmap,
		  dest->stripe_nsectors);
}

/*
 * used to prune items that are in the cache.  The caller
 * must hold the hash table lock.
 */
static void __remove_rbio_from_cache(struct btrfs_raid_bio *rbio)
{
	int bucket = rbio_bucket(rbio);
	struct btrfs_stripe_hash_table *table;
	struct btrfs_stripe_hash *h;
	bool freeit = false;

	/*
	 * check the bit again under the hash table lock.
	 */
	if (!test_bit(RBIO_CACHE_BIT, &rbio->flags))
		return;

	table = rbio->bioc->fs_info->stripe_hash_table;
	h = table->table + bucket;

	/* hold the lock for the bucket because we may be
	 * removing it from the hash table
	 */
	spin_lock(&h->lock);

	/*
	 * hold the lock for the bio list because we need
	 * to make sure the bio list is empty
	 */
	spin_lock(&rbio->bio_list_lock);

	if (test_and_clear_bit(RBIO_CACHE_BIT, &rbio->flags)) {
		list_del_init(&rbio->stripe_cache);
		table->cache_size -= 1;
		freeit = true;

		/* if the bio list isn't empty, this rbio is
		 * still involved in an IO.  We take it out
		 * of the cache list, and drop the ref that
		 * was held for the list.
		 *
		 * If the bio_list was empty, we also remove
		 * the rbio from the hash_table, and drop
		 * the corresponding ref
		 */
		if (bio_list_empty(&rbio->bio_list)) {
			if (!list_empty(&rbio->hash_list)) {
				list_del_init(&rbio->hash_list);
				refcount_dec(&rbio->refs);
				BUG_ON(!list_empty(&rbio->plug_list));
			}
		}
	}

	spin_unlock(&rbio->bio_list_lock);
	spin_unlock(&h->lock);

	if (freeit)
		free_raid_bio(rbio);
}

/*
 * prune a given rbio from the cache
 */
static void remove_rbio_from_cache(struct btrfs_raid_bio *rbio)
{
	struct btrfs_stripe_hash_table *table;

	if (!test_bit(RBIO_CACHE_BIT, &rbio->flags))
		return;

	table = rbio->bioc->fs_info->stripe_hash_table;

	spin_lock(&table->cache_lock);
	__remove_rbio_from_cache(rbio);
	spin_unlock(&table->cache_lock);
}

/*
 * remove everything in the cache
 */
static void btrfs_clear_rbio_cache(struct btrfs_fs_info *info)
{
	struct btrfs_stripe_hash_table *table;
	struct btrfs_raid_bio *rbio;

	table = info->stripe_hash_table;

	spin_lock(&table->cache_lock);
	while (!list_empty(&table->stripe_cache)) {
		rbio = list_first_entry(&table->stripe_cache,
					struct btrfs_raid_bio, stripe_cache);
		__remove_rbio_from_cache(rbio);
	}
	spin_unlock(&table->cache_lock);
}

/*
 * remove all cached entries and free the hash table
 * used by unmount
 */
void btrfs_free_stripe_hash_table(struct btrfs_fs_info *info)
{
	if (!info->stripe_hash_table)
		return;
	btrfs_clear_rbio_cache(info);
	kvfree(info->stripe_hash_table);
	info->stripe_hash_table = NULL;
}

/*
 * insert an rbio into the stripe cache.  It
 * must have already been prepared by calling
 * cache_rbio_pages
 *
 * If this rbio was already cached, it gets
 * moved to the front of the lru.
 *
 * If the size of the rbio cache is too big, we
 * prune an item.
 */
static void cache_rbio(struct btrfs_raid_bio *rbio)
{
	struct btrfs_stripe_hash_table *table;

	if (!test_bit(RBIO_CACHE_READY_BIT, &rbio->flags))
		return;

	table = rbio->bioc->fs_info->stripe_hash_table;

	spin_lock(&table->cache_lock);
	spin_lock(&rbio->bio_list_lock);

	/* bump our ref if we were not in the list before */
	if (!test_and_set_bit(RBIO_CACHE_BIT, &rbio->flags))
		refcount_inc(&rbio->refs);

	if (!list_empty(&rbio->stripe_cache)){
		list_move(&rbio->stripe_cache, &table->stripe_cache);
	} else {
		list_add(&rbio->stripe_cache, &table->stripe_cache);
		table->cache_size += 1;
	}

	spin_unlock(&rbio->bio_list_lock);

	if (table->cache_size > RBIO_CACHE_SIZE) {
		struct btrfs_raid_bio *found;

		found = list_last_entry(&table->stripe_cache,
					struct btrfs_raid_bio,
					stripe_cache);

		if (found != rbio)
			__remove_rbio_from_cache(found);
	}

	spin_unlock(&table->cache_lock);
}

/*
 * Returns true if the bio list inside this rbio covers an entire stripe (no
 * rmw required).
 */
static int rbio_is_full(struct btrfs_raid_bio *rbio)
{
	unsigned long size = rbio->bio_list_bytes;
	int ret = 1;

	spin_lock(&rbio->bio_list_lock);
	if (size != rbio->nr_data * BTRFS_STRIPE_LEN)
		ret = 0;
	BUG_ON(size > rbio->nr_data * BTRFS_STRIPE_LEN);
	spin_unlock(&rbio->bio_list_lock);

	return ret;
}

/*
 * returns 1 if it is safe to merge two rbios together.
 * The merging is safe if the two rbios correspond to
 * the same stripe and if they are both going in the same
 * direction (read vs write), and if neither one is
 * locked for final IO
 *
 * The caller is responsible for locking such that
 * rmw_locked is safe to test
 */
static int rbio_can_merge(struct btrfs_raid_bio *last,
			  struct btrfs_raid_bio *cur)
{
	if (test_bit(RBIO_RMW_LOCKED_BIT, &last->flags) ||
	    test_bit(RBIO_RMW_LOCKED_BIT, &cur->flags))
		return 0;

	/*
	 * we can't merge with cached rbios, since the
	 * idea is that when we merge the destination
	 * rbio is going to run our IO for us.  We can
	 * steal from cached rbios though, other functions
	 * handle that.
	 */
	if (test_bit(RBIO_CACHE_BIT, &last->flags) ||
	    test_bit(RBIO_CACHE_BIT, &cur->flags))
		return 0;

	if (last->bioc->full_stripe_logical != cur->bioc->full_stripe_logical)
		return 0;

	/* we can't merge with different operations */
	if (last->operation != cur->operation)
		return 0;
	/*
	 * We've need read the full stripe from the drive.
	 * check and repair the parity and write the new results.
	 *
	 * We're not allowed to add any new bios to the
	 * bio list here, anyone else that wants to
	 * change this stripe needs to do their own rmw.
	 */
	if (last->operation == BTRFS_RBIO_PARITY_SCRUB)
		return 0;

	if (last->operation == BTRFS_RBIO_READ_REBUILD)
		return 0;

	return 1;
}

/* Return the sector index for @stripe_nr and @sector_nr. */
static unsigned int rbio_sector_index(const struct btrfs_raid_bio *rbio,
				      unsigned int stripe_nr,
				      unsigned int sector_nr)
{
	unsigned int ret;

	ASSERT_RBIO_STRIPE(stripe_nr < rbio->real_stripes, rbio, stripe_nr);
	ASSERT_RBIO_SECTOR(sector_nr < rbio->stripe_nsectors, rbio, sector_nr);

	ret = stripe_nr * rbio->stripe_nsectors + sector_nr;
	ASSERT(ret < rbio->nr_sectors);
	return ret;
}

/* Return the paddr array index for @stripe_nr, @sector_nr and @step_nr. */
static unsigned int rbio_paddr_index(const struct btrfs_raid_bio *rbio,
				     unsigned int stripe_nr,
				     unsigned int sector_nr,
				     unsigned int step_nr)
{
	unsigned int ret;

	ASSERT_RBIO_SECTOR(step_nr < rbio->sector_nsteps, rbio, step_nr);

	ret = rbio_sector_index(rbio, stripe_nr, sector_nr) * rbio->sector_nsteps + step_nr;
	ASSERT(ret < rbio->nr_sectors * rbio->sector_nsteps);
	return ret;
}

static phys_addr_t rbio_stripe_paddr(const struct btrfs_raid_bio *rbio,
					  unsigned int stripe_nr, unsigned int sector_nr,
					  unsigned int step_nr)
{
	return rbio->stripe_paddrs[rbio_paddr_index(rbio, stripe_nr, sector_nr, step_nr)];
}

static phys_addr_t rbio_pstripe_paddr(const struct btrfs_raid_bio *rbio,
					   unsigned int sector_nr, unsigned int step_nr)
{
	return rbio_stripe_paddr(rbio, rbio->nr_data, sector_nr, step_nr);
}

static phys_addr_t rbio_qstripe_paddr(const struct btrfs_raid_bio *rbio,
					   unsigned int sector_nr, unsigned int step_nr)
{
	if (rbio->nr_data + 1 == rbio->real_stripes)
		return INVALID_PADDR;
	return rbio_stripe_paddr(rbio, rbio->nr_data + 1, sector_nr, step_nr);
}

/* Return a paddr pointer into the rbio::stripe_paddrs[] for the specified sector. */
static phys_addr_t *rbio_stripe_paddrs(const struct btrfs_raid_bio *rbio,
				       unsigned int stripe_nr, unsigned int sector_nr)
{
	return &rbio->stripe_paddrs[rbio_paddr_index(rbio, stripe_nr, sector_nr, 0)];
}

/*
 * The first stripe in the table for a logical address
 * has the lock.  rbios are added in one of three ways:
 *
 * 1) Nobody has the stripe locked yet.  The rbio is given
 * the lock and 0 is returned.  The caller must start the IO
 * themselves.
 *
 * 2) Someone has the stripe locked, but we're able to merge
 * with the lock owner.  The rbio is freed and the IO will
 * start automatically along with the existing rbio.  1 is returned.
 *
 * 3) Someone has the stripe locked, but we're not able to merge.
 * The rbio is added to the lock owner's plug list, or merged into
 * an rbio already on the plug list.  When the lock owner unlocks,
 * the next rbio on the list is run and the IO is started automatically.
 * 1 is returned
 *
 * If we return 0, the caller still owns the rbio and must continue with
 * IO submission.  If we return 1, the caller must assume the rbio has
 * already been freed.
 */
static noinline int lock_stripe_add(struct btrfs_raid_bio *rbio)
{
	struct btrfs_stripe_hash *h;
	struct btrfs_raid_bio *cur;
	struct btrfs_raid_bio *pending;
	struct btrfs_raid_bio *freeit = NULL;
	struct btrfs_raid_bio *cache_drop = NULL;
	int ret = 0;

	h = rbio->bioc->fs_info->stripe_hash_table->table + rbio_bucket(rbio);

	spin_lock(&h->lock);
	list_for_each_entry(cur, &h->hash_list, hash_list) {
		if (cur->bioc->full_stripe_logical != rbio->bioc->full_stripe_logical)
			continue;

		spin_lock(&cur->bio_list_lock);

		/* Can we steal this cached rbio's pages? */
		if (bio_list_empty(&cur->bio_list) &&
		    list_empty(&cur->plug_list) &&
		    test_bit(RBIO_CACHE_BIT, &cur->flags) &&
		    !test_bit(RBIO_RMW_LOCKED_BIT, &cur->flags)) {
			list_del_init(&cur->hash_list);
			refcount_dec(&cur->refs);

			steal_rbio(cur, rbio);
			cache_drop = cur;
			spin_unlock(&cur->bio_list_lock);

			goto lockit;
		}

		/* Can we merge into the lock owner? */
		if (rbio_can_merge(cur, rbio)) {
			merge_rbio(cur, rbio);
			spin_unlock(&cur->bio_list_lock);
			freeit = rbio;
			ret = 1;
			goto out;
		}


		/*
		 * We couldn't merge with the running rbio, see if we can merge
		 * with the pending ones.  We don't have to check for rmw_locked
		 * because there is no way they are inside finish_rmw right now
		 */
		list_for_each_entry(pending, &cur->plug_list, plug_list) {
			if (rbio_can_merge(pending, rbio)) {
				merge_rbio(pending, rbio);
				spin_unlock(&cur->bio_list_lock);
				freeit = rbio;
				ret = 1;
				goto out;
			}
		}

		/*
		 * No merging, put us on the tail of the plug list, our rbio
		 * will be started with the currently running rbio unlocks
		 */
		list_add_tail(&rbio->plug_list, &cur->plug_list);
		spin_unlock(&cur->bio_list_lock);
		ret = 1;
		goto out;
	}
lockit:
	refcount_inc(&rbio->refs);
	list_add(&rbio->hash_list, &h->hash_list);
out:
	spin_unlock(&h->lock);
	if (cache_drop)
		remove_rbio_from_cache(cache_drop);
	if (freeit)
		free_raid_bio(freeit);
	return ret;
}

static void recover_rbio_work_locked(struct work_struct *work);

/*
 * called as rmw or parity rebuild is completed.  If the plug list has more
 * rbios waiting for this stripe, the next one on the list will be started
 */
static noinline void unlock_stripe(struct btrfs_raid_bio *rbio)
{
	int bucket;
	struct btrfs_stripe_hash *h;
	bool keep_cache = false;

	bucket = rbio_bucket(rbio);
	h = rbio->bioc->fs_info->stripe_hash_table->table + bucket;

	if (list_empty(&rbio->plug_list))
		cache_rbio(rbio);

	spin_lock(&h->lock);
	spin_lock(&rbio->bio_list_lock);

	if (!list_empty(&rbio->hash_list)) {
		/*
		 * if we're still cached and there is no other IO
		 * to perform, just leave this rbio here for others
		 * to steal from later
		 */
		if (list_empty(&rbio->plug_list) &&
		    test_bit(RBIO_CACHE_BIT, &rbio->flags)) {
			keep_cache = true;
			clear_bit(RBIO_RMW_LOCKED_BIT, &rbio->flags);
			BUG_ON(!bio_list_empty(&rbio->bio_list));
			goto done;
		}

		list_del_init(&rbio->hash_list);
		refcount_dec(&rbio->refs);

		/*
		 * we use the plug list to hold all the rbios
		 * waiting for the chance to lock this stripe.
		 * hand the lock over to one of them.
		 */
		if (!list_empty(&rbio->plug_list)) {
			struct btrfs_raid_bio *next;
			struct list_head *head = rbio->plug_list.next;

			next = list_entry(head, struct btrfs_raid_bio,
					  plug_list);

			list_del_init(&rbio->plug_list);

			list_add(&next->hash_list, &h->hash_list);
			refcount_inc(&next->refs);
			spin_unlock(&rbio->bio_list_lock);
			spin_unlock(&h->lock);

			if (next->operation == BTRFS_RBIO_READ_REBUILD) {
				start_async_work(next, recover_rbio_work_locked);
			} else if (next->operation == BTRFS_RBIO_WRITE) {
				steal_rbio(rbio, next);
				start_async_work(next, rmw_rbio_work_locked);
			} else if (next->operation == BTRFS_RBIO_PARITY_SCRUB) {
				steal_rbio(rbio, next);
				start_async_work(next, scrub_rbio_work_locked);
			}

			goto done_nolock;
		}
	}
done:
	spin_unlock(&rbio->bio_list_lock);
	spin_unlock(&h->lock);

done_nolock:
	if (!keep_cache)
		remove_rbio_from_cache(rbio);
}

static void rbio_endio_bio_list(struct bio *cur, blk_status_t status)
{
	struct bio *next;

	while (cur) {
		next = cur->bi_next;
		cur->bi_next = NULL;
		cur->bi_status = status;
		bio_endio(cur);
		cur = next;
	}
}

/*
 * this frees the rbio and runs through all the bios in the
 * bio_list and calls end_io on them
 */
static void rbio_orig_end_io(struct btrfs_raid_bio *rbio, blk_status_t status)
{
	struct bio *cur = bio_list_get(&rbio->bio_list);
	struct bio *extra;

	kfree(rbio->csum_buf);
	bitmap_free(rbio->csum_bitmap);
	rbio->csum_buf = NULL;
	rbio->csum_bitmap = NULL;

	/*
	 * Clear the data bitmap, as the rbio may be cached for later usage.
	 * do this before before unlock_stripe() so there will be no new bio
	 * for this bio.
	 */
	bitmap_clear(&rbio->dbitmap, 0, rbio->stripe_nsectors);

	/*
	 * At this moment, rbio->bio_list is empty, however since rbio does not
	 * always have RBIO_RMW_LOCKED_BIT set and rbio is still linked on the
	 * hash list, rbio may be merged with others so that rbio->bio_list
	 * becomes non-empty.
	 * Once unlock_stripe() is done, rbio->bio_list will not be updated any
	 * more and we can call bio_endio() on all queued bios.
	 */
	unlock_stripe(rbio);
	extra = bio_list_get(&rbio->bio_list);
	free_raid_bio(rbio);

	rbio_endio_bio_list(cur, status);
	if (extra)
		rbio_endio_bio_list(extra, status);
}

/*
 * Get paddr pointer for the sector specified by its @stripe_nr and @sector_nr.
 *
 * @rbio:               The raid bio
 * @stripe_nr:          Stripe number, valid range [0, real_stripe)
 * @sector_nr:		Sector number inside the stripe,
 *			valid range [0, stripe_nsectors)
 * @bio_list_only:      Whether to use sectors inside the bio list only.
 *
 * The read/modify/write code wants to reuse the original bio page as much
 * as possible, and only use stripe_sectors as fallback.
 *
 * Return NULL if bio_list_only is set but the specified sector has no
 * coresponding bio.
 */
static phys_addr_t *sector_paddrs_in_rbio(struct btrfs_raid_bio *rbio,
					  int stripe_nr, int sector_nr,
					  bool bio_list_only)
{
	phys_addr_t *ret = NULL;
	const int index = rbio_paddr_index(rbio, stripe_nr, sector_nr, 0);

	ASSERT(index >= 0 && index < rbio->nr_sectors * rbio->sector_nsteps);

	scoped_guard(spinlock, &rbio->bio_list_lock) {
		if (rbio->bio_paddrs[index] != INVALID_PADDR || bio_list_only) {
			/* Don't return sector without a valid page pointer */
			if (rbio->bio_paddrs[index] != INVALID_PADDR)
				ret = &rbio->bio_paddrs[index];
			return ret;
		}
	}
	return &rbio->stripe_paddrs[index];
}

/*
 * Similar to sector_paddr_in_rbio(), but with extra consideration for
 * bs > ps cases, where we can have multiple steps for a fs block.
 */
static phys_addr_t sector_paddr_in_rbio(struct btrfs_raid_bio *rbio,
					int stripe_nr, int sector_nr, int step_nr,
					bool bio_list_only)
{
	phys_addr_t ret = INVALID_PADDR;
	const int index = rbio_paddr_index(rbio, stripe_nr, sector_nr, step_nr);

	ASSERT(index >= 0 && index < rbio->nr_sectors * rbio->sector_nsteps);

	scoped_guard(spinlock, &rbio->bio_list_lock) {
		if (rbio->bio_paddrs[index] != INVALID_PADDR || bio_list_only) {
			/* Don't return sector without a valid page pointer */
			if (rbio->bio_paddrs[index] != INVALID_PADDR)
				ret = rbio->bio_paddrs[index];
			return ret;
		}
	}
	return rbio->stripe_paddrs[index];
}

/*
 * allocation and initial setup for the btrfs_raid_bio.  Not
 * this does not allocate any pages for rbio->pages.
 */
static struct btrfs_raid_bio *alloc_rbio(struct btrfs_fs_info *fs_info,
					 struct btrfs_io_context *bioc)
{
	const unsigned int real_stripes = bioc->num_stripes - bioc->replace_nr_stripes;
	const unsigned int stripe_npages = BTRFS_STRIPE_LEN >> PAGE_SHIFT;
	const unsigned int num_pages = stripe_npages * real_stripes;
	const unsigned int stripe_nsectors =
		BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits;
	const unsigned int num_sectors = stripe_nsectors * real_stripes;
	const unsigned int step = min(fs_info->sectorsize, PAGE_SIZE);
	const unsigned int sector_nsteps = fs_info->sectorsize / step;
	struct btrfs_raid_bio *rbio;

	/*
	 * For bs <= ps cases, ps must be aligned to bs.
	 * For bs > ps cases, bs must be aligned to ps.
	 */
	ASSERT(IS_ALIGNED(PAGE_SIZE, fs_info->sectorsize) ||
	       IS_ALIGNED(fs_info->sectorsize, PAGE_SIZE));
	/*
	 * Our current stripe len should be fixed to 64k thus stripe_nsectors
	 * (at most 16) should be no larger than BITS_PER_LONG.
	 */
	ASSERT(stripe_nsectors <= BITS_PER_LONG);

	/*
	 * Real stripes must be between 2 (2 disks RAID5, aka RAID1) and 256
	 * (limited by u8).
	 */
	ASSERT(real_stripes >= 2);
	ASSERT(real_stripes <= U8_MAX);

	rbio = kzalloc_obj(*rbio, GFP_NOFS);
	if (!rbio)
		return ERR_PTR(-ENOMEM);
	rbio->stripe_pages = kzalloc_objs(struct page *, num_pages, GFP_NOFS);
	rbio->bio_paddrs = kzalloc_objs(phys_addr_t,
					num_sectors * sector_nsteps, GFP_NOFS);
	rbio->stripe_paddrs = kzalloc_objs(phys_addr_t,
					   num_sectors * sector_nsteps,
					   GFP_NOFS);
	rbio->error_bitmap = bitmap_zalloc(num_sectors, GFP_NOFS);
	rbio->stripe_uptodate_bitmap = bitmap_zalloc(num_sectors, GFP_NOFS);

	if (!rbio->stripe_pages || !rbio->bio_paddrs || !rbio->stripe_paddrs ||
	    !rbio->error_bitmap || !rbio->stripe_uptodate_bitmap) {
		free_raid_bio_pointers(rbio);
		kfree(rbio);
		return ERR_PTR(-ENOMEM);
	}
	for (int i = 0; i < num_sectors * sector_nsteps; i++) {
		rbio->stripe_paddrs[i] = INVALID_PADDR;
		rbio->bio_paddrs[i] = INVALID_PADDR;
	}

	bio_list_init(&rbio->bio_list);
	init_waitqueue_head(&rbio->io_wait);
	INIT_LIST_HEAD(&rbio->plug_list);
	spin_lock_init(&rbio->bio_list_lock);
	INIT_LIST_HEAD(&rbio->stripe_cache);
	INIT_LIST_HEAD(&rbio->hash_list);
	btrfs_get_bioc(bioc);
	rbio->bioc = bioc;
	rbio->nr_pages = num_pages;
	rbio->nr_sectors = num_sectors;
	rbio->real_stripes = real_stripes;
	rbio->stripe_npages = stripe_npages;
	rbio->stripe_nsectors = stripe_nsectors;
	rbio->sector_nsteps = sector_nsteps;
	refcount_set(&rbio->refs, 1);
	atomic_set(&rbio->stripes_pending, 0);

	ASSERT(btrfs_nr_parity_stripes(bioc->map_type));
	rbio->nr_data = real_stripes - btrfs_nr_parity_stripes(bioc->map_type);
	ASSERT(rbio->nr_data > 0);

	return rbio;
}

/* allocate pages for all the stripes in the bio, including parity */
static int alloc_rbio_pages(struct btrfs_raid_bio *rbio)
{
	int ret;

	ret = btrfs_alloc_page_array(rbio->nr_pages, rbio->stripe_pages, GFP_NOFS);
	if (ret < 0)
		return ret;
	/* Mapping all sectors */
	index_stripe_sectors(rbio);
	return 0;
}

/* only allocate pages for p/q stripes */
static int alloc_rbio_parity_pages(struct btrfs_raid_bio *rbio)
{
	const int data_pages = rbio->nr_data * rbio->stripe_npages;
	int ret;

	ret = btrfs_alloc_page_array(rbio->nr_pages - data_pages,
				     rbio->stripe_pages + data_pages, GFP_NOFS);
	if (ret < 0)
		return ret;

	index_stripe_sectors(rbio);
	return 0;
}

/*
 * Return the total number of errors found in the vertical stripe of @sector_nr.
 *
 * @faila and @failb will also be updated to the first and second stripe
 * number of the errors.
 */
static int get_rbio_vertical_errors(struct btrfs_raid_bio *rbio, int sector_nr,
				    int *faila, int *failb)
{
	int stripe_nr;
	int found_errors = 0;

	if (faila || failb) {
		/*
		 * Both @faila and @failb should be valid pointers if any of
		 * them is specified.
		 */
		ASSERT(faila && failb);
		*faila = -1;
		*failb = -1;
	}

	for (stripe_nr = 0; stripe_nr < rbio->real_stripes; stripe_nr++) {
		int total_sector_nr = stripe_nr * rbio->stripe_nsectors + sector_nr;

		if (test_bit(total_sector_nr, rbio->error_bitmap)) {
			found_errors++;
			if (faila) {
				/* Update faila and failb. */
				if (*faila < 0)
					*faila = stripe_nr;
				else if (*failb < 0)
					*failb = stripe_nr;
			}
		}
	}
	return found_errors;
}

static int bio_add_paddrs(struct bio *bio, phys_addr_t *paddrs, unsigned int nr_steps,
			  unsigned int step)
{
	int added = 0;
	int ret;

	for (int i = 0; i < nr_steps; i++) {
		ret = bio_add_page(bio, phys_to_page(paddrs[i]), step,
				   offset_in_page(paddrs[i]));
		if (ret != step)
			goto revert;
		added += ret;
	}
	return added;
revert:
	/*
	 * We don't need to revert the bvec, as the bio will be submitted immediately,
	 * as long as the size is reduced the extra bvec will not be accessed.
	 */
	bio->bi_iter.bi_size -= added;
	return 0;
}

/*
 * Add a single sector @sector into our list of bios for IO.
 *
 * Return 0 if everything went well.
 * Return <0 for error, and no byte will be added to @rbio.
 */
static int rbio_add_io_paddrs(struct btrfs_raid_bio *rbio, struct bio_list *bio_list,
			      phys_addr_t *paddrs, unsigned int stripe_nr,
			      unsigned int sector_nr, enum req_op op)
{
	const u32 sectorsize = rbio->bioc->fs_info->sectorsize;
	const u32 step = min(sectorsize, PAGE_SIZE);
	struct bio *last = bio_list->tail;
	int ret;
	struct bio *bio;
	struct btrfs_io_stripe *stripe;
	u64 disk_start;

	/*
	 * Note: here stripe_nr has taken device replace into consideration,
	 * thus it can be larger than rbio->real_stripe.
	 * So here we check against bioc->num_stripes, not rbio->real_stripes.
	 */
	ASSERT_RBIO_STRIPE(stripe_nr >= 0 && stripe_nr < rbio->bioc->num_stripes,
			   rbio, stripe_nr);
	ASSERT_RBIO_SECTOR(sector_nr >= 0 && sector_nr < rbio->stripe_nsectors,
			   rbio, sector_nr);
	ASSERT(paddrs != NULL);

	stripe = &rbio->bioc->stripes[stripe_nr];
	disk_start = stripe->physical + sector_nr * sectorsize;

	/* if the device is missing, just fail this stripe */
	if (!stripe->dev->bdev) {
		int found_errors;

		set_bit(stripe_nr * rbio->stripe_nsectors + sector_nr,
			rbio->error_bitmap);

		/* Check if we have reached tolerance early. */
		found_errors = get_rbio_vertical_errors(rbio, sector_nr,
							NULL, NULL);
		if (unlikely(found_errors > rbio->bioc->max_errors))
			return -EIO;
		return 0;
	}

	/* see if we can add this page onto our existing bio */
	if (last) {
		u64 last_end = last->bi_iter.bi_sector << SECTOR_SHIFT;
		last_end += last->bi_iter.bi_size;

		/*
		 * we can't merge these if they are from different
		 * devices or if they are not contiguous
		 */
		if (last_end == disk_start && !last->bi_status &&
		    last->bi_bdev == stripe->dev->bdev) {
			ret = bio_add_paddrs(last, paddrs, rbio->sector_nsteps, step);
			if (ret == sectorsize)
				return 0;
		}
	}

	/* put a new bio on the list */
	bio = bio_alloc(stripe->dev->bdev,
			max(BTRFS_STRIPE_LEN >> PAGE_SHIFT, 1),
			op, GFP_NOFS);
	bio->bi_iter.bi_sector = disk_start >> SECTOR_SHIFT;
	bio->bi_private = rbio;

	ret = bio_add_paddrs(bio, paddrs, rbio->sector_nsteps, step);
	ASSERT(ret == sectorsize);
	bio_list_add(bio_list, bio);
	return 0;
}

static void index_one_bio(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u32 step = min(fs_info->sectorsize, PAGE_SIZE);
	const u32 step_bits = min(fs_info->sectorsize_bits, PAGE_SHIFT);
	struct bvec_iter iter = bio->bi_iter;
	phys_addr_t paddr;
	u32 offset = (bio->bi_iter.bi_sector << SECTOR_SHIFT) -
		     rbio->bioc->full_stripe_logical;

	btrfs_bio_for_each_block(paddr, bio, &iter, step) {
		unsigned int index = (offset >> step_bits);

		rbio->bio_paddrs[index] = paddr;
		offset += step;
	}
}

/*
 * helper function to walk our bio list and populate the bio_pages array with
 * the result.  This seems expensive, but it is faster than constantly
 * searching through the bio list as we setup the IO in finish_rmw or stripe
 * reconstruction.
 *
 * This must be called before you trust the answers from page_in_rbio
 */
static void index_rbio_pages(struct btrfs_raid_bio *rbio)
{
	struct bio *bio;

	spin_lock(&rbio->bio_list_lock);
	bio_list_for_each(bio, &rbio->bio_list)
		index_one_bio(rbio, bio);

	spin_unlock(&rbio->bio_list_lock);
}

static void bio_get_trace_info(struct btrfs_raid_bio *rbio, struct bio *bio,
			       struct raid56_bio_trace_info *trace_info)
{
	const struct btrfs_io_context *bioc = rbio->bioc;
	int i;

	ASSERT(bioc);

	/* We rely on bio->bi_bdev to find the stripe number. */
	if (!bio->bi_bdev)
		goto not_found;

	for (i = 0; i < bioc->num_stripes; i++) {
		if (bio->bi_bdev != bioc->stripes[i].dev->bdev)
			continue;
		trace_info->stripe_nr = i;
		trace_info->devid = bioc->stripes[i].dev->devid;
		trace_info->offset = (bio->bi_iter.bi_sector << SECTOR_SHIFT) -
				     bioc->stripes[i].physical;
		return;
	}

not_found:
	trace_info->devid = -1;
	trace_info->offset = -1;
	trace_info->stripe_nr = -1;
}

static inline void bio_list_put(struct bio_list *bio_list)
{
	struct bio *bio;

	while ((bio = bio_list_pop(bio_list)))
		bio_put(bio);
}

static void assert_rbio(struct btrfs_raid_bio *rbio)
{
	if (!IS_ENABLED(CONFIG_BTRFS_ASSERT))
		return;

	/*
	 * At least two stripes (2 disks RAID5), and since real_stripes is U8,
	 * we won't go beyond 256 disks anyway.
	 */
	ASSERT_RBIO(rbio->real_stripes >= 2, rbio);
	ASSERT_RBIO(rbio->nr_data > 0, rbio);

	/*
	 * This is another check to make sure nr data stripes is smaller
	 * than total stripes.
	 */
	ASSERT_RBIO(rbio->nr_data < rbio->real_stripes, rbio);
}

static inline void *kmap_local_paddr(phys_addr_t paddr)
{
	/* The sector pointer must have a page mapped to it. */
	ASSERT(paddr != INVALID_PADDR);

	return kmap_local_page(phys_to_page(paddr)) + offset_in_page(paddr);
}

/*
 * The page/offset array model used by the async_tx helpers below replaces the
 * old kmap-local pointer array.  Each entry in pages[] + offsets[] represents
 * what was previously a single kmap_local_paddr() return value.
 *
 * For example, the old pattern:
 *   pointers[i] = kmap_local_paddr(paddr);
 *   raid6_gen_syndrome(..., pointers);
 *   kunmap_local(pointers[i]);
 *
 * becomes:
 *   pages[i] = phys_to_page(paddr);
 *   offsets[i] = offset_in_page(paddr);
 *   async_gen_syndrome(pages, offsets, ...);
 *
 * The async_tx API handles any necessary kmap internally.
 */

/*
 * Maximum number of sectors per stripe.
 * BTRFS_STRIPE_LEN (64 KiB) divided by the minimum sector size (PAGE_SIZE,
 * typically 4 KiB) gives at most 16 entries.  If stripe length ever changes,
 * update this to BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits.
 */
#define RECOVER_MAX_STRIPE_SECTORS 16
static struct dma_async_tx_descriptor *
generate_pq_vertical_step(struct btrfs_raid_bio *rbio, unsigned int sector_nr,
			  unsigned int step_nr,
			  struct page **pages, unsigned int *offsets,
			  struct dma_async_tx_descriptor *depend_tx)
{
	const u32 step = min(rbio->bioc->fs_info->sectorsize, PAGE_SIZE);
	const bool has_qstripe = rbio->bioc->map_type & BTRFS_BLOCK_GROUP_RAID6;
	struct async_submit_ctl submit;
	struct dma_async_tx_descriptor *tx;
	int stripe;

	for (stripe = 0; stripe < rbio->nr_data; stripe++) {
		phys_addr_t paddr = sector_paddr_in_rbio(rbio, stripe, sector_nr, step_nr, 0);

		pages[stripe] = phys_to_page(paddr);
		offsets[stripe] = offset_in_page(paddr);
	}

	{
		phys_addr_t paddr = rbio_pstripe_paddr(rbio, sector_nr, step_nr);

		pages[stripe] = phys_to_page(paddr);
		offsets[stripe] = offset_in_page(paddr);
	}
	stripe++;

	if (has_qstripe) {
		phys_addr_t paddr = rbio_qstripe_paddr(rbio, sector_nr, step_nr);

		pages[stripe] = phys_to_page(paddr);
		offsets[stripe] = offset_in_page(paddr);

		assert_rbio(rbio);
		init_async_submit(&submit, ASYNC_TX_FENCE, depend_tx, NULL, NULL, NULL);
		tx = async_gen_syndrome(pages, offsets, rbio->real_stripes, step, &submit);
	} else {
		init_async_submit(&submit, ASYNC_TX_FENCE | ASYNC_TX_XOR_ZERO_DST,
				  depend_tx, NULL, NULL, NULL);
		tx = async_xor_offs(pages[rbio->nr_data], offsets[rbio->nr_data],
				    pages, offsets, rbio->nr_data, step, &submit);
	}
	return tx;
}

/*
 * Generate PQ for one vertical stripe, chaining into @depend_tx.
 * The caller must issue_pending + wait for the returned descriptor
 * (which may be NULL if the operation completed synchronously),
 * then call generate_pq_vertical_finish() to mark parity uptodate.
 */
static struct dma_async_tx_descriptor *
generate_pq_vertical(struct btrfs_raid_bio *rbio, int sectornr,
		     struct page **pages, unsigned int *offsets,
		     struct dma_async_tx_descriptor *depend_tx)
{
	struct dma_async_tx_descriptor *tx = depend_tx;

	for (int i = 0; i < rbio->sector_nsteps; i++)
		tx = generate_pq_vertical_step(rbio, sectornr, i, pages, offsets, tx);

	return tx;
}

static void generate_pq_vertical_finish(struct btrfs_raid_bio *rbio, int sectornr)
{
	const bool has_qstripe = (rbio->bioc->map_type & BTRFS_BLOCK_GROUP_RAID6);

	set_bit(rbio_sector_index(rbio, rbio->nr_data, sectornr),
		rbio->stripe_uptodate_bitmap);
	if (has_qstripe)
		set_bit(rbio_sector_index(rbio, rbio->nr_data + 1, sectornr),
			rbio->stripe_uptodate_bitmap);
}

static int rmw_assemble_write_bios(struct btrfs_raid_bio *rbio,
				   struct bio_list *bio_list)
{
	/* The total sector number inside the full stripe. */
	int total_sector_nr;
	int sectornr;
	int stripe;
	int ret;

	ASSERT(bio_list_size(bio_list) == 0);

	/* We should have at least one data sector. */
	ASSERT(bitmap_weight(&rbio->dbitmap, rbio->stripe_nsectors));

	/*
	 * Reset errors, as we may have errors inherited from from degraded
	 * write.
	 */
	bitmap_clear(rbio->error_bitmap, 0, rbio->nr_sectors);

	/*
	 * Start assembly.  Make bios for everything from the higher layers (the
	 * bio_list in our rbio) and our P/Q.  Ignore everything else.
	 */
	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		phys_addr_t *paddrs;

		stripe = total_sector_nr / rbio->stripe_nsectors;
		sectornr = total_sector_nr % rbio->stripe_nsectors;

		/* This vertical stripe has no data, skip it. */
		if (!test_bit(sectornr, &rbio->dbitmap))
			continue;

		if (stripe < rbio->nr_data) {
			paddrs = sector_paddrs_in_rbio(rbio, stripe, sectornr, 1);
			if (paddrs == NULL)
				continue;
		} else {
			paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
		}

		ret = rbio_add_io_paddrs(rbio, bio_list, paddrs, stripe,
					 sectornr, REQ_OP_WRITE);
		if (ret)
			goto error;
	}

	if (likely(!rbio->bioc->replace_nr_stripes))
		return 0;

	/*
	 * Make a copy for the replace target device.
	 *
	 * Thus the source stripe number (in replace_stripe_src) should be valid.
	 */
	ASSERT(rbio->bioc->replace_stripe_src >= 0);

	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		phys_addr_t *paddrs;

		stripe = total_sector_nr / rbio->stripe_nsectors;
		sectornr = total_sector_nr % rbio->stripe_nsectors;

		/*
		 * For RAID56, there is only one device that can be replaced,
		 * and replace_stripe_src[0] indicates the stripe number we
		 * need to copy from.
		 */
		if (stripe != rbio->bioc->replace_stripe_src) {
			/*
			 * We can skip the whole stripe completely, note
			 * total_sector_nr will be increased by one anyway.
			 */
			ASSERT(sectornr == 0);
			total_sector_nr += rbio->stripe_nsectors - 1;
			continue;
		}

		/* This vertical stripe has no data, skip it. */
		if (!test_bit(sectornr, &rbio->dbitmap))
			continue;

		if (stripe < rbio->nr_data) {
			paddrs = sector_paddrs_in_rbio(rbio, stripe, sectornr, 1);
			if (paddrs == NULL)
				continue;
		} else {
			paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
		}

		ret = rbio_add_io_paddrs(rbio, bio_list, paddrs,
					 rbio->real_stripes,
					 sectornr, REQ_OP_WRITE);
		if (ret)
			goto error;
	}

	return 0;
error:
	bio_list_put(bio_list);
	return -EIO;
}

static void set_rbio_range_error(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	u32 offset = (bio->bi_iter.bi_sector << SECTOR_SHIFT) -
		     rbio->bioc->full_stripe_logical;
	int total_nr_sector = offset >> fs_info->sectorsize_bits;

	ASSERT(total_nr_sector < rbio->nr_data * rbio->stripe_nsectors);

	bitmap_set(rbio->error_bitmap, total_nr_sector,
		   bio->bi_iter.bi_size >> fs_info->sectorsize_bits);

	/*
	 * Special handling for raid56_alloc_missing_rbio() used by
	 * scrub/replace.  Unlike call path in raid56_parity_recover(), they
	 * pass an empty bio here.  Thus we have to find out the missing device
	 * and mark the stripe error instead.
	 */
	if (bio->bi_iter.bi_size == 0) {
		bool found_missing = false;
		int stripe_nr;

		for (stripe_nr = 0; stripe_nr < rbio->real_stripes; stripe_nr++) {
			if (!rbio->bioc->stripes[stripe_nr].dev->bdev) {
				found_missing = true;
				bitmap_set(rbio->error_bitmap,
					   stripe_nr * rbio->stripe_nsectors,
					   rbio->stripe_nsectors);
			}
		}
		ASSERT(found_missing);
	}
}

/*
 * Return the index inside the rbio->stripe_sectors[] array.
 *
 * Return -1 if not found.
 */
static int find_stripe_sector_nr(struct btrfs_raid_bio *rbio, phys_addr_t paddr)
{
	for (int i = 0; i < rbio->nr_sectors; i++) {
		if (rbio->stripe_paddrs[i * rbio->sector_nsteps] == paddr)
			return i;
	}
	return -1;
}

/*
 * this sets each page in the bio uptodate.  It should only be used on private
 * rbio pages, nothing that comes in from the higher layers
 */
static void set_bio_pages_uptodate(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	const u32 sectorsize = rbio->bioc->fs_info->sectorsize;
	const u32 step = min(sectorsize, PAGE_SIZE);
	u32 offset = 0;
	phys_addr_t paddr;

	ASSERT(!bio_flagged(bio, BIO_CLONED));

	btrfs_bio_for_each_block_all(paddr, bio, step) {
		/* Hitting the first step of a sector. */
		if (IS_ALIGNED(offset, sectorsize)) {
			int sector_nr = find_stripe_sector_nr(rbio, paddr);

			ASSERT(sector_nr >= 0);
			if (sector_nr >= 0)
				set_bit(sector_nr, rbio->stripe_uptodate_bitmap);
		}
		offset += step;
	}
}

static int get_bio_sector_nr(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	phys_addr_t bvec_paddr = bvec_phys(bio_first_bvec_all(bio));
	int i;

	for (i = 0; i < rbio->nr_sectors; i++) {
		if (rbio->stripe_paddrs[i * rbio->sector_nsteps] == bvec_paddr)
			break;
		if (rbio->bio_paddrs[i * rbio->sector_nsteps] == bvec_paddr)
			break;
	}
	ASSERT(i < rbio->nr_sectors);
	return i;
}

static void rbio_update_error_bitmap(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	int total_sector_nr = get_bio_sector_nr(rbio, bio);
	const u32 bio_size = bio_get_size(bio);

	/*
	 * Since we can have multiple bios touching the error_bitmap, we cannot
	 * call bitmap_set() without protection.
	 *
	 * Instead use set_bit() for each bit, as set_bit() itself is atomic.
	 */
	for (int i = total_sector_nr; i < total_sector_nr +
	     (bio_size >> rbio->bioc->fs_info->sectorsize_bits); i++)
		set_bit(i, rbio->error_bitmap);
}

/* Verify the data sectors at read time. */
static void verify_bio_data_sectors(struct btrfs_raid_bio *rbio,
				    struct bio *bio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u32 step = min(fs_info->sectorsize, PAGE_SIZE);
	const u32 nr_steps = rbio->sector_nsteps;
	int total_sector_nr = get_bio_sector_nr(rbio, bio);
	u32 offset = 0;
	phys_addr_t paddrs[BTRFS_MAX_BLOCKSIZE / PAGE_SIZE];
	phys_addr_t paddr;

	/* No data csum for the whole stripe, no need to verify. */
	if (!rbio->csum_bitmap || !rbio->csum_buf)
		return;

	/* P/Q stripes, they have no data csum to verify against. */
	if (total_sector_nr >= rbio->nr_data * rbio->stripe_nsectors)
		return;

	btrfs_bio_for_each_block_all(paddr, bio, step) {
		u8 csum_buf[BTRFS_CSUM_SIZE];
		u8 *expected_csum;

		paddrs[(offset / step) % nr_steps] = paddr;
		offset += step;

		/* Not yet covering the full fs block, continue to the next step. */
		if (!IS_ALIGNED(offset, fs_info->sectorsize))
			continue;

		/* No csum for this sector, skip to the next sector. */
		if (!test_bit(total_sector_nr, rbio->csum_bitmap)) {
			total_sector_nr++;
			continue;
		}

		expected_csum = rbio->csum_buf + total_sector_nr * fs_info->csum_size;
		btrfs_calculate_block_csum_pages(fs_info, paddrs, csum_buf);
		if (unlikely(memcmp(csum_buf, expected_csum, fs_info->csum_size) != 0))
			set_bit(total_sector_nr, rbio->error_bitmap);
		total_sector_nr++;
	}
}

static void raid_wait_read_end_io(struct bio *bio)
{
	struct btrfs_raid_bio *rbio = bio->bi_private;

	if (bio->bi_status) {
		rbio_update_error_bitmap(rbio, bio);
	} else {
		set_bio_pages_uptodate(rbio, bio);
		verify_bio_data_sectors(rbio, bio);
	}

	bio_put(bio);
	if (atomic_dec_and_test(&rbio->stripes_pending))
		wake_up(&rbio->io_wait);
}

static void submit_read_wait_bio_list(struct btrfs_raid_bio *rbio,
			     struct bio_list *bio_list)
{
	struct bio *bio;

	atomic_set(&rbio->stripes_pending, bio_list_size(bio_list));
	while ((bio = bio_list_pop(bio_list))) {
		bio->bi_end_io = raid_wait_read_end_io;

		if (trace_raid56_read_enabled()) {
			struct raid56_bio_trace_info trace_info = { 0 };

			bio_get_trace_info(rbio, bio, &trace_info);
			trace_call__raid56_read(rbio, bio, &trace_info);
		}
		submit_bio(bio);
	}

	wait_event(rbio->io_wait, atomic_read(&rbio->stripes_pending) == 0);
}

static int alloc_rbio_data_pages(struct btrfs_raid_bio *rbio)
{
	const int data_pages = rbio->nr_data * rbio->stripe_npages;
	int ret;

	ret = btrfs_alloc_page_array(data_pages, rbio->stripe_pages, GFP_NOFS);
	if (ret < 0)
		return ret;

	index_stripe_sectors(rbio);
	return 0;
}

/*
 * We use plugging call backs to collect full stripes.
 * Any time we get a partial stripe write while plugged
 * we collect it into a list.  When the unplug comes down,
 * we sort the list by logical block number and merge
 * everything we can into the same rbios
 */
struct btrfs_plug_cb {
	struct blk_plug_cb cb;
	struct btrfs_fs_info *info;
	struct list_head rbio_list;
};

/*
 * rbios on the plug list are sorted for easier merging.
 */
static int plug_cmp(void *priv, const struct list_head *a,
		    const struct list_head *b)
{
	const struct btrfs_raid_bio *ra = container_of(a, struct btrfs_raid_bio,
						       plug_list);
	const struct btrfs_raid_bio *rb = container_of(b, struct btrfs_raid_bio,
						       plug_list);
	u64 a_sector = ra->bio_list.head->bi_iter.bi_sector;
	u64 b_sector = rb->bio_list.head->bi_iter.bi_sector;

	if (a_sector < b_sector)
		return -1;
	if (a_sector > b_sector)
		return 1;
	return 0;
}

static void raid_unplug(struct blk_plug_cb *cb, bool from_schedule)
{
	struct btrfs_plug_cb *plug = container_of(cb, struct btrfs_plug_cb, cb);
	struct btrfs_raid_bio *cur;
	struct btrfs_raid_bio *last = NULL;

	list_sort(NULL, &plug->rbio_list, plug_cmp);

	while (!list_empty(&plug->rbio_list)) {
		cur = list_first_entry(&plug->rbio_list,
				       struct btrfs_raid_bio, plug_list);
		list_del_init(&cur->plug_list);

		if (rbio_is_full(cur)) {
			/* We have a full stripe, queue it down. */
			start_async_work(cur, rmw_rbio_work);
			continue;
		}
		if (last) {
			if (rbio_can_merge(last, cur)) {
				merge_rbio(last, cur);
				free_raid_bio(cur);
				continue;
			}
			start_async_work(last, rmw_rbio_work);
		}
		last = cur;
	}
	if (last)
		start_async_work(last, rmw_rbio_work);
	kfree(plug);
}

/* Add the original bio into rbio->bio_list, and update rbio::dbitmap. */
static void rbio_add_bio(struct btrfs_raid_bio *rbio, struct bio *orig_bio)
{
	const struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u64 orig_logical = orig_bio->bi_iter.bi_sector << SECTOR_SHIFT;
	const u64 full_stripe_start = rbio->bioc->full_stripe_logical;
	const u32 orig_len = orig_bio->bi_iter.bi_size;
	const u32 sectorsize = fs_info->sectorsize;
	u64 cur_logical;

	ASSERT_RBIO_LOGICAL(orig_logical >= full_stripe_start &&
			    orig_logical + orig_len <= full_stripe_start +
			    rbio->nr_data * BTRFS_STRIPE_LEN,
			    rbio, orig_logical);

	bio_list_add(&rbio->bio_list, orig_bio);
	rbio->bio_list_bytes += orig_bio->bi_iter.bi_size;

	/* Update the dbitmap. */
	for (cur_logical = orig_logical; cur_logical < orig_logical + orig_len;
	     cur_logical += sectorsize) {
		int bit = ((u32)(cur_logical - full_stripe_start) >>
			   fs_info->sectorsize_bits) % rbio->stripe_nsectors;

		set_bit(bit, &rbio->dbitmap);
	}
}

/*
 * our main entry point for writes from the rest of the FS.
 */
void raid56_parity_write(struct bio *bio, struct btrfs_io_context *bioc)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	struct btrfs_raid_bio *rbio;
	struct btrfs_plug_cb *plug = NULL;
	struct blk_plug_cb *cb;

	rbio = alloc_rbio(fs_info, bioc);
	if (IS_ERR(rbio)) {
		bio->bi_status = errno_to_blk_status(PTR_ERR(rbio));
		bio_endio(bio);
		return;
	}
	rbio->operation = BTRFS_RBIO_WRITE;
	rbio_add_bio(rbio, bio);

	/*
	 * Don't plug on full rbios, just get them out the door
	 * as quickly as we can
	 */
	if (!rbio_is_full(rbio)) {
		cb = blk_check_plugged(raid_unplug, fs_info, sizeof(*plug));
		if (cb) {
			plug = container_of(cb, struct btrfs_plug_cb, cb);
			if (!plug->info) {
				plug->info = fs_info;
				INIT_LIST_HEAD(&plug->rbio_list);
			}
			list_add_tail(&rbio->plug_list, &plug->rbio_list);
			return;
		}
	}

	/*
	 * Either we don't have any existing plug, or we're doing a full stripe,
	 * queue the rmw work now.
	 */
	start_async_work(rbio, rmw_rbio_work);
}

static int verify_one_sector(struct btrfs_raid_bio *rbio,
			     int stripe_nr, int sector_nr)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	phys_addr_t *paddrs;
	u8 csum_buf[BTRFS_CSUM_SIZE];
	u8 *csum_expected;

	if (!rbio->csum_bitmap || !rbio->csum_buf)
		return 0;

	/* No way to verify P/Q as they are not covered by data csum. */
	if (stripe_nr >= rbio->nr_data)
		return 0;
	/*
	 * If we're rebuilding a read, we have to use pages from the
	 * bio list if possible.
	 */
	if (rbio->operation == BTRFS_RBIO_READ_REBUILD) {
		paddrs = sector_paddrs_in_rbio(rbio, stripe_nr, sector_nr, 0);
	} else {
		paddrs = rbio_stripe_paddrs(rbio, stripe_nr, sector_nr);
	}

	csum_expected = rbio->csum_buf +
			(stripe_nr * rbio->stripe_nsectors + sector_nr) *
			fs_info->csum_size;
	btrfs_calculate_block_csum_pages(fs_info, paddrs, csum_buf);
	if (unlikely(memcmp(csum_buf, csum_expected, fs_info->csum_size) != 0))
		return -EIO;
	return 0;
}

static struct dma_async_tx_descriptor *
recover_vertical_step(struct btrfs_raid_bio *rbio,
		      unsigned int sector_nr,
		      unsigned int step_nr,
		      int faila, int failb,
		      struct page **pages, unsigned int *offsets,
		      struct page **src_pages, unsigned int *src_offsets,
		      struct dma_async_tx_descriptor *depend_tx)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u32 step = min(fs_info->sectorsize, PAGE_SIZE);
	struct async_submit_ctl submit;
	struct dma_async_tx_descriptor *tx;

	ASSERT(step_nr < rbio->sector_nsteps);
	ASSERT(sector_nr < rbio->stripe_nsectors);

	for (int i = 0; i < rbio->real_stripes; i++) {
		phys_addr_t paddr;

		if (rbio->operation == BTRFS_RBIO_READ_REBUILD)
			paddr = sector_paddr_in_rbio(rbio, i, sector_nr, step_nr, 0);
		else
			paddr = rbio_stripe_paddr(rbio, i, sector_nr, step_nr);

		pages[i] = phys_to_page(paddr);
		offsets[i] = offset_in_page(paddr);
	}

	/* All raid6 handling here */
	if (rbio->bioc->map_type & BTRFS_BLOCK_GROUP_RAID6) {
		/* Single failure, rebuild from parity raid5 style */
		if (failb < 0) {
			if (faila == rbio->nr_data)
				/*
				 * Only P stripe has failed without a bad data or
				 * Q stripe.  No recovery needed for this step.
				 * Returning NULL breaks the async_tx dependency
				 * chain, but this path issues no DMA writes
				 * (it is a pure no-op), so no ordering between
				 * steps is required.
				 */
				return NULL;
			goto pstripe;
		}

		if (failb == rbio->real_stripes - 1) {
			if (faila == rbio->real_stripes - 2)
				/*
				 * Only P and Q are corrupted; data stripes are
				 * intact.  No recovery needed for this step.
				 * Safe to break the chain because this path
				 * issues no DMA writes (pure no-op).
				 */
				return NULL;
			goto pstripe;
		}

		if (failb == rbio->real_stripes - 2) {
			init_async_submit(&submit, ASYNC_TX_FENCE,
					  depend_tx, NULL, NULL, NULL);
			tx = async_raid6_datap_recov(rbio->real_stripes, step,
						     faila, pages, offsets, &submit);
		} else {
			init_async_submit(&submit, ASYNC_TX_FENCE,
					  depend_tx, NULL, NULL, NULL);
			tx = async_raid6_2data_recov(rbio->real_stripes, step,
						     faila, failb, pages, offsets,
						     &submit);
		}
		return tx;
	}

	/* Rebuild from P stripe here (raid5 or raid6 single failure). */
	ASSERT(failb == -1);
pstripe:
	{
		const unsigned int nr_data = rbio->nr_data;
		int src_cnt = 0;

		/*
		 * Build source list: P stripe + all non-failed data stripes.
		 * The result is written into pages[faila].
		 * The caller provides src_pages/src_offsets (sized for nr_data).
		 */
		src_pages[src_cnt] = pages[nr_data];
		src_offsets[src_cnt] = offsets[nr_data];
		src_cnt++;
		for (int i = 0; i < nr_data; i++) {
			if (i != faila) {
				src_pages[src_cnt] = pages[i];
				src_offsets[src_cnt] = offsets[i];
				src_cnt++;
			}
		}

		init_async_submit(&submit, ASYNC_TX_FENCE | ASYNC_TX_XOR_ZERO_DST,
				  depend_tx, NULL, NULL, NULL);
		tx = async_xor_offs(pages[faila], offsets[faila],
				    src_pages, src_offsets, src_cnt,
				    step, &submit);
		return tx;
	}
}

/*
 * Per-sector issue step for vertical stripe recovery.
 *
 * Determines which stripes need recovery, then submits the async_tx chain
 * (chained to @depend_tx for cross-sector batching).  Sets @faila and @failb
 * for the subsequent finish step.
 *
 * Return value:
 *   0  : success, @tx_ret describes pending work (or NULL for none)
 *  -EIO: too many errors, no work submitted
 */
static int recover_vertical_submit(struct btrfs_raid_bio *rbio, int sector_nr,
				   struct page **pages, unsigned int *offsets,
				   struct page **src_pages, unsigned int *src_offsets,
				   int *faila, int *failb,
				   struct dma_async_tx_descriptor **tx_ret,
				   struct dma_async_tx_descriptor *depend_tx)
{
	int found_errors;
	struct dma_async_tx_descriptor *tx = depend_tx;

	*tx_ret = NULL;
	*faila = -1;
	*failb = -1;

	/*
	 * Now we just use bitmap to mark the horizontal stripes in
	 * which we have data when doing parity scrub.
	 */
	if (rbio->operation == BTRFS_RBIO_PARITY_SCRUB &&
	    !test_bit(sector_nr, &rbio->dbitmap))
		return 0;

	found_errors = get_rbio_vertical_errors(rbio, sector_nr, faila, failb);
	/*
	 * No errors in the vertical stripe, skip it.  Can happen for recovery
	 * which only part of a stripe failed csum check.
	 */
	if (!found_errors)
		return 0;

	if (unlikely(found_errors > rbio->bioc->max_errors))
		return -EIO;

	for (int i = 0; i < rbio->sector_nsteps; i++) {
		tx = recover_vertical_step(rbio, sector_nr, i,
					   *faila, *failb,
					   pages, offsets,
					   src_pages, src_offsets,
					   tx);
	}
	*tx_ret = tx;
	return 0;
}

/*
 * Finish step for recover_vertical_submit().
 * Must be called after the DMA chain (if any) has completed.
 * Verifies csums for the recovered stripes and sets the uptodate bits.
 */
static int recover_vertical_finish(struct btrfs_raid_bio *rbio, int sector_nr,
				   int faila, int failb)
{
	int ret = 0;

	if (faila >= 0) {
		ret = verify_one_sector(rbio, faila, sector_nr);
		if (ret < 0)
			return ret;

		set_bit(rbio_sector_index(rbio, faila, sector_nr),
			rbio->stripe_uptodate_bitmap);
	}
	if (failb >= 0) {
		ret = verify_one_sector(rbio, failb, sector_nr);
		if (ret < 0)
			return ret;

		set_bit(rbio_sector_index(rbio, failb, sector_nr),
			rbio->stripe_uptodate_bitmap);
	}
	return ret;
}

/*
 * Sequential wrapper: submit, wait, finish for one sector.
 * Used by callers that do their own complex error filtering
 * (e.g. scrub recovery) and cannot batch across sectors.
 */
static int recover_vertical(struct btrfs_raid_bio *rbio, int sector_nr,
			    struct page **pages, unsigned int *offsets,
			    struct page **src_pages, unsigned int *src_offsets)
{
	struct dma_async_tx_descriptor *tx = NULL;
	int faila, failb;
	int ret;

	ret = recover_vertical_submit(rbio, sector_nr, pages, offsets,
				      src_pages, src_offsets,
				      &faila, &failb, &tx, NULL);
	if (ret < 0)
		return ret;

	if (tx) {
		async_tx_issue_pending(tx);
		dma_wait_for_async_tx(tx);
	}

	return recover_vertical_finish(rbio, sector_nr, faila, failb);
}

static int recover_sectors(struct btrfs_raid_bio *rbio)
{
	struct page **pages = NULL;
	unsigned int *offsets = NULL;
	struct page **src_pages = NULL;
	unsigned int *src_offsets = NULL;
	int faila_arr[RECOVER_MAX_STRIPE_SECTORS];
	int failb_arr[RECOVER_MAX_STRIPE_SECTORS];
	int sectornr;
	int ret = 0;

	ASSERT(rbio->stripe_nsectors <= RECOVER_MAX_STRIPE_SECTORS);

	pages = kzalloc_objs(struct page *, rbio->real_stripes, GFP_NOFS);
	offsets = kzalloc_objs(unsigned int, rbio->real_stripes, GFP_NOFS);
	if (!pages || !offsets) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * src_pages/src_offsets hold the pstripe source list:
	 *   P stripe (1) + non-failed data stripes (at most nr_data - 1)
	 * = at most nr_data entries.
	 */
	src_pages = kzalloc_objs(struct page *, rbio->nr_data, GFP_NOFS);
	src_offsets = kzalloc_objs(unsigned int, rbio->nr_data, GFP_NOFS);
	if (!src_pages || !src_offsets) {
		ret = -ENOMEM;
		goto out;
	}

	if (rbio->operation == BTRFS_RBIO_READ_REBUILD) {
		spin_lock(&rbio->bio_list_lock);
		set_bit(RBIO_RMW_LOCKED_BIT, &rbio->flags);
		spin_unlock(&rbio->bio_list_lock);
	}

	index_rbio_pages(rbio);

	{
		struct dma_async_tx_descriptor *tx = NULL;

		/*
		 * Phase 1: submit all sectors' recovery chains.
		 *
		 * pages[] / offsets[] are overwritten by each sector's submit
		 * call.  This is safe because the async_tx API consumes the
		 * arrays synchronously during each call: either the DMA prep
		 * function converts page+offset to DMA addresses and stores
		 * them in the hardware descriptor, or the synchronous fallback
		 * reads the data immediately.  No async_tx function retains a
		 * reference to the arrays after it returns.
		 */
		for (sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++) {
			int f_a = -1, f_b = -1;

			ret = recover_vertical_submit(rbio, sectornr, pages, offsets,
						      src_pages, src_offsets,
						      &f_a, &f_b, &tx, tx);
			if (ret < 0) {
				if (tx) {
					async_tx_issue_pending(tx);
					dma_wait_for_async_tx(tx);
				}
				goto out;
			}
			faila_arr[sectornr] = f_a;
			failb_arr[sectornr] = f_b;
		}

		/* Phase 2: wait for all DMAs to complete. */
		if (tx) {
			async_tx_issue_pending(tx);
			dma_wait_for_async_tx(tx);
		}

		/* Phase 3: verify recovered stripes and set uptodate bits. */
		for (sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++) {
			ret = recover_vertical_finish(rbio, sectornr,
						      faila_arr[sectornr],
						      failb_arr[sectornr]);
			if (ret < 0)
				goto out;
		}
	}

out:
	kfree(src_pages);
	kfree(src_offsets);
	kfree(pages);
	kfree(offsets);
	return ret;
}

static void recover_rbio(struct btrfs_raid_bio *rbio)
{
	struct bio_list bio_list = BIO_EMPTY_LIST;
	int total_sector_nr;
	int ret = 0;

	/*
	 * Either we're doing recover for a read failure or degraded write,
	 * caller should have set error bitmap correctly.
	 */
	ASSERT(bitmap_weight(rbio->error_bitmap, rbio->nr_sectors));

	/* For recovery, we need to read all sectors including P/Q. */
	ret = alloc_rbio_pages(rbio);
	if (ret < 0)
		goto out;

	index_rbio_pages(rbio);

	/*
	 * Read everything that hasn't failed. However this time we will
	 * not trust any cached sector.
	 * As we may read out some stale data but higher layer is not reading
	 * that stale part.
	 *
	 * So here we always re-read everything in recovery path.
	 */
	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		int stripe = total_sector_nr / rbio->stripe_nsectors;
		int sectornr = total_sector_nr % rbio->stripe_nsectors;
		phys_addr_t *paddrs;

		/*
		 * Skip the range which has error.  It can be a range which is
		 * marked error (for csum mismatch), or it can be a missing
		 * device.
		 */
		if (!rbio->bioc->stripes[stripe].dev->bdev ||
		    test_bit(total_sector_nr, rbio->error_bitmap)) {
			/*
			 * Also set the error bit for missing device, which
			 * may not yet have its error bit set.
			 */
			set_bit(total_sector_nr, rbio->error_bitmap);
			continue;
		}

		paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, stripe,
					 sectornr, REQ_OP_READ);
		if (ret < 0) {
			bio_list_put(&bio_list);
			goto out;
		}
	}

	submit_read_wait_bio_list(rbio, &bio_list);
	ret = recover_sectors(rbio);
out:
	rbio_orig_end_io(rbio, errno_to_blk_status(ret));
}

static void recover_rbio_work(struct work_struct *work)
{
	struct btrfs_raid_bio *rbio;

	rbio = container_of(work, struct btrfs_raid_bio, work);
	if (!lock_stripe_add(rbio))
		recover_rbio(rbio);
}

static void recover_rbio_work_locked(struct work_struct *work)
{
	recover_rbio(container_of(work, struct btrfs_raid_bio, work));
}

static void set_rbio_raid6_extra_error(struct btrfs_raid_bio *rbio, int mirror_num)
{
	bool found = false;
	int sector_nr;

	/*
	 * This is for RAID6 extra recovery tries, thus mirror number should
	 * be large than 2.
	 * Mirror 1 means read from data stripes. Mirror 2 means rebuild using
	 * RAID5 methods.
	 */
	ASSERT(mirror_num > 2);
	for (sector_nr = 0; sector_nr < rbio->stripe_nsectors; sector_nr++) {
		int found_errors;
		int faila;
		int failb;

		found_errors = get_rbio_vertical_errors(rbio, sector_nr,
							 &faila, &failb);
		/* This vertical stripe doesn't have errors. */
		if (!found_errors)
			continue;

		/*
		 * If we found errors, there should be only one error marked
		 * by previous set_rbio_range_error().
		 */
		ASSERT(found_errors == 1);
		found = true;

		/* Now select another stripe to mark as error. */
		failb = rbio->real_stripes - (mirror_num - 1);
		if (failb <= faila)
			failb--;

		/* Set the extra bit in error bitmap. */
		if (failb >= 0)
			set_bit(failb * rbio->stripe_nsectors + sector_nr,
				rbio->error_bitmap);
	}

	/* We should found at least one vertical stripe with error.*/
	ASSERT(found);
}

/*
 * the main entry point for reads from the higher layers.  This
 * is really only called when the normal read path had a failure,
 * so we assume the bio they send down corresponds to a failed part
 * of the drive.
 */
void raid56_parity_recover(struct bio *bio, struct btrfs_io_context *bioc,
			   int mirror_num)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	struct btrfs_raid_bio *rbio;

	rbio = alloc_rbio(fs_info, bioc);
	if (IS_ERR(rbio)) {
		bio->bi_status = errno_to_blk_status(PTR_ERR(rbio));
		bio_endio(bio);
		return;
	}

	rbio->operation = BTRFS_RBIO_READ_REBUILD;
	rbio_add_bio(rbio, bio);

	set_rbio_range_error(rbio, bio);

	/*
	 * Loop retry:
	 * for 'mirror == 2', reconstruct from all other stripes.
	 * for 'mirror_num > 2', select a stripe to fail on every retry.
	 */
	if (mirror_num > 2)
		set_rbio_raid6_extra_error(rbio, mirror_num);

	start_async_work(rbio, recover_rbio_work);
}

static void fill_data_csums(struct btrfs_raid_bio *rbio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	struct btrfs_root *csum_root;
	const u64 start = rbio->bioc->full_stripe_logical;
	const u32 len = (rbio->nr_data * rbio->stripe_nsectors) <<
			fs_info->sectorsize_bits;
	int ret;

	/* The rbio should not have its csum buffer initialized. */
	ASSERT(!rbio->csum_buf && !rbio->csum_bitmap);

	/*
	 * Skip the csum search if:
	 *
	 * - The rbio doesn't belong to data block groups
	 *   Then we are doing IO for tree blocks, no need to search csums.
	 *
	 * - The rbio belongs to mixed block groups
	 *   This is to avoid deadlock, as we're already holding the full
	 *   stripe lock, if we trigger a metadata read, and it needs to do
	 *   raid56 recovery, we will deadlock.
	 */
	if (!(rbio->bioc->map_type & BTRFS_BLOCK_GROUP_DATA) ||
	    rbio->bioc->map_type & BTRFS_BLOCK_GROUP_METADATA)
		return;

	rbio->csum_buf = kzalloc(rbio->nr_data * rbio->stripe_nsectors *
				 fs_info->csum_size, GFP_NOFS);
	rbio->csum_bitmap = bitmap_zalloc(rbio->nr_data * rbio->stripe_nsectors,
					  GFP_NOFS);
	if (!rbio->csum_buf || !rbio->csum_bitmap) {
		ret = -ENOMEM;
		goto error;
	}

	csum_root = btrfs_csum_root(fs_info, rbio->bioc->full_stripe_logical);
	if (unlikely(!csum_root)) {
		btrfs_err(fs_info,
			  "missing csum root for extent at bytenr %llu",
			  rbio->bioc->full_stripe_logical);
		ret = -EUCLEAN;
		goto error;
	}

	ret = btrfs_lookup_csums_bitmap(csum_root, NULL, start, start + len - 1,
					rbio->csum_buf, rbio->csum_bitmap);
	if (ret < 0)
		goto error;
	if (bitmap_empty(rbio->csum_bitmap, len >> fs_info->sectorsize_bits))
		goto no_csum;
	return;

error:
	/*
	 * We failed to allocate memory or grab the csum, but it's not fatal,
	 * we can still continue.  But better to warn users that RMW is no
	 * longer safe for this particular sub-stripe write.
	 */
	btrfs_warn_rl(fs_info,
"sub-stripe write for full stripe %llu is not safe, failed to get csum: %d",
			rbio->bioc->full_stripe_logical, ret);
no_csum:
	kfree(rbio->csum_buf);
	bitmap_free(rbio->csum_bitmap);
	rbio->csum_buf = NULL;
	rbio->csum_bitmap = NULL;
}

static int rmw_read_wait_recover(struct btrfs_raid_bio *rbio)
{
	struct bio_list bio_list = BIO_EMPTY_LIST;
	int total_sector_nr;
	int ret = 0;

	/*
	 * Fill the data csums we need for data verification.  We need to fill
	 * the csum_bitmap/csum_buf first, as our endio function will try to
	 * verify the data sectors.
	 */
	fill_data_csums(rbio);

	/*
	 * Build a list of bios to read all sectors (including data and P/Q).
	 *
	 * This behavior is to compensate the later csum verification and recovery.
	 */
	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		int stripe = total_sector_nr / rbio->stripe_nsectors;
		int sectornr = total_sector_nr % rbio->stripe_nsectors;
		phys_addr_t *paddrs;

		paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, stripe,
					 sectornr, REQ_OP_READ);
		if (ret) {
			bio_list_put(&bio_list);
			return ret;
		}
	}

	/*
	 * We may or may not have any corrupted sectors (including missing dev
	 * and csum mismatch), just let recover_sectors() to handle them all.
	 */
	submit_read_wait_bio_list(rbio, &bio_list);
	return recover_sectors(rbio);
}

static void raid_wait_write_end_io(struct bio *bio)
{
	struct btrfs_raid_bio *rbio = bio->bi_private;

	if (bio->bi_status)
		rbio_update_error_bitmap(rbio, bio);
	bio_put(bio);
	if (atomic_dec_and_test(&rbio->stripes_pending))
		wake_up(&rbio->io_wait);
}

static void submit_write_bios(struct btrfs_raid_bio *rbio,
			      struct bio_list *bio_list)
{
	struct bio *bio;

	atomic_set(&rbio->stripes_pending, bio_list_size(bio_list));
	while ((bio = bio_list_pop(bio_list))) {
		bio->bi_end_io = raid_wait_write_end_io;

		if (trace_raid56_write_enabled()) {
			struct raid56_bio_trace_info trace_info = { 0 };

			bio_get_trace_info(rbio, bio, &trace_info);
			trace_call__raid56_write(rbio, bio, &trace_info);
		}
		submit_bio(bio);
	}
}

/*
 * To determine if we need to read any sector from the disk.
 * Should only be utilized in RMW path, to skip cached rbio.
 */
static bool need_read_stripe_sectors(struct btrfs_raid_bio *rbio)
{
	int i;

	for (i = 0; i < rbio->nr_data * rbio->stripe_nsectors; i++) {
		phys_addr_t paddr = rbio->stripe_paddrs[i * rbio->sector_nsteps];

		/*
		 * We have a sector which doesn't have page nor uptodate,
		 * thus this rbio can not be cached one, as cached one must
		 * have all its data sectors present and uptodate.
		 */
		if (paddr == INVALID_PADDR ||
		    !test_bit(i, rbio->stripe_uptodate_bitmap))
			return true;
	}
	return false;
}

static void rmw_rbio(struct btrfs_raid_bio *rbio)
{
	struct bio_list bio_list;
	struct page **pages = NULL;
	unsigned int *offsets = NULL;
	int sectornr;
	int ret = 0;

	/*
	 * Allocate the pages for parity first, as P/Q pages will always be
	 * needed for both full-stripe and sub-stripe writes.
	 */
	ret = alloc_rbio_parity_pages(rbio);
	if (ret < 0)
		goto out;

	/*
	 * Either full stripe write, or we have every data sector already
	 * cached, can go to write path immediately.
	 */
	if (!rbio_is_full(rbio) && need_read_stripe_sectors(rbio)) {
		/*
		 * Now we're doing sub-stripe write, also need all data stripes
		 * to do the full RMW.
		 */
		ret = alloc_rbio_data_pages(rbio);
		if (ret < 0)
			goto out;

		index_rbio_pages(rbio);

		ret = rmw_read_wait_recover(rbio);
		if (ret < 0)
			goto out;
	}

	/*
	 * At this stage we're not allowed to add any new bios to the
	 * bio list any more, anyone else that wants to change this stripe
	 * needs to do their own rmw.
	 */
	spin_lock(&rbio->bio_list_lock);
	set_bit(RBIO_RMW_LOCKED_BIT, &rbio->flags);
	spin_unlock(&rbio->bio_list_lock);

	bitmap_clear(rbio->error_bitmap, 0, rbio->nr_sectors);

	index_rbio_pages(rbio);

	/*
	 * We don't cache full rbios because we're assuming
	 * the higher layers are unlikely to use this area of
	 * the disk again soon.  If they do use it again,
	 * hopefully they will send another full bio.
	 */
	if (!rbio_is_full(rbio))
		cache_rbio_pages(rbio);
	else
		clear_bit(RBIO_CACHE_READY_BIT, &rbio->flags);

	pages = kzalloc_objs(struct page *, rbio->real_stripes, GFP_NOFS);
	offsets = kzalloc_objs(unsigned int, rbio->real_stripes, GFP_NOFS);
	if (!pages || !offsets) {
		ret = -ENOMEM;
		goto out;
	}

	{
		struct dma_async_tx_descriptor *tx = NULL;

		/* Chain all sectors so a DMA engine can pipeline across them. */
		for (sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++)
			tx = generate_pq_vertical(rbio, sectornr, pages, offsets, tx);

		if (tx) {
			async_tx_issue_pending(tx);
			dma_wait_for_async_tx(tx);
		}

		for (sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++)
			generate_pq_vertical_finish(rbio, sectornr);
	}

	bio_list_init(&bio_list);
	ret = rmw_assemble_write_bios(rbio, &bio_list);
	if (ret < 0)
		goto out;

	/* We should have at least one bio assembled. */
	ASSERT(bio_list_size(&bio_list));
	submit_write_bios(rbio, &bio_list);
	wait_event(rbio->io_wait, atomic_read(&rbio->stripes_pending) == 0);

	/* We may have more errors than our tolerance during the read. */
	for (sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++) {
		int found_errors;

		found_errors = get_rbio_vertical_errors(rbio, sectornr, NULL, NULL);
		if (unlikely(found_errors > rbio->bioc->max_errors)) {
			ret = -EIO;
			break;
		}
	}
out:
	kfree(pages);
	kfree(offsets);
	rbio_orig_end_io(rbio, errno_to_blk_status(ret));
}

static void rmw_rbio_work(struct work_struct *work)
{
	struct btrfs_raid_bio *rbio;

	rbio = container_of(work, struct btrfs_raid_bio, work);
	if (lock_stripe_add(rbio) == 0)
		rmw_rbio(rbio);
}

static void rmw_rbio_work_locked(struct work_struct *work)
{
	rmw_rbio(container_of(work, struct btrfs_raid_bio, work));
}

/*
 * The following code is used to scrub/replace the parity stripe
 *
 * Caller must have already increased bio_counter for getting @bioc.
 *
 * Note: We need make sure all the pages that add into the scrub/replace
 * raid bio are correct and not be changed during the scrub/replace. That
 * is those pages just hold metadata or file data with checksum.
 */

struct btrfs_raid_bio *raid56_parity_alloc_scrub_rbio(struct bio *bio,
				struct btrfs_io_context *bioc,
				struct btrfs_device *scrub_dev,
				unsigned long *dbitmap, int stripe_nsectors)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	struct btrfs_raid_bio *rbio;
	int i;

	rbio = alloc_rbio(fs_info, bioc);
	if (IS_ERR(rbio))
		return NULL;
	bio_list_add(&rbio->bio_list, bio);
	/*
	 * This is a special bio which is used to hold the completion handler
	 * and make the scrub rbio is similar to the other types
	 */
	ASSERT(!bio->bi_iter.bi_size);
	rbio->operation = BTRFS_RBIO_PARITY_SCRUB;

	/*
	 * After mapping bioc with BTRFS_MAP_WRITE, parities have been sorted
	 * to the end position, so this search can start from the first parity
	 * stripe.
	 */
	for (i = rbio->nr_data; i < rbio->real_stripes; i++) {
		if (bioc->stripes[i].dev == scrub_dev) {
			rbio->scrubp = i;
			break;
		}
	}
	ASSERT_RBIO_STRIPE(i < rbio->real_stripes, rbio, i);

	bitmap_copy(&rbio->dbitmap, dbitmap, stripe_nsectors);
	return rbio;
}

static int alloc_rbio_sector_pages(struct btrfs_raid_bio *rbio,
				  int sector_nr)
{
	const u32 step = min(PAGE_SIZE, rbio->bioc->fs_info->sectorsize);
	const u32 base = sector_nr * rbio->sector_nsteps;

	for (int i = base; i < base + rbio->sector_nsteps; i++) {
		const unsigned int page_index = (i * step) >> PAGE_SHIFT;
		struct page *page;

		if (rbio->stripe_pages[page_index])
			continue;
		page = alloc_page(GFP_NOFS);
		if (!page)
			return -ENOMEM;
		rbio->stripe_pages[page_index] = page;
	}
	return 0;
}

/*
 * We just scrub the parity that we have correct data on the same horizontal,
 * so we needn't allocate all pages for all the stripes.
 */
static int alloc_rbio_essential_pages(struct btrfs_raid_bio *rbio)
{
	int total_sector_nr;

	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		int sectornr = total_sector_nr % rbio->stripe_nsectors;
		int ret;

		if (!test_bit(sectornr, &rbio->dbitmap))
			continue;
		ret = alloc_rbio_sector_pages(rbio, total_sector_nr);
		if (ret < 0)
			return ret;
	}
	index_stripe_sectors(rbio);
	return 0;
}

/* Return true if the content of the step matches the calculated one. */
static bool verify_one_parity_step(struct btrfs_raid_bio *rbio,
				   struct page **pages, unsigned int *offsets,
				   unsigned int sector_nr,
				   unsigned int step_nr)
{
	const unsigned int nr_data = rbio->nr_data;
	const bool has_qstripe = (rbio->real_stripes - rbio->nr_data == 2);
	const u32 step = min(rbio->bioc->fs_info->sectorsize, PAGE_SIZE);
	struct async_submit_ctl submit;
	struct dma_async_tx_descriptor *tx;
	void *scrub_kaddr, *calc_kaddr;
	bool ret = false;

	ASSERT(step_nr < rbio->sector_nsteps);

	/* Fill page/offset arrays for data stripes. */
	for (int stripe = 0; stripe < nr_data; stripe++) {
		phys_addr_t paddr = sector_paddr_in_rbio(rbio, stripe, sector_nr,
							 step_nr, 0);
		pages[stripe] = phys_to_page(paddr);
		offsets[stripe] = offset_in_page(paddr);
	}

	ASSERT(rbio->scrubp >= nr_data);
	ASSERT(rbio->scrubp < rbio->real_stripes);

	/* The parity destination pages must be set by the caller. */
	ASSERT(pages[nr_data]);
	ASSERT(offsets[nr_data] == 0);
	if (has_qstripe) {
		ASSERT(pages[rbio->real_stripes - 1]);
		ASSERT(offsets[rbio->real_stripes - 1] == 0);
	}

	if (has_qstripe) {
		assert_rbio(rbio);
		init_async_submit(&submit, 0, NULL, NULL, NULL, NULL);
		tx = async_gen_syndrome(pages, offsets, rbio->real_stripes,
					step, &submit);
	} else {
		init_async_submit(&submit, ASYNC_TX_XOR_ZERO_DST, NULL, NULL, NULL, NULL);
		/*
		 * async_xor_offs takes src_cnt = nr_data.  pages[0..nr_data-1]
		 * are data sources; pages[nr_data] (the destination) is
		 * excluded from the source list by the count.
		 * ASYNC_TX_XOR_ZERO_DST zeroes the destination first, then
		 * XORs all sources: dest = 0 ^ D0 ^ ... ^ D{n-1} = parity.
		 */
		tx = async_xor_offs(pages[rbio->nr_data], offsets[rbio->nr_data],
				    pages, offsets, rbio->nr_data, step, &submit);
	}
	/* async_tx returns NULL when the operation completed synchronously. */
	if (tx) {
		async_tx_issue_pending(tx);
		dma_wait_for_async_tx(tx);
	}

	/* Check scrubbing parity and repair it. */
	scrub_kaddr = kmap_local_paddr(rbio_stripe_paddr(rbio, rbio->scrubp,
							 sector_nr, step_nr));
	calc_kaddr = kmap_local_page(pages[rbio->scrubp]);
	if (memcmp(scrub_kaddr, (void *)((char *)calc_kaddr + offsets[rbio->scrubp]), step) != 0)
		memcpy(scrub_kaddr, (void *)((char *)calc_kaddr + offsets[rbio->scrubp]), step);
	else
		ret = true;
	kunmap_local(calc_kaddr);
	kunmap_local(scrub_kaddr);

	return ret;
}

/*
 * The @pages and @offsets arrays should have the P/Q parity pages already set.
 */
static void verify_one_parity_sector(struct btrfs_raid_bio *rbio,
				     struct page **pages, unsigned int *offsets,
				     unsigned int sector_nr)
{
	bool found_error = false;

	for (int step_nr = 0; step_nr < rbio->sector_nsteps; step_nr++) {
		bool match;

		match = verify_one_parity_step(rbio, pages, offsets, sector_nr, step_nr);
		if (!match)
			found_error = true;
	}
	if (!found_error)
		bitmap_clear(&rbio->dbitmap, sector_nr, 1);
}

static int finish_parity_scrub(struct btrfs_raid_bio *rbio)
{
	struct btrfs_io_context *bioc = rbio->bioc;
	unsigned long *pbitmap = &rbio->finish_pbitmap;
	int nr_data = rbio->nr_data;
	int sectornr;
	bool has_qstripe;
	struct page **pages;
	unsigned int *offsets;
	struct page *p_page = NULL;
	struct page *q_page = NULL;
	struct bio_list bio_list;
	bool is_replace = false;
	int ret = 0;

	bio_list_init(&bio_list);

	if (rbio->real_stripes - rbio->nr_data == 1)
		has_qstripe = false;
	else if (rbio->real_stripes - rbio->nr_data == 2)
		has_qstripe = true;
	else
		BUG();

	/*
	 * Replace is running and our P/Q stripe is being replaced, then we
	 * need to duplicate the final write to replace target.
	 */
	if (bioc->replace_nr_stripes && bioc->replace_stripe_src == rbio->scrubp) {
		is_replace = true;
		bitmap_copy(pbitmap, &rbio->dbitmap, rbio->stripe_nsectors);
	}

	/*
	 * Because the higher layers(scrubber) are unlikely to
	 * use this area of the disk again soon, so don't cache
	 * it.
	 */
	clear_bit(RBIO_CACHE_READY_BIT, &rbio->flags);

	pages = kzalloc_objs(struct page *, rbio->real_stripes, GFP_NOFS);
	offsets = kzalloc_objs(unsigned int, rbio->real_stripes, GFP_NOFS);
	if (!pages || !offsets) {
		ret = -ENOMEM;
		goto out;
	}

	p_page = alloc_page(GFP_NOFS);
	if (!p_page) {
		ret = -ENOMEM;
		goto out;
	}
	pages[nr_data] = p_page;
	offsets[nr_data] = 0;

	if (has_qstripe) {
		q_page = alloc_page(GFP_NOFS);
		if (!q_page) {
			ret = -ENOMEM;
			goto out;
		}
		pages[rbio->real_stripes - 1] = q_page;
		offsets[rbio->real_stripes - 1] = 0;
	}

	bitmap_clear(rbio->error_bitmap, 0, rbio->nr_sectors);

	/* Temporary parity destination pages are set once and reused for all sectors */
	for_each_set_bit(sectornr, &rbio->dbitmap, rbio->stripe_nsectors)
		verify_one_parity_sector(rbio, pages, offsets, sectornr);

out:
	if (p_page)
		__free_page(p_page);
	if (q_page)
		__free_page(q_page);
	kfree(pages);
	kfree(offsets);
	if (ret)
		return ret;

	/*
	 * time to start writing.  Make bios for everything from the
	 * higher layers (the bio_list in our rbio) and our p/q.  Ignore
	 * everything else.
	 */
	for_each_set_bit(sectornr, &rbio->dbitmap, rbio->stripe_nsectors) {
		phys_addr_t *paddrs;

		paddrs = rbio_stripe_paddrs(rbio, rbio->scrubp, sectornr);
		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, rbio->scrubp,
					 sectornr, REQ_OP_WRITE);
		if (ret)
			goto cleanup;
	}

	if (!is_replace)
		goto submit_write;

	/*
	 * Replace is running and our parity stripe needs to be duplicated to
	 * the target device.  Check we have a valid source stripe number.
	 */
	ASSERT_RBIO(rbio->bioc->replace_stripe_src >= 0, rbio);
	for_each_set_bit(sectornr, pbitmap, rbio->stripe_nsectors) {
		phys_addr_t *paddrs;

		paddrs = rbio_stripe_paddrs(rbio, rbio->scrubp, sectornr);
		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, rbio->real_stripes,
					 sectornr, REQ_OP_WRITE);
		if (ret)
			goto cleanup;
	}

submit_write:
	submit_write_bios(rbio, &bio_list);
	return 0;

cleanup:
	bio_list_put(&bio_list);
	return ret;
}

static inline int is_data_stripe(struct btrfs_raid_bio *rbio, int stripe)
{
	if (stripe >= 0 && stripe < rbio->nr_data)
		return 1;
	return 0;
}

static int recover_scrub_rbio(struct btrfs_raid_bio *rbio)
{
	struct page **pages = NULL;
	unsigned int *offsets = NULL;
	struct page **src_pages = NULL;
	unsigned int *src_offsets = NULL;
	int sector_nr;
	int ret = 0;

	pages = kzalloc_objs(struct page *, rbio->real_stripes, GFP_NOFS);
	offsets = kzalloc_objs(unsigned int, rbio->real_stripes, GFP_NOFS);
	if (!pages || !offsets) {
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * src_pages/src_offsets hold the pstripe source list:
	 *   P stripe (1) + non-failed data stripes (at most nr_data - 1)
	 * = at most nr_data entries.
	 */
	src_pages = kzalloc_objs(struct page *, rbio->nr_data, GFP_NOFS);
	src_offsets = kzalloc_objs(unsigned int, rbio->nr_data, GFP_NOFS);
	if (!src_pages || !src_offsets) {
		ret = -ENOMEM;
		goto out;
	}

	for (sector_nr = 0; sector_nr < rbio->stripe_nsectors; sector_nr++) {
		int dfail = 0, failp = -1;
		int faila;
		int failb;
		int found_errors;

		found_errors = get_rbio_vertical_errors(rbio, sector_nr,
							 &faila, &failb);
		if (unlikely(found_errors > rbio->bioc->max_errors)) {
			ret = -EIO;
			goto out;
		}
		if (found_errors == 0)
			continue;

		/* We should have at least one error here. */
		ASSERT(faila >= 0 || failb >= 0);

		if (is_data_stripe(rbio, faila))
			dfail++;
		else if (is_parity_stripe(faila))
			failp = faila;

		if (is_data_stripe(rbio, failb))
			dfail++;
		else if (is_parity_stripe(failb))
			failp = failb;
		/*
		 * Because we can not use a scrubbing parity to repair the
		 * data, so the capability of the repair is declined.  (In the
		 * case of RAID5, we can not repair anything.)
		 */
		if (unlikely(dfail > rbio->bioc->max_errors - 1)) {
			ret = -EIO;
			goto out;
		}
		/*
		 * If all data is good, only parity is correctly, just repair
		 * the parity, no need to recover data stripes.
		 */
		if (dfail == 0)
			continue;

		/*
		 * Here means we got one corrupted data stripe and one
		 * corrupted parity on RAID6, if the corrupted parity is
		 * scrubbing parity, luckily, use the other one to repair the
		 * data, or we can not repair the data stripe.
		 */
		if (unlikely(failp != rbio->scrubp)) {
			ret = -EIO;
			goto out;
		}

		ret = recover_vertical(rbio, sector_nr, pages, offsets,
				       src_pages, src_offsets);
		if (ret < 0)
			goto out;
	}
out:
	kfree(src_pages);
	kfree(src_offsets);
	kfree(pages);
	kfree(offsets);
	return ret;
}

static int scrub_assemble_read_bios(struct btrfs_raid_bio *rbio)
{
	struct bio_list bio_list = BIO_EMPTY_LIST;
	int total_sector_nr;
	int ret = 0;

	/* Build a list of bios to read all the missing parts. */
	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		int sectornr = total_sector_nr % rbio->stripe_nsectors;
		int stripe = total_sector_nr / rbio->stripe_nsectors;
		phys_addr_t *paddrs;

		/* No data in the vertical stripe, no need to read. */
		if (!test_bit(sectornr, &rbio->dbitmap))
			continue;

		/*
		 * A parity-scrub rbio carries no data in its bio list: the
		 * only bio there is the empty completion bio added by
		 * raid56_parity_alloc_scrub_rbio().  Every sector is read
		 * from the stripe, so only assert that invariant here.
		 */
		ASSERT(!sector_paddrs_in_rbio(rbio, stripe, sectornr, 1));

		paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
		/*
		 * The bio cache may have handed us an uptodate sector.  If so,
		 * use it.
		 */
		if (test_bit(rbio_sector_index(rbio, stripe, sectornr),
			     rbio->stripe_uptodate_bitmap))
			continue;

		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, stripe,
					 sectornr, REQ_OP_READ);
		if (ret) {
			bio_list_put(&bio_list);
			return ret;
		}
	}

	submit_read_wait_bio_list(rbio, &bio_list);
	return 0;
}

static void scrub_rbio(struct btrfs_raid_bio *rbio)
{
	int sector_nr;
	int ret;

	ret = alloc_rbio_essential_pages(rbio);
	if (ret)
		goto out;

	bitmap_clear(rbio->error_bitmap, 0, rbio->nr_sectors);

	ret = scrub_assemble_read_bios(rbio);
	if (ret < 0)
		goto out;

	/* We may have some failures, recover the failed sectors first. */
	ret = recover_scrub_rbio(rbio);
	if (ret < 0)
		goto out;

	/*
	 * We have every sector properly prepared. Can finish the scrub
	 * and writeback the good content.
	 */
	ret = finish_parity_scrub(rbio);
	wait_event(rbio->io_wait, atomic_read(&rbio->stripes_pending) == 0);
	for (sector_nr = 0; sector_nr < rbio->stripe_nsectors; sector_nr++) {
		int found_errors;

		found_errors = get_rbio_vertical_errors(rbio, sector_nr, NULL, NULL);
		if (unlikely(found_errors > rbio->bioc->max_errors)) {
			ret = -EIO;
			break;
		}
	}
out:
	rbio_orig_end_io(rbio, errno_to_blk_status(ret));
}

static void scrub_rbio_work_locked(struct work_struct *work)
{
	scrub_rbio(container_of(work, struct btrfs_raid_bio, work));
}

void raid56_parity_submit_scrub_rbio(struct btrfs_raid_bio *rbio)
{
	if (!lock_stripe_add(rbio))
		start_async_work(rbio, scrub_rbio_work_locked);
}

/*
 * This is for scrub call sites where we already have correct data contents.
 * This allows us to avoid reading data stripes again.
 *
 * Unfortunately here we have to do folio copy, other than reusing the pages.
 * This is due to the fact rbio has its own page management for its cache.
 */
void raid56_parity_cache_data_folios(struct btrfs_raid_bio *rbio,
				     struct folio **data_folios, u64 data_logical)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u64 offset_in_full_stripe = data_logical -
					  rbio->bioc->full_stripe_logical;
	unsigned int findex = 0;
	unsigned int foffset = 0;
	int ret;

	/*
	 * If we hit ENOMEM temporarily, but later at
	 * raid56_parity_submit_scrub_rbio() time it succeeded, we just do
	 * the extra read, not a big deal.
	 *
	 * If we hit ENOMEM later at raid56_parity_submit_scrub_rbio() time,
	 * the bio would got proper error number set.
	 */
	ret = alloc_rbio_data_pages(rbio);
	if (ret < 0)
		return;

	/* data_logical must be at stripe boundary and inside the full stripe. */
	ASSERT(IS_ALIGNED(offset_in_full_stripe, BTRFS_STRIPE_LEN));
	ASSERT(offset_in_full_stripe < (rbio->nr_data << BTRFS_STRIPE_LEN_SHIFT));

	for (unsigned int cur_off = offset_in_full_stripe;
	     cur_off < offset_in_full_stripe + BTRFS_STRIPE_LEN;
	     cur_off += PAGE_SIZE) {
		const unsigned int pindex = cur_off >> PAGE_SHIFT;
		struct dma_async_tx_descriptor *tx;
		struct async_submit_ctl submit;

		ASSERT(IS_ALIGNED(foffset, PAGE_SIZE));
		init_async_submit(&submit, 0, NULL, NULL, NULL, NULL);
		tx = async_memcpy(rbio->stripe_pages[pindex],
				  folio_page(data_folios[findex], foffset >> PAGE_SHIFT),
				  0, 0, PAGE_SIZE, &submit);
		if (tx) {
			async_tx_issue_pending(tx);
			dma_wait_for_async_tx(tx);
		}

		foffset += PAGE_SIZE;
		ASSERT(foffset <= folio_size(data_folios[findex]));
		if (foffset == folio_size(data_folios[findex])) {
			findex++;
			foffset = 0;
		}
	}
	bitmap_set(rbio->stripe_uptodate_bitmap,
		   offset_in_full_stripe >> fs_info->sectorsize_bits,
		   BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits);
}
