/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _DRIVERS_IOMMU_DMA_PMD_PRIV_H
#define _DRIVERS_IOMMU_DMA_PMD_PRIV_H

#include <linux/dma-pmd.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/types.h>

struct iommu_domain;

/**
 * struct dma_pmd_window - A domain's IOVA window reserved for DMA_PMD pages
 * @base: First IOVA of the window. A page at @phys is mapped, in every domain
 *        that has a window, at @base + @phys.
 * @size: Window size in bytes, or 0 if this domain has no window, either
 *        because nothing has pooled through it yet, or because it could not
 *        find a free range that large. 0 must make both the map and the unmap
 *        side decline, see dma_pmd_window_owns().
 * @domain_idx: Dense domain index + DMA_PMD_IDX_FIRST, used for per-PMD-page
 *       presence (bit @domain_idx - DMA_PMD_IDX_FIRST), or one of
 *       DMA_PMD_IDX_NONE, DMA_PMD_IDX_NOHUGE or DMA_PMD_IDX_NOSPACE while
 *       @size is 0. Of the three sentinels, only DMA_PMD_IDX_NONE is retried.
 *
 * Lives in the domain's iommu_dma_cookie. The window is a real allocation out
 * of the domain's iova_domain, so no ordinary IOVA can ever fall inside it,
 * which is what lets the unmap path recognise a pooled IOVA by range alone.
 */
struct dma_pmd_window {
	dma_addr_t base;
	u64 size;
	u16 domain_idx;
};

#ifdef CONFIG_DMA_PMD

#define DMA_PMD_BLOCKS(order)		(1U << (PMD_ORDER - (order)))

/*
 * Number of IOMMU domains that may use DMA_PMD at once.
 *
 * Each such domain is given a dense index, which is the bit position it uses
 * in dma_pmd_meta.domains_mapped. One word per PMD page records every domain
 * that has mapped this PMD page. Used to speed up map and unmap.
 */
#define DMA_PMD_MAX_DOMAINS		BITS_PER_LONG

/*
 * Sentinel values of dma_pmd_window.domain_idx below DMA_PMD_IDX_FIRST:
 * never attempted (0, matching kzalloc), or permanently declined. A real
 * domain index is >= DMA_PMD_IDX_FIRST (subtract DMA_PMD_IDX_FIRST for
 * the bitmap bit position).
 */
enum {
	DMA_PMD_IDX_NONE,	/* not attempted yet */
	DMA_PMD_IDX_NOHUGE,	/* @domain can never pool */
	DMA_PMD_IDX_NOSPACE,	/* no index or no IOVA range */
	DMA_PMD_IDX_FIRST,	/* first valid domain index */
};

/*
 * Locking
 * -------
 * dma_pmd_meta_mutex	Serialises population of dma_pmd_meta_array. Taken
 *			from init and on-demand from dma_pmd_meta_ensure_pfn()
 *			when can_block is true. It is not nested with pool->lock
 *			or meta->map_lock, and sits inside dma_pmd_pools_lock
 *			when dma_pmd_pool_create() calls dma_pmd_meta_init().
 *			The only reverse edge on dma_pmd_pools_lock is the
 *			shrinker's mutex_trylock(), which cannot block.
 *
 * dma_pmd_pools_lock	Mutex over the list of live pools. Outermost of the
 *			three, and it must not be taken from a context that
 *			cannot sleep.
 *
 * pool->lock		IRQ-safe spinlock over one pool's partial/idle/full
 *			lists and its counters. The alloc and free fast paths
 *			take it on its own.
 *
 * meta->map_lock	IRQ-safe spinlock over one PMD frame's domains_mapped
 *			bitmap. Innermost, and never held across anything that
 *			sleeps.
 *
 * The full order is dma_pmd_pools_lock -> pool->lock -> meta->map_lock, as
 * taken by dma_pmd_domain_release().
 *
 * dma_pmd_domains_lock IRQ-safe spinlock over the domain index table and the
 *			per-domain windows. Taken on its own; it is never
 *			nested with dma_pmd_pools_lock in either direction.
 *
 * dma_pmd_srcu	Read side lets the unmap path dereference a domain out
 *			of dma_pmd_domains[] without blocking a concurrent
 *			dma_pmd_domain_release().
 */

