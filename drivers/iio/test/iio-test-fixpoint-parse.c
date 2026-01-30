// SPDX-License-Identifier: GPL-2.0-only
/* Unit tests for IIO fixpoint parsing functions
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <kunit/test.h>
#include <linux/iio/iio.h>
#include <linux/math.h>

#define PRECISION(x)	(int_pow(10, (x) - 1))

/* Test iio_str_to_fixpoint64() with valid positive integers */
static void iio_test_str_to_fixpoint64_positive_integers(struct kunit *test)
{
	s64 integer, fract;
	int ret;

	/* Simple positive integer */
	ret = iio_str_to_fixpoint64("42", 0, &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 42);
	KUNIT_EXPECT_EQ(test, fract, 0);

	/* Positive integer with leading + */
	ret = iio_str_to_fixpoint64("+10", 0, &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 10);
	KUNIT_EXPECT_EQ(test, fract, 0);

	/* Large positive integer */
	ret = iio_str_to_fixpoint64("123456789", 0, &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 123456789);
	KUNIT_EXPECT_EQ(test, fract, 0);
}

/* Test iio_str_to_fixpoint64() with valid negative integers */
static void iio_test_str_to_fixpoint64_negative_integers(struct kunit *test)
{
	s64 integer, fract;
	int ret;

	/* Simple negative integer */
	ret = iio_str_to_fixpoint64("-23", 0, &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, -23);
	KUNIT_EXPECT_EQ(test, fract, 0);

	/* Large negative integer */
	ret = iio_str_to_fixpoint64("-987654321", 0, &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, -987654321);
	KUNIT_EXPECT_EQ(test, fract, 0);
}

/* Test iio_str_to_fixpoint64() with zero */
static void iio_test_str_to_fixpoint64_zero(struct kunit *test)
{
	s64 integer, fract;
	int ret;

	/* Zero */
	ret = iio_str_to_fixpoint64("0", 0, &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, 0);

	/* Zero with decimal */
	ret = iio_str_to_fixpoint64("0.0", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, 0);

	/* leading zeros */
	ret = iio_str_to_fixpoint64("00000000000000000000042", 0, &integer,
				    &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 42);
	KUNIT_EXPECT_EQ(test, fract, 0);
}

/* Test iio_str_to_fixpoint64() with valid decimal numbers */
static void iio_test_str_to_fixpoint64_positive_decimals(struct kunit *test)
{
	s64 integer, fract;
	int ret;

	/* Positive decimal */
	ret = iio_str_to_fixpoint64("3.14", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 3);
	KUNIT_EXPECT_EQ(test, fract, 140000);

	/* Decimal less than 1 */
	ret = iio_str_to_fixpoint64("0.5", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, 500000);

	/* Decimal with trailing zeros */
	ret = iio_str_to_fixpoint64("+123.000", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 123);
	KUNIT_EXPECT_EQ(test, fract, 0);

	/* High precision decimal */
	ret = iio_str_to_fixpoint64("1.123456789", PRECISION(9), &integer,
				    &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 1);
	KUNIT_EXPECT_EQ(test, fract, 123456789);

	/* Small decimal */
	ret = iio_str_to_fixpoint64("0.000000001", PRECISION(9), &integer,
				    &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, 1);
}

/* Test iio_str_to_fixpoint64() with negative decimals */
static void iio_test_str_to_fixpoint64_negative_decimals(struct kunit *test)
{
	s64 integer, fract;
	int ret;

	/* Negative decimal */
	ret = iio_str_to_fixpoint64("-2.71", PRECISION(5), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, -2);
	KUNIT_EXPECT_EQ(test, fract, 71000);

	/* Negative decimal less than -1 */
	ret = iio_str_to_fixpoint64("-0.5", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, -500000);

	/* Negative with high precision */
	ret = iio_str_to_fixpoint64("-0.000000001", PRECISION(9), &integer,
				    &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, -1);
}

/* Test iio_str_to_fixpoint64() with precision edge cases */
static void iio_test_str_to_fixpoint64_precision_edge_cases(struct kunit *test)
{
	s64 integer, fract;
	int ret;

	/* More digits than precision - should truncate */
	ret = iio_str_to_fixpoint64("1.23456", PRECISION(3), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 1);
	KUNIT_EXPECT_EQ(test, fract, 234);

	/* Fewer digits than precision - should pad with zeros */
	ret = iio_str_to_fixpoint64("1.23", PRECISION(7), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 1);
	KUNIT_EXPECT_EQ(test, fract, 2300000);

	/* Single digit fractional with high precision */
	ret = iio_str_to_fixpoint64("5.1", PRECISION(9), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 5);
	KUNIT_EXPECT_EQ(test, fract, 100000000);
}

