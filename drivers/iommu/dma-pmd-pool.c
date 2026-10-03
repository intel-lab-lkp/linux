// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * DMA_PMD streaming page pool, async page reclaim, shrinker, and debugfs.
 *
 * See Documentation/core-api/dma-pmd.rst for the architecture overview.
 */

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/dma-pmd.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/workqueue.h>

#include "dma-pmd-priv.h"

/*
 * DMA_PMD pages whose last block has just been freed and which are over the
 * pool's idle watermark are pushed here locklessly for later release.
 */
static LLIST_HEAD(dma_pmd_free_list);

/* Dedicated WQ_MEM_RECLAIM workqueue, can run without allocations. */
static struct workqueue_struct *dma_pmd_wq __ro_after_init;

/*
 * Running total and global ceiling on DMA_PMD pages pinned across all pools.
 * If zero, it is set at boot at 1/8 of total RAM.
 * Without this the bound is per-pool, so the footprint scales with the number
 * of pools (one per RX queue, two per CPU for sockets) with nothing watching
 * the aggregate.
 */
static atomic_long_t dma_pmd_nr_pages __cacheline_aligned_in_smp;
static unsigned long dma_pmd_max_pages __read_mostly;
core_param(dma_pmd_max_pages, dma_pmd_max_pages, ulong, 0644);

/* All live pools. */
static LIST_HEAD(dma_pmd_pools);
static DEFINE_MUTEX(dma_pmd_pools_lock);
static LLIST_HEAD(dma_pmd_dead_pools);

static void dma_pmd_schedule_reclaim(void);

static void dma_pmd_pool_free_kref(struct kref *kref)
{
	struct dma_pmd_pool *pool = container_of(kref, struct dma_pmd_pool, refcount);

	llist_add(&pool->dead_node, &dma_pmd_dead_pools);
	dma_pmd_schedule_reclaim();
}

/*
 * Releasing an idle PMD page proceeds in three stages:
 *
 * 1. Detach from pool->idle (or when the last block of a destroyed/over-watermark
 *    PMD page frees in __dma_pmd_free_page()). Because __free_pages_prepare()
 *    may run in hardirq/softirq or with allocator locks held, the PMD page is
 *    pushed locklessly onto dma_pmd_free_list and dma_pmd_reclaim_work is
 *    scheduled on dma_pmd_wq.
 * 2. In process context, dma_pmd_release_page() unmaps all IOMMU domains
 *    (dma_pmd_unmap_all()), clears meta->pooled so new lockless readers stop
 *    entering @meta, and queues dma_pmd_release_page_rcu() via call_rcu().
 * 3. After an RCU grace period (once no concurrent dma_pmd_free_page() reader
 *    can still be dereferencing @meta), dma_pmd_release_page_rcu() unfreezes
 *    all order-pool->order blocks (already split by split_page_compound()),
 *    returns them to the buddy allocator, and drops pool->refcount.
 */

/**
 * dma_pmd_release_page_rcu - Return a retired PMD page to the buddy allocator
 * @head: @rcu member of the PMD page being released
 *
 * Runs once no reader can still be resolving a PFN in this PMD page. All blocks
 * are unfrozen and handed back to the buddy allocator, and the pool reference
 * is dropped last.
 */
static void dma_pmd_release_page_rcu(struct rcu_head *head)
{
	/*
	 * Read @meta->pool before returning the blocks to buddy: once the last
	 * block is freed, the PMD frame can be reallocated and @meta overwritten.
	 */
	struct dma_pmd_meta *meta = container_of(head, struct dma_pmd_meta, rcu);
	struct page *page = pfn_to_page(dma_pmd_meta_to_pfn(meta));
	struct dma_pmd_pool *pool = READ_ONCE(meta->pool);
	unsigned int nr = DMA_PMD_BLOCKS(pool->order);
	unsigned int order = pool->order;
	unsigned long flags;
	unsigned int i;

	for (i = 0; i < nr; i++) {
		struct page *block = page + (i << order);

		page_ref_unfreeze(block, 1);
		__free_pages(block, order);
	}

	atomic_long_dec(&dma_pmd_nr_pages);

	spin_lock_irqsave(&pool->lock, flags);
	pool->pmd_free_cnt++;
	spin_unlock_irqrestore(&pool->lock, flags);

	kref_put(&pool->refcount, dma_pmd_pool_free_kref);
}

