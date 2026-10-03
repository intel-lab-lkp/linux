// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * KUnit tests for the opaque struct dma_pmd_meta table API.
 */
#include <kunit/test.h>
#include <linux/gfp.h>
#include <linux/dma-pmd.h>
#include <linux/iommu.h>
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
	WRITE_ONCE(m->flags, pooled ? DMA_PMD_POOLED : 0);
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
	KUNIT_ASSERT_TRUE(test, dma_pmd_meta_ensure_pfn(pfn, true));
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

static void test_pool_alloc_and_recycle(struct kunit *test)
{
	struct dma_pmd_pool *pool, *pool_wm;
	struct page *p1, *p2, *b1, *b2, *b3;
	unsigned int order;

	KUNIT_EXPECT_NULL(test, dma_pmd_pool_destroy(NULL));

	for (order = 0; order <= PMD_ORDER + 1; order++) {
		pool = dma_pmd_pool_create(order, 2);
		if (order > PMD_ORDER) {
			KUNIT_EXPECT_NULL(test, pool);
			continue;
		}
		KUNIT_ASSERT_NOT_NULL(test, pool);
		KUNIT_EXPECT_FALSE(test, dma_pmd_pool_has_free(pool));

		p1 = dma_pmd_pool_alloc(pool, GFP_KERNEL | __GFP_ZERO);
		if (!p1)
			dma_pmd_pool_destroy(pool);
		KUNIT_ASSERT_NOT_NULL(test, p1);
		KUNIT_EXPECT_TRUE(test, dma_is_pmd_page(page_to_pfn(p1)));
		KUNIT_EXPECT_EQ(test, dma_pmd_pool_has_free(pool), order < PMD_ORDER);

		/* Freeing p1 returns it to the pool; next allocation reuses index 0. */
		__free_pages(p1, order);
		KUNIT_EXPECT_TRUE(test, dma_pmd_pool_has_free(pool));
		p2 = dma_pmd_pool_alloc(pool, GFP_KERNEL);
		KUNIT_EXPECT_PTR_EQ(test, p1, p2);

		/* Destroy with p2 still in flight; freeing p2 releases the 2M page. */
		dma_pmd_pool_destroy(pool);
		if (p2)
			__free_pages(p2, order);
	}

	/*
	 * Exercise max_idle_2m = 1 watermark release: an order-(PMD_ORDER - 1)
	 * pool has 2 blocks per 2M page, so 3 allocations span two 2M pages.
	 */
	pool_wm = dma_pmd_pool_create(PMD_ORDER - 1, 1);
	KUNIT_ASSERT_NOT_NULL(test, pool_wm);
	b1 = dma_pmd_pool_alloc(pool_wm, GFP_KERNEL);
	b2 = dma_pmd_pool_alloc(pool_wm, GFP_KERNEL);
	b3 = dma_pmd_pool_alloc(pool_wm, GFP_KERNEL);
	if (b1)
		__free_pages(b1, PMD_ORDER - 1);
	if (b2)
		__free_pages(b2, PMD_ORDER - 1);
	if (b3)
		__free_pages(b3, PMD_ORDER - 1);
	dma_pmd_pool_destroy(pool_wm);
}

static void test_window_helpers(struct kunit *test)
{
	struct iommu_domain dummy_domain = {};
	struct dma_pmd_window win = {};
	struct dma_pmd_pool *pool;

	KUNIT_ASSERT_EQ(test, dma_pmd_meta_init(), 0);

	/* Empty window (size == 0) never owns any IOVA. */
	KUNIT_EXPECT_FALSE(test, dma_pmd_window_owns(&win, 0));
	KUNIT_EXPECT_FALSE(test, dma_pmd_window_owns(&win, SZ_4G));

	win.base = SZ_4G;
	win.size = SZ_2G;
	KUNIT_EXPECT_FALSE(test, dma_pmd_window_owns(&win, SZ_4G - 1));
	KUNIT_EXPECT_TRUE(test, dma_pmd_window_owns(&win, SZ_4G));
	KUNIT_EXPECT_TRUE(test, dma_pmd_window_owns(&win, SZ_4G + SZ_2G - 1));
	KUNIT_EXPECT_FALSE(test, dma_pmd_window_owns(&win, SZ_4G + SZ_2G));
	/* Releasing an unregistered domain with an active pool is a safe no-op. */
	pool = dma_pmd_pool_create(0, 1);
	KUNIT_ASSERT_NOT_NULL(test, pool);
	dma_pmd_domain_release(&dummy_domain);
	dma_pmd_pool_destroy(pool);
}

