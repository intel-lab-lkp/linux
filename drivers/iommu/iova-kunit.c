// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the IOVA allocator.
 *
 * Exercises the augmented-rbtree based allocator: basic alloc/free,
 * size-aligned allocations, top-down ordering, the limit_pfn-aware
 * 32-bit augmentation (relevant for the dma-iommu pci_32bit_workaround
 * pattern), the alignment-aware two-phase search, and randomly
 * fragmented stress.
 *
 * Each test verifies that the augmented invariants
 * (__subtree_max_gap, __subtree_max_gap32, gap_to_prev, clamped_gap32)
 * remain consistent after every batch of operations.
 */
#include <kunit/test.h>
#include <linux/dma-mapping.h>
#include <linux/iova.h>
#include <linux/random.h>

#define TEST_GRANULE PAGE_SIZE
/* Highest pfn that fits in 32 bits — triggers the is_32bit alloc path. */
#define TEST_LIMIT_32BIT (DMA_BIT_MASK(32) >> PAGE_SHIFT)
/* A 64-bit-ish limit well above dma_32bit_pfn. 1ULL avoids UB on ILP32. */
#define TEST_LIMIT_64BIT ((1ULL << 36) >> PAGE_SHIFT)
/*
 * A small <=32-bit limit used by tests that want to actually exhaust the
 * 32-bit-restricted region within a tractable number of allocations and
 * exercise the 64-bit fallback path.
 */
#define TEST_LIMIT_32BIT_RESTRICTED 1024UL

struct iova_test_ctx {
	struct iova_domain iovad;
	bool initialized;
};

static int iova_test_init(struct kunit *test)
{
	struct iova_test_ctx *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;

	ret = iova_cache_get();
	if (ret)
		return ret;

	init_iova_domain(&ctx->iovad, TEST_GRANULE, 1);
	ret = iova_domain_init_rcaches(&ctx->iovad);
	if (ret) {
		put_iova_domain(&ctx->iovad);
		iova_cache_put();
		return ret;
	}
	ctx->initialized = true;

	KUNIT_ASSERT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));
	return 0;
}

static void iova_test_exit(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;

	if (ctx && ctx->initialized) {
		put_iova_domain(&ctx->iovad);
		ctx->initialized = false;
		iova_cache_put();
	}
}

static void test_size_aligned(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	int order;

	for (order = 0; order < 8; ++order) {
		unsigned long size = 1UL << order;
		struct iova *iova = alloc_iova(&ctx->iovad, size,
					       TEST_LIMIT_32BIT, true);

		KUNIT_ASSERT_NOT_NULL(test, iova);
		KUNIT_EXPECT_EQ(test, iova->pfn_lo & (size - 1), 0);
		KUNIT_EXPECT_EQ(test, iova->pfn_hi - iova->pfn_lo + 1, size);
		__free_iova(&ctx->iovad, iova);
		KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));
	}
}

static void test_top_down_preference(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *iovas[16];
	int i;

	for (i = 0; i < ARRAY_SIZE(iovas); ++i) {
		iovas[i] = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_32BIT, false);
		KUNIT_ASSERT_NOT_NULL(test, iovas[i]);
		if (i > 0)
			KUNIT_EXPECT_LT(test, iovas[i]->pfn_lo,
					iovas[i - 1]->pfn_lo);
	}
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	for (i = 0; i < ARRAY_SIZE(iovas); ++i)
		__free_iova(&ctx->iovad, iovas[i]);
}

static void test_reserve_iova(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	const unsigned long reserve_lo = TEST_LIMIT_32BIT / 2;
	struct iova *r, *iova;
	int i;

	/* Reserve the entire top half through the limit_pfn, inclusive. */
	r = reserve_iova(&ctx->iovad, reserve_lo, TEST_LIMIT_32BIT);
	KUNIT_ASSERT_NOT_NULL(test, r);
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	/* All allocs must land below the reserved range. */
	for (i = 0; i < 100; ++i) {
		iova = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_32BIT, false);
		KUNIT_ASSERT_NOT_NULL(test, iova);
		KUNIT_EXPECT_LT(test, iova->pfn_hi, reserve_lo);
	}
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));
}

/*
 * The pci_32bit_workaround scenario: every PCI device's first IOVA
 * allocation hits the 32-bit-restricted path before falling back to
 * 64-bit. Mix the two and verify the limit_pfn-aware augmentation
 * keeps both correct.
 */
static void test_32bit_in_64bit_domain(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *iova;
	int i;

	/* Fill the high 64-bit space. */
	for (i = 0; i < 1000; ++i) {
		iova = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_64BIT, true);
		KUNIT_ASSERT_NOT_NULL(test, iova);
	}
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	/* A 32-bit alloc must still find a slot below DMA_BIT_MASK(32). */
	iova = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_32BIT, true);
	KUNIT_ASSERT_NOT_NULL(test, iova);
	KUNIT_EXPECT_LE(test, iova->pfn_hi, TEST_LIMIT_32BIT);
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	__free_iova(&ctx->iovad, iova);
}

