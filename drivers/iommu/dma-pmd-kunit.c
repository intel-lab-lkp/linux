// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * KUnit tests for the opaque struct dma_pmd_meta table API.
 */
#include <kunit/test.h>
#include <linux/gfp.h>
#include <linux/dma-pmd.h>
#include <linux/mm.h>

#include "dma-pmd-priv.h"

static void test_meta_init_and_roundtrip(struct kunit *test)
{
	struct dma_pmd_meta *m_pfn, *m_phys;
	unsigned long pfn, base_pfn;
	phys_addr_t pa, base_pa;
	struct page *page;

	KUNIT_ASSERT_EQ(test, dma_pmd_meta_init(), 0);
	/* Second call must be idempotent. */
	KUNIT_ASSERT_EQ(test, dma_pmd_meta_init(), 0);

	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);

	pfn = page_to_pfn(page);
	base_pfn = ALIGN_DOWN(pfn, 1UL << PMD_ORDER);
	pa = page_to_phys(page);
	base_pa = ALIGN_DOWN(pa, PMD_SIZE);

	m_pfn = dma_pmd_meta_of_pfn(pfn);
	m_phys = dma_pmd_meta_from_phys(pa);

	KUNIT_EXPECT_PTR_EQ(test, m_pfn, m_phys);
	KUNIT_EXPECT_EQ(test, dma_pmd_meta_to_pfn(m_pfn), base_pfn);
	KUNIT_EXPECT_EQ(test, dma_pmd_meta_to_phys(m_phys), base_pa);

	__free_page(page);
}

static void test_meta_invalid_phys(struct kunit *test)
{
	struct dma_pmd_meta *m;

	KUNIT_ASSERT_EQ(test, dma_pmd_meta_init(), 0);

	m = dma_pmd_meta_from_phys(PHYS_ADDR_MAX);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_EQ(test, dma_pmd_meta_to_phys(m), PHYS_ADDR_MAX);
	KUNIT_EXPECT_FALSE(test, dma_is_pmd_page(ULONG_MAX >> PAGE_SHIFT));
}

static void dma_pmd_meta_set_pooled(struct dma_pmd_meta *m, bool pooled)
{
	WRITE_ONCE(m->pooled, pooled);
}

static void test_meta_pooled_toggle(struct kunit *test)
{
	unsigned long pfn, base_pfn;
	struct dma_pmd_meta *m;
	struct page *page;

	KUNIT_ASSERT_EQ(test, dma_pmd_meta_init(), 0);

	page = alloc_pages(GFP_KERNEL, PMD_ORDER);
	KUNIT_ASSERT_NOT_NULL(test, page);

	pfn = page_to_pfn(page);
	base_pfn = ALIGN_DOWN(pfn, 1UL << PMD_ORDER);
	/* The buddy allocator returns naturally aligned blocks. */
	KUNIT_EXPECT_EQ(test, pfn, base_pfn);
	m = dma_pmd_meta_of_pfn(pfn);

	KUNIT_EXPECT_FALSE(test, dma_is_pmd_page(pfn));

	dma_pmd_meta_set_pooled(m, true);
	KUNIT_EXPECT_TRUE(test, dma_is_pmd_page(base_pfn));
	KUNIT_EXPECT_TRUE(test, dma_is_pmd_page(pfn));
	KUNIT_EXPECT_TRUE(test, dma_is_pmd_page(base_pfn + (1UL << PMD_ORDER) - 1));

	dma_pmd_meta_set_pooled(m, false);
	KUNIT_EXPECT_FALSE(test, dma_is_pmd_page(pfn));

	__free_pages(page, PMD_ORDER);
}

static struct kunit_case dma_pmd_meta_test_cases[] = {
	KUNIT_CASE(test_meta_init_and_roundtrip),
	KUNIT_CASE(test_meta_invalid_phys),
	KUNIT_CASE(test_meta_pooled_toggle),
	{}
};

static struct kunit_suite dma_pmd_meta_test_suite = {
	.name = "dma_pmd_meta",
	.test_cases = dma_pmd_meta_test_cases,
};

kunit_test_suite(dma_pmd_meta_test_suite);
MODULE_DESCRIPTION("KUnit tests for struct dma_pmd_meta table");
MODULE_LICENSE("Dual BSD/GPL");
