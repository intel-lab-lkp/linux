// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for dual-bitmap primitives
 *
 * Tests the dual-bitmap consistency checking algorithm used by the page
 * consistency checker. These tests verify the core invariant maintenance
 * and corruption detection logic.
 */

#include <kunit/test.h>
#include <linux/dual_bitmap.h>

#define TEST_BITMAP_BITS 256

struct dual_bitmap_test_context {
	struct dual_bitmap db;
	unsigned long primary[BITS_TO_LONGS(TEST_BITMAP_BITS)];
	unsigned long secondary[BITS_TO_LONGS(TEST_BITMAP_BITS)];
};

static int dual_bitmap_test_init(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->db.bitmap[DUAL_BITMAP_PRIMARY] = ctx->primary;
	ctx->db.bitmap[DUAL_BITMAP_SECONDARY] = ctx->secondary;
	ctx->db.nbits = TEST_BITMAP_BITS;

	/* Initialize: primary all zeros, secondary all ones */
	dual_bitmap_init(&ctx->db);

	test->priv = ctx;
	return 0;
}

static void test_initial_state_consistent(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;
	unsigned long violations;

	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_EQ(test, violations, 0UL);
}

static void test_set_maintains_consistency(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;
	unsigned long violations;
	bool was_set;

	/* Set bit 42 */
	was_set = dual_bitmap_set(&ctx->db, 42);
	KUNIT_EXPECT_FALSE(test, was_set);

	/* Verify consistency */
	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_EQ(test, violations, 0UL);

	/* Verify individual bit consistency */
	KUNIT_EXPECT_TRUE(test, dual_bitmap_consistent(&ctx->db, 42));
}

static void test_clear_maintains_consistency(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;
	unsigned long violations;
	bool was_set;

	/* First set the bit */
	dual_bitmap_set(&ctx->db, 100);

	/* Now clear it */
	was_set = dual_bitmap_clear(&ctx->db, 100);
	KUNIT_EXPECT_TRUE(test, was_set);

	/* Verify consistency */
	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_EQ(test, violations, 0UL);
}

static void test_double_set_detected(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;
	bool was_set;

	/* Set bit 50 */
	was_set = dual_bitmap_set(&ctx->db, 50);
	KUNIT_EXPECT_FALSE(test, was_set);

	/* Try to set it again - should report it was already set */
	was_set = dual_bitmap_set(&ctx->db, 50);
	KUNIT_EXPECT_TRUE(test, was_set);
}

static void test_double_clear_detected(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;
	bool was_set;

	/* Clear bit 60 which is already clear (never set) */
	was_set = dual_bitmap_clear(&ctx->db, 60);
	KUNIT_EXPECT_FALSE(test, was_set);
}

static void test_corruption_in_primary_detected(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;
	unsigned long violations;

	/* Corrupt the primary bitmap directly */
	set_bit(75, ctx->primary);

	/* Validation should detect the corruption */
	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_GT(test, violations, 0UL);

	/* Individual bit check should also fail */
	KUNIT_EXPECT_FALSE(test, dual_bitmap_consistent(&ctx->db, 75));
}

static void test_corruption_in_secondary_detected(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;
	unsigned long violations;

	/* Corrupt the secondary bitmap directly */
	clear_bit(80, ctx->secondary);

	/* Validation should detect the corruption */
	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_GT(test, violations, 0UL);

	/* Individual bit check should also fail */
	KUNIT_EXPECT_FALSE(test, dual_bitmap_consistent(&ctx->db, 80));
}

static void test_multiple_operations(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;
	unsigned long violations;
	unsigned long i;

	/* Set bits 0-63 */
	for (i = 0; i < 64; i++)
		dual_bitmap_set(&ctx->db, i);

	/* Clear bits 32-63 */
	for (i = 32; i < 64; i++)
		dual_bitmap_clear(&ctx->db, i);

	/* Validate entire bitmap */
	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_EQ(test, violations, 0UL);

	/* Verify expected state: bits 0-31 set, rest clear */
	for (i = 0; i < 32; i++)
		KUNIT_EXPECT_TRUE(test, test_bit(i, ctx->primary));
	for (i = 32; i < TEST_BITMAP_BITS; i++)
		KUNIT_EXPECT_FALSE(test, test_bit(i, ctx->primary));
}

