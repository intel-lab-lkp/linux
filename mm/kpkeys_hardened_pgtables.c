// SPDX-License-Identifier: GPL-2.0-only
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/highmem.h>
#include <linux/kpkeys.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/set_memory.h>

__ro_after_init DEFINE_STATIC_KEY_FALSE(kpkeys_hardened_pgtables_key);

static int set_pkey_pgtable(struct page *page, unsigned int nr_pages)
{
	unsigned long addr = (unsigned long)page_address(page);
	int ret;

	ret = set_memory_pkey(addr, nr_pages, KPKEYS_PKEY_PGTABLES);

	WARN_ON(ret);
	return ret;
}

static int set_pkey_default(struct page *page, unsigned int nr_pages)
{
	unsigned long addr = (unsigned long)page_address(page);
	int ret;

	ret = set_memory_pkey(addr, nr_pages, KPKEYS_PKEY_DEFAULT);

	WARN_ON(ret);
	return ret;
}

/* pkeys block allocator (PBA) - implemented below */
static bool pba_enabled(void);
static struct page *pba_pgtable_alloc(gfp_t gfp);
static void pba_pgtable_free(struct page *page);
static int pba_prepare_direct_map_split(void);
static bool pba_ready_for_direct_map_split(void);
static void pba_init(void);
static void pba_init_late(void);

/* Trivial allocator in case the linear map is PTE-mapped (no block mapping) */
static struct page *noblock_pgtable_alloc(gfp_t gfp)
{
	struct page *page;
	int ret;

	page = alloc_pages_noprof(gfp, 0);
	if (!page)
		return page;

	ret = set_pkey_pgtable(page, 1);
	if (ret) {
		__free_page(page);
		return NULL;
	}

	return page;
}

static void noblock_pgtable_free(struct page *page)
{
	set_pkey_default(page, 1);
	__free_page(page);
}

/* Public interface */
struct page *kpkeys_pgtable_alloc(gfp_t gfp)
{
	if (pba_enabled())
		return pba_pgtable_alloc(gfp);
	else
		return noblock_pgtable_alloc(gfp);
}

void kpkeys_pgtable_free(struct page *page)
{
	if (pba_enabled())
		pba_pgtable_free(page);
	else
		noblock_pgtable_free(page);
}

int kpkeys_prepare_direct_map_split(void)
{
	if (pba_enabled())
		return pba_prepare_direct_map_split();

	return 0;
}

bool kpkeys_ready_for_direct_map_split(void)
{
	if (pba_enabled())
		return pba_ready_for_direct_map_split();

	return true;
}

void __init kpkeys_hardened_pgtables_init(void)
{
	if (!arch_kpkeys_enabled())
		return;

	pba_init();
	static_branch_enable(&kpkeys_hardened_pgtables_key);
}

void __init kpkeys_hardened_pgtables_init_late(void)
{
	if (!arch_kpkeys_enabled())
		return;

	if (pba_enabled())
		pba_init_late();
}

/*
 * pkeys block allocator (PBA): dedicated page table allocator for block-mapped
 * linear map. Block splitting is minimised by prioritising the allocation and
 * freeing of full blocks.
 */
#define PBA_GFP_ALLOC		GFP_KERNEL
#define PBA_GFP_OPT_MASK	(__GFP_ZERO | __GFP_ACCOUNT | __GFP_PGTABLE_SPLIT)

/*
 * Pages need to be reserved for splitting the linear map; __GFP_PGTABLE_SPLIT
 * must be passed to access these pages. 4 pages are reserved:
 *
 * - 2 in case a PMD and/or PTE page needs to be allocated if set_memory_pkey()
 *   splits the linear map while refilling our own page cache (see
 *   __refill_pages()). These 2 pages must always be available as we cannot
 *   refill recursively. They are protected by alloc_mutex and are guaranteed to
 *   be replenished when refilling is complete and we release the mutex.
 *
 * - 2 for splitting the linear map for any other purpose (e.g. calling
 *   set_memory_pkey() or set_memory_ro() on an arbitrary range). These pages
 *   are replenished before the split is attempted, see
 *   kpkeys_prepare_direct_map_split().
 */
#define PBA_NR_RESERVED_PAGES	4

#define BLOCK_ORDER		PMD_ORDER
#define BLOCK_NR_PAGES		(1ul << (BLOCK_ORDER))

