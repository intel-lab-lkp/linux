// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/page_io.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, 
 *  Asynchronous swapping added 30.12.95. Stephen Tweedie
 *  Removed race in async swapping. 14.4.1996. Bruno Haible
 *  Add swap of shared pages through the page cache. 20.2.1998. Stephen Tweedie
 *  Always use brw_page, life becomes simpler. 12 May 1998 Eric Biederman
 */

#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/swapops.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/psi.h>
#include <linux/uio.h>
#include <linux/sched/task.h>
#include <linux/delayacct.h>
#include <linux/zswap.h>
#include "swap.h"

int generic_swapfile_activate(struct swap_info_struct *sis,
				struct file *swap_file,
				sector_t *span)
{
	struct address_space *mapping = swap_file->f_mapping;
	struct inode *inode = mapping->host;
	unsigned blocks_per_page;
	unsigned long page_no;
	unsigned blkbits;
	sector_t probe_block;
	sector_t last_block;
	sector_t lowest_block = -1;
	sector_t highest_block = 0;
	int nr_extents = 0;
	int ret;

	blkbits = inode->i_blkbits;
	blocks_per_page = PAGE_SIZE >> blkbits;

	/*
	 * Map all the blocks into the extent tree.  This code doesn't try
	 * to be very smart.
	 */
	probe_block = 0;
	page_no = 0;
	last_block = i_size_read(inode) >> blkbits;
	while ((probe_block + blocks_per_page) <= last_block &&
			page_no < sis->max) {
		unsigned block_in_page;
		sector_t first_block;

		cond_resched();

		first_block = probe_block;
		ret = bmap(inode, &first_block);
		if (ret || !first_block)
			goto bad_bmap;

		/*
		 * It must be PAGE_SIZE aligned on-disk
		 */
		if (first_block & (blocks_per_page - 1)) {
			probe_block++;
			goto reprobe;
		}

		for (block_in_page = 1; block_in_page < blocks_per_page;
					block_in_page++) {
			sector_t block;

			block = probe_block + block_in_page;
			ret = bmap(inode, &block);
			if (ret || !block)
				goto bad_bmap;

			if (block != first_block + block_in_page) {
				/* Discontiguity */
				probe_block++;
				goto reprobe;
			}
		}

		first_block >>= (PAGE_SHIFT - blkbits);
		if (page_no) {	/* exclude the header page */
			if (first_block < lowest_block)
				lowest_block = first_block;
			if (first_block > highest_block)
				highest_block = first_block;
		}

		/*
		 * We found a PAGE_SIZE-length, PAGE_SIZE-aligned run of blocks
		 */
		ret = add_swap_extent(sis, page_no, 1, first_block);
		if (ret < 0)
			goto out;
		nr_extents += ret;
		page_no++;
		probe_block += blocks_per_page;
reprobe:
		continue;
	}
	ret = nr_extents;
	*span = 1 + highest_block - lowest_block;
	if (page_no == 0)
		page_no = 1;	/* force Empty message */
	sis->max = page_no;
	sis->pages = page_no - 1;
out:
	return ret;
bad_bmap:
	pr_err("swapon: swapfile has holes\n");
	ret = -EINVAL;
	goto out;
}

static bool is_folio_zero_filled(struct folio *folio)
{
	unsigned int pos, last_pos;
	unsigned long *data;
	unsigned int i;

	last_pos = PAGE_SIZE / sizeof(*data) - 1;
	for (i = 0; i < folio_nr_pages(folio); i++) {
		data = kmap_local_folio(folio, i * PAGE_SIZE);
		/*
		 * Check last word first, incase the page is zero-filled at
		 * the start and has non-zero data at the end, which is common
		 * in real-world workloads.
		 */
		if (data[last_pos]) {
			kunmap_local(data);
			return false;
		}
		for (pos = 0; pos < last_pos; pos++) {
			if (data[pos]) {
				kunmap_local(data);
				return false;
			}
		}
		kunmap_local(data);
	}

	return true;
}

