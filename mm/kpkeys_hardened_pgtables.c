// SPDX-License-Identifier: GPL-2.0-only
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/highmem.h>
#include <linux/kpkeys.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
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
static void pba_init(void);

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

void __init kpkeys_hardened_pgtables_init(void)
{
	if (!arch_kpkeys_enabled())
		return;

	pba_init();
	static_branch_enable(&kpkeys_hardened_pgtables_key);
}

/*
 * pkeys block allocator (PBA): dedicated page table allocator for block-mapped
 * linear map. Block splitting is minimised by prioritising the allocation and
 * freeing of full blocks.
 */
#define PBA_GFP_ALLOC		GFP_KERNEL
#define PBA_GFP_OPT_MASK	(__GFP_ZERO | __GFP_ACCOUNT)

#define BLOCK_ORDER		PMD_ORDER

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
};

static struct pkeys_block_allocator pkeys_block_allocator = {
	.cached_list = LIST_HEAD_INIT(pkeys_block_allocator.cached_list),
	.nr_cached = 0,
	.lock = __SPIN_LOCK_UNLOCKED(pkeys_block_allocator.lock),
};

static __ro_after_init DEFINE_STATIC_KEY_FALSE(pba_enabled_key);

static bool pba_enabled(void)
{
	return static_branch_likely(&pba_enabled_key);
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
	unsigned int nr_pages = 1 << order;

	if (alloc_one) {
		page++;
		nr_pages--;
	}

	guard(spinlock_bh)(&pba->lock);

	cached_list_add_pages(page, nr_pages);
}

static struct page *__refill_pages(bool alloc_one)
{
	struct page *page;
	unsigned int order;
	int ret;

	for (int i = 0; i < ARRAY_SIZE(refill_orders); ++i) {
		order = refill_orders[i];
		page = alloc_pages_noprof(PBA_GFP_ALLOC, order);
		if (page)
			break;
	}

	if (!page)
		return NULL;

	pr_debug("%s: order=%d, pfn=%lx\n", __func__, order, page_to_pfn(page));

	ret = set_pkey_pgtable(page, 1 << order);

	if (ret) {
		__free_pages(page, order);
		return NULL;
	}

	/* Each page is going to be allocated individually */
	split_page(page, order);

	__refill_pages_add_to_cache(page, order, alloc_one);

	return page;
}

static struct page *refill_pages_and_alloc_one(void)
{
	return __refill_pages(true);
}

static bool cached_page_available(void)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;

	return pba->nr_cached > 0;
}

static struct page *get_cached_page(gfp_t gfp)
{
	struct pkeys_block_allocator *pba = &pkeys_block_allocator;
	struct page *page;

	guard(spinlock_bh)(&pba->lock);

	if (!cached_page_available())
		return NULL;

	page = list_first_entry_or_null(&pba->cached_list, struct page, lru);
	if (WARN_ON(!page))
		return NULL;

	cached_list_del_page(page);
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
}

static void __init pba_init(void)
{
	if (arch_has_pte_only_direct_map())
		return;

	static_branch_enable(&pba_enabled_key);
}
