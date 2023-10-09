// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <kunit/test.h>
#include <asm/word-at-a-time.h>

static void test_wordatatime_has_zero(struct kunit *test)
{
	const struct word_at_a_time constants = WORD_AT_A_TIME_CONSTANTS;
	unsigned long val, data;

	val = -1UL;
	KUNIT_ASSERT_FALSE(test, has_zero(val, &data, &constants));

	for (int i = 0; i < BITS_PER_LONG; i += 8) {
		val = ~(0xffUL << i);
		KUNIT_ASSERT_TRUE(test, has_zero(val, &data, &constants));
	}

	for (int i = 0; i < BITS_PER_LONG; i++) {
		val = ~(0x1UL << i);
		KUNIT_ASSERT_FALSE(test, has_zero(val, &data, &constants));
	}

	for (int i = 0; i < BITS_PER_LONG; i++) {
		val = 0x1UL << i;
		KUNIT_ASSERT_TRUE(test, has_zero(val, &data, &constants));
	}
}

static void test_wordatatime_find_zero(struct kunit *test)
{
	const struct word_at_a_time constants = WORD_AT_A_TIME_CONSTANTS;
	unsigned long val, data;

	for (int i = 0; i < BITS_PER_LONG; i += 8) {
		val = ~(0xffUL << i);
		KUNIT_ASSERT_TRUE(test, has_zero(val, &data, &constants));
		data = prep_zero_mask(val, data, &constants);
		data = create_zero_mask(data);
#ifdef CONFIG_CPU_BIG_ENDIAN
		KUNIT_ASSERT_EQ(test, find_zero(data), (BITS_PER_LONG / 8 - 1) - (i / 8));
#else
		KUNIT_ASSERT_EQ(test, find_zero(data), i / 8);
#endif
	}
}
static struct kunit_case wordatatime_test_cases[] = {
	KUNIT_CASE(test_wordatatime_has_zero),
	KUNIT_CASE(test_wordatatime_find_zero),
	{}
};

static struct kunit_suite wordatatime_test_suite = {
	.name = "wordatatime_test",
	.test_cases = wordatatime_test_cases,
};

kunit_test_suites(&wordatatime_test_suite);
MODULE_LICENSE("GPL");
