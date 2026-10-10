// SPDX-License-Identifier: GPL-2.0-only
/* Check where stack_trace_save() and ftrace_return_address() start. */
#include <kunit/test.h>
#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/module.h>
#include <linux/stacktrace.h>

#define RISCV_STACKTRACE_DEPTH 16

static noinline unsigned int
riscv_stacktrace_leaf(unsigned long *entries, unsigned int skip)
{
	unsigned int count = stack_trace_save(entries, RISCV_STACKTRACE_DEPTH, skip);

	barrier();
	return count;
}

static noinline unsigned int
riscv_stacktrace_middle(unsigned long *entries, unsigned int skip)
{
	unsigned int count = riscv_stacktrace_leaf(entries, skip);

	barrier();
	return count;
}

static noinline unsigned int
riscv_stacktrace_outer(unsigned long *entries, unsigned int skip)
{
	unsigned int count = riscv_stacktrace_middle(entries, skip);

	barrier();
	return count;
}

static void riscv_stacktrace_check_skip(struct kunit *test, unsigned int skip)
{
	static const char * const expected[] = {
		"riscv_stacktrace_leaf",
		"riscv_stacktrace_middle",
		"riscv_stacktrace_outer",
	};
	unsigned long entries[RISCV_STACKTRACE_DEPTH];
	char symbol[KSYM_SYMBOL_LEN];
	unsigned int count;

	count = riscv_stacktrace_outer(entries, skip);
	KUNIT_ASSERT_GT(test, count, 0U);
	sprint_symbol_no_offset(symbol, entries[0]);
	kunit_info(test, "skip=%u first=%s count=%u\n", skip, symbol, count);
	KUNIT_EXPECT_STREQ(test, symbol, expected[skip]);
}

static void riscv_stacktrace_skip_zero(struct kunit *test)
{
	riscv_stacktrace_check_skip(test, 0);
}

static void riscv_stacktrace_skip_one(struct kunit *test)
{
	riscv_stacktrace_check_skip(test, 1);
}

static void riscv_stacktrace_skip_two(struct kunit *test)
{
	riscv_stacktrace_check_skip(test, 2);
}

static noinline void *riscv_return_address_leaf(unsigned int level)
{
	void *addr = (void *)ftrace_return_address(level);

	barrier();
	return addr;
}

static noinline void *riscv_return_address_middle(unsigned int level)
{
	void *addr = riscv_return_address_leaf(level);

	barrier();
	return addr;
}

static noinline void *riscv_return_address_outer(unsigned int level)
{
	void *addr = riscv_return_address_middle(level);

	barrier();
	return addr;
}

/*
 * ftrace_return_address(n) is what CALLER_ADDRn expands to. Like
 * __builtin_return_address(n), level 0 is the caller of the function that
 * uses it.
 */
static void riscv_return_address_check(struct kunit *test, unsigned int level)
{
	static const char * const expected[] = {
		"riscv_return_address_middle",
		"riscv_return_address_outer",
	};
	char symbol[KSYM_SYMBOL_LEN];
	void *addr;

	addr = riscv_return_address_outer(level);
	KUNIT_ASSERT_NOT_NULL(test, addr);
	sprint_symbol_no_offset(symbol, (unsigned long)addr);
	kunit_info(test, "level=%u caller=%s\n", level, symbol);
	KUNIT_EXPECT_STREQ(test, symbol, expected[level]);
}

static void riscv_return_address_level_zero(struct kunit *test)
{
	riscv_return_address_check(test, 0);
}

static void riscv_return_address_level_one(struct kunit *test)
{
	riscv_return_address_check(test, 1);
}

static struct kunit_case riscv_stacktrace_cases[] = {
	KUNIT_CASE(riscv_stacktrace_skip_zero),
	KUNIT_CASE(riscv_stacktrace_skip_one),
	KUNIT_CASE(riscv_stacktrace_skip_two),
	KUNIT_CASE(riscv_return_address_level_zero),
	KUNIT_CASE(riscv_return_address_level_one),
	{}
};

static struct kunit_suite riscv_stacktrace_suite = {
	.name = "riscv-stacktrace-skip",
	.test_cases = riscv_stacktrace_cases,
};

kunit_test_suite(riscv_stacktrace_suite);

MODULE_LICENSE("GPL");
