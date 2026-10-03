/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _DRIVERS_IOMMU_DMA_PMD_PRIV_H
#define _DRIVERS_IOMMU_DMA_PMD_PRIV_H

#include <linux/dma-mapping.h>
#include <linux/dma-pmd.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/types.h>

struct device;
struct iommu_domain;

/**
 * struct dma_pmd_window - A domain's IOVA window reserved for DMA_PMD pages
 * @base: First IOVA of the window. The leading ARENA_REGION_SIZE bytes
 *        [base, base + ARENA_REGION_SIZE) form ARENA_REGION; a pooled RAM
 *        page at @phys is mapped at @base + ARENA_REGION_SIZE + @phys.
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
#define DMA_PMD_ARENA_PAGES		8192U
#define ARENA_REGION_SIZE		((u64)DMA_PMD_ARENA_PAGES * PMD_SIZE)

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

enum {
	DMA_PMD_POOLED		= BIT(0),
	DMA_PMD_DECRYPTED	= BIT(1),
	DMA_PMD_PINNED		= BIT(2),
};

/**
 * struct dma_pmd_meta - Metadata for a single DMA_PMD page
 * @flags:	Bitmask of DMA_PMD_POOLED, DMA_PMD_DECRYPTED, DMA_PMD_PINNED
 *		(offset 0), read locklessly by dma_is_pmd_page() and dma_is_pmd_direct().
 * @nr_free:	Number of zeroed available blocks in @free_bitmap
 * @nr_dirty:	Number of dirty available blocks in @dirty_bitmap
 * @map_lock:	Spinlock protecting slow-path updates to @domains_mapped
 *
 * Map fast path:
 * @domains_mapped: One bit per domain index: set once this PMD page's leaf
 *		PTE is installed in that domain.
 *
 * Alloc/free fast path, all under pool->lock (or dma_pmd_arena.lock):
 * @pool:	Owning dma_pmd_pool (holds a kref on the pool while pooled)
 * @arena_page:	Allocated 2MB struct page for an ARENA_REGION entry
 * @list:	Node in pool->partial, pool->idle or pool->full
 *
 * Slow path only (disjoint lifetimes):
 * @llnode:	Node in lockless dma_pmd_free_list for async buddy release
 * @rcu:	RCU head used to defer buddy release past lockless readers
 *
 * @free_bitmap: Bitmap of zeroed available block indices (up to 1 << PMD_ORDER)
 *		 for pools, or allocated 4KB blocks for ARENA_REGION entries
 * @dirty_bitmap: Bitmap of dirty available block indices for pools
 *
 * Lives in the sparse per-PMD-frame array @dma_pmd_meta_array indexed by
 * (pfn >> PMD_ORDER), preceded by DMA_PMD_ARENA_PAGES entries for
 * ARENA_REGION. Only chunks covering valid RAM and ARENA_REGION are backed by
 * physical pages (128 KB per GB of RAM).
 */
struct dma_pmd_meta {
	u8 flags;
	u16 nr_free;
	u16 nr_dirty;
	spinlock_t map_lock;

	unsigned long domains_mapped;

	union {
		struct dma_pmd_pool *pool;
		struct page *arena_page;
	};
	struct list_head list;

	union {
		struct llist_node llnode;
		struct rcu_head rcu;
	};

	DECLARE_BITMAP(free_bitmap, 1U << PMD_ORDER) ____cacheline_aligned;
	DECLARE_BITMAP(dirty_bitmap, 1U << PMD_ORDER) ____cacheline_aligned;
} __aligned(DMA_PMD_META_SIZE);

static_assert(offsetof(struct dma_pmd_meta, flags) == 0);
static_assert(sizeof(struct dma_pmd_meta) == DMA_PMD_META_SIZE);

static inline unsigned int dma_pmd_meta_avail(const struct dma_pmd_meta *m)
{
	return m->nr_free + m->nr_dirty;
}

/**
 * struct dma_pmd_pool - Pool managing PMD-backed blocks of a fixed order
 *
 * @lock:	Spinlock protecting @partial, @idle, @full, @num_idle_pages,
 *		the block bitmaps and the u64 statistics counters. The
 *		atomic64_t counters below are deliberately outside it.
 * @order:	Block order managed by this pool (<= PMD_ORDER)
 * @destroyed:	Set when dma_pmd_pool_destroy() has been called
 * @partial:	List of partially used PMD pages with @nr_free > 0 (clean-only
 *		at head, mixed clean/dirty at tail)
 * @partial_dirty: List of partially used PMD pages with @nr_free == 0 and
 *		@nr_dirty > 0
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
 * @block_scrub_cnt: Statistics counter of dirty blocks zeroed by scrubber
 * @numa_mismatch_cnt: Block allocations satisfied from a non-target NUMA node
 * @next_alloc_attempt: Do not attempt a new order-9 allocation before this time.
 *		Damps repeated high-order GFP_ATOMIC failures under
 *		fragmentation, which would otherwise be retried on every
 *		pool miss from softirq context.
 * @refcount:	Reference count; held by pool creator and each active PMD page
 * @node:	Node in the global dma_pmd_pools list
 * @dead_node:	Node in dma_pmd_dead_pools once @refcount reaches 0
 * @fail_backoff: PMD page acquisitions skipped because @next_alloc_attempt was set
 * @fail_budget: PMD page acquisitions refused by global dma_pmd_max_pages
 * @fail_nomem: PMD page acquisitions that ran out of memory for the order-9
 *		allocation. Silent otherwise: the request carries __GFP_NOWARN
 *		so the page allocator says nothing.
 * @fail_split: split_page_compound() rejections
 * @block_alloc_fail: PMD page acquisitions that failed, making
 *		dma_pmd_pool_alloc() return NULL so the caller had to fall
 *		back to a plain page
 * @fail_gfp:	dma_pmd_pool_alloc() calls declined due to unsupported GFP
 *		flags (__GFP_DMA, __GFP_DMA32, __GFP_THISNODE, __GFP_ACCOUNT)
 * @pmd_map_cnt: PMD page mappings established across all domains
 * @fallback_nowindow: Buffers mapped individually because the domain has no
 *		usable IOVA window - no free range large enough, no free
 *		domain index, or a window the device cannot address
 * @fallback_nopool: Buffers mapped individually because the domain can never
 *		pool: direct isolation, or no PMD page size
 * @fallback_maperr: Buffers mapped individually because the PMD page mapping
 *		itself failed
 * @domain_forget_cnt: Cached mappings dropped by dma_pmd_domain_release()
 */