static void swap_zeromap_folio_set(struct folio *folio)
{
	struct obj_cgroup *objcg = get_obj_cgroup_from_folio(folio);
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	int nr_pages = folio_nr_pages(folio);
	swp_entry_t entry;
	unsigned int i;

	for (i = 0; i < folio_nr_pages(folio); i++) {
		entry = page_swap_entry(folio_page(folio, i));
		set_bit(swp_offset(entry), sis->zeromap);
	}

	count_vm_events(SWPOUT_ZERO, nr_pages);
	if (objcg) {
		count_objcg_events(objcg, SWPOUT_ZERO, nr_pages);
		obj_cgroup_put(objcg);
	}
}

static void swap_zeromap_folio_clear(struct folio *folio)
{
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	swp_entry_t entry;
	unsigned int i;

	for (i = 0; i < folio_nr_pages(folio); i++) {
		entry = page_swap_entry(folio_page(folio, i));
		clear_bit(swp_offset(entry), sis->zeromap);
	}
}

/*
 * We may have stale swap cache pages in memory: notice
 * them here and get rid of the unnecessary final write.
 */
int swap_writeout(struct swap_io_ctx *ctx, struct folio *folio)
{
	int ret = 0;

	if (folio_free_swap(folio))
		goto out_unlock;

	/*
	 * Arch code may have to preserve more data than just the page
	 * contents, e.g. memory tags.
	 */
	ret = arch_prepare_to_swap(folio);
	if (ret) {
		folio_mark_dirty(folio);
		goto out_unlock;
	}

	/*
	 * Use a bitmap (zeromap) to avoid doing IO for zero-filled pages.
	 * The bits in zeromap are protected by the locked swapcache folio
	 * and atomic updates are used to protect against read-modify-write
	 * corruption due to other zero swap entries seeing concurrent updates.
	 */
	if (is_folio_zero_filled(folio)) {
		swap_zeromap_folio_set(folio);
		goto out_unlock;
	}

	/*
	 * Clear bits this folio occupies in the zeromap to prevent zero data
	 * being read in from any previous zero writes that occupied the same
	 * swap entries.
	 */
	swap_zeromap_folio_clear(folio);

	if (zswap_store(folio)) {
		count_mthp_stat(folio_order(folio), MTHP_STAT_ZSWPOUT);
		goto out_unlock;
	}

	rcu_read_lock();
	if (!mem_cgroup_zswap_writeback_enabled(folio_memcg(folio))) {
		rcu_read_unlock();
		folio_mark_dirty(folio);
		return AOP_WRITEPAGE_ACTIVATE;
	}
	rcu_read_unlock();

	__swap_writepage(ctx, folio);
	return 0;
out_unlock:
	folio_unlock(folio);
	return ret;
}

static inline void count_swpout_vm_event(struct folio *folio)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	if (unlikely(folio_test_pmd_mappable(folio))) {
		count_memcg_folio_events(folio, THP_SWPOUT, 1);
		count_vm_event(THP_SWPOUT);
	}
#endif
	count_mthp_stat(folio_order(folio), MTHP_STAT_SWPOUT);
	count_memcg_folio_events(folio, PSWPOUT, folio_nr_pages(folio));
	count_vm_events(PSWPOUT, folio_nr_pages(folio));
}

#if defined(CONFIG_MEMCG) && defined(CONFIG_BLK_CGROUP)
static void bio_associate_blkg_from_page(struct bio *bio, struct folio *folio)
{
	struct cgroup_subsys_state *css;
	struct mem_cgroup *memcg;

	if (!folio_memcg_charged(folio))
		return;

	rcu_read_lock();
	memcg = folio_memcg(folio);
	css = cgroup_e_css(memcg->css.cgroup, &io_cgrp_subsys);
	bio_associate_blkg_from_css(bio, css);
	rcu_read_unlock();
}
#else
#define bio_associate_blkg_from_page(bio, folio)		do { } while (0)
#endif /* CONFIG_MEMCG && CONFIG_BLK_CGROUP */

struct swap_iocb {
	union {
		struct kiocb	iocb;
		struct bio	bio;
	};
	struct bio_vec		bvec[SWAP_CLUSTER_MAX];
	int			nr_vecs;
	int			len;
};
static mempool_t *sio_pool;

