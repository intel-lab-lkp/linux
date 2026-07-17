// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include "kwatch.h"
#include <linux/string.h>

static void kwatch_test_parse_deref_chain(struct kunit *test)
{
	struct kwatch_config cfg;
	int ret;

	// Test 1: stack
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "stack");
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_STACK);
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 1);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], 0);

	// Test 2: arg1
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg1");
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_ARG1);
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 1);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], 0);

	// Test 3: arg6+8
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg6+8");
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_ARG6);
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 1);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], 8);

	// Test 4: arg2-16
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg2-16");
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_ARG2);
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 1);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], -16);

	// Test 5: arg3->8
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg3->8");
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_ARG3);
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 2);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], 0);
	KUNIT_EXPECT_EQ(test, cfg.offsets[1], 8);

	// Test 6: arg4+8->16
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg4+8->16");
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_ARG4);
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 2);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], 8);
	KUNIT_EXPECT_EQ(test, cfg.offsets[1], 16);

	// Test 7: arg5-8->-16
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg5-8->-16");
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_ARG5);
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 2);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], -8);
	KUNIT_EXPECT_EQ(test, cfg.offsets[1], -16);

	// Test 8: stack->0->8
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "stack->0->8");
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_STACK);
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 3);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], 0);
	KUNIT_EXPECT_EQ(test, cfg.offsets[1], 0);
	KUNIT_EXPECT_EQ(test, cfg.offsets[2], 8);

	// Test 9: arg1->+8
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg1->+8");
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_ARG1);
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 2);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], 0);
	KUNIT_EXPECT_EQ(test, cfg.offsets[1], 8);

	// Test 9.1: arg1-> (implicit 0 should fail)
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg1->");
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	// Test 9.2: stack->->8 (implicit 0 should fail)
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "stack->->8");
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	// Test 10: Invalid base
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "invalid_base");
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	// Test 11: Invalid offset
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg1+abc");
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	// Test 12: Invalid arg
	memset(&cfg, 0, sizeof(cfg));
	ret = kwatch_deref_parse(&cfg, "arg7");
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	// Test 13: Absolute address. Use a width-appropriate literal: a 64-bit
	// address would overflow unsigned long and fail kstrtoul() on 32-bit.
	memset(&cfg, 0, sizeof(cfg));
#if BITS_PER_LONG == 64
	ret = kwatch_deref_parse(&cfg, "0xffffffff81000000+8");
#else
	ret = kwatch_deref_parse(&cfg, "0xc1000000+8");
#endif
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, cfg.base, KWATCH_BASE_ABS_ADDR);
#if BITS_PER_LONG == 64
	KUNIT_EXPECT_EQ(test, cfg.sym_addr, 0xffffffff81000000UL);
#else
	KUNIT_EXPECT_EQ(test, cfg.sym_addr, 0xc1000000UL);
#endif
	KUNIT_EXPECT_EQ(test, cfg.offset_count, 1);
	KUNIT_EXPECT_EQ(test, cfg.offsets[0], 8);
}

static struct kunit_case kwatch_deref_test_cases[] = {
	KUNIT_CASE(kwatch_test_parse_deref_chain),
	{}
};

static struct kunit_suite kwatch_deref_test_suite = {
	.name = "kwatch_deref",
	.test_cases = kwatch_deref_test_cases,
};

kunit_test_suite(kwatch_deref_test_suite);

MODULE_DESCRIPTION("KUnit tests for the KWatch watch expression parser");
MODULE_LICENSE("GPL");
