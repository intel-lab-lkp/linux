// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for struct stack_trace counted_by attribute.
 */

#include <kunit/test.h>
#include <linux/stacktrace.h>

#ifndef CONFIG_ARCH_STACKWALK
static void test_stack_trace_counted_by(struct kunit *test)
{
	unsigned long entries_buf[4];
	struct stack_trace trace = {
		.entries = entries_buf,
		.max_entries = 4,
	};

	KUNIT_EXPECT_EQ(test, trace.max_entries, 4U);
	KUNIT_EXPECT_PTR_EQ(test, trace.entries, (unsigned long *)entries_buf);

	/* Write to the allocated elements to verify access */
	trace.entries[0] = 0xdeadbeef;
	trace.entries[1] = 0xbeefcafe;
	trace.entries[2] = 0xcafebabe;
	trace.entries[3] = 0x12345678;

	KUNIT_EXPECT_EQ(test, trace.entries[0], 0xdeadbeefUL);
	KUNIT_EXPECT_EQ(test, trace.entries[1], 0xbeefcafeUL);
	KUNIT_EXPECT_EQ(test, trace.entries[2], 0xcafebabeUL);
	KUNIT_EXPECT_EQ(test, trace.entries[3], 0x12345678UL);
}
#else
static void test_stack_trace_counted_by(struct kunit *test)
{
	kunit_skip(test, "CONFIG_ARCH_STACKWALK is enabled, struct stack_trace is not defined");
}
#endif

static struct kunit_case stacktrace_test_cases[] = {
	KUNIT_CASE(test_stack_trace_counted_by),
	{}
};

static struct kunit_suite stacktrace_test_suite = {
	.name = "stacktrace_counted_by",
	.test_cases = stacktrace_test_cases,
};

kunit_test_suite(stacktrace_test_suite);

MODULE_LICENSE("GPL");