int sio_pool_init(void)
{
	if (!sio_pool) {
		mempool_t *pool = mempool_create_kmalloc_pool(
			SWAP_CLUSTER_MAX, sizeof(struct swap_iocb));
		if (cmpxchg(&sio_pool, NULL, pool))
			mempool_destroy(pool);
	}
	if (!sio_pool)
		return -ENOMEM;
	return 0;
}

static bool swap_can_merge(struct swap_io_ctx *ctx, struct folio *folio)
{
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	struct bio_vec *last_bv = &ctx->sio->bvec[ctx->sio->nr_vecs - 1];
	struct folio *prev_folio = page_folio(last_bv->bv_page);
	size_t prev_folio_size = folio_size(prev_folio);

	if (ctx->sis != sis)
		return false;
	return sis->ops->can_merge(folio, prev_folio, prev_folio_size);
}

static void swap_add_page(struct swap_io_ctx *ctx, struct folio *folio, int rw)
{
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	struct swap_iocb *sio = ctx->sio;

	if (sio && !swap_can_merge(ctx, folio)) {
		if (rw == WRITE)
			swap_write_submit(ctx);
		else
			swap_read_submit(ctx);
		sio = ctx->sio;
	}

	if (!sio) {
		ctx->sis = sis;
		ctx->sio = sio = mempool_alloc(sio_pool, GFP_NOIO);
		sio->nr_vecs = 0;
		sio->len = 0;
	}
	bvec_set_folio(&sio->bvec[sio->nr_vecs], folio, folio_size(folio), 0);
	sio->len += folio_size(folio);
	sio->nr_vecs += 1;
	if (sio->nr_vecs == ARRAY_SIZE(sio->bvec)) {
		if (rw == WRITE)
			swap_write_submit(ctx);
		else
			swap_read_submit(ctx);
	}
}

void __swap_writepage(struct swap_io_ctx *ctx, struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio), folio);

	count_swpout_vm_event(folio);
	folio_start_writeback(folio);
	folio_unlock(folio);
	swap_add_page(ctx, folio, WRITE);
}

static bool swap_read_folio_zeromap(struct folio *folio)
{
	int nr_pages = folio_nr_pages(folio);
	struct obj_cgroup *objcg;
	bool is_zeromap;

	/*
	 * Swapping in a large folio that is partially in the zeromap is not
	 * currently handled. Return true without marking the folio uptodate so
	 * that an IO error is emitted (e.g. do_swap_page() will sigbus).
	 */
	if (WARN_ON_ONCE(swap_zeromap_batch(folio->swap, nr_pages,
			&is_zeromap) != nr_pages))
		return true;

	if (!is_zeromap)
		return false;

	objcg = get_obj_cgroup_from_folio(folio);
	count_vm_events(SWPIN_ZERO, nr_pages);
	if (objcg) {
		count_objcg_events(objcg, SWPIN_ZERO, nr_pages);
		obj_cgroup_put(objcg);
	}

	folio_zero_range(folio, 0, folio_size(folio));
	folio_mark_uptodate(folio);
	return true;
}

void swap_read_folio(struct swap_io_ctx *ctx, struct folio *folio)
{
	struct swap_info_struct *sis = __swap_entry_to_info(folio->swap);
	bool synchronous = sis->flags & SWP_SYNCHRONOUS_IO;
	bool workingset = folio_test_workingset(folio);
	unsigned long pflags;
	bool in_thrashing;

	VM_BUG_ON_FOLIO(!folio_test_swapcache(folio) && !synchronous, folio);
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(folio_test_uptodate(folio), folio);

	/*
	 * Count submission time as memory stall and delay. When the device
	 * is congested, or the submitting cgroup IO-throttled, submission
	 * can be a significant part of overall IO time.
	 */
	if (workingset) {
		delayacct_thrashing_start(&in_thrashing);
		psi_memstall_enter(&pflags);
	}
	delayacct_swapin_start();

	if (swap_read_folio_zeromap(folio)) {
		folio_unlock(folio);
		goto finish;
	}

	if (zswap_load(folio) != -ENOENT)
		goto finish;

	/* We have to read from slower devices. Increase zswap protection. */
	zswap_folio_swapin(folio);
	swap_add_page(ctx, folio, READ);

finish:
	if (workingset) {
		delayacct_thrashing_end(&in_thrashing);
		psi_memstall_leave(&pflags);
	}
	delayacct_swapin_end();
}