/**
 * dma_pmd_release_page - Retire a PMD page and schedule its buddy release
 * @meta: PMD page metadata structure (must have all usable blocks idle)
 *
 * Cost: Slow path; unmaps the IOMMU domains and clears the membership bit.
 *       The blocks themselves are returned after an RCU grace period.
 * Locking: Process context (calls iommu_unmap). Must not hold @pool->lock.
 * Frequency: Rare (background reclaim workqueue, shrinker, pool destruction).
 */
static void dma_pmd_release_page(struct dma_pmd_meta *meta)
{
	dma_pmd_unmap_all(meta);

	/*
	 * Clear membership before the grace period. A reader that passed
	 * dma_is_pmd_page() just before this may still be reading @meta, so
	 * the PMD page and its metadata entry must outlive the grace period.
	 */
	WRITE_ONCE(meta->pooled, false);
	call_rcu(&meta->rcu, dma_pmd_release_page_rcu);
}

static void dma_pmd_reclaim_work_fn(struct work_struct *work)
{
	struct llist_node *node = llist_del_all(&dma_pmd_free_list);
	struct dma_pmd_pool *pool, *ptmp;
	struct dma_pmd_meta *meta, *tmp;

	llist_for_each_entry_safe(meta, tmp, node, llnode)
		dma_pmd_release_page(meta);

	node = llist_del_all(&dma_pmd_dead_pools);
	if (node) {
		mutex_lock(&dma_pmd_pools_lock);
		llist_for_each_entry_safe(pool, ptmp, node, dead_node)
			list_del(&pool->node);
		mutex_unlock(&dma_pmd_pools_lock);

		llist_for_each_entry_safe(pool, ptmp, node, dead_node)
			kfree(pool);
	}
}

static DECLARE_WORK(dma_pmd_reclaim_work, dma_pmd_reclaim_work_fn);

static void dma_pmd_schedule_reclaim(void)
{
	struct workqueue_struct *wq = READ_ONCE(dma_pmd_wq);

	if (likely(wq))
		queue_work(wq, &dma_pmd_reclaim_work);
	else
		schedule_work(&dma_pmd_reclaim_work);
}

unsigned int dma_pmd_pools_forget_domain(int idx)
{
	struct dma_pmd_pool *pool;
	struct dma_pmd_meta *meta;
	unsigned int dropped = 0;
	unsigned long flags;

	mutex_lock(&dma_pmd_pools_lock);
	list_for_each_entry(pool, &dma_pmd_pools, node) {
		spin_lock_irqsave(&pool->lock, flags);
		list_for_each_entry(meta, &pool->partial, list)
			dropped += dma_pmd_forget_domain(meta, idx);
		list_for_each_entry(meta, &pool->idle, list)
			dropped += dma_pmd_forget_domain(meta, idx);
		list_for_each_entry(meta, &pool->full, list)
			dropped += dma_pmd_forget_domain(meta, idx);
		spin_unlock_irqrestore(&pool->lock, flags);
	}
	mutex_unlock(&dma_pmd_pools_lock);

	dma_pmd_schedule_reclaim();
	flush_work(&dma_pmd_reclaim_work);
	return dropped;
}

/**
 * dma_pmd_pool_create - Create a DMA_PMD page pool
 * @order: Block order to dispense, must be <= PMD_ORDER.
 * @max_idle_pages: Maximum number of completely idle PMD pages to keep cached in
 *               the pool before asynchronously releasing excess PMD pages to buddy
 *               (0 uses default of 16 = 32 MB). This is a per-pool bound; the
 *               aggregate across all pools is additionally capped by
 *               dma_pmd_max_pages and trimmed by the shrinker.
 *
 * New PMD physical pages are allocated on demand on the NUMA node of the
 * calling CPU (the NAPI CPU for RX queue refill, or the application CPU for TX).
 *
 * Cost: Process-context control plane; allocates pool metadata structure, and
 *       on first call the global membership bitmap.
 * Locking: May sleep (GFP_KERNEL).
 * Frequency: Rare (driver queue initialization or subsystem init).
 *
 * Return: Pointer to the created pool, or NULL on failure.
 */
