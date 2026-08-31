// SPDX-License-Identifier: GPL-2.0+
/*
 * Test cases for minmax in_range_inclusive() helpers.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <kunit/test.h>
#include <linux/minmax.h>

/*
 * When @min <= @max is valid in:
 *        Valid in reading   Example    'True' range is
 * Case 1 Both readings      [5, 10]     interval as written in either reading
 * Case 2 Signed only        [-10, 5]    signed interval
 * Case 3 Unsigned only      [5, -10]    unsigned interval
 * Case 4 Neither reading    [-5, -10]   neither; all values except [unsigned(-9), unsigned(-6)]
 *
 * Visualizing the wraparound-continuous unsigned number scale as a full circle with
 * 0 at 12 o'clock and S32_MAX/S64_MAX at 6 o'clock, with ranges being calculated
 * only clockwise starting from @min until @max might help greatly in understanding
 * the following true and false ranges.
 */
static void u32_tests(struct kunit *test)
{
	/* Case 1 first true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(5, 5, 10));
	/* Case 1 last true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(10, 5, 10));
	/* Case 1 first false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(11, 5, 10));
	/* Case 1 last false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(4, 5, 10));
	/* Case 1 0 not in range */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(0, 5, 10));
	/* Case 1 U32_MAX not in range */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(U32_MAX, 5, 10));

	/* Case 2 first true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(-10, -10, 5));
	/* Case 2 last true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(5, -10, 5));
	/* Case 2 first false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(6, -10, 5));
	/* Case 2 last false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(-11, -10, 5));
	/* Case 2 0 in range */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(0, -10, 5));
	/* Case 2 U32_MAX in range */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(U32_MAX, -10, 5));

	/* Case 3 first true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(5, 5, -10));
	/* Case 3 last true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(-10, 5, -10));
	/* Case 3 first false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(-9, 5, -10));
	/* Case 3 last false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(4, 5, -10));
	/* Case 3 0 not in range */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(0, 5, -10));
	/* Case 3 U32_MAX not in range */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(U32_MAX, 5, -10));

	/* Case 4 first true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(-5, -5, -10));
	/* Case 4 last true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(-10, -5, -10));
	/* Case 4 first false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(-9, -5, -10));
	/* Case 4 last false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(-6, -5, -10));
	/* Case 4 0 in range */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(0, -5, -10));
	/* Case 4 U32_MAX in range */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(U32_MAX, -5, -10));
}

static void u64_tests(struct kunit *test)
{
	/* Case 1 first true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(5ULL, 5ULL, 10ULL));
	/* Case 1 last true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(10ULL, 5ULL, 10ULL));
	/* Case 1 first false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(11ULL, 5ULL, 10ULL));
	/* Case 1 last false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(4ULL, 5ULL, 10ULL));
	/* Case 1 0 not in range */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(0ULL, 5ULL, 10ULL));
	/* Case 1 U64_MAX not in range */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(U64_MAX, 5ULL, 10ULL));

	/* Case 2 first true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(-10ULL, -10ULL, 5ULL));
	/* Case 2 last true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(5ULL, -10ULL, 5ULL));
	/* Case 2 first false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(6ULL, -10ULL, 5ULL));
	/* Case 2 last false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(-11ULL, -10ULL, 5ULL));
	/* Case 2 0 in range */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(0ULL, -10ULL, 5ULL));
	/* Case 2 U64_MAX in range */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(U64_MAX, -10ULL, 5ULL));

	/* Case 3 first true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(5ULL, 5ULL, -10ULL));
	/* Case 3 last true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(-10ULL, 5ULL, -10ULL));
	/* Case 3 first false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(-9ULL, 5ULL, -10ULL));
	/* Case 3 last false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(4ULL, 5ULL, -10ULL));
	/* Case 3 0 not in range */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(0ULL, 5ULL, -10ULL));
	/* Case 3 U64_MAX not in range */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(U64_MAX, 5ULL, -10ULL));

	/* Case 4 first true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(-5ULL, -5ULL, -10ULL));
	/* Case 4 last true value */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(-10ULL, -5ULL, -10ULL));
	/* Case 4 first false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(-9ULL, -5ULL, -10ULL));
	/* Case 4 last false value */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(-6ULL, -5ULL, -10ULL));
	/* Case 4 0 in range */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(0ULL, -5ULL, -10ULL));
	/* Case 4 U64_MAX in range */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(U64_MAX, -5ULL, -10ULL));
}