static void sio_write_end(struct swap_iocb *sio, bool failed)
{
	int p;

	for (p = 0; p < sio->nr_vecs; p++) {
		struct page *page = sio->bvec[p].bv_page;

		if (failed) {
			set_page_dirty(page);
			ClearPageReclaim(page);
		}
		end_page_writeback(page);
	}
	mempool_free(sio, sio_pool);
}

static void sio_write_complete(struct kiocb *iocb, long ret)
{
	struct swap_iocb *sio = container_of(iocb, struct swap_iocb, iocb);
	bool failed = ret != sio->len;

	if (failed) {
		struct page *page = sio->bvec[0].bv_page;

		/*
		 * In the case of swap-over-nfs, this can be a temporary failure
		 * if the system has limited memory for allocating transmit
		 * buffers.  Mark the page dirty and avoid
		 * folio_rotate_reclaimable but rate-limit the messages.
		 */
		pr_err_ratelimited("Write error %ld on dio swapfile (%llu)\n",
				   ret, swap_dev_pos(page_swap_entry(page)));
	}

	sio_write_end(sio, failed);
}

static void end_swap_bio_write(struct bio *bio)
{
	struct swap_iocb *sio = container_of(bio, struct swap_iocb, bio);
	bool failed = !!bio->bi_status;

	if (failed)
		pr_alert_ratelimited("Write-error on swap-device (%u:%u:%llu)\n",
				     MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
				     (unsigned long long)bio->bi_iter.bi_sector);
	sio_write_end(sio, failed);
}

static void sio_read_end(struct swap_iocb *sio)
{
	int p;

	for (p = 0; p < sio->nr_vecs; p++) {
		struct folio *folio = page_folio(sio->bvec[p].bv_page);

		count_mthp_stat(folio_order(folio), MTHP_STAT_SWPIN);
		count_memcg_folio_events(folio, PSWPIN, folio_nr_pages(folio));
		folio_mark_uptodate(folio);
		folio_unlock(folio);
	}
	count_vm_events(PSWPIN, sio->len >> PAGE_SHIFT);
	mempool_free(sio, sio_pool);
}

static void sio_read_fail(struct swap_iocb *sio)
{
	int p;

	for (p = 0; p < sio->nr_vecs; p++)
		folio_unlock(page_folio(sio->bvec[p].bv_page));
	mempool_free(sio, sio_pool);
}

static void sio_read_complete(struct kiocb *iocb, long ret)
{
	struct swap_iocb *sio = container_of(iocb, struct swap_iocb, iocb);

	if (ret != sio->len) {
		pr_alert_ratelimited("Read-error on swap-device\n");
		sio_read_fail(sio);
		return;
	}

	sio_read_end(sio);
}

static void end_swap_bio_read(struct bio *bio)
{
	struct swap_iocb *sio = container_of(bio, struct swap_iocb, bio);

	if (bio->bi_status) {
		pr_alert_ratelimited("Read-error on swap-device (%u:%u:%llu)\n",
				     MAJOR(bio_dev(bio)), MINOR(bio_dev(bio)),
				     (unsigned long long)bio->bi_iter.bi_sector);
		sio_read_fail(sio);
		return;
	}

	sio_read_end(sio);
}

