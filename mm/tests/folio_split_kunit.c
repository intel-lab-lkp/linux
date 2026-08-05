// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/huge_mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>

static void folio_check_splittable_mappingless_swapcache(struct kunit *test)
{
	struct folio *folio;
	int ret;

	folio = folio_alloc(GFP_KERNEL, 2);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	folio_lock(folio);

	KUNIT_EXPECT_PTR_EQ(test, folio->mapping, NULL);
	ret = folio_check_splittable(folio, 0, SPLIT_TYPE_UNIFORM);
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);

	/* Only the eligibility check is exercised here. */
	folio_set_swapbacked(folio);
	folio_set_swapcache(folio);

	ret = folio_check_splittable(folio, 0, SPLIT_TYPE_UNIFORM);
	KUNIT_EXPECT_EQ(test, ret, 0);
	ret = folio_check_splittable(folio, 1, SPLIT_TYPE_UNIFORM);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	ret = folio_check_splittable(folio, 0, SPLIT_TYPE_NON_UNIFORM);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	folio_clear_swapcache(folio);
	folio_clear_swapbacked(folio);
	folio_unlock(folio);
	folio_put(folio);
}

static struct kunit_case folio_split_test_cases[] = {
	KUNIT_CASE(folio_check_splittable_mappingless_swapcache),
	{}
};

static struct kunit_suite folio_split_test_suite = {
	.name = "folio_split",
	.test_cases = folio_split_test_cases,
};

kunit_test_suite(folio_split_test_suite);

MODULE_DESCRIPTION("Tests for folio splitting");
MODULE_LICENSE("GPL");