/* Check whether sizeof logic to select 32- vs 64-bit comparisons works */
static void sizeof_logic_tests(struct kunit *test)
{
	u64 u64_val    = BIT_ULL(32) | 7;
	u64 u64_minval = BIT_ULL(32) | 5;
	u64 u64_maxval = BIT_ULL(32) | 10;
	u32 u32_minval = 5, u32_maxval = 10, u32_val = 100;

	/* If sizeof trick does not work:
	 * - u64_val will get truncated to 7
	 * - 7 is in [5, 10] so test should pass.
	 */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(u64_val, u32_minval, u32_maxval));

	/*
	 * If sizeof trick does not work:
	 * - u64_maxval will get truncated to 10
	 * - 100 is not in [5, 10], so test should fail.
	 */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(u32_val, u32_minval, u64_maxval));

	/*
	 * If sizeof trick does not work:
	 * - u64_minval gets truncated to 5
	 * - 0 is not in [5, 10], so test should fail.
	 * Otherwise:
	 * - @min > @max here, so this is Case 4
	 * - 0 should be in range and test should pass.
	 */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(0, u64_minval, u32_maxval));
}

/*
 * Each argument is widened to the selected width according to its own type:
 * signed arguments sign-extend, unsigned ones zero-extend. The same written
 * values can therefore give opposite answers depending on the declared types.
 */
static void sign_extension_tests(struct kunit *test)
{
	u32 u32_val = -9;
	s32 s32_val = -9;
	s32 s32_minval = 5, s32_maxval = -10;
	s64 s64_minval = 5, s64_maxval = -10;

	/* Case 3 range. -9 is out of range in both readings */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(u32_val, s32_minval, s32_maxval));
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(s32_val, s32_minval, s32_maxval));

	/*
	 * Sign extension kicks in due to sizeof logic. minval "stays in place"
	 * because it is positive and maxval "moves" because it is negative and
	 * signed, thereby increasing the True range.
	 * Negative unsigned -9 "stays in place", so it becomes in range.
	 * Negative signed -9 "moves", so it still remains out of range.
	 */
	KUNIT_EXPECT_TRUE(test, in_range_inclusive(u32_val, s64_minval, s64_maxval));
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(s32_val, s64_minval, s64_maxval));
}

#define TEST_RANGE_MIN 0
#define TEST_RANGE_MAX 100
static void misc_tests(struct kunit *test)
{
	s32 s32_val = -5;

	/*
	 * Test common device driver use case of rejecting negative input from
	 * userspace.
	 */
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(s32_val, TEST_RANGE_MIN, TEST_RANGE_MAX));

	/* If min == max, only one true solution exists, val = min */
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(5, 5, 5));
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(0, 5, 5));
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(U32_MAX, 5, 5));
	KUNIT_EXPECT_FALSE(test, in_range_inclusive(U64_MAX, 5, 5));

	/*
	 * When max = (min - 1), Case 4 kicks in, and there is no 'false' range,
	 * i.e. all values of val result in 'true'.
	 */
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(5, 5, 4));
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(4, 5, 4));
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(0, 5, 4));
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(U32_MAX, 5, 4));
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(U64_MAX, 5, 4));

	/*
	 * [0, U32_MAX] and [0, U64_MAX] are both the same case as above with
	 * min = 0 and max = (min - 1)
	 */
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(0, 0, U32_MAX));
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(U32_MAX, 0, U32_MAX));
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(0, 0, U64_MAX));
	KUNIT_EXPECT_TRUE(test,  in_range_inclusive(U64_MAX, 0, U64_MAX));
}

static struct kunit_case in_range_incl_test_cases[] = {
	KUNIT_CASE(u32_tests),
	KUNIT_CASE(u64_tests),
	KUNIT_CASE(sizeof_logic_tests),
	KUNIT_CASE(sign_extension_tests),
	KUNIT_CASE(misc_tests),
	{}
};

static struct kunit_suite in_range_incl_test_suite = {
	.name = "in_range_inclusive",
	.test_cases = in_range_incl_test_cases,
};

kunit_test_suites(&in_range_incl_test_suite);

MODULE_AUTHOR("Guru Das Srinagesh <linux@gurudas.dev>");
MODULE_DESCRIPTION("Test cases for in_range_inclusive()");
MODULE_LICENSE("GPL");