struct dma_pmd_pool *dma_pmd_pool_create(unsigned int order, unsigned int max_idle_pages)
{
	struct dma_pmd_pool *pool;

	if (order > PMD_ORDER)
		return NULL;

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;

	kref_init(&pool->refcount);
	spin_lock_init(&pool->lock);
	pool->order = order;
	pool->max_idle_pages = max_idle_pages ? : 16;
	pool->next_alloc_attempt = jiffies;
	INIT_LIST_HEAD(&pool->partial);
	INIT_LIST_HEAD(&pool->idle);
	INIT_LIST_HEAD(&pool->full);

	mutex_lock(&dma_pmd_pools_lock);
	if (dma_pmd_meta_init()) {
		mutex_unlock(&dma_pmd_pools_lock);
		kfree(pool);
		return NULL;
	}
	list_add(&pool->node, &dma_pmd_pools);
	mutex_unlock(&dma_pmd_pools_lock);

	return pool;
}
EXPORT_SYMBOL(dma_pmd_pool_create);

/**
 * dma_pmd_pool_destroy - Destroy a DMA_PMD page pool and unmap cached PMD pages
 * @pool: Pool to destroy
 *
 * Unmaps and frees completely idle PMD pages. If any blocks are still in flight
 * (e.g., held by socket queues or SKBs), the pool structure and active pages
 * remain on @dma_pmd_pools via @pool->refcount (visible to dma_pmd_domain_release())
 * and are automatically unmapped and reclaimed as their last blocks free.
 *
 * Cost: Control plane teardown; flushes async reclaim workqueue.
 * Locking: Process context (may sleep in flush_work). Acquires @pool->lock.
 * Frequency: Rare (driver queue teardown or module unload).
 *
 * Return: Always NULL, so callers can write `pool = dma_pmd_pool_destroy(pool)`.
 */
struct dma_pmd_pool *dma_pmd_pool_destroy(struct dma_pmd_pool *pool)
{
	struct dma_pmd_meta *meta, *tmp;
	LIST_HEAD(release_list);
	unsigned long flags;
	int srcu_idx;

	if (!pool)
		return NULL;

	srcu_idx = srcu_read_lock(&dma_pmd_srcu);
	spin_lock_irqsave(&pool->lock, flags);
	pool->destroyed = true;

	/* Take out the idle PMD pages; the unmap happens outside the lock. */
	list_splice_init(&pool->idle, &release_list);
	pool->num_idle_pages = 0;

	spin_unlock_irqrestore(&pool->lock, flags);

	list_for_each_entry_safe(meta, tmp, &release_list, list) {
		list_del(&meta->list);
		dma_pmd_release_page(meta);
	}
	srcu_read_unlock(&dma_pmd_srcu, srcu_idx);

	kref_put(&pool->refcount, dma_pmd_pool_free_kref);
	flush_work(&dma_pmd_reclaim_work);
	return NULL;
}
EXPORT_SYMBOL(dma_pmd_pool_destroy);

/**
 * dma_pmd_add_page - Allocate, split, and register a new PMD page
 * @pool: Owning pool
 * @gfp: GFP allocation flags
 * @nid: Target NUMA node (or NUMA_NO_NODE for local node)
 *
 * Cost: Slow path (pool miss); allocates PMD page from buddy on @nid,
 *       splits into compound blocks, and sets the PMD page's membership bit.
 * Locking: Lockless with respect to @pool->lock (caller inserts into list).
 * Frequency: Rare after warmup (only when the pool holds no PMD page with a
 *            free block).
 *
 * Return: Pointer to new dma_pmd_meta with all usable blocks free, or NULL
 *         on failure.
 */
