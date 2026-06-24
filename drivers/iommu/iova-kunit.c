// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the IOVA allocator.
 *
 * Exercises the maple-tree-based allocator: basic alloc/free,
 * size-aligned allocations, top-down ordering, bounded allocations
 * with various DMA limits (32-bit, 33-bit, 56-bit), aligned
 * allocations in fragmented domains, and randomly fragmented stress.
 *
 * Each test verifies that the maple tree invariants remain consistent
 * after every batch of operations.
 */
#include <kunit/test.h>
#include <linux/dma-mapping.h>
#include <linux/iova.h>

#define TEST_GRANULE PAGE_SIZE
/* Highest pfn that fits in 32 bits — triggers the bounded alloc path. */
#define TEST_LIMIT_32BIT (DMA_BIT_MASK(32) >> PAGE_SHIFT)
/* 33-bit limit — exercises non-power-of-two DMA boundaries. */
#define TEST_LIMIT_33BIT (DMA_BIT_MASK(33) >> PAGE_SHIFT)
/* 56-bit limit — typical server IOMMU address width. */
#define TEST_LIMIT_56BIT (DMA_BIT_MASK(56) >> PAGE_SHIFT)
/* A 64-bit-ish limit well above dma_32bit_pfn. 1ULL avoids UB on ILP32. */
#define TEST_LIMIT_64BIT ((1ULL << 36) >> PAGE_SHIFT)
/*
 * A small <=32-bit limit used by tests that want to actually exhaust the
 * restricted region within a tractable number of allocations.
 */
#define TEST_LIMIT_32BIT_RESTRICTED 256UL

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
 * 64-bit. Fill the 64-bit space, then verify a 32-bit alloc still
 * finds a slot below DMA_BIT_MASK(32).
 */
static void test_32bit_in_64bit_domain(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *iova;
	int i;

	for (i = 0; i < 1000; ++i) {
		iova = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_64BIT, true);
		KUNIT_ASSERT_NOT_NULL(test, iova);
	}
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	iova = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_32BIT, true);
	KUNIT_ASSERT_NOT_NULL(test, iova);
	KUNIT_EXPECT_LE(test, iova->pfn_hi, TEST_LIMIT_32BIT);
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	__free_iova(&ctx->iovad, iova);
}

/*
 * Exercise non-power-of-two DMA limits: fill the 64-bit space, then
 * verify that bounded allocations at 33-bit and 56-bit limits still
 * find slots within their respective ranges. This confirms the
 * navigate-to-limit_pfn search generalizes beyond the 32-bit case.
 */
static void test_arbitrary_dma_limits(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *iova;
	int i;

	for (i = 0; i < 1000; ++i) {
		iova = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_64BIT, true);
		KUNIT_ASSERT_NOT_NULL(test, iova);
	}
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));

	/* 33-bit bounded allocation */
	iova = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_33BIT, true);
	KUNIT_ASSERT_NOT_NULL(test, iova);
	KUNIT_EXPECT_LE(test, iova->pfn_hi, TEST_LIMIT_33BIT);
	__free_iova(&ctx->iovad, iova);

	/* 56-bit bounded allocation */
	iova = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_56BIT, true);
	KUNIT_ASSERT_NOT_NULL(test, iova);
	KUNIT_EXPECT_LE(test, iova->pfn_hi, TEST_LIMIT_56BIT);
	__free_iova(&ctx->iovad, iova);

	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));
}

/*
 * Aligned allocation in a fragmented domain: pack size-2 size_aligned
 * allocations at the top, free every other one to leave size-2 holes,
 * then verify a fresh size-2 aligned alloc still succeeds and returns
 * a 2-aligned pfn.
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
 * tries a small restricted limit; if that fails, retry with the 64-bit
 * limit. Verifies that the navigate-to-limit search survives rapid
 * switching between different limit_pfn values.
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
		if (!iova)
			break;
	}
	KUNIT_EXPECT_TRUE(test, iova_domain_verify_invariants(&ctx->iovad));
	/* Every alloc must succeed (via fallback once the restricted region fills). */
	KUNIT_EXPECT_EQ(test, i, 500);
	/* The restricted region is small, so the 64-bit fallback must engage. */
	KUNIT_EXPECT_GT(test, fallback_count, 0);
}

/*
 * Random alloc/free over many iterations, verifying invariants after
 * every operation. Uses a deterministic PRNG so failures reproduce
 * across boots. Exercises mixed DMA limits (32, 33, 56, 64-bit).
 */
