// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * KUnit tests for split_page_compound().
 */
#include <kunit/test.h>
#include <linux/gfp.h>
#include <linux/mm.h>

static void test_split_compound_pieces(struct kunit *test)
{
	const unsigned int old_order = 5, new_order = 2;
	const unsigned int step = 1U << new_order;
	const unsigned int nr = 1U << old_order;
	struct page *page;
	unsigned int i, j;
	int ret;

	page = alloc_pages(GFP_KERNEL, old_order);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_EXPECT_FALSE(test, PageCompound(page));

	ret = split_page_compound(page, old_order, new_order);
	if (ret)
		__free_pages(page, old_order);
	KUNIT_ASSERT_EQ(test, ret, 0);

	for (i = 0; i < nr; i += step) {
		struct page *sub = page + i;

		/* All heads 0..N-1 are returned frozen (refcount 0). */
		KUNIT_EXPECT_EQ(test, page_count(sub), 0);
		KUNIT_EXPECT_TRUE(test, PageHead(sub));
		KUNIT_EXPECT_EQ(test, compound_order(sub), new_order);

		for (j = 1; j < step; j++) {
			KUNIT_EXPECT_TRUE(test, PageTail(sub + j));
			KUNIT_EXPECT_PTR_EQ(test, compound_head(sub + j), sub);
		}

		page_ref_unfreeze(sub, 1);

		/* Tail get_page()/put_page() must operate on this piece's head. */
		get_page(sub + 1);
		KUNIT_EXPECT_EQ(test, page_count(sub), 2);
		put_page(sub + 1);
		KUNIT_EXPECT_EQ(test, page_count(sub), 1);
	}

	/* Each compound piece frees independently at new_order. */
	for (i = 0; i < nr; i += step)
		__free_pages(page + i, new_order);
}

static void test_split_order0_pieces(struct kunit *test)
{
	const unsigned int old_order = 3, nr = 1U << old_order;
	struct page *page;
	unsigned int i;
	int ret;

	page = alloc_pages(GFP_KERNEL, old_order);
	KUNIT_ASSERT_NOT_NULL(test, page);

	ret = split_page_compound(page, old_order, 0);
	if (ret)
		__free_pages(page, old_order);
	KUNIT_ASSERT_EQ(test, ret, 0);

	for (i = 0; i < nr; i++) {
		struct page *sub = page + i;

		KUNIT_EXPECT_FALSE(test, PageCompound(sub));
		KUNIT_EXPECT_EQ(test, page_count(sub), 0);
		page_ref_unfreeze(sub, 1);
		__free_pages(sub, 0);
	}
}

static void test_split_ebusy_extra_ref(struct kunit *test)
{
	const unsigned int old_order = 4;
	const unsigned int new_order = 2;
	struct page *page;
	int ret;

	page = alloc_pages(GFP_KERNEL, old_order);
	KUNIT_ASSERT_NOT_NULL(test, page);

	/* Simulate a concurrent speculative reference. */
	get_page(page);
	ret = split_page_compound(page, old_order, new_order);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	KUNIT_EXPECT_FALSE(test, PageCompound(page));

	put_page(page);
	__free_pages(page, old_order);
}

static struct kunit_case split_page_compound_test_cases[] = {
	KUNIT_CASE(test_split_compound_pieces),
	KUNIT_CASE(test_split_order0_pieces),
	KUNIT_CASE(test_split_ebusy_extra_ref),
	{}
};

static struct kunit_suite split_page_compound_test_suite = {
	.name = "split_page_compound",
	.test_cases = split_page_compound_test_cases,
};

kunit_test_suite(split_page_compound_test_suite);
MODULE_DESCRIPTION("KUnit tests for split_page_compound()");
MODULE_LICENSE("Dual BSD/GPL");