static struct dma_pmd_meta *dma_pmd_add_page(struct dma_pmd_pool *pool,
					     gfp_t gfp, int nid)
{
	/*
	 * Require full GFP_KERNEL (not just gfpflags_allow_blocking()):
	 * dma_pmd_meta_ensure_pfn() and dma_pmd_page_prepare() use GFP_KERNEL.
	 */
	bool can_block = (gfp & GFP_KERNEL) == GFP_KERNEL;
	unsigned int nr = DMA_PMD_BLOCKS(pool->order);
	struct dma_pmd_meta *meta;
	gfp_t alloc_gfp, base_gfp;
	struct page *page;

	/*
	 * A high-order allocation is expensive when it fails, and under fragmentation
	 * it fails on every pool miss. For GFP_ATOMIC, back off briefly.
	 */
	if (!can_block && time_before(jiffies, READ_ONCE(pool->next_alloc_attempt)))
		return NULL;

	/* Aggregate ceiling across every pool, independent of max_idle_pages. */
	if (atomic_long_inc_return(&dma_pmd_nr_pages) > READ_ONCE(dma_pmd_max_pages)) {
		atomic_long_dec(&dma_pmd_nr_pages);
		if (!can_block)
			WRITE_ONCE(pool->next_alloc_attempt,
				   jiffies + DIV_ROUND_UP(HZ, 100));
		return NULL;
	}

	/*
	 * The following flags are discarded:
	 *
	 * __GFP_MOVABLE would let the PMD page come from ZONE_MOVABLE or a CMA
	 * pageblock. A pooled PMD page is pinned for the life of the pool and
	 * has no movable_operations, so memory hot-remove and CMA allocation
	 * would fail on it forever.
	 *
	 * __GFP_HIGHMEM goes because pooled blocks are handed out as
	 * directly addressable DMA buffers, and __GFP_COMP because the PMD page
	 * is shaped into compound pieces by hand. What is left is also free
	 * of everything slab treats as a bug in GFP_SLAB_BUG_MASK, so the
	 * metadata allocation below can use it as it stands.
	 * __GFP_ZERO is handled per-block in dma_pmd_pool_alloc_bulk_node().
	 */
	base_gfp = gfp & ~(__GFP_COMP | __GFP_HIGHMEM | __GFP_MOVABLE | __GFP_ZERO);

	/*
	 * A caller that can block gets __GFP_RETRY_MAYFAIL so the allocation
	 * is allowed to compact for an order-9 page, which is the difference
	 * between a pool that is populated before traffic starts and one that
	 * never populates at all on a fragmented machine. It still fails
	 * rather than triggering the OOM killer: this is an optimisation, and
	 * every caller has a working fallback.
	 */
	alloc_gfp = base_gfp | __GFP_NOWARN;
	alloc_gfp |= can_block ? __GFP_RETRY_MAYFAIL : __GFP_NORETRY;

	page = alloc_pages_node(nid, alloc_gfp, PMD_ORDER);
	if (!page) {
		atomic_long_dec(&dma_pmd_nr_pages);
		/* Retry backoff; could be made shorter or tunable in future. */
		if (!can_block)
			WRITE_ONCE(pool->next_alloc_attempt,
				   jiffies + DIV_ROUND_UP(HZ, 100));
		return NULL;
	}

	/*
	 * Ensure the metadata chunk for this PFN is populated. Fails if the PFN
	 * is past the reservation, or if it belongs to hotplugged memory and the
	 * caller cannot block to populate its metadata page.
	 */
	if (unlikely(!dma_pmd_meta_ensure_pfn(page_to_pfn(page), can_block))) {
		__free_pages(page, PMD_ORDER);
		atomic_long_dec(&dma_pmd_nr_pages);
		return NULL;
	}

	/* Fails with -EBUSY if a concurrent PFN walker holds a speculative ref. */
	if (split_page_compound(page, PMD_ORDER, pool->order)) {
		__free_pages(page, PMD_ORDER);
		atomic_long_dec(&dma_pmd_nr_pages);
		return NULL;
	}

	meta = dma_pmd_meta_of_pfn(page_to_pfn(page));
	memset(meta, 0, sizeof(*meta));
	meta->pool = pool;
	spin_lock_init(&meta->map_lock);

	bitmap_set(meta->dirty_bitmap, 0, nr);
	meta->nr_dirty = nr;

	kref_get(&pool->refcount);

	/*
	 * Publish last. Once the bit is set, __free_pages_prepare() on any CPU
	 * will route this PMD page's blocks back into the pool, so every @meta
	 * field a reader consults has to be in place first.
	 *
	 * No explicit barrier: a block cannot reach any reader until this
	 * function returns and the caller publishes the PMD page under
	 * @pool->lock, whose release orders both stores against every consumer.
	 */
	WRITE_ONCE(meta->pooled, true);

	return meta;
}