/* Test iio_str_to_fixpoint64() with newline characters */
static void iio_test_str_to_fixpoint64_with_newline(struct kunit *test)
{
	s64 integer, fract;
	int ret;

	/* Integer with newline */
	ret = iio_str_to_fixpoint64("-42\n", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, -42);
	KUNIT_EXPECT_EQ(test, fract, 0);

	/* Decimal with newline */
	ret = iio_str_to_fixpoint64("3.141\n", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 3);
	KUNIT_EXPECT_EQ(test, fract, 141000);
}

/* Test iio_str_to_fixpoint64() with edge cases */
static void iio_test_str_to_fixpoint64_edge_cases(struct kunit *test)
{
	s64 integer, fract;
	int ret;

	/* Leading decimal point */
	ret = iio_str_to_fixpoint64(".5", PRECISION(4), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, 5000);

	/* Leading decimal with sign */
	ret = iio_str_to_fixpoint64("-.5", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, -500000);

	ret = iio_str_to_fixpoint64("+.5", PRECISION(3), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, 500);
}

/* Test iio_str_to_fixpoint64() with invalid inputs */
static void iio_test_str_to_fixpoint64_invalid(struct kunit *test)
{
	s64 integer, fract;
	int ret;

	/* Empty string */
	ret = iio_str_to_fixpoint64("", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	/* Just a sign */
	ret = iio_str_to_fixpoint64("-", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = iio_str_to_fixpoint64("+", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	/* Just a decimal point */
	ret = iio_str_to_fixpoint64(".", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	/* Non-numeric characters */
	ret = iio_str_to_fixpoint64("abc", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = iio_str_to_fixpoint64("12a", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = iio_str_to_fixpoint64("3.4x", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = iio_str_to_fixpoint64("0xff", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	/* Multiple decimal points */
	ret = iio_str_to_fixpoint64("12.34.56", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	/* Trailing decimal without digits */
	ret = iio_str_to_fixpoint64("42.", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	/* Trailing spaces */
	ret = iio_str_to_fixpoint64("42 ", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	/* Too many digits in fractional part */
	ret = iio_str_to_fixpoint64("1.123456789012345678901", PRECISION(21),
				    &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL); /* fails when checking precision */
}

/* Test iio_str_to_fixpoint() with valid inputs */
static void iio_test_str_to_fixpoint_valid(struct kunit *test)
{
	int integer, fract;
	int ret;

	/* Test with 6 decimal places */
	ret = iio_str_to_fixpoint("10.001234", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 10);
	KUNIT_EXPECT_EQ(test, fract, 1234);

	ret = iio_str_to_fixpoint("-0.5", PRECISION(3), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 0);
	KUNIT_EXPECT_EQ(test, fract, -500);

	/* Test with 9 decimal places */
	ret = iio_str_to_fixpoint("5.123456789", PRECISION(9), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 5);
	KUNIT_EXPECT_EQ(test, fract, 123456789);

	ret = iio_str_to_fixpoint("1.0", PRECISION(9), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 1);
	KUNIT_EXPECT_EQ(test, fract, 0);

	/* Test with 3 decimal places */
	ret = iio_str_to_fixpoint("-7.8", PRECISION(3), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, -7);
	KUNIT_EXPECT_EQ(test, fract, 800);

	/* Truncation with 2 decimal places */
	ret = iio_str_to_fixpoint("3.1415", PRECISION(2), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 3);
	KUNIT_EXPECT_EQ(test, fract, 14);

	/* Integer with 6 decimal places */
	ret = iio_str_to_fixpoint("42", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer, 42);
	KUNIT_EXPECT_EQ(test, fract, 0);
}

/* Test both functions with overflow cases */
static void iio_test_str_to_fixpoint_overflow(struct kunit *test)
{
	s64 integer64, fract64;
	int integer, fract;
	int ret;

	/* integer overflow - value exceeds U64_MAX */
	ret = iio_str_to_fixpoint64("18446744073709551616", PRECISION(6),
				    &integer64, &fract64);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);

	/* integer underflow - value less than S64_MIN */
	ret = iio_str_to_fixpoint64("-9223372036854775809", PRECISION(6),
				    &integer64, &fract64);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);

	/* fractional underflow */
	ret = iio_str_to_fixpoint64("-0.9223372036854775810", PRECISION(19),
				    &integer64, &fract64);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);

	/* Integer overflow - value exceeds U32_MAX */
	ret = iio_str_to_fixpoint("4294967296", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);

	/* Integer underflow - value less than INT_MIN */
	ret = iio_str_to_fixpoint("-2147483649", PRECISION(6), &integer,
				  &fract);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);

	/* fractional overflow */
	ret = iio_str_to_fixpoint("0.4294967296", PRECISION(10), &integer,
				  &fract);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);

	/* fractional underflow */
	ret = iio_str_to_fixpoint("-0.2147483649", PRECISION(10), &integer,
				  &fract);
	KUNIT_EXPECT_EQ(test, ret, -ERANGE);
}

/* Test iio_str_to_fixpoint() with invalid inputs */
static void iio_test_str_to_fixpoint_invalid(struct kunit *test)
{
	int integer, fract;
	int ret;

	/* Empty string */
	ret = iio_str_to_fixpoint("", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_NE(test, ret, 0);

	/* Non-numeric */
	ret = iio_str_to_fixpoint("abc", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_NE(test, ret, 0);

	/* Invalid characters */
	ret = iio_str_to_fixpoint("12.34x", PRECISION(6), &integer, &fract);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

/* Test both functions with boundary values */
static void iio_test_fixpoint_boundary_values(struct kunit *test)
{
	s64 integer64, fract64;
	int integer32, fract32;
	int ret;

	/* S32_MAX */
	ret = iio_str_to_fixpoint("2147483647", PRECISION(6), &integer32,
				  &fract32);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer32, S32_MAX);
	KUNIT_EXPECT_EQ(test, fract32, 0);

	/* U32_MAX */
	ret = iio_str_to_fixpoint("4294967295", PRECISION(6), &integer32,
				  &fract32);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, (u32)integer32, U32_MAX);
	KUNIT_EXPECT_EQ(test, fract32, 0);

	/* S32_MIN */
	ret = iio_str_to_fixpoint("-2147483648", PRECISION(6), &integer32,
				  &fract32);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer32, S32_MIN);
	KUNIT_EXPECT_EQ(test, fract32, 0);

	/* S32_MIN with fractional part */
	ret = iio_str_to_fixpoint("-2147483648.2147483647", PRECISION(10),
				  &integer32, &fract32);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer32, S32_MIN);
	KUNIT_EXPECT_EQ(test, fract32, S32_MAX);

	/* S64_MAX */
	ret = iio_str_to_fixpoint64("9223372036854775807", PRECISION(6),
				    &integer64, &fract64);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer64, S64_MAX);
	KUNIT_EXPECT_EQ(test, fract64, 0);

	/* U64_MAX */
	ret = iio_str_to_fixpoint64("18446744073709551615", PRECISION(6),
				    &integer64, &fract64);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, (u64)integer64, U64_MAX);
	KUNIT_EXPECT_EQ(test, fract64, 0);

	/* S64_MIN */
	ret = iio_str_to_fixpoint64("-9223372036854775808", PRECISION(6),
				    &integer64, &fract64);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer64, S64_MIN);
	KUNIT_EXPECT_EQ(test, fract64, 0);

	/* S64_MIN with fractional part */
	ret = iio_str_to_fixpoint64("-9223372036854775808.9223372036854775807",
				    PRECISION(19), &integer64, &fract64);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, integer64, S64_MIN);
	KUNIT_EXPECT_EQ(test, fract64, S64_MAX);
}

static struct kunit_case iio_fixpoint_parse_test_cases[] = {
	KUNIT_CASE(iio_test_str_to_fixpoint64_positive_integers),
	KUNIT_CASE(iio_test_str_to_fixpoint64_negative_integers),
	KUNIT_CASE(iio_test_str_to_fixpoint64_zero),
	KUNIT_CASE(iio_test_str_to_fixpoint64_positive_decimals),
	KUNIT_CASE(iio_test_str_to_fixpoint64_negative_decimals),
	KUNIT_CASE(iio_test_str_to_fixpoint64_precision_edge_cases),
	KUNIT_CASE(iio_test_str_to_fixpoint64_with_newline),
	KUNIT_CASE(iio_test_str_to_fixpoint64_edge_cases),
	KUNIT_CASE(iio_test_str_to_fixpoint64_invalid),
	KUNIT_CASE(iio_test_str_to_fixpoint_valid),
	KUNIT_CASE(iio_test_str_to_fixpoint_overflow),
	KUNIT_CASE(iio_test_str_to_fixpoint_invalid),
	KUNIT_CASE(iio_test_fixpoint_boundary_values),
	{ }
};

static struct kunit_suite iio_fixpoint_parse_test_suite = {
	.name = "iio-fixpoint-parse",
	.test_cases = iio_fixpoint_parse_test_cases,
};

kunit_test_suite(iio_fixpoint_parse_test_suite);

MODULE_AUTHOR("Rodrigo Alencar <rodrigo.alencar@analog.com>");
MODULE_AUTHOR("IIO Kunit Test");
MODULE_DESCRIPTION("Test IIO fixpoint parsing functions");
MODULE_LICENSE("GPL");