/*
 * Refilling the cache is done by attempting allocation in decreasing orders
 * (higher orders may not be available due to memory pressure). The specific
 * orders are tweaked based on the page size.
 *
 * - A whole block (PMD_ORDER) is the preferred size. A lower order is used
 *   for page sizes above 16K to avoid reserving too much memory for page
 *   tables (a PMD block is 512 MB for 64K pages on arm64).
 *
 * - The next order corresponds to the contpte size on arm64, which helps to
 *   reduce TLB pressure. Other architectures may prefer other values.
 *
 * - The last order *must* be 2 (4 pages) to guarantee that __refill_pages()
 *   actually increases the number of cached pages - up to 2 cached pages
 *   may be used up by set_memory_pkey() for splitting the linear map.
 */
static const unsigned int refill_orders[] =
#if PAGE_SHIFT <= 12
	{ BLOCK_ORDER, 4, 2 }	/* 4K pages */
#elif PAGE_SHIFT <= 14
	{ BLOCK_ORDER, 7, 2 }	/* 16K pages */
#else
	{ 9, 5, 2 }		/* 64K pages */
#endif
;

struct pkeys_block_allocator {
	struct list_head cached_list;
	unsigned long nr_cached;
	spinlock_t lock;
	struct mutex alloc_mutex;
};

static struct pkeys_block_allocator pkeys_block_allocator = {
	.cached_list = LIST_HEAD_INIT(pkeys_block_allocator.cached_list),
	.nr_cached = 0,
	.lock = __SPIN_LOCK_UNLOCKED(pkeys_block_allocator.lock),
	.alloc_mutex = __MUTEX_INITIALIZER(pkeys_block_allocator.alloc_mutex)
};

static struct {
	struct page *head_page;
	unsigned int order;
} pba_early_region __initdata;

static __ro_after_init DEFINE_STATIC_KEY_FALSE(pba_enabled_key);
static __ro_after_init DEFINE_STATIC_KEY_FALSE(pba_can_set_pkey);

static bool pba_enabled(void)
{
	return static_branch_likely(&pba_enabled_key);
}

static bool alloc_mutex_locked(void)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;

	return mutex_get_owner(&pba->alloc_mutex) == (unsigned long)current;
}

/*
 * __ref is used as this is called from __refill_pages() which is not __init.
 * The call to pba_init_late() guarantees this is not called after boot has
 * completed.
 */
static void __ref register_early_region(struct page *head_page,
					unsigned int order)
{
	/*
	 * Only one region is expected to be registered. Any further region
	 * is left untracked (i.e. unprotected).
	 */
	if (WARN_ON(pba_early_region.head_page))
		return;

	pr_debug("%s: order=%d, pfn=%lx\n", __func__, order,
		 page_to_pfn(head_page));

	pba_early_region.head_page = head_page;
	pba_early_region.order = order;
}

/*
 * Private per-page allocator data. It needs to be preserved when a page table
 * page is allocated, so we cannot use page->private, which overlaps with
 * struct ptdesc::ptl. page->mapping is unused in struct ptdesc so we store it
 * there instead.
 */
struct pba_page_data {
	bool in_block;
	u32 block_nr_free; /* Only used for the head page of a block */
};

static struct pba_page_data *page_pba_data(struct page *page)
{
	BUILD_BUG_ON(sizeof(struct pba_page_data) > sizeof(page->mapping));

	return (struct pba_page_data *)&page->mapping;
}

static void mark_block_cached(struct page *head_page, struct page *cached_pages,
			      unsigned int nr_cached_pages)
{
	page_pba_data(head_page)->in_block = true;
	page_pba_data(head_page)->block_nr_free = nr_cached_pages;

	for (unsigned int i = 0; i < nr_cached_pages; i++)
		page_pba_data(&cached_pages[i])->in_block = true;
}

static void mark_block_noncached(struct page *head_page)
{
	for (unsigned int i = 0; i < BLOCK_NR_PAGES; i++)
		head_page[i].mapping = NULL;
}

static struct page *block_head_page(struct page *page)
{
	unsigned long page_pfn;

	if (!page_pba_data(page)->in_block)
		return NULL;

	page_pfn = page_to_pfn(page);

	return pfn_to_page(ALIGN_DOWN(page_pfn, BLOCK_NR_PAGES));
}

static void inc_block_nr_free(struct page *page)
{
	struct page *head_page = block_head_page(page);

	if (head_page)
		page_pba_data(head_page)->block_nr_free++;
}