/*
 * 256 B per entry (128 KB per GB of RAM). Verified by static_assert().
 */
#define DMA_PMD_META_SHIFT		8
#define DMA_PMD_META_SIZE		BIT(DMA_PMD_META_SHIFT)

/**
 * struct dma_pmd_meta - Metadata for a single DMA_PMD page
 * @pooled:	True while this page is owned by an dma_pmd_pool (offset 0)
 *		read locklessly by dma_is_pmd_page().
 * @nr_free:	Number of zeroed available blocks in @free_bitmap
 * @nr_dirty:	Number of dirty available blocks in @dirty_bitmap
 * @map_lock:	Spinlock protecting slow-path updates to @domains_mapped
 *
 * Map fast path:
 * @domains_mapped: One bit per domain index: set once this PMD page's leaf
 *		PTE is installed in that domain.
 *
 * Alloc/free fast path, all under pool->lock:
 * @pool:	Owning dma_pmd_pool (holds a kref on the pool while pooled)
 * @list:	Node in pool->partial, pool->idle or pool->full
 *
 * Slow path only (disjoint lifetimes):
 * @llnode:	Node in lockless dma_pmd_free_list for async buddy release
 * @rcu:	RCU head used to defer buddy release past lockless readers
 *
 * @free_bitmap: Bitmap of zeroed available block indices (up to 1 << PMD_ORDER)
 * @dirty_bitmap: Bitmap of dirty available block indices
 *
 * Lives in the sparse per-PMD-frame array @dma_pmd_meta_array indexed by
 * (pfn >> PMD_ORDER). Only chunks covering valid RAM are backed by physical
 * pages (128 KB per GB of RAM).
 */
struct dma_pmd_meta {
	bool pooled;
	u16 nr_free;
	u16 nr_dirty;
	spinlock_t map_lock;

	unsigned long domains_mapped;

	struct dma_pmd_pool *pool;
	struct list_head list;

	union {
		struct llist_node llnode;
		struct rcu_head rcu;
	};

	DECLARE_BITMAP(free_bitmap, 1U << PMD_ORDER) ____cacheline_aligned;
	DECLARE_BITMAP(dirty_bitmap, 1U << PMD_ORDER) ____cacheline_aligned;
} __aligned(DMA_PMD_META_SIZE);

static_assert(offsetof(struct dma_pmd_meta, pooled) == 0);
static_assert(sizeof(struct dma_pmd_meta) == DMA_PMD_META_SIZE);

static inline unsigned int dma_pmd_meta_avail(const struct dma_pmd_meta *m)
{
	return m->nr_free + m->nr_dirty;
}

/**
 * struct dma_pmd_pool - Pool managing PMD-backed blocks of a fixed order
 *
 * @lock:	Spinlock protecting @partial, @idle, @full, @num_idle_pages,
 *		block bitmaps and the statistics counters
 * @order:	Block order managed by this pool (<= PMD_ORDER)
 * @destroyed:	Set when dma_pmd_pool_destroy() has been called
 * @partial:	List of PMD pages with at least 1 free block, but not all
 *		of them (MRU ordered). Candidates for allocation.
 * @idle:	List of PMD pages whose every usable block is free. Held
 *		separately because they can be released under pressure.
 * @full:	List of PMD pages with 0 free blocks
 * @num_idle_pages: Length of @idle
 * @max_idle_pages: High watermark of completely idle PMD pages retained in
 *		@idle before triggering asynchronous release to buddy
 * @pmd_alloc_cnt: Statistics counter of PMD pages allocated from buddy
 * @pmd_free_cnt: Statistics counter of PMD pages released back to buddy
 * @block_alloc_cnt: Statistics counter of block allocations satisfied
 * @block_free_cnt: Statistics counter of block frees recycled into pool
 * @next_alloc_attempt: Do not attempt a new order-9 allocation before this time.
 *		Damps repeated high-order GFP_ATOMIC failures under
 *		fragmentation, which would otherwise be retried on every
 *		pool miss from softirq context.
 * @refcount:	Reference count; held by pool creator and each active PMD page
 * @node:	Node in the global dma_pmd_pools list
 * @dead_node:	Node in dma_pmd_dead_pools once @refcount reaches 0
 */