static unsigned int dma_pmd_scan_bitmap(struct dma_pmd_pool *pool,
					unsigned long *bitmap, u16 *countp,
					unsigned long base_pfn, unsigned int nr,
					unsigned long want, struct page **out,
					bool unfreeze)
{
	unsigned int idx, got = 0, used = 0;

	if (!*countp || !want)
		return 0;

	for_each_set_bit(idx, bitmap, nr) {
		struct page *block = pfn_to_page(base_pfn + (idx << pool->order));

		__clear_bit(idx, bitmap);
		used++;
		if (unlikely(folio_contain_hwpoisoned_page(page_folio(block)))) {
			pr_err_once("dma_pmd: poisoned block at pfn %lu, leaking it to keep pool %p intact\n",
				    page_to_pfn(block), pool);
		} else {
			if (unfreeze)
				page_ref_unfreeze(block, 1);
			out[got++] = block;
		}

		if (used == *countp || got == want)
			break;
	}
	*countp -= used;
	pool->block_alloc_cnt += used;
	return got;
}

/* Extract up to @want available blocks from @meta under @pool->lock. */
static unsigned long dma_pmd_take_blocks(struct dma_pmd_pool *pool,
					 struct dma_pmd_meta *meta,
					 unsigned long want,
					 struct page **out, bool zero)
{
	unsigned long base_pfn = dma_pmd_meta_to_pfn(meta);
	unsigned int avail_before = dma_pmd_meta_avail(meta);
	unsigned int nr = DMA_PMD_BLOCKS(pool->order);
	unsigned int got = 0, used;

	if (zero) {
		got += dma_pmd_scan_bitmap(pool, meta->free_bitmap, &meta->nr_free,
					   base_pfn, nr, want, out, true);
		got += dma_pmd_scan_bitmap(pool, meta->dirty_bitmap, &meta->nr_dirty,
					   base_pfn, nr, want - got, out + got, false);
	} else {
		got += dma_pmd_scan_bitmap(pool, meta->dirty_bitmap, &meta->nr_dirty,
					   base_pfn, nr, want, out, false);
		got += dma_pmd_scan_bitmap(pool, meta->free_bitmap, &meta->nr_free,
					   base_pfn, nr, want - got, out + got, true);
	}
	used = avail_before - dma_pmd_meta_avail(meta);
	if (!dma_pmd_meta_avail(meta) || WARN_ON_ONCE(!used)) {
		meta->nr_free = 0;
		meta->nr_dirty = 0;
		list_move(&meta->list, &pool->full);
	}

	return got;
}

/**
 * dma_pmd_pool_alloc_bulk_node - Allocate up to @nr_pages blocks from @pool
 * @pool: Pool to allocate from
 * @gfp: GFP flags used if a new PMD page must be allocated from buddy
 * @nid: Target NUMA node if a new PMD page is allocated (or NUMA_NO_NODE)
 * @nr_pages: Number of blocks requested
 * @page_array: Output array of at least @nr_pages entries
 *
 * Cost: Fast path is O(nr_pages) bitmap scan from @pool->partial or @pool->idle
 *       under a single lock acquisition. Slow path (both empty) calls
 *       dma_pmd_add_page().
 * Locking: Acquires @pool->lock (irqsave) for the bitmap scan. Drops lock
 *          during slow-path PMD buddy allocation and per-block initialization.
 * Frequency: High (called per RX buffer refill or per SKB TX page frag refill).
 *
 * A pool hands out a block of a PMD page it already owns, on the node that
 * PMD page came from, so it cannot satisfy a hard placement constraint:
 * requests with __GFP_THISNODE, __GFP_DMA or __GFP_DMA32 are declined, as is
 * __GFP_ACCOUNT (pooled blocks are shared across callers and cannot be charged
 * to a single memcg), so the caller falls back to the page allocator. Flags
 * that only widen what is acceptable, such as __GFP_HIGHMEM, are ignored
 * harmlessly.
 *
 * Return: Number of blocks placed in @page_array (0 on failure).
 */
