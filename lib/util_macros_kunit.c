// SPDX-License-Identifier: GPL-2.0+
/*
 * Test cases for bitfield helpers.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <kunit/test.h>
#include <linux/util_macros.h>

#define FIND_CLOSEST_RANGE_CHECK(val_from, val_to, array, exp_idx)	\
{									\
	int i;								\
	for (i = val_from; i <= val_to; i++) {				\
		int found = find_closest(i, array, ARRAY_SIZE(array));	\
		KUNIT_ASSERT_EQ(ctx, exp_idx, found);			\
	}								\
}

static void test_find_closest(struct kunit *ctx)
{
	/* This will test a few arrays that are found in drivers */
	static const int ina226_avg_tab[] = { 1, 4, 16, 64, 128, 256, 512, 1024 };
	static const unsigned int ad7616_oversampling_avail[] = {
		1, 2, 4, 8, 16, 32, 64, 128,
	};
	static u32 wd_timeout_table[] = { 2, 4, 6, 8, 16, 32, 48, 64 };
	static int array_prog3a[] = { 1, 4, 7, 10 };
	static int array_prog3b[] = { 2, 5, 8, 11 };
	static int array_prog4a[] = { 1, 5, 9, 13 };
	static int array_prog4b[] = { 2, 6, 10, 14 };

	FIND_CLOSEST_RANGE_CHECK(-10, 2, ina226_avg_tab, 0);
	FIND_CLOSEST_RANGE_CHECK(3, 10, ina226_avg_tab, 1);
	FIND_CLOSEST_RANGE_CHECK(11, 40, ina226_avg_tab, 2);
	FIND_CLOSEST_RANGE_CHECK(41, 96, ina226_avg_tab, 3);
	FIND_CLOSEST_RANGE_CHECK(97, 192, ina226_avg_tab, 4);
	FIND_CLOSEST_RANGE_CHECK(193, 384, ina226_avg_tab, 5);
	FIND_CLOSEST_RANGE_CHECK(385, 768, ina226_avg_tab, 6);
	FIND_CLOSEST_RANGE_CHECK(769, 2048, ina226_avg_tab, 7);

	/* The array that found the bug that caused this kunit to exist */
	FIND_CLOSEST_RANGE_CHECK(-10, 1, ad7616_oversampling_avail, 0);
	FIND_CLOSEST_RANGE_CHECK(2, 3, ad7616_oversampling_avail, 1);
	FIND_CLOSEST_RANGE_CHECK(4, 6, ad7616_oversampling_avail, 2);
	FIND_CLOSEST_RANGE_CHECK(7, 12, ad7616_oversampling_avail, 3);
	FIND_CLOSEST_RANGE_CHECK(13, 24, ad7616_oversampling_avail, 4);
	FIND_CLOSEST_RANGE_CHECK(25, 48, ad7616_oversampling_avail, 5);
	FIND_CLOSEST_RANGE_CHECK(49, 96, ad7616_oversampling_avail, 6);
	FIND_CLOSEST_RANGE_CHECK(97, 256, ad7616_oversampling_avail, 7);

	FIND_CLOSEST_RANGE_CHECK(0, 3, wd_timeout_table, 0);
	FIND_CLOSEST_RANGE_CHECK(4, 5, wd_timeout_table, 1);
	FIND_CLOSEST_RANGE_CHECK(6, 7, wd_timeout_table, 2);
	FIND_CLOSEST_RANGE_CHECK(8, 12, wd_timeout_table, 3);
	FIND_CLOSEST_RANGE_CHECK(13, 24, wd_timeout_table, 4);
	FIND_CLOSEST_RANGE_CHECK(25, 40, wd_timeout_table, 5);
	FIND_CLOSEST_RANGE_CHECK(41, 56, wd_timeout_table, 6);
	FIND_CLOSEST_RANGE_CHECK(57, 128, wd_timeout_table, 7);

	FIND_CLOSEST_RANGE_CHECK(-1, 2, array_prog3a, 0);
	FIND_CLOSEST_RANGE_CHECK(3, 5, array_prog3a, 1);
	FIND_CLOSEST_RANGE_CHECK(6, 8, array_prog3a, 2);
	FIND_CLOSEST_RANGE_CHECK(9, 20, array_prog3a, 3);

	FIND_CLOSEST_RANGE_CHECK(-1, 3, array_prog3b, 0);
	FIND_CLOSEST_RANGE_CHECK(4, 6, array_prog3b, 1);
	FIND_CLOSEST_RANGE_CHECK(7, 9, array_prog3b, 2);
	FIND_CLOSEST_RANGE_CHECK(10, 20, array_prog3b, 3);

	FIND_CLOSEST_RANGE_CHECK(-1, 3, array_prog4a, 0);
	FIND_CLOSEST_RANGE_CHECK(4, 7, array_prog4a, 1);
	FIND_CLOSEST_RANGE_CHECK(8, 11, array_prog4a, 2);
	FIND_CLOSEST_RANGE_CHECK(12, 20, array_prog4a, 3);

	FIND_CLOSEST_RANGE_CHECK(-1, 4, array_prog4b, 0);
	FIND_CLOSEST_RANGE_CHECK(5, 8, array_prog4b, 1);
	FIND_CLOSEST_RANGE_CHECK(9, 12, array_prog4b, 2);
	FIND_CLOSEST_RANGE_CHECK(13, 20, array_prog4b, 3);
}