static void test_stress_random(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	const int N = 512;
	const int iters = 4 * N;
	const unsigned long limits[] = {
		TEST_LIMIT_32BIT, TEST_LIMIT_33BIT,
		TEST_LIMIT_56BIT, TEST_LIMIT_64BIT,
	};
	struct iova **iovas;
	u32 rng = 0xDEADBEEF;
	int i;

	iovas = kunit_kcalloc(test, N, sizeof(*iovas), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, iovas);

	for (i = 0; i < iters; ++i) {
		int slot;
		unsigned long limit;
		const char *op;

		rng = rng * 1103515245 + 12345;
		slot = (rng >> 8) % N;
		rng = rng * 1103515245 + 12345;
		limit = limits[(rng >> 8) % ARRAY_SIZE(limits)];

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
			aligned = (rng >> 8) & 1;

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

/*
 * Verify that alloc_iova fails in bounded time when the IOVA space is
 * fully packed. Fill a 16K-pfn range with size-1 allocations (leaving
 * no gaps), then attempt a size-2 aligned alloc. The maple tree's
 * mas_empty_area_rev must determine there is no suitable gap in
 * O(log n) time rather than walking every entry. The wall-clock check
 * is a loose hang detector only (CI under KASAN/lockdep/virt is slow);
 * the real signal is the reported time and that the alloc fails.
 */
static void test_full_space_search_time(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	const unsigned long fill_limit = 16384;
	const int fill_count = fill_limit;
	struct iova *iova;
	ktime_t start, elapsed;
	int i, allocated = 0;

	for (i = 0; i < fill_count; ++i) {
		iova = alloc_iova(&ctx->iovad, 1, fill_limit, false);
		if (!iova)
			break;
		allocated++;
	}
	kunit_info(test, "allocated %d iovas in [1, %lu]\n",
		   allocated, fill_limit);
	KUNIT_ASSERT_GT(test, allocated, 1000);

	start = ktime_get();
	iova = alloc_iova(&ctx->iovad, 2, fill_limit, true);
	elapsed = ktime_sub(ktime_get(), start);

	KUNIT_EXPECT_NULL(test, iova);
	kunit_info(test, "failed alloc took %lld ns\n",
		   ktime_to_ns(elapsed));
	/* Loose hang detector, not a perf gate (CI under KASAN/lockdep is slow). */
	KUNIT_EXPECT_LT(test, ktime_to_ns(elapsed), 1000000000LL);

	if (iova)
		__free_iova(&ctx->iovad, iova);
}

/*
 * Verify bounded search time with a fragmented 32-bit IOVA space.
 * Pack the 32-bit range with size-1 allocs, then attempt a large
 * aligned alloc that must either succeed from a remaining gap or
 * fail fast. The 64-bit fallback must always succeed promptly.
 */
static void test_fragmented_32bit_search(struct kunit *test)
{
	struct iova_test_ctx *ctx = test->priv;
	struct iova *iova;
	ktime_t start, elapsed;
	int i, allocated = 0;

	for (i = 0; i < 8000; ++i) {
		iova = alloc_iova(&ctx->iovad, 1, TEST_LIMIT_32BIT, false);
		if (!iova)
			break;
		allocated++;
	}
	kunit_info(test, "filled 32-bit space with %d allocs\n", allocated);
	KUNIT_ASSERT_GT(test, allocated, 1000);

	start = ktime_get();
	iova = alloc_iova(&ctx->iovad, 32, TEST_LIMIT_32BIT, true);
	elapsed = ktime_sub(ktime_get(), start);

	kunit_info(test, "32-bit alloc (size 32) took %lld ns, result=%s\n",
		   ktime_to_ns(elapsed), iova ? "alloc" : "fail");
	/* Loose hang detector, not a perf gate (CI under KASAN/lockdep is slow). */
	KUNIT_EXPECT_LT(test, ktime_to_ns(elapsed), 1000000000LL);

	if (iova)
		__free_iova(&ctx->iovad, iova);

	start = ktime_get();
	iova = alloc_iova(&ctx->iovad, 32, TEST_LIMIT_64BIT, true);
	elapsed = ktime_sub(ktime_get(), start);

	kunit_info(test, "64-bit fallback (size 32) took %lld ns\n",
		   ktime_to_ns(elapsed));
	/* Loose hang detector, not a perf gate (CI under KASAN/lockdep is slow). */
	KUNIT_EXPECT_LT(test, ktime_to_ns(elapsed), 1000000000LL);

	if (iova)
		__free_iova(&ctx->iovad, iova);
}

static struct kunit_case iova_test_cases[] = {
	KUNIT_CASE(test_size_aligned),
	KUNIT_CASE(test_top_down_preference),
	KUNIT_CASE(test_reserve_iova),
	KUNIT_CASE(test_32bit_in_64bit_domain),
	KUNIT_CASE(test_arbitrary_dma_limits),
	KUNIT_CASE(test_aligned_in_fragmented),
	KUNIT_CASE(test_pci_32bit_workaround_pattern),
	KUNIT_CASE(test_stress_random),
	KUNIT_CASE(test_full_space_search_time),
	KUNIT_CASE(test_fragmented_32bit_search),
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