unsigned long dma_pmd_pool_alloc_bulk_node(struct dma_pmd_pool *pool, gfp_t gfp,
					   int nid, unsigned long nr_pages,
					   struct page **page_array)
{
	unsigned long allocated = 0, flags, i;
	bool zero;

	if (unlikely(!pool || pool->destroyed || !nr_pages ||
		     (gfp & (__GFP_DMA | __GFP_DMA32 | __GFP_THISNODE |
			     __GFP_ACCOUNT))))
		return 0;

	zero = want_init_on_alloc(gfp);

	spin_lock_irqsave(&pool->lock, flags);
	while (allocated < nr_pages) {
		struct dma_pmd_meta *meta;

		/* Partially used pages first, idle ones next. */
		meta = list_first_entry_or_null(&pool->partial, struct dma_pmd_meta, list);
		if (!meta && !list_empty(&pool->idle)) {
			meta = list_first_entry(&pool->idle, struct dma_pmd_meta, list);
			list_move(&meta->list, &pool->partial);
			pool->num_idle_pages--;
		}
		if (!meta) {
			/* Fallback to a new allocation */
			spin_unlock_irqrestore(&pool->lock, flags);
			meta = dma_pmd_add_page(pool, gfp, nid);
			if (!meta)
				goto out_prep;
			spin_lock_irqsave(&pool->lock, flags);
			pool->pmd_alloc_cnt++;
			list_add(&meta->list, &pool->partial);
		}

		allocated += dma_pmd_take_blocks(pool, meta,
						 nr_pages - allocated,
						 &page_array[allocated], zero);
	}
	spin_unlock_irqrestore(&pool->lock, flags);

out_prep:
	for (i = 0; i < allocated; i++) {
		if (!page_count(page_array[i])) {
			page_ref_unfreeze(page_array[i], 1);
			if (zero)
				memset(page_address(page_array[i]), 0, PAGE_SIZE << pool->order);
		}
	}
	return allocated;
}
EXPORT_SYMBOL(dma_pmd_pool_alloc_bulk_node);

/**
 * dma_pmd_pool_has_free - Can @pool hand out a block without a new PMD page?
 * @pool: Pool to query
 *
 * Cost: O(1) lockless list check.
 * Locking: None. The answer is advisory and can be stale the moment it is
 *          returned.
 * Frequency: High; intended for per-buffer decisions on the RX path.
 *
 * For callers choosing between an existing non-pooled buffer and a pooled
 * replacement. Answering "no" when the pool is empty keeps them from trading a
 * perfectly reusable page for a failed allocation and another singly-mapped
 * page, which is strictly worse than leaving it alone.
 *
 * Return: true if a PMD page with a free block is present.
 */
bool dma_pmd_pool_has_free(struct dma_pmd_pool *pool)
{
	return pool && !READ_ONCE(pool->destroyed) &&
	       (!list_empty_careful(&pool->partial) || !list_empty_careful(&pool->idle));
}
EXPORT_SYMBOL(dma_pmd_pool_has_free);

/**
 * __dma_pmd_free_page - Recycle a freed block back into its DMA_PMD pool
 * @page: Block being freed
 *
 * Called from __free_pages_prepare() when a block's refcount reaches 0.
 * Marks the block index available in @meta->dirty_bitmap[] (or @meta->free_bitmap[]
 * when init_on_free zeroes it). If the PMD page becomes completely idle and the
 * pool exceeds @max_idle_pages, queues the PMD page to dma_pmd_free_list for
 * asynchronous unmapping and buddy release.
 *
 * Cost: O(1) bit set under spinlock (~15-20 cycles).
 * Locking: Runs inside the rcu_read_lock() section opened by
 *          dma_pmd_free_page(), and acquires @pool->lock (irqsave).
 *          Safe from any context (IRQ, softirq, process). Never sleeps.
 * Frequency: Very high (called on every kfree_skb / napi_consume_skb / put_page
 *            for buffers allocated from an dma_pmd_pool).
 *
 * Return: true if the page belonged to a DMA_PMD pool and was recycled, false
 *         if it is a normal system page that should be freed by the buddy
 *         allocator.
 */