#define FIND_CLOSEST_DESC_RANGE_CHECK(val_from, val_to, array, exp_idx)	\
{									\
	int i;								\
	for (i = val_from; i <= val_to; i++) {				\
		int found = find_closest_descending(i, array,		\
						ARRAY_SIZE(array));	\
		KUNIT_ASSERT_EQ(ctx, exp_idx, found);			\
	}								\
}

static void test_find_closest_descending(struct kunit *ctx)
{
	/* Same arrays as 'test_find_closest' but reversed */
	static const int ina226_avg_tab[] = { 1024, 512, 256, 128, 64, 16, 4, 1 };
	static const unsigned int ad7616_oversampling_avail[] = {
		128, 64, 32, 16, 8, 4, 2, 1
	};
	static u32 wd_timeout_table[] = { 64, 48, 32, 16, 8, 6, 4, 2 };
	static int array_prog3a[] = { 10, 7, 4, 1 };
	static int array_prog3b[] = { 11, 8, 5, 2 };
	static int array_prog4a[] = { 13, 9, 5, 1 };
	static int array_prog4b[] = { 14, 10, 6, 2 };

	FIND_CLOSEST_DESC_RANGE_CHECK(-10, 2, ina226_avg_tab, 7);
	FIND_CLOSEST_DESC_RANGE_CHECK(3, 10, ina226_avg_tab, 6);
	FIND_CLOSEST_DESC_RANGE_CHECK(11, 40, ina226_avg_tab, 5);
	FIND_CLOSEST_DESC_RANGE_CHECK(41, 96, ina226_avg_tab, 4);
	FIND_CLOSEST_DESC_RANGE_CHECK(97, 192, ina226_avg_tab, 3);
	FIND_CLOSEST_DESC_RANGE_CHECK(193, 384, ina226_avg_tab, 2);
	FIND_CLOSEST_DESC_RANGE_CHECK(385, 768, ina226_avg_tab, 1);
	FIND_CLOSEST_DESC_RANGE_CHECK(769, 2048, ina226_avg_tab, 0);

	FIND_CLOSEST_DESC_RANGE_CHECK(-10, 1, ad7616_oversampling_avail, 7);
	FIND_CLOSEST_DESC_RANGE_CHECK(2, 3, ad7616_oversampling_avail, 6);
	FIND_CLOSEST_DESC_RANGE_CHECK(4, 6, ad7616_oversampling_avail, 5);
	FIND_CLOSEST_DESC_RANGE_CHECK(7, 12, ad7616_oversampling_avail, 4);
	FIND_CLOSEST_DESC_RANGE_CHECK(13, 24, ad7616_oversampling_avail, 3);
	FIND_CLOSEST_DESC_RANGE_CHECK(25, 48, ad7616_oversampling_avail, 2);
	FIND_CLOSEST_DESC_RANGE_CHECK(49, 96, ad7616_oversampling_avail, 1);
	FIND_CLOSEST_DESC_RANGE_CHECK(97, 256, ad7616_oversampling_avail, 0);

	FIND_CLOSEST_DESC_RANGE_CHECK(0, 3, wd_timeout_table, 7);
	FIND_CLOSEST_DESC_RANGE_CHECK(4, 5, wd_timeout_table, 6);
	FIND_CLOSEST_DESC_RANGE_CHECK(6, 7, wd_timeout_table, 5);
	FIND_CLOSEST_DESC_RANGE_CHECK(8, 12, wd_timeout_table, 4);
	FIND_CLOSEST_DESC_RANGE_CHECK(13, 24, wd_timeout_table, 3);
	FIND_CLOSEST_DESC_RANGE_CHECK(25, 40, wd_timeout_table, 2);
	FIND_CLOSEST_DESC_RANGE_CHECK(41, 56, wd_timeout_table, 1);
	FIND_CLOSEST_DESC_RANGE_CHECK(57, 128, wd_timeout_table, 0);

	FIND_CLOSEST_DESC_RANGE_CHECK(-1, 2, array_prog3a, 3);
	FIND_CLOSEST_DESC_RANGE_CHECK(3, 5, array_prog3a, 2);
	FIND_CLOSEST_DESC_RANGE_CHECK(6, 8, array_prog3a, 1);
	FIND_CLOSEST_DESC_RANGE_CHECK(9, 20, array_prog3a, 0);

	FIND_CLOSEST_DESC_RANGE_CHECK(-1, 3, array_prog3b, 3);
	FIND_CLOSEST_DESC_RANGE_CHECK(4, 6, array_prog3b, 2);
	FIND_CLOSEST_DESC_RANGE_CHECK(7, 9, array_prog3b, 1);
	FIND_CLOSEST_DESC_RANGE_CHECK(10, 20, array_prog3b, 0);

	FIND_CLOSEST_DESC_RANGE_CHECK(-1, 3, array_prog4a, 3);
	FIND_CLOSEST_DESC_RANGE_CHECK(4, 7, array_prog4a, 2);
	FIND_CLOSEST_DESC_RANGE_CHECK(8, 11, array_prog4a, 1);
	FIND_CLOSEST_DESC_RANGE_CHECK(12, 20, array_prog4a, 0);

	FIND_CLOSEST_DESC_RANGE_CHECK(-1, 4, array_prog4b, 3);
	FIND_CLOSEST_DESC_RANGE_CHECK(5, 8, array_prog4b, 2);
	FIND_CLOSEST_DESC_RANGE_CHECK(9, 12, array_prog4b, 1);
	FIND_CLOSEST_DESC_RANGE_CHECK(13, 20, array_prog4b, 0);
}

static struct kunit_case __refdata util_macros_test_cases[] = {
	KUNIT_CASE(test_find_closest),
	KUNIT_CASE(test_find_closest_descending),
	{}
};

static struct kunit_suite util_macros_test_suite = {
	.name = "util_macros.h",
	.test_cases = util_macros_test_cases,
};

kunit_test_suites(&util_macros_test_suite);

MODULE_AUTHOR("Alexandru Ardelean <aardelean@baylibre.com>");
MODULE_DESCRIPTION("Test cases for util_macros.h helpers");
MODULE_LICENSE("GPL");