static void test_boundary_bits(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;
	unsigned long violations;

	/* Test first bit */
	dual_bitmap_set(&ctx->db, 0);
	KUNIT_EXPECT_TRUE(test, dual_bitmap_consistent(&ctx->db, 0));

	/* Test last bit */
	dual_bitmap_set(&ctx->db, TEST_BITMAP_BITS - 1);
	KUNIT_EXPECT_TRUE(test, dual_bitmap_consistent(&ctx->db, TEST_BITMAP_BITS - 1));

	/* Test word boundary (last bit of first word / first bit of second word) */
	dual_bitmap_set(&ctx->db, BITS_PER_LONG - 1);
	dual_bitmap_set(&ctx->db, BITS_PER_LONG);
	KUNIT_EXPECT_TRUE(test, dual_bitmap_consistent(&ctx->db, BITS_PER_LONG - 1));
	KUNIT_EXPECT_TRUE(test, dual_bitmap_consistent(&ctx->db, BITS_PER_LONG));

	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_EQ(test, violations, 0UL);
}

static void test_dual_bitmap_test_func(struct kunit *test)
{
	struct dual_bitmap_test_context *ctx = test->priv;

	/* Initially all bits should be clear (not allocated) */
	KUNIT_EXPECT_FALSE(test, dual_bitmap_test(&ctx->db, 10));

	/* After setting, bit should be set */
	dual_bitmap_set(&ctx->db, 10);
	KUNIT_EXPECT_TRUE(test, dual_bitmap_test(&ctx->db, 10));

	/* After clearing, bit should be clear again */
	dual_bitmap_clear(&ctx->db, 10);
	KUNIT_EXPECT_FALSE(test, dual_bitmap_test(&ctx->db, 10));
}

/* Test with non-word-aligned nbits to exercise partial-word handling */
#define TEST_UNALIGNED_BITS 100  /* not a multiple of BITS_PER_LONG */

struct dual_bitmap_unaligned_context {
	struct dual_bitmap db;
	unsigned long primary[BITS_TO_LONGS(TEST_UNALIGNED_BITS)];
	unsigned long secondary[BITS_TO_LONGS(TEST_UNALIGNED_BITS)];
};

static void test_non_aligned_nbits(struct kunit *test)
{
	struct dual_bitmap_unaligned_context *ctx;
	unsigned long violations;
	unsigned long i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	ctx->db.bitmap[DUAL_BITMAP_PRIMARY] = ctx->primary;
	ctx->db.bitmap[DUAL_BITMAP_SECONDARY] = ctx->secondary;
	ctx->db.nbits = TEST_UNALIGNED_BITS;

	dual_bitmap_init(&ctx->db);

	/* Initial state should be consistent */
	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_EQ(test, violations, 0UL);

	/* Set and clear bits near the non-aligned boundary */
	for (i = TEST_UNALIGNED_BITS - 5; i < TEST_UNALIGNED_BITS; i++) {
		dual_bitmap_set(&ctx->db, i);
		KUNIT_EXPECT_TRUE(test, dual_bitmap_consistent(&ctx->db, i));
	}

	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_EQ(test, violations, 0UL);

	/* Clear them back */
	for (i = TEST_UNALIGNED_BITS - 5; i < TEST_UNALIGNED_BITS; i++)
		dual_bitmap_clear(&ctx->db, i);

	violations = dual_bitmap_validate(&ctx->db);
	KUNIT_EXPECT_EQ(test, violations, 0UL);
}

static struct kunit_case dual_bitmap_test_cases[] = {
	KUNIT_CASE(test_initial_state_consistent),
	KUNIT_CASE(test_set_maintains_consistency),
	KUNIT_CASE(test_clear_maintains_consistency),
	KUNIT_CASE(test_double_set_detected),
	KUNIT_CASE(test_double_clear_detected),
	KUNIT_CASE(test_corruption_in_primary_detected),
	KUNIT_CASE(test_corruption_in_secondary_detected),
	KUNIT_CASE(test_multiple_operations),
	KUNIT_CASE(test_boundary_bits),
	KUNIT_CASE(test_dual_bitmap_test_func),
	KUNIT_CASE(test_non_aligned_nbits),
	{},
};

static struct kunit_suite dual_bitmap_test_suite = {
	.name = "dual_bitmap",
	.init = dual_bitmap_test_init,
	.test_cases = dual_bitmap_test_cases,
};

kunit_test_suites(&dual_bitmap_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for dual-bitmap consistency primitives");