struct dma_pmd_pool {
	/* First cacheline: hot fields touched on block alloc and free. */
	spinlock_t		lock;
	u8			order;
	bool			destroyed;
	struct list_head	partial;
	struct list_head	idle;
	struct list_head	full;
	unsigned int		num_idle_pages;
	unsigned int		max_idle_pages;

	/* Statistics, all updated under @lock. */
	u64			pmd_alloc_cnt;
	u64			pmd_free_cnt;
	u64			block_alloc_cnt;
	u64			block_free_cnt;

	/* Cold. */
	unsigned long		next_alloc_attempt;
	struct kref		refcount;
	struct list_head	node;
	struct llist_node	dead_node;
} ____cacheline_aligned;

extern unsigned long dma_pmd_meta_nframes;
extern struct srcu_struct dma_pmd_srcu;

static inline struct dma_pmd_meta *dma_pmd_meta_base(void)
{
	/*
	 * Acquire pairs with smp_store_release() in dma_pmd_meta_init():
	 * orders both @dma_pmd_meta_nframes and the populated backing pages.
	 */
	return smp_load_acquire(&dma_pmd_meta_array);
}

int dma_pmd_meta_init(void);
bool dma_pmd_meta_ensure_pfn(unsigned long pfn, bool can_block);
struct dma_pmd_meta *dma_pmd_meta_of_pfn(unsigned long pfn);
struct dma_pmd_meta *dma_pmd_meta_from_phys(phys_addr_t pa);
unsigned long dma_pmd_meta_to_pfn(const struct dma_pmd_meta *m);
phys_addr_t dma_pmd_meta_to_phys(const struct dma_pmd_meta *m);

unsigned int dma_pmd_pools_forget_domain(int idx);
void dma_pmd_unmap_all(struct dma_pmd_meta *meta);
unsigned int dma_pmd_forget_domain(struct dma_pmd_meta *meta, int idx);

static inline bool dma_is_pmd_phys(phys_addr_t phys)
{
	return dma_is_pmd_page(phys >> PAGE_SHIFT);
}

/**
 * dma_pmd_window_owns - Was @dma handed out by a DMA_PMD mapping?
 * @win: The unmapping domain's window
 * @dma: IOVA being unmapped
 *
 * Only DMA_PMD pages can be mapped within the reserved IOVA window
 * so a range check suffices.
 * A domain that has no window has @size 0, so the unsigned subtraction
 * underflows to a huge value and the test is always false. That is the same
 * check that stops the map path from using a window the domain does not own.
 */
static inline bool dma_pmd_window_owns(const struct dma_pmd_window *win, dma_addr_t dma)
{
	/* Acquire pairs with the release of @size in dma_pmd_window_assign(). */
	u64 size = smp_load_acquire(&win->size);

	return size && dma - win->base < size;
}

void dma_pmd_domain_release(struct iommu_domain *domain);

#else /* !CONFIG_DMA_PMD */

static inline bool dma_is_pmd_phys(phys_addr_t phys)
{
	return false;
}

static inline bool dma_pmd_window_owns(const struct dma_pmd_window *win, dma_addr_t dma)
{
	return false;
}

static inline void dma_pmd_domain_release(struct iommu_domain *domain)
{
}

#endif /* CONFIG_DMA_PMD */
#endif /* _DRIVERS_IOMMU_DMA_PMD_PRIV_H */
