// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unit tests for IIO fixed-point functions
 *
 * Copyright (c) 2025 BayLibre, SAS
 */

#include <kunit/test.h>
#include <linux/iio/iio.h>
#include <linux/math.h>
#include <linux/types.h>

static void iio_test_iio_str_to_fixed_point(struct kunit *test)
{
	int int_part, fract_part;
	int ret;

	/* Positive value > 1 */
	ret = iio_str_to_fixpoint("1.234", 100, &int_part, &fract_part);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, int_part * 1000 + fract_part, 1234);

	/* Truncates rather than rounding closest. */
	ret = iio_str_to_fixpoint("1.234567", 100, &int_part, &fract_part);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, int_part * 1000 + fract_part, 1234);

	/* Positive value < 1 */
	ret = iio_str_to_fixpoint("0.001", 100, &int_part, &fract_part);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, int_part * 1000 + fract_part, 1);

	/* Negative value > -1 */
	ret = iio_str_to_fixpoint("-0.001", 100, &int_part, &fract_part);
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, int_part * 1000 + fract_part, -1);

	/* Negative value < -1 */
	ret = iio_str_to_fixpoint("-1.001", 100, &int_part, &fract_part);
	KUNIT_EXPECT_EQ(test, 0, ret);
	/* The fractional part is subtracted rather than added in this case! */
	KUNIT_EXPECT_EQ(test, int_part * 1000 - fract_part, -1001);
}

static const struct kunit_case iio_fixed_point_test_cases[] = {
	KUNIT_CASE(iio_test_iio_str_to_fixed_point),
	{ }
};

static struct kunit_suite iio_fixed_point_test_suite = {
	.name = "iio-fixed-point",
	.test_cases = iio_fixed_point_test_cases,
};
kunit_test_suite(iio_fixed_point_test_suite);

MODULE_AUTHOR("David Lechner <dlechner@baylibre.com>");
MODULE_DESCRIPTION("Test IIO fixed-point functions");
MODULE_LICENSE("GPL");