static void dec_block_nr_free(struct page *page)
{
	struct page *head_page = block_head_page(page);

	if (head_page)
		page_pba_data(head_page)->block_nr_free--;
}

static void cached_list_add_pages(struct page *page, unsigned int nr_pages)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;

	for (unsigned int i = 0; i < nr_pages; i++)
		list_add(&page[i].lru, &pba->cached_list);

	pba->nr_cached += nr_pages;
}

static void cached_list_del_page(struct page *page)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;

	list_del(&page->lru);
	pba->nr_cached--;
}

static void __refill_pages_add_to_cache(struct page *page, unsigned int order,
					bool alloc_one)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;
	struct page *head_page = page;
	unsigned int nr_pages = 1 << order;

	if (alloc_one) {
		page++;
		nr_pages--;
	}

	if (order == BLOCK_ORDER)
		mark_block_cached(head_page, page, nr_pages);

	guard(spinlock_bh)(&pba->lock);

	cached_list_add_pages(page, nr_pages);
}

static struct page *__refill_pages(bool alloc_one)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;
	struct page *page;
	unsigned int order;
	int ret = 0;

	for (int i = 0; i < ARRAY_SIZE(refill_orders); ++i) {
		order = refill_orders[i];
		page = alloc_pages_noprof(PBA_GFP_ALLOC, order);
		if (page)
			break;
	}

	if (!page)
		return NULL;

	pr_debug("%s: order=%d, pfn=%lx\n", __func__, order, page_to_pfn(page));

	guard(mutex)(&pba->alloc_mutex);

	if (static_branch_likely(&pba_can_set_pkey))
		ret = set_pkey_pgtable(page, 1 << order);
	else
		register_early_region(page, order);

	if (ret) {
		__free_pages(page, order);
		return NULL;
	}

	/* Each page is going to be allocated individually */
	split_page(page, order);

	__refill_pages_add_to_cache(page, order, alloc_one);

	return page;
}

static int refill_pages(void)
{
	return __refill_pages(false) ? 0 : -ENOMEM;
}

static struct page *refill_pages_and_alloc_one(void)
{
	return __refill_pages(true);
}

static unsigned long release_page_list(struct list_head *page_list)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;
	unsigned long nr_freed = 0;
	struct page *page, *tmp;

	/* _safe is required because __free_page() overwrites page->lru */
	list_for_each_entry_safe(page, tmp, page_list, lru) {
		int ret = 0;

		ret = set_pkey_default(page, 1);

		if (ret) {
			guard(spinlock_bh)(&pba->lock);
			cached_list_add_pages(page, 1);
			break;
		}

		__free_page(page);
		nr_freed++;
	}

	return nr_freed;
}

static unsigned long release_whole_block(struct list_head *page_list,
					struct page *block_head)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;
	unsigned long nr_freed = 0;
	struct page *page, *tmp;
	int ret;

	/* Reset the pkey for the full block to avoid splitting the linear map */
	ret = set_pkey_default(block_head, BLOCK_NR_PAGES);

	if (ret) {
		guard(spinlock_bh)(&pba->lock);
		cached_list_add_pages(block_head, BLOCK_NR_PAGES);
		return 0;
	}

	list_for_each_entry_safe(page, tmp, page_list, lru) {
		__free_page(page);
		nr_freed++;
	}

	return nr_freed;
}

static bool cached_page_available(gfp_t gfp)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;

	if (gfp & __GFP_PGTABLE_SPLIT) {
		pr_debug("%s: split pgtable (nr_cached: %lu, in_alloc: %d)\n",
			__func__, pba->nr_cached, alloc_mutex_locked());
		return true;
	}

	return pba->nr_cached > PBA_NR_RESERVED_PAGES;
}

static struct page *get_cached_page(gfp_t gfp)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;
	struct page *page;

	guard(spinlock_bh)(&pba->lock);

	if (!cached_page_available(gfp))
		return NULL;

	page = list_first_entry_or_null(&pba->cached_list, struct page, lru);
	if (WARN_ON(!page))
		return NULL;

	cached_list_del_page(page);
	dec_block_nr_free(page);
	return page;
}

static void check_gfp(gfp_t gfp)
{
	VM_WARN_ON_ONCE((gfp & PBA_GFP_ALLOC) != PBA_GFP_ALLOC);

	gfp &= ~(PBA_GFP_ALLOC | PBA_GFP_OPT_MASK);

	VM_WARN_ONCE(gfp, "Unexpected gfp: %pGg\n", &gfp);
}

