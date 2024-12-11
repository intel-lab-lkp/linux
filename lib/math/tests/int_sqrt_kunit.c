// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/string.h>

struct test_case_params {
	unsigned long x;
	unsigned long expected_result;
	const char *name;
};

static const struct test_case_params params[] = {
	{ 0, 0, "edge case: square root of 0" },
	{ 1, 1, "perfect square: square root of 1" },
	{ 2, 1, "non-perfect square: square root of 2" },
	{ 3, 1, "non-perfect square: sqaure root of 3" },
	{ 4, 2, "perfect square: square root of 4" },
	{ 5, 2, "non-perfect square: square  root of 5" },
	{ 6, 2, "non-perfect square: square root of 6" },
	{ 7, 2, "non-perfect square: square root of 7" },
	{ 8, 2, "non-perfect square: square root of 8" },
	{ 9, 3, "perfect square: square root of 9" },
	{ 16, 4, "perfect square: square root of 16" },
	{ 81, 9, "perfect square: square root of 81" },
	{ 256, 16, "perfect square: square root of 256" },
	{ 2147483648, 46340, "large input: square root of 2147483648" },
	{ 4294967295, 65535, "edge case: ULONG_MAX for 32-bit" },
};

static void get_desc(const struct test_case_params *tc, char *desc)
{
	strscpy(desc, tc->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(int_sqrt, params, get_desc);

static void int_sqrt_test(struct kunit *test)
{
	const struct test_case_params *tc = (const struct test_case_params *)test->param_value;

	KUNIT_EXPECT_EQ(test, tc->expected_result, int_sqrt(tc->x));
}

static struct kunit_case math_int_sqrt_test_cases[] = {
	KUNIT_CASE_PARAM(int_sqrt_test, int_sqrt_gen_params),
	{}
};

static struct kunit_suite int_sqrt_test_suite = {
	.name = "math-int_sqrt",
	.test_cases = math_int_sqrt_test_cases,
};

kunit_test_suites(&int_sqrt_test_suite);

MODULE_DESCRIPTION("math.int_sqrt KUnit test suite");
MODULE_LICENSE("GPL");