/*
 * Aligned allocation in a fragmented domain: pack size-2 size_aligned
 * allocations at the top, free every other one to leave size-2 holes,
 * then verify a fresh size-2 aligned alloc still succeeds and returns
 * a 2-aligned pfn. The augmented-rbtree invariants must remain
 * consistent throughout.
 */
static void test_aligned_in_fragmented(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	const int N = 64;
	struct iova **iovas;
	struct iova *iova;
	int i;

	iovas = kunit_kcalloc(test, N, sizeof(*iovas), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, iovas);

	for (i = 0; i < N; ++i) {
		iovas[i] = alloc_iova(&ctx->iovad, 2, TEST_LIMIT_32BIT, true);
		KUNIT_ASSERT_NOT_NULL(test, iovas[i]);
		KUNIT_EXPECT_EQ(test, iovas[i]->pfn_lo & 1, 0);
	}
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	/* Free every other to create size-2 gaps interleaved with allocs. */
	for (i = 0; i < N; i += 2) {
		__free_iova(&ctx->iovad, iovas[i]);
		iovas[i] = NULL;
	}
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	iova = alloc_iova(&ctx->iovad, 2, TEST_LIMIT_32BIT, true);
	KUNIT_ASSERT_NOT_NULL(test, iova);
	KUNIT_EXPECT_EQ(test, iova->pfn_lo & 1, 0);
	__free_iova(&ctx->iovad, iova);
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	for (i = 0; i < N; ++i)
		if (iovas[i])
			__free_iova(&ctx->iovad, iovas[i]);
}

/*
 * Mimic dma-iommu's pci_32bit_workaround pattern: every alloc first
 * tries the 32-bit limit; if that fails, retry with the 64-bit limit.
 * Use a deliberately small <=32-bit limit so the 32-bit region is
 * actually exhausted partway through and the 64-bit fallback path is
 * exercised. Verifies that the dual-augmented invariant survives the
 * rapid switching between is_32bit=true and is_32bit=false.
 */
static void test_pci_32bit_workaround_pattern(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	int fallback_count = 0;
	int i;

	for (i = 0; i < 500; ++i) {
		unsigned long size = (i % 4) + 1;
		struct iova *iova = alloc_iova(&ctx->iovad, size,
					       TEST_LIMIT_32BIT_RESTRICTED,
					       true);

		if (!iova) {
			iova = alloc_iova(&ctx->iovad, size,
					  TEST_LIMIT_64BIT, true);
			fallback_count++;
		}
		KUNIT_ASSERT_NOT_NULL(test, iova);
	}
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));
	KUNIT_EXPECT_GT(test, fallback_count, 0);
}

/*
 * Random alloc/free over many iterations, verifying invariants after
 * every operation. Uses a deterministic PRNG so failures reproduce
 * across boots.
 */
static void test_stress_random(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	const int N = 512;
	const int iters = 4 * N;
	struct iova **iovas;
	u32 rng = 0xDEADBEEF;
	int i;

	iovas = kunit_kcalloc(test, N, sizeof(*iovas), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, iovas);

	for (i = 0; i < iters; ++i) {
		int slot;
		bool use_32bit;
		unsigned long limit;
		const char *op;

		rng = rng * 1103515245 + 12345;
		slot = (rng >> 8) % N;
		rng = rng * 1103515245 + 12345;
		use_32bit = rng & 1;
		limit = use_32bit ? TEST_LIMIT_32BIT : TEST_LIMIT_64BIT;

		if (iovas[slot]) {
			op = "free";
			__free_iova(&ctx->iovad, iovas[slot]);
			iovas[slot] = NULL;
		} else {
			unsigned long size;
			bool aligned;

			rng = rng * 1103515245 + 12345;
			size = 1UL << ((rng >> 8) % 4);
			rng = rng * 1103515245 + 12345;
			aligned = rng & 1;

			op = "alloc";
			iovas[slot] = alloc_iova(&ctx->iovad, size, limit,
						 aligned);
		}
		if (!iova_domain_verify_invariants(&ctx->iovad)) {
			kunit_info(test, "iter %d slot %d: invariant broken after %s\n",
				   i, slot, op);
			KUNIT_FAIL(test, "verify failed");
			break;
		}
	}

	for (i = 0; i < N; ++i)
		if (iovas[i])
			__free_iova(&ctx->iovad, iovas[i]);
}

static struct kunit_case iova_test_cases[] = {
	KUNIT_CASE(test_size_aligned),
	KUNIT_CASE(test_top_down_preference),
	KUNIT_CASE(test_reserve_iova),
	KUNIT_CASE(test_32bit_in_64bit_domain),
	KUNIT_CASE(test_aligned_in_fragmented),
	KUNIT_CASE(test_pci_32bit_workaround_pattern),
	KUNIT_CASE(test_stress_random),
	{}
};

static struct kunit_suite iova_test_suite = {
	.name = "iova",
	.init = iova_test_init,
	.exit = iova_test_exit,
	.test_cases = iova_test_cases,
};
kunit_test_suite(iova_test_suite);

MODULE_DESCRIPTION("KUnit tests for the IOVA allocator");
MODULE_LICENSE("GPL");