static void swap_bdev_submit_write(struct swap_io_ctx *ctx)
{
	struct swap_iocb *sio = ctx->sio;
	struct bio *bio = &sio->bio;

	bio_init(bio, ctx->sis->bdev, sio->bvec, ARRAY_SIZE(sio->bvec),
			REQ_OP_WRITE | REQ_SWAP);
	bio->bi_iter.bi_size = sio->len;
	bio->bi_iter.bi_sector = swap_folio_sector(bio_first_folio_all(bio));
	bio_associate_blkg_from_page(bio, bio_first_folio_all(bio));

	if (ctx->sis->flags & SWP_SYNCHRONOUS_IO) {
		submit_bio_wait(bio);
		end_swap_bio_write(bio);
	} else {
		bio->bi_end_io = end_swap_bio_write;
		submit_bio(bio);
	}
}

static void swap_bdev_submit_read(struct swap_io_ctx *ctx)
{
	struct swap_iocb *sio = ctx->sio;
	struct bio *bio = &sio->bio;

	bio_init(bio, ctx->sis->bdev, sio->bvec, ARRAY_SIZE(sio->bvec),
			REQ_OP_READ);
	bio->bi_iter.bi_size = sio->len;
	bio->bi_iter.bi_sector = swap_folio_sector(bio_first_folio_all(bio));

	if (ctx->sis->flags & SWP_SYNCHRONOUS_IO) {
		/*
		 * Keep this task valid during swap readpage because the oom
		 * killer may attempt to access it in the page fault retry
		 * time check.
		 */
		get_task_struct(current);
		submit_bio_wait(bio);
		end_swap_bio_read(bio);
		put_task_struct(current);
	} else {
		bio->bi_end_io = end_swap_bio_read;
		submit_bio(bio);
	}
}

static bool swap_bdev_can_merge(struct folio *folio, struct folio *prev_folio,
		size_t prev_folio_size)
{
	return swap_folio_sector(folio) ==
		swap_folio_sector(prev_folio) +
			(prev_folio_size >> SECTOR_SHIFT);
}

const struct swap_ops swap_bdev_ops = {
	.submit_write		= swap_bdev_submit_write,
	.submit_read		= swap_bdev_submit_read,
	.can_merge		= swap_bdev_can_merge,
};

static void swap_fs_submit(struct swap_io_ctx *ctx, int rw)
{
	struct swap_iocb *sio = ctx->sio;
	struct address_space *mapping = sio->iocb.ki_filp->f_mapping;
	struct iov_iter iter;
	int ret;

	init_sync_kiocb(&sio->iocb, ctx->sis->swap_file);
	sio->iocb.ki_pos = swap_dev_pos(page_folio(sio->bvec[0].bv_page)->swap);
	if (rw == WRITE)
		sio->iocb.ki_complete = sio_write_complete;
	else
		sio->iocb.ki_complete = sio_read_complete;

	iov_iter_bvec(&iter, rw == WRITE ? ITER_SOURCE : ITER_DEST,
			sio->bvec, sio->nr_vecs, sio->len);
	ret = mapping->a_ops->swap_rw(&sio->iocb, &iter);
	if (ret != -EIOCBQUEUED)
		sio->iocb.ki_complete(&sio->iocb, ret);
}

static void swap_fs_submit_write(struct swap_io_ctx *ctx)
{
	swap_fs_submit(ctx, WRITE);
}

static void swap_fs_submit_read(struct swap_io_ctx *ctx)
{
	swap_fs_submit(ctx, READ);
}

static bool swap_fs_can_merge(struct folio *folio, struct folio *prev_folio,
		size_t prev_folio_size)
{
	return swap_dev_pos(folio->swap) ==
		swap_dev_pos(prev_folio->swap) + prev_folio_size;
}

const struct swap_ops swap_fs_ops = {
	.submit_write		= swap_fs_submit_write,
	.submit_read		= swap_fs_submit_read,
	.can_merge		= swap_fs_can_merge,
};

void swap_write_submit(struct swap_io_ctx *ctx)
{
	if (!ctx->sio)
		return;
	ctx->sis->ops->submit_write(ctx);
	ctx->sio = NULL;
	ctx->sis = NULL;
}

void swap_read_submit(struct swap_io_ctx *ctx)
{
	if (!ctx->sio)
		return;
	ctx->sis->ops->submit_read(ctx);
	ctx->sio = NULL;
	ctx->sis = NULL;
}
