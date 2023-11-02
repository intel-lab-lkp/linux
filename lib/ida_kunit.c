// SPDX-License-Identifier: GPL-2.0+
/*
 * Test cases for IDA functions.
 */

#include <linux/idr.h>
#include <kunit/test.h>

#define IDA_STRESS_LIMIT (IDA_BITMAP_BITS * XA_CHUNK_SIZE + 1)

static int ida_test_init(struct kunit *test)
{
	static DEFINE_IDA(ida);

	ida_init(&ida);
	test->priv = &ida;
	return 0;
}

static void ida_test_exit(struct kunit *test)
{
	struct ida *ida = test->priv;

	ida_destroy(ida);
	KUNIT_EXPECT_TRUE(test, ida_is_empty(ida));
}

static const unsigned int ida_params[] = {
	0,
	1,
	BITS_PER_XA_VALUE - 1,
	BITS_PER_XA_VALUE,
	BITS_PER_XA_VALUE + 1,
	IDA_BITMAP_BITS - BITS_PER_XA_VALUE - 1,
	IDA_BITMAP_BITS - BITS_PER_XA_VALUE,
	IDA_BITMAP_BITS - BITS_PER_XA_VALUE + 1,
	IDA_BITMAP_BITS - 1,
	IDA_BITMAP_BITS,
	IDA_BITMAP_BITS + 1,
	IDA_BITMAP_BITS + BITS_PER_XA_VALUE - 1,
	IDA_BITMAP_BITS + BITS_PER_XA_VALUE,
	IDA_BITMAP_BITS + BITS_PER_XA_VALUE + 1,
	IDA_BITMAP_BITS + IDA_BITMAP_BITS + BITS_PER_XA_VALUE - 1,
	IDA_BITMAP_BITS + IDA_BITMAP_BITS + BITS_PER_XA_VALUE + 1,
};

static void uint_get_desc(const unsigned int *p, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%u", *p);
}

KUNIT_ARRAY_PARAM(ida, ida_params, uint_get_desc);

static void test_alloc_one(struct kunit *test)
{
	struct ida *ida = test->priv;
	const unsigned int *p = test->param_value;
	unsigned int min = *p;

	KUNIT_ASSERT_EQ(test, ida_weight(ida), 0);
	KUNIT_ASSERT_EQ(test, ida_alloc_min(ida, min, GFP_KERNEL), min);
	KUNIT_ASSERT_EQ(test, ida_weight(ida), 1);
	ida_free(ida, min);
	KUNIT_ASSERT_EQ(test, ida_weight(ida), 0);
}

static void test_alloc_many(struct kunit *test)
{
	struct ida *ida = test->priv;
	const unsigned int *p = test->param_value;
	unsigned int n, num = *p;

	KUNIT_ASSERT_EQ(test, ida_weight(ida), 0);

	for (n = 0; n < num; n++) {
		KUNIT_ASSERT_EQ(test, ida_alloc(ida, GFP_KERNEL), n);
		KUNIT_ASSERT_EQ(test, ida_weight(ida), n + 1);
	}

	kunit_info(test, "weight %lu", ida_weight(ida));

	for (n = 0; n < num; n++) {
		ida_free(ida, n);
		KUNIT_ASSERT_EQ(test, ida_weight(ida), num - n - 1);
	}

	KUNIT_ASSERT_EQ(test, 0, ida_weight(ida));
}

static void test_alloc_min(struct kunit *test)
{
	struct ida *ida = test->priv;
	const unsigned int *p = test->param_value;
	unsigned int n, min = *p;

	KUNIT_ASSERT_EQ(test, ida_weight(ida), 0);
	for (n = min; n < IDA_STRESS_LIMIT; n++) {
		KUNIT_ASSERT_EQ(test, ida_alloc_min(ida, min, GFP_KERNEL), n);
		KUNIT_ASSERT_EQ(test, ida_weight(ida), n - min + 1);
	}
}

static void test_alloc_group(struct kunit *test)
{
	struct ida *ida = test->priv;
	const unsigned int *p = test->param_value;
	unsigned int n, group = *p;
	unsigned long w;

	for (n = 0; n < IDA_STRESS_LIMIT; n += (1 + group)) {
		KUNIT_ASSERT_EQ(test,
				ida_alloc_group_range(ida, 0, -1, group, GFP_KERNEL),
				n);
		KUNIT_ASSERT_EQ(test, ida_weight(ida), n + 1 + group);
	}

	KUNIT_ASSERT_NE(test, w = ida_weight(ida), 0);

	for (n = 0; n < IDA_STRESS_LIMIT; n += (1 + group)) {
		ida_free_group(ida, n, group);
		KUNIT_ASSERT_EQ(test, ida_weight(ida), w - n - 1 - group);
	}
}

static struct kunit_case ida_test_cases[] = {
	KUNIT_CASE_PARAM(test_alloc_one, ida_gen_params),
	KUNIT_CASE_PARAM(test_alloc_many, ida_gen_params),
	KUNIT_CASE_PARAM(test_alloc_min, ida_gen_params),
	KUNIT_CASE_PARAM(test_alloc_group, ida_gen_params),
	{}
};

static struct kunit_suite ida_suite = {
	.name = "ida",
	.test_cases = ida_test_cases,
	.init = ida_test_init,
	.exit = ida_test_exit,
};

kunit_test_suites(&ida_suite);
