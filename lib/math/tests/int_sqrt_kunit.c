// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/math.h>

struct test_case_params {
	unsigned long x;
	unsigned long expected_result;
	const char *name;
};

static const struct test_case_params params[] = {
	{ 0, 0, "edge-case: square root of 0" },
	{ 4, 2, "perfect square: square root of 4" },
	{ 81, 9, "perfect square: square root of 9" },
	{ 2, 1, "non-perfect square: square root of 2" },
	{ 5, 2, "non-perfect square: square root of 5"},
	{ ULONG_MAX, 4294967295, "large input"},
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