struct dma_pmd_pool {
	/* First cacheline: hot fields touched on block alloc and free. */
	spinlock_t		lock;
	u8			order;
	bool			destroyed;
	struct list_head	partial;
	struct list_head	partial_dirty;
	struct list_head	idle;
	struct list_head	full;
	unsigned int		num_idle_pages;
	unsigned int		max_idle_pages;

	/* Statistics, all updated under @lock. */
	u64			pmd_alloc_cnt;
	u64			pmd_free_cnt;
	u64			block_alloc_cnt;
	u64			block_free_cnt;
	u64			block_scrub_cnt;
	u64			numa_mismatch_cnt;

	/* Cold. */
	unsigned long		next_alloc_attempt;
	struct kref		refcount;
	struct list_head	node;
	struct llist_node	dead_node;

	/*
	 * Statistics updated without @lock. dma_pmd_add_page() runs outside
	 * it, and the map slow path holds @meta->map_lock - taking @lock there
	 * would invert the order against dma_pmd_domain_release(), which
	 * walks @lock then map_lock.
	 */
	atomic64_t		fail_backoff;
	atomic64_t		fail_budget;
	atomic64_t		fail_nomem;
	atomic64_t		fail_split;
	atomic64_t		block_alloc_fail;
	atomic64_t		fail_gfp;
	atomic64_t		pmd_map_cnt;
	atomic64_t		fallback_nowindow;
	atomic64_t		fallback_nopool;
	atomic64_t		fallback_maperr;
	atomic64_t		domain_forget_cnt;
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

/*
 * Highest PFN covered by the allocated @dma_pmd_meta_array reservation.
 * Every domain's IOVA window is sized up to this bound.
 */
static inline unsigned long dma_pmd_top_pfn(void)
{
	return dma_pmd_meta_nframes << PMD_ORDER;
}

static inline struct dma_pmd_meta *dma_pmd_arena_meta(unsigned int slot)
{
	return (dma_pmd_meta_base() - DMA_PMD_ARENA_PAGES) + slot;
}

/* IOVA of @phys in @win. Only valid once the PMD page's PTE is installed. */
static inline dma_addr_t dma_pmd_window_iova(const struct dma_pmd_window *win,
					     phys_addr_t phys)
{
	return win->base + ARENA_REGION_SIZE + phys;
}

/* Offset of @m (either arena or RAM) from the start of a domain's window. */
static inline dma_addr_t dma_pmd_meta_win_offset(const struct dma_pmd_meta *m)
{
	return (dma_addr_t)(m - dma_pmd_arena_meta(0)) << PMD_SHIFT;
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
int dma_pmd_window_assign(struct device *dev, struct iommu_domain *domain,
			  struct dma_pmd_window *win);
bool dma_pmd_domain_active(unsigned int idx, const struct iommu_domain *domain);

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

dma_addr_t dma_pmd_dma_map_phys(struct device *dev, struct iommu_domain *domain,
				struct dma_pmd_window *win, phys_addr_t phys,
				size_t size, int prot, u64 dma_mask);

void dma_pmd_domain_release(struct iommu_domain *domain);

void *dma_pmd_arena_alloc(struct device *dev, size_t size, dma_addr_t *dma, int node);

#else /* !CONFIG_DMA_PMD */

static inline bool dma_is_pmd_phys(phys_addr_t phys)
{
	return false;
}

static inline bool dma_pmd_window_owns(const struct dma_pmd_window *win, dma_addr_t dma)
{
	return false;
}

static inline dma_addr_t dma_pmd_dma_map_phys(struct device *dev,
					      struct iommu_domain *domain,
					      struct dma_pmd_window *win,
					      phys_addr_t phys, size_t size,
					      int prot, u64 dma_mask)
{
	return DMA_MAPPING_ERROR;
}

static inline void dma_pmd_domain_release(struct iommu_domain *domain)
{
}

#endif /* CONFIG_DMA_PMD */
#endif /* _DRIVERS_IOMMU_DMA_PMD_PRIV_H */
