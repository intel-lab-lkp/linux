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
 *    (dma_pmd_unmap_all(), added in a later commit), clears meta->pooled so
 *    new lockless readers stop entering @meta, and queues
 *    dma_pmd_release_page_rcu() via call_rcu().
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

	spin_lock_irqsave(&pool->lock, flags);
	pool->pmd_free_cnt++;
	spin_unlock_irqrestore(&pool->lock, flags);

	kref_put(&pool->refcount, dma_pmd_pool_free_kref);
}

/**
 * dma_pmd_release_page - Retire a PMD page and schedule its buddy release
 * @meta: PMD page metadata structure (must have all usable blocks idle)
 *
 * Cost: Slow path; clears the membership bit. The blocks themselves are
 *       returned after an RCU grace period.
 * Locking: Must not hold @pool->lock.
 * Frequency: Rare (background reclaim workqueue, pool destruction).
 */
static void dma_pmd_release_page(struct dma_pmd_meta *meta)
{
	/* dma_pmd_unmap_all(meta) will be called here once IOMMU mappings are added. */

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

/**
 * dma_pmd_pool_create - Create a DMA_PMD page pool
 * @order: Block order to dispense, must be <= PMD_ORDER.
 * @max_idle_pages: Maximum number of completely idle PMD pages to keep cached in
 *               the pool before asynchronously releasing excess PMD pages to buddy
 *               (0 uses default of 16 = 32 MB).
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
 * Frees completely idle 2M pages immediately. Device DMA must be quiesced by
 * the caller first, but blocks already handed to the networking stack (e.g.,
 * RX skbs waiting in socket queues or TX skbs in TCP retransmit queues) may
 * still be in flight; those 2M pages stay on @pool->partial / @pool->full and
 * keep @pool alive on @dma_pmd_pools via @pool->refcount until their last
 * blocks free.
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

	if (!pool)
		return NULL;

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

	kref_put(&pool->refcount, dma_pmd_pool_free_kref);
	flush_work(&dma_pmd_reclaim_work);
	return NULL;
}
EXPORT_SYMBOL(dma_pmd_pool_destroy);

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

static int __init dma_pmd_init(void)
{
	/*
	 * Reclaim frees memory, so it must not queue behind arbitrary work on
	 * system_wq when the machine is already short of it. Failure is not
	 * fatal: dma_pmd_schedule_reclaim() falls back to system_wq.
	 */
	dma_pmd_wq = alloc_workqueue("dma_pmd", WQ_MEM_RECLAIM, 0);
	if (!dma_pmd_wq)
		pr_warn("dma_pmd: no reclaim workqueue, falling back to system_wq\n");

	return 0;
}
subsys_initcall(dma_pmd_init);