static int prepare_page(struct page *page, gfp_t gfp)
{
	if (gfp & __GFP_ACCOUNT) {
		int ret = memcg_kmem_charge_page(page, gfp, 0);

		if (unlikely(ret))
			return ret;
	}

	/*
	 * __refill_pages() only guarantees that page_private is zeroed for the
	 * head page, so it is safer to zero it every time we allocate a new
	 * page.
	 */
	set_page_private(page, 0);

	if (gfp & __GFP_ZERO) {
		u64 saved_pkey_reg;

		/*
		 * The page is mapped with KPKEYS_PKEY_PGTABLES so we need
		 * to switch to the corresponding kpkeys level to write to it.
		 */
		saved_pkey_reg = kpkeys_set_level(KPKEYS_LVL_PGTABLES);
		clear_highpage(page);
		kpkeys_restore_pkey_reg(saved_pkey_reg);
	}

	return 0;
}

static struct page *pba_pgtable_alloc(gfp_t gfp)
{
	struct page *page;

	check_gfp(gfp);

	page = get_cached_page(gfp);

	if (!page)
		page = refill_pages_and_alloc_one();
	WARN_ON(!page);

	if (page && prepare_page(page, gfp)) {
		kpkeys_pgtable_free(page);
		return NULL;
	}

	return page;
}

static void pba_pgtable_free(struct page *page)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;

	memcg_kmem_uncharge_page(page, 0);

	guard(spinlock_bh)(&pba->lock);

	cached_list_add_pages(page, 1);
	inc_block_nr_free(page);
}

static int pba_prepare_direct_map_split(void)
{
	if (pba_ready_for_direct_map_split())
		return 0;

	/* Ensure we have at least PBA_NR_RESERVED_PAGES available */
	return refill_pages();
}

static bool pba_ready_for_direct_map_split(void)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;

	/*
	 * For a regular split, we must ensure the reserve is fully replenished
	 * before splitting (which may consume 2 pages out of 4).
	 *
	 * When refilling our cache, alloc_mutex is locked and we must use
	 * pages from the reserve (remaining 2 pages).
	 */
	return READ_ONCE(pba->nr_cached) >= PBA_NR_RESERVED_PAGES ||
		alloc_mutex_locked();
}

static void __init pba_init(void)
{
	int ret;

	if (arch_has_pte_only_direct_map())
		return;

	static_branch_enable(&pba_enabled_key);

	/*
	 * Refill the cache so that the reserve pages are available for
	 * splitting next time we need to refill.
	 *
	 * We cannot split the linear map at this stage, so the allocated
	 * region will be registered as early region (pba_early_region) and
	 * its pkey set later.
	 */
	ret = refill_pages();
	WARN_ON(ret);
}

static void __init pba_init_late(void)
{
	static_branch_enable(&pba_can_set_pkey);

	if (pba_early_region.head_page)
		set_pkey_pgtable(pba_early_region.head_page,
				 1 << pba_early_region.order);
}

/* Shrinker */

/* Keep some pages around to avoid shrinking causing a refill right away */
#define PBA_UNSHRINKABLE_PAGES		16
/* Don't shrink a block that is almost full to avoid excessive splitting */
#define PBA_SHRINK_BLOCK_MIN_PAGES	(BLOCK_NR_PAGES / 8)

static unsigned long count_shrinkable_pages(void)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;
	unsigned long nr_cached = READ_ONCE(pba->nr_cached);

	return nr_cached > PBA_UNSHRINKABLE_PAGES ?
		nr_cached - PBA_UNSHRINKABLE_PAGES : 0;
}

static unsigned long pba_shrink_count(struct shrinker *shrink,
				      struct shrink_control *sc)
{

	return count_shrinkable_pages() ?: SHRINK_EMPTY;
}

static bool block_worth_shrinking(unsigned long nr_pages_target_block,
				  unsigned long nr_pages_nonblock,
				  struct shrink_control *sc)
{
	/*
	 * Avoid partially shrinking a block (which means splitting it) if
	 * we can reclaim enough/more non-block pages instead, or if we would
	 * reclaim only few pages (below PBA_SHRINK_BLOCK_MIN_PAGES)
	 */
	return nr_pages_nonblock < nr_pages_target_block &&
		nr_pages_nonblock < sc->nr_to_scan &&
		nr_pages_target_block >= PBA_SHRINK_BLOCK_MIN_PAGES;
}

