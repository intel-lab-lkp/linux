// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <kunit/static_stub.h>

#include <linux/math.h>


static void test_pow_0(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 1, int_pow(64, 0));
}

static void test_pow_1(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 64, int_pow(64, 1));
}

static void test_base_0(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 0, int_pow(0, 5));
}

static void test_base_1(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 1, int_pow(1, 100));
	KUNIT_EXPECT_EQ(test, 1, int_pow(1, 0));
}

static void test_base_0_pow_0(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 1, int_pow(0, 0));
}

static void test_base_2_pow_2(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 4, int_pow(2, 2));
}

static void test_max_base(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, U64_MAX, int_pow(U64_MAX, 1));
}

static void test_large_result(struct kunit *test)
{
	UNIT_EXPECT_EQ(test, 1152921504606846976, int_pow(2, 60));
}

static struct kunit_case math_int_pow_test_cases[] = {
	KUNIT_CASE(test_pow_0),
	KUNIT_CASE(test_pow_1),
	KUNIT_CASE(test_base_0),
	KUNIT_CASE(test_base_1),
	KUNIT_CASE(test_base_0_pow_0),
	KUNIT_CASE(test_base_2_pow_2),
	KUNIT_CASE(test_max_base),
	KUNIT_CASE(test_large_result),
	{}
};

static struct kunit_suite int_pow_test_suite = {
	.name = "lib-math-int_pow",
	.test_cases = math_int_pow_test_cases,
};

kunit_test_suites(&int_pow_test_suite);

MODULE_DESCRIPTION("math.int_pow KUnit test suite");
MODULE_LICENSE("GPL v2");
