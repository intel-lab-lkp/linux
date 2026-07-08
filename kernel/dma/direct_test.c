// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for DMA allocation live update preservation.
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <kunit/static_stub.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/kho/abi/dma_alloc.h>
#include <linux/kexec_handover.h>
#include <linux/slab.h>

static const size_t dma_test_sizes[] = {
	128,
	PAGE_SIZE,
	2 * PAGE_SIZE,
	3 * PAGE_SIZE,
	8 * PAGE_SIZE,
};

static void dma_size_desc(const size_t *size, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "size=%zu", *size);
}

KUNIT_ARRAY_PARAM(dma_size, dma_test_sizes, dma_size_desc);

static struct page *mock_kho_restore_pages(phys_addr_t phys, unsigned long nr_pages)
{
	struct page *page = phys_to_page(phys);

	if (!kho_test_pages_preserved(phys, nr_pages))
		return NULL;

	kho_unpreserve_pages(page, nr_pages);
	return page;
}

static struct folio *mock_kho_restore_folio(phys_addr_t phys)
{
	struct folio *folio = page_folio(phys_to_page(phys));

	if (!kho_test_pages_preserved(phys, (1 << folio_order(folio))))
		return NULL;

	kho_unpreserve_folio(folio);
	return folio;
}

static void test_dma_direct_preserve_restore_common(struct kunit *test,
						    bool coherent,
						    bool atomic,
						    size_t size)
{
	unsigned long nr_pages = 1 << get_order(size);
	dma_addr_t handle1, handle2;
	int ret, expected_ret = 0;
	struct device dev = {0};
	gfp_t gfp = GFP_KERNEL;
	void *addr1, *addr2;
	u64 state;

	kunit_activate_static_stub(test, kho_restore_pages, mock_kho_restore_pages);
	kunit_activate_static_stub(test, kho_restore_folio, mock_kho_restore_folio);

	device_initialize(&dev);
	dev.coherent_dma_mask = DMA_BIT_MASK(64);
	dev.dma_mask = &dev.coherent_dma_mask;
#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL)
	dev.dma_coherent = coherent;
#endif

	if (atomic)
		gfp = GFP_ATOMIC;

	addr1 = dma_alloc_coherent(&dev, size, &handle1, gfp);
	if (!addr1) {
		kunit_skip(test, "DMA allocation failed (unsupported configuration)");
		return;
	}

	if (IS_ENABLED(CONFIG_DMA_COHERENT_POOL) && dma_is_from_pool(addr1, size)) {
		expected_ret = -EOPNOTSUPP;
		kunit_info(test, "DMA allocation using pool, expecting -EOPNOTSUPP");
	} else if (dma_is_from_cma(dma_to_phys(&dev, handle1), size)) {
		expected_ret = -EOPNOTSUPP;
		kunit_info(test, "DMA allocation using CMA, expecting -EOPNOTSUPP");
	}

	ret = dma_preserve_coherent_allocation(&dev, addr1, size, handle1, &state);
	KUNIT_ASSERT_EQ(test, ret, expected_ret);

	if (!expected_ret) {
		KUNIT_EXPECT_TRUE_MSG(test,
				      kho_test_pages_preserved(dma_to_phys(&dev, handle1), nr_pages),
				      "Allocated block not tracked in KHO");

		addr2 = dma_restore_coherent_allocation(&dev, size, &handle2, gfp, state);
		KUNIT_ASSERT_NOT_NULL(test, addr2);

		KUNIT_EXPECT_EQ(test, handle1, handle2);
		dma_free_coherent(&dev, size, addr2, handle2);
		KUNIT_EXPECT_FALSE_MSG(test,
				       kho_test_pages_preserved(dma_to_phys(&dev, handle1), nr_pages),
				       "Allocated block still tracked after free");
	} else {
		dma_free_coherent(&dev, size, addr1, handle1);
	}
}

static void test_dma_direct_coherent(struct kunit *test)
{
	const size_t *size = test->param_value;

	test_dma_direct_preserve_restore_common(test, true, false, *size);
}

static void test_dma_direct_non_coherent(struct kunit *test)
{
	const size_t *size = test->param_value;

	test_dma_direct_preserve_restore_common(test, false, false, *size);
}

static void test_dma_direct_coherent_atomic(struct kunit *test)
{
	const size_t *size = test->param_value;

	test_dma_direct_preserve_restore_common(test, true, true, *size);
}

static void test_dma_direct_non_coherent_atomic(struct kunit *test)
{
	const size_t *size = test->param_value;

	test_dma_direct_preserve_restore_common(test, false, true, *size);
}

static void test_dmam_preservation(struct kunit *test)
{
	dma_addr_t handle, non_dmam_handle;
	void *addr, *non_dmam_addr;
	struct device dev = {0};
	size_t size = 128;
	u64 state;
	int ret;

	kunit_activate_static_stub(test, kho_restore_pages, mock_kho_restore_pages);
	kunit_activate_static_stub(test, kho_restore_folio, mock_kho_restore_folio);

	device_initialize(&dev);
	dev.coherent_dma_mask = DMA_BIT_MASK(64);
	dev.dma_mask = &dev.coherent_dma_mask;
#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
    defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL)
	dev.dma_coherent = true;
#endif

	non_dmam_addr = dma_alloc_coherent(&dev, size, &non_dmam_handle, GFP_KERNEL);
	if (!non_dmam_addr) {
		kunit_skip(test, "DMA allocation failed");
		return;
	}

	ret = dmam_preserve_coherent_allocation(&dev, non_dmam_addr, size, non_dmam_handle, &state);
	KUNIT_EXPECT_EQ_MSG(test, ret, -EINVAL, "Preserving non-devres allocation should fail");
	dma_free_coherent(&dev, size, non_dmam_addr, non_dmam_handle);

	addr = dmam_alloc_coherent(&dev, size, &handle, GFP_KERNEL);
	if (!addr) {
		kunit_skip(test, "DMA allocation failed");
		return;
	}

	ret = dmam_preserve_coherent_allocation(&dev, addr, size, handle, &state);
	KUNIT_ASSERT_EQ_MSG(test, ret, 0, "Failed to preserve dmam allocation");
	KUNIT_EXPECT_TRUE(test, kho_test_pages_preserved(dma_to_phys(&dev, handle), 1));

	dmam_unpreserve_coherent_allocation(&dev, addr, size, handle, state);
	KUNIT_EXPECT_FALSE(test, kho_test_pages_preserved(dma_to_phys(&dev, handle), 1));

	dmam_free_coherent(&dev, size, addr, handle);
}

static struct kunit_case dma_direct_test_cases[] = {
	KUNIT_CASE_PARAM(test_dma_direct_coherent, dma_size_gen_params),
	KUNIT_CASE_PARAM(test_dma_direct_non_coherent, dma_size_gen_params),
	KUNIT_CASE_PARAM(test_dma_direct_coherent_atomic, dma_size_gen_params),
	KUNIT_CASE_PARAM(test_dma_direct_non_coherent_atomic, dma_size_gen_params),
	KUNIT_CASE(test_dmam_preservation),
	{}
};

static struct kunit_suite dma_direct_test_suite = {
	.name = "dma_direct_liveupdate",
	.test_cases = dma_direct_test_cases,
};
kunit_test_suite(dma_direct_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit test for DMA direct live update preservation");
MODULE_LICENSE("GPL");