static void test_arena_alloc_and_free(struct kunit *test)
{
	struct device dev = { .numa_node = NUMA_NO_NODE };
	dma_addr_t d1, d2, d3, dl1;
	void *v1, *v2, *v3, *vl1;

	KUNIT_EXPECT_NULL(test, dma_pmd_arena_alloc(NULL, SZ_4K, &d1, NUMA_NO_NODE));
	KUNIT_EXPECT_NULL(test, dma_pmd_dma_alloc(NULL, SZ_4K, &d1, GFP_KERNEL, 0));
	KUNIT_EXPECT_NULL(test, dma_pmd_dma_alloc(&dev, SZ_4K, &d1, GFP_KERNEL, 0));
	KUNIT_EXPECT_NULL(test, dma_pmd_dma_alloc(&dev, SZ_4K, &d1,
						  GFP_KERNEL | __GFP_ACCOUNT, 0));
	KUNIT_EXPECT_FALSE(test, dma_pmd_free(&dev, SZ_4K, NULL, 0));
	KUNIT_EXPECT_FALSE(test, dma_is_pmd_dma(&dev, 0));

	v1 = dma_pmd_arena_alloc(&dev, SZ_64K, &d1, NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, v1);
	v2 = dma_pmd_arena_alloc(&dev, SZ_64K, &d2, NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, v2);
	KUNIT_EXPECT_PTR_EQ(test, v2, v1 + SZ_64K);
	KUNIT_EXPECT_EQ(test, d2, d1 + SZ_64K);

	/* Freeing v1 unreserves [0, 64K); next 64K alloc reuses it. */
	KUNIT_EXPECT_TRUE(test, dma_pmd_arena_free(&dev, SZ_64K, v1, d1));
	v3 = dma_pmd_arena_alloc(&dev, SZ_64K, &d3, NUMA_NO_NODE);
	KUNIT_EXPECT_PTR_EQ(test, v3, v1);
	KUNIT_EXPECT_EQ(test, d3, d1);

	/*
	 * Allocate 3MB (spans 2 contiguous fully_empty slots in ARENA_REGION);
	 * the trailing 1MB in the second slot is marked partial_empty and can
	 * satisfy a <= 1MB allocation before both are freed.
	 */
	vl1 = dma_pmd_arena_alloc(&dev, SZ_2M + SZ_1M, &dl1, NUMA_NO_NODE);
	KUNIT_ASSERT_NOT_NULL(test, vl1);
	KUNIT_EXPECT_TRUE(test, dma_pmd_arena_free(&dev, SZ_2M + SZ_1M, vl1, dl1));
	KUNIT_EXPECT_TRUE(test, dma_pmd_arena_free(&dev, SZ_64K, v2, d2));
	KUNIT_EXPECT_TRUE(test, dma_pmd_arena_free(&dev, SZ_64K, v3, d3));
}

static struct kunit_case dma_pmd_meta_test_cases[] = {
	KUNIT_CASE(test_meta_init_and_roundtrip),
	KUNIT_CASE(test_meta_invalid_phys),
	KUNIT_CASE(test_meta_pooled_toggle),
	KUNIT_CASE(test_pool_alloc_and_recycle),
	KUNIT_CASE(test_window_helpers),
	KUNIT_CASE(test_arena_alloc_and_free),
	{}
};

static struct kunit_suite dma_pmd_meta_test_suite = {
	.name = "dma_pmd_meta",
	.test_cases = dma_pmd_meta_test_cases,
};

kunit_test_suite(dma_pmd_meta_test_suite);
MODULE_DESCRIPTION("KUnit tests for struct dma_pmd_meta table and DMA_PMD pool");
MODULE_LICENSE("Dual BSD/GPL");