bool __dma_pmd_free_page(struct page *page)
{
	struct dma_pmd_meta *meta = dma_pmd_meta_of_pfn(page_to_pfn(page));
	unsigned int nr = DMA_PMD_BLOCKS(meta->pool->order), idx;
	struct dma_pmd_pool *pool = meta->pool;
	unsigned long pfn = page_to_pfn(page);
	bool should_release = false;
	bool zeroed_on_free = false;
	unsigned int avail;
	unsigned long flags;

	idx = (pfn - dma_pmd_meta_to_pfn(meta)) >> pool->order;

	/*
	 * Never reuse or return a hwpoisoned block to buddy: leave its bit
	 * clear in free_bitmap/dirty_bitmap so the PMD page stays pinned and isolated.
	 */
	if (unlikely(folio_contain_hwpoisoned_page(page_folio(page)))) {
		pr_err_once("dma_pmd: poisoned block at pfn %lu, leaking it to keep pool %p intact\n",
			    pfn, pool);
		return true;
	}

	/*
	 * Reject double frees (bit already set in free_bitmap or dirty_bitmap),
	 * out-of-range indices, or mismatched orders: leak the block rather than
	 * corrupt the pool or hand a live PMD page's block to the buddy allocator.
	 */
	if (WARN_ON_ONCE(idx >= nr || compound_order(page) != pool->order ||
			 test_bit(idx, meta->free_bitmap) ||
			 test_bit(idx, meta->dirty_bitmap)))
		return true;

	/*
	 * Pooled blocks stay allocated from buddy until dma_pmd_release_page_rcu()
	 * (which runs KASAN/KMSAN/pgalloc_tag_sub via __free_pages()), and never
	 * come from HIGHMEM (CONFIG_DMA_PMD is 64-bit only).
	 */
	if (want_init_on_free()) {
		memset(page_address(page), 0, PAGE_SIZE << pool->order);
		zeroed_on_free = true;
	}

	spin_lock_irqsave(&pool->lock, flags);

	if (WARN_ON_ONCE(test_bit(idx, meta->free_bitmap) ||
			 test_bit(idx, meta->dirty_bitmap))) {
		spin_unlock_irqrestore(&pool->lock, flags);
		return true;
	}

	if (zeroed_on_free) {
		__set_bit(idx, meta->free_bitmap);
		meta->nr_free++;
	} else {
		__set_bit(idx, meta->dirty_bitmap);
		meta->nr_dirty++;
	}
	avail = dma_pmd_meta_avail(meta);
	pool->block_free_cnt++;

	if (avail == 1 && !pool->destroyed)
		list_move(&meta->list, &pool->partial);

	if (avail == nr) {
		if (pool->destroyed || pool->num_idle_pages >= pool->max_idle_pages) {
			list_del_init(&meta->list);
			llist_add(&meta->llnode, &dma_pmd_free_list);
			should_release = true;
		} else {
			list_move(&meta->list, &pool->idle);
			pool->num_idle_pages++;
		}
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	if (should_release)
		dma_pmd_schedule_reclaim();

	return true;
}
EXPORT_SYMBOL(__dma_pmd_free_page);

/*
 * Idle PMD pages are retained per pool up to max_idle_pages, which is a throughput
 * knob and deliberately does not react to memory pressure. The shrinker is
 * what makes that safe: under reclaim every completely idle PMD page in every
 * pool becomes available, so the pools give the memory back instead of
 * pinning their high-water mark for the lifetime of the machine.
 *
 * Both callbacks use mutex_trylock() rather than mutex_lock(): a thread that
 * already holds dma_pmd_pools_lock can enter direct reclaim from an
 * allocation it makes under that lock, and reclaim calls straight back in
 * here. Giving up is always correct, since there is nothing this shrinker
 * could free that the holder is not already about to account for.
 */
static unsigned long dma_pmd_shrink_count(struct shrinker *shrink,
					  struct shrink_control *sc)
{
	struct dma_pmd_pool *pool;
	unsigned long nr = 0;

	if (!mutex_trylock(&dma_pmd_pools_lock))
		return 0;

	list_for_each_entry(pool, &dma_pmd_pools, node)
		nr += READ_ONCE(pool->num_idle_pages);

	mutex_unlock(&dma_pmd_pools_lock);

	return nr << PMD_ORDER;
}

static unsigned long dma_pmd_shrink_scan(struct shrinker *shrink,
					 struct shrink_control *sc)
{
	struct dma_pmd_meta *meta, *tmp;
	struct dma_pmd_pool *pool;
	unsigned long freed = 0;
	LIST_HEAD(release_list);
	unsigned long flags;
	int srcu_idx;

	if (!mutex_trylock(&dma_pmd_pools_lock))
		return SHRINK_STOP;

	srcu_idx = srcu_read_lock(&dma_pmd_srcu);
	list_for_each_entry(pool, &dma_pmd_pools, node) {
		/*
		 * Every PMD page on @idle is a candidate, so this detaches one
		 * per iteration and stops as soon as the reclaim path has
		 * what it asked for: the IRQs-off section is bounded by the
		 * work done, not by how many PMD pages the pool holds.
		 */
		spin_lock_irqsave(&pool->lock, flags);
		list_for_each_entry_safe(meta, tmp, &pool->idle, list) {
			if (freed >= sc->nr_to_scan)
				break;
			list_move(&meta->list, &release_list);
			pool->num_idle_pages--;
			freed += 1UL << PMD_ORDER;
		}
		spin_unlock_irqrestore(&pool->lock, flags);

		if (freed >= sc->nr_to_scan)
			break;
	}

	mutex_unlock(&dma_pmd_pools_lock);

	/*
	 * Retiring is done outside both locks: dma_pmd_release_page() unmaps
	 * from the IOMMU, which is far too long to hold pool->lock for.
	 */
	list_for_each_entry_safe(meta, tmp, &release_list, list) {
		list_del(&meta->list);
		dma_pmd_release_page(meta);
	}
	srcu_read_unlock(&dma_pmd_srcu, srcu_idx);

	/*
	 * Both counts are in pages, matching dma_pmd_shrink_count(). A
	 * 2M page is 512 of them and cannot be freed in parts, so a
	 * SHRINK_BATCH sized request always overshoots. Report what was
	 * really reclaimed so that do_shrink_slab() charges its budget for
	 * the whole PMD page instead of the 128 pages it asked for, and does
	 * not come back four times over for the same pressure.
	 *
	 * Zero scanned pages would leave that budget untouched and spin,
	 * so a scan that reclaimed nothing has to say SHRINK_STOP.
	 */
	sc->nr_scanned = freed;

	return freed ?: SHRINK_STOP;
}

static int __init dma_pmd_init(void)
{
	struct shrinker *shrink;

	/*
	 * Hard ceiling on memory diverted from the buddy allocator into 2MB
	 * pools, in PMD pages. One eighth of RAM is far above what the per-pool
	 * watermarks should ever reach; it exists to bound a pathological
	 * configuration (many queues, many CPUs, all pools at their high
	 * water mark) rather than to be hit in normal operation. The floor
	 * keeps small machines usable. Zero here means the dma_pmd_max_pages=
	 * boot parameter did not override it.
	 *
	 * Lowering the cap later only stops pools from growing; the PMD pages
	 * already held are returned by the shrinker as they fall idle.
	 */
	if (!dma_pmd_max_pages)
		dma_pmd_max_pages = max(totalram_pages() >> (PMD_ORDER + 3), 16UL);

	/*
	 * Reclaim frees memory, so it must not queue behind arbitrary work on
	 * system_wq when the machine is already short of it. Failure is not
	 * fatal: dma_pmd_schedule_reclaim() falls back to system_wq.
	 */
	dma_pmd_wq = alloc_workqueue("dma_pmd", WQ_MEM_RECLAIM, 0);
	if (!dma_pmd_wq)
		pr_warn("dma_pmd: no reclaim workqueue, falling back to system_wq\n");

	shrink = shrinker_alloc(0, "dma_pmd");
	if (!shrink) {
		pr_warn("dma_pmd: shrinker registration failed\n");
		return 0;
	}

	shrink->count_objects = dma_pmd_shrink_count;
	shrink->scan_objects = dma_pmd_shrink_scan;
	shrink->seeks = DEFAULT_SEEKS;
	shrinker_register(shrink);

	return 0;
}
subsys_initcall(dma_pmd_init);
