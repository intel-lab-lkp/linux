// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitmap.h>

#include <kunit/test.h>

#include "internal.h"

/* This just checks for basic arithmetic errors. */
static void test_pindex_helpers(struct kunit *test)
{
	unsigned long bitmap[bitmap_size(NR_PCP_LISTS)];

	/* Bit means "pindex not yet used". */
	bitmap_fill(bitmap, NR_PCP_LISTS);

	for (unsigned int order = 0; order < NR_PAGE_ORDERS; order++) {
		for (unsigned int mt = 0; mt < MIGRATE_PCPTYPES; mt++)  {
			if (!pcp_allowed_order(order))
				continue;

			for (int sensitive = 0; sensitive < NR_SENSITIVITIES; sensitive++) {
				freetype_t ft = migrate_to_freetype(mt, sensitive);
				unsigned int pindex = order_to_pindex(ft, order);
				int got_order;

				KUNIT_ASSERT_LT_MSG(test, pindex, NR_PCP_LISTS,
					"invalid pindex %d (order %d mt %d sensitive %d)",
					pindex, order, mt, sensitive);
				KUNIT_EXPECT_TRUE_MSG(test, test_bit(pindex, bitmap),
					"pindex %d reused (order %d mt %d sensitive %d)",
					pindex, order, mt, sensitive);

				/*
				 * For THP, two migratetypes map to the
				 * same pindex, just manually exclude one
				 * of those cases.
				 */
				if (!(IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE) &&
				      order == HPAGE_PMD_ORDER &&
				      mt == min(MIGRATE_UNMOVABLE, MIGRATE_RECLAIMABLE)))
					clear_bit(pindex, bitmap);

				got_order = pindex_to_order(pindex);
				KUNIT_EXPECT_EQ_MSG(test, order, got_order,
					"roundtrip failed, got %d want %d (pindex %d mt %d sensitive %d)",
					got_order, order, pindex, mt, sensitive);

			}
		}
	}

	KUNIT_EXPECT_TRUE_MSG(test, bitmap_empty(bitmap, NR_PCP_LISTS),
		"unused pindices: %*pbl", NR_PCP_LISTS, bitmap);
}

static struct kunit_case page_alloc_test_cases[] = {
	KUNIT_CASE(test_pindex_helpers),
	{}
};

static struct kunit_suite page_alloc_test_suite = {
	.name = "page_alloc",
	.test_cases = page_alloc_test_cases,
};

kunit_test_suite(page_alloc_test_suite);

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
