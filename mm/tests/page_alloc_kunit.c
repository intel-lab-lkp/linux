// SPDX-License-Identifier: GPL-2.0-only
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mermap.h>
#include <linux/mm_types.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/set_memory.h>
#include <linux/sched/mm.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include <kunit/resource.h>
#include <kunit/test.h>

#include "internal.h"

struct free_pages_ctx {
	unsigned int order;
	struct list_head pages;
};

static inline void action_many__free_pages(void *context)
{
	struct free_pages_ctx *ctx = context;
	struct page *page, *tmp;

	list_for_each_entry_safe(page, tmp, &ctx->pages, lru)
		__free_pages(page, ctx->order);
}

/*
 * Allocate a bunch of pages with the same order and GFP flags, transparently
 * take care of error handling and cleanup. Does this all via a single KUnit
 * resource, i.e. has a fixed memory overhead.
 */
static inline struct free_pages_ctx *
do_many_alloc_pages(struct kunit *test, gfp_t gfp,
		    unsigned int order, unsigned int count)
{
	struct free_pages_ctx *ctx = kunit_kzalloc(
		test, sizeof(struct free_pages_ctx), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	INIT_LIST_HEAD(&ctx->pages);
	ctx->order = order;

	for (int i = 0; i < count; i++) {
		struct page *page = alloc_pages(gfp, order);

		if (!page) {
			struct page *page, *tmp;

			list_for_each_entry_safe(page, tmp, &ctx->pages, lru)
				__free_pages(page, order);

			KUNIT_FAIL_AND_ABORT(test,
				"Failed to alloc order %d page (GFP *%pG) iter %d",
				order, &gfp, i);
		}
		list_add(&page->lru, &ctx->pages);
	}

	KUNIT_ASSERT_EQ(test,
		kunit_add_action_or_reset(test, action_many__free_pages, ctx), 0);
	return ctx;
}

#ifdef CONFIG_PAGE_ALLOC_UNMAPPED

static const gfp_t gfp_params_array[] = {
	0,
	__GFP_ZERO,
};

static void gfp_param_get_desc(const gfp_t *gfp, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%pGg", gfp);
}

KUNIT_ARRAY_PARAM(gfp, gfp_params_array, gfp_param_get_desc);

/* Do some allocations that force the allocator to map/unmap some blocks.  */
static void test_alloc_map_unmap(struct kunit *test)
{
	unsigned long page_majority;
	struct free_pages_ctx *ctx;
	const gfp_t *gfp_extra = test->param_value;
	gfp_t gfp = GFP_KERNEL | __GFP_THISNODE | __GFP_UNMAPPED | *gfp_extra;
	struct page *page;

	kunit_attach_mm();
	mermap_mm_prepare(current->mm);

	/* No cleanup here - assuming kthread "belongs" to this test. */
	set_cpus_allowed_ptr(current, cpumask_of_node(numa_node_id()));

	/*
	 * First allocate more than half of the memory in the node as
	 * unmapped. Assuming the memory starts out mapped, this should
	 * exercise the unmap.
	 */
	page_majority = (node_present_pages(numa_node_id()) / 2) + 1;
	ctx = do_many_alloc_pages(test, gfp, 0, page_majority);

	/* Check pages are unmapped */
	list_for_each_entry(page, &ctx->pages, lru) {
		freetype_t ft = get_pfnblock_freetype(page, page_to_pfn(page));

		/*
		 * Logically it should be an EXPECT, but that would
		 * cause heavy log spam on failure so use ASSERT for
		 * concision.
		 */
		KUNIT_ASSERT_FALSE(test, kernel_page_present(page));
		KUNIT_ASSERT_TRUE(test, freetype_flags(ft) & FREETYPE_UNMAPPED);
	}

	/*
	 * Now free them again and allocate the same amount without
	 * __GFP_UNMAPPED. This will exercise the mapping logic.
	 */
	kunit_release_action(test, action_many__free_pages, ctx);
	gfp &= ~__GFP_UNMAPPED;
	ctx = do_many_alloc_pages(test, gfp, 0, page_majority);

	/* Check pages are mapped. */
	list_for_each_entry(page, &ctx->pages, lru)
		KUNIT_ASSERT_TRUE(test, kernel_page_present(page));
}

#endif /* CONFIG_PAGE_ALLOC_UNMAPPED */

static void __test_pindex_helpers(struct kunit *test, unsigned long *bitmap,
				  int mt, unsigned int ftflags, unsigned int order)
{
	freetype_t ft = migrate_to_freetype(mt, ftflags);
	unsigned int pindex;
	int got_order;

	if (!pcp_allowed_order(order))
		return;

	if (mt >= MIGRATE_PCPTYPES)
		return;

	if (freetype_idx(ft) < 0)
		return;

	pindex = order_to_pindex(ft, order);

	KUNIT_ASSERT_LT_MSG(test, pindex, NR_PCP_LISTS,
		"invalid pindex %d (order %d mt %d flags %#x)",
		pindex, order, mt, ftflags);
	KUNIT_EXPECT_TRUE_MSG(test, test_bit(pindex, bitmap),
		"pindex %d reused (order %d mt %d flags %#x)",
		pindex, order, mt, ftflags);

	/*
	 * For THP, two migratetypes map to the same pindex,
	 * just manually exclude one of those cases.
	 */
	if (!(IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
		order == HPAGE_PMD_ORDER &&
		mt == min(MIGRATE_UNMOVABLE, MIGRATE_RECLAIMABLE)))
		clear_bit(pindex, bitmap);

	got_order = pindex_to_order(pindex);
	KUNIT_EXPECT_EQ_MSG(test, order, got_order,
		"roundtrip failed, got %d want %d (pindex %d mt %d flags %#x)",
		got_order, order, pindex, mt, ftflags);
}

/* This just checks for basic arithmetic errors. */
static void test_pindex_helpers(struct kunit *test)
{
	unsigned long bitmap[bitmap_size(NR_PCP_LISTS)];

	/* Bit means "pindex not yet used". */
	bitmap_fill(bitmap, NR_PCP_LISTS);

	for (unsigned int order = 0; order < NR_PAGE_ORDERS; order++) {
		for (int mt = 0; mt < MIGRATE_TYPES; mt++) {
			__test_pindex_helpers(test, bitmap, mt, 0, order);
			if (FREETYPE_UNMAPPED)
				__test_pindex_helpers(test, bitmap, mt,
						      FREETYPE_UNMAPPED, order);
		}
	}

	KUNIT_EXPECT_TRUE_MSG(test, bitmap_empty(bitmap, NR_PCP_LISTS),
		"unused pindices: %*pbl", NR_PCP_LISTS, bitmap);
}

static void __test_freetype_idx(struct kunit *test, unsigned int order,
				int migratetype, unsigned int ftflags,
				unsigned long *bitmap)
{
	freetype_t ft = migrate_to_freetype(migratetype, ftflags);
	int idx = freetype_idx(ft);

	if (idx == -1)
		return;
	KUNIT_ASSERT_GE(test, idx, 0);
	KUNIT_ASSERT_LT(test, idx, NR_FREETYPE_IDXS);

	KUNIT_EXPECT_LT_MSG(test, idx, NR_PCP_LISTS,
		"invalid idx %d (order %d mt %d flags %#x)",
		idx, order, migratetype, ftflags);
	clear_bit(idx, bitmap);
}

static void test_freetype_idx(struct kunit *test)
{
	unsigned long bitmap[bitmap_size(NR_FREETYPE_IDXS)];

	/* Bit means "pindex not yet used". */
	bitmap_fill(bitmap, NR_FREETYPE_IDXS);

	for (unsigned int order = 0; order < NR_PAGE_ORDERS; order++) {
		for (int mt = 0; mt < MIGRATE_TYPES; mt++) {
			__test_freetype_idx(test, order, mt, 0, bitmap);
			if (FREETYPE_UNMAPPED)
				__test_freetype_idx(test, order, mt,
						    FREETYPE_UNMAPPED, bitmap);
		}
	}

	KUNIT_EXPECT_TRUE_MSG(test, bitmap_empty(bitmap, NR_FREETYPE_IDXS),
		"unused idxs: %*pbl", NR_PCP_LISTS, bitmap);
}

static struct kunit_case test_cases[] = {
#ifdef CONFIG_PAGE_ALLOC_UNMAPPED
	KUNIT_CASE_PARAM(test_alloc_map_unmap, gfp_gen_params),
#endif
	KUNIT_CASE(test_pindex_helpers),
	KUNIT_CASE(test_freetype_idx),
	{}
};

static struct kunit_suite test_suite = {
	.name = "page_alloc",
	.test_cases = test_cases,
};

kunit_test_suite(test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