static unsigned long pba_shrink_scan(struct shrinker *shrink,
				     struct shrink_control *sc)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;
	LIST_HEAD(pages_to_free);
	struct page *page, *tmp;
	unsigned long nr_pages_nonblock = 0, nr_pages_target_block = 0;
	unsigned long nr_pages_uncached = 0, nr_freed = 0;
	unsigned long nr_pages_shrinkable;
	struct page *target_block = NULL;

	sc->nr_scanned = 0;

	pr_debug("%s: nr_to_scan = %lu, nr_cached = %lu\n",
		 __func__, sc->nr_to_scan, pba->nr_cached);

	spin_lock_bh(&pba->lock);
	nr_pages_shrinkable = count_shrinkable_pages();

	/*
	 * Count pages that don't belong to any block, and find the block
	 * with the highest number of free pages
	 */
	list_for_each_entry(page, &pba->cached_list, lru) {
		struct page *block = block_head_page(page);
		unsigned long block_nr_free;

		if (!block) {
			nr_pages_nonblock++;
			continue;
		}

		block_nr_free = page_pba_data(block)->block_nr_free;

		if (block_nr_free > nr_pages_target_block) {
			target_block = block;
			nr_pages_target_block = block_nr_free;
		}

		/* We will free this block, so no need to continue scanning */
		if (nr_pages_target_block == BLOCK_NR_PAGES)
			break;
	}

	if (nr_pages_target_block == BLOCK_NR_PAGES) {
		/*
		 * If a whole block is empty, take the opportunity to free it
		 * completely (regardless of the requested nr_to_scan) to avoid
		 * splitting the linear map. If nr_pages_shrinkable is too low,
		 * we bail out as we would have to split the block to shrink it
		 * partially (and there is nothing else we can shrink).
		 */
		if (nr_pages_shrinkable < BLOCK_NR_PAGES) {
			spin_unlock_bh(&pba->lock);
			pr_debug("%s: cannot free empty block, bailing out\n",
				 __func__);
			goto out;
		}

		sc->nr_to_scan = BLOCK_NR_PAGES;
	} else if (block_worth_shrinking(nr_pages_target_block,
					 nr_pages_nonblock, sc)) {
		/* Shrink block (partially) */
		sc->nr_to_scan = min(sc->nr_to_scan, nr_pages_target_block);
	} else {
		/* Free non-block pages */
		sc->nr_to_scan = min(sc->nr_to_scan, nr_pages_nonblock);
		target_block = NULL;
	}

	list_for_each_entry_safe(page, tmp, &pba->cached_list, lru) {
		struct page *block = block_head_page(page);

		if (!(nr_pages_uncached < sc->nr_to_scan &&
		      nr_pages_uncached < nr_pages_shrinkable))
			break;

		if (block == target_block) {
			list_move(&page->lru, &pages_to_free);
			nr_pages_uncached++;
		}
	}

	pba->nr_cached -= nr_pages_uncached;
	sc->nr_scanned = nr_pages_uncached;

	if (target_block)
		mark_block_noncached(target_block);
	spin_unlock_bh(&pba->lock);

	if (target_block)
		pr_debug("%s: freeing block (pfn = %lx, %lu/%lu free pages)\n",
			 __func__, page_to_pfn(target_block),
			 nr_pages_target_block, BLOCK_NR_PAGES);
	else
		pr_debug("%s: freeing non-block (%lu free pages)\n",
			 __func__, nr_pages_nonblock);

	if (nr_pages_target_block == BLOCK_NR_PAGES) {
		VM_WARN_ON(nr_pages_uncached != BLOCK_NR_PAGES);
		nr_freed = release_whole_block(&pages_to_free, target_block);
	} else {
		nr_freed = release_page_list(&pages_to_free);
	}

	pr_debug("%s: freed %lu pages, nr_cached = %lu\n", __func__,
		 nr_freed, pba->nr_cached);
out:
	return nr_freed ?: SHRINK_STOP;
}

static int __init pba_init_shrinker(void)
{
	struct shrinker *shrinker;

	if (!pba_enabled())
		return 0;

	shrinker = shrinker_alloc(0, "kpkeys-pgtable-block");
	if (!shrinker)
		return -ENOMEM;

	shrinker->count_objects = pba_shrink_count;
	shrinker->scan_objects = pba_shrink_scan;
	shrinker->seeks = 0;
	shrinker->batch = BLOCK_NR_PAGES;
	shrinker_register(shrinker);
	return 0;
}
late_initcall(pba_init_shrinker);
