// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * test_fprobe.c - simple sanity test for fprobe
 */

#include <linux/kernel.h>
#include <linux/fprobe.h>
#include <linux/random.h>
#include <kunit/test.h>

#define div_factor 3

static struct kunit *current_test;

static u32 rand1, entry_val, exit_val;

/* Use indirect calls to avoid inlining the target functions */
static u32 (*target)(u32 value);
static u32 (*target2)(u32 value);
static u32 (*target3)(u32 value);
static unsigned long target_ip;
static unsigned long target2_ip;
static unsigned long target3_ip;
static int entry_return_value;

static noinline u32 fprobe_selftest_target(u32 value)
{
	return (value / div_factor);
}

static noinline u32 fprobe_selftest_target2(u32 value)
{
	return (value / div_factor) + 1;
}

static noinline u32 fprobe_selftest_target3(u32 value)
{
	return (value / div_factor) + 2;
}

static notrace int fp_entry_handler(struct fprobe *fp, unsigned long ip,
				    unsigned long ret_ip,
				    struct ftrace_regs *fregs, void *data)
{
	KUNIT_EXPECT_FALSE(current_test, preemptible());
	/* This can be called on the fprobe_selftest_target and the fprobe_selftest_target2 */
	if (ip != target_ip && ip != target3_ip)
		KUNIT_EXPECT_EQ(current_test, ip, target2_ip);
	entry_val = (rand1 / div_factor);
	if (fp->entry_data_size) {
		KUNIT_EXPECT_NOT_NULL(current_test, data);
		if (data)
			*(u32 *)data = entry_val;
	} else
		KUNIT_EXPECT_NULL(current_test, data);

	return entry_return_value;
}

static notrace void fp_exit_handler(struct fprobe *fp, unsigned long ip,
				    unsigned long ret_ip,
				    struct ftrace_regs *fregs, void *data)
{
	unsigned long ret = ftrace_regs_get_return_value(fregs);

	KUNIT_EXPECT_FALSE(current_test, preemptible());
	if (ip != target_ip) {
		KUNIT_EXPECT_EQ(current_test, ip, target2_ip);
		KUNIT_EXPECT_EQ(current_test, ret, (rand1 / div_factor) + 1);
	} else
		KUNIT_EXPECT_EQ(current_test, ret, (rand1 / div_factor));
	KUNIT_EXPECT_EQ(current_test, entry_val, (rand1 / div_factor));
	exit_val = entry_val + div_factor;
	if (fp->entry_data_size) {
		KUNIT_EXPECT_NOT_NULL(current_test, data);
		if (data)
			KUNIT_EXPECT_EQ(current_test, *(u32 *)data, entry_val);
	} else
		KUNIT_EXPECT_NULL(current_test, data);
}

/* Test entry only (no rethook) */
static void test_fprobe_entry(struct kunit *test)
{
	struct fprobe fp_entry = {
		.entry_handler = fp_entry_handler,
	};

	current_test = test;

	/* Before register, unregister should be failed. */
	KUNIT_EXPECT_NE(test, 0, unregister_fprobe(&fp_entry));
	KUNIT_EXPECT_EQ(test, 0, register_fprobe(&fp_entry, "fprobe_selftest_target*", NULL));

	entry_val = 0;
	exit_val = 0;
	target(rand1);
	KUNIT_EXPECT_NE(test, 0, entry_val);
	KUNIT_EXPECT_EQ(test, 0, exit_val);

	entry_val = 0;
	exit_val = 0;
	target2(rand1);
	KUNIT_EXPECT_NE(test, 0, entry_val);
	KUNIT_EXPECT_EQ(test, 0, exit_val);

	KUNIT_EXPECT_EQ(test, 0, unregister_fprobe(&fp_entry));
}

static void test_fprobe(struct kunit *test)
{
	struct fprobe fp = {
		.entry_handler = fp_entry_handler,
		.exit_handler = fp_exit_handler,
	};

	current_test = test;
	KUNIT_EXPECT_EQ(test, 0, register_fprobe(&fp, "fprobe_selftest_target*", NULL));

	entry_val = 0;
	exit_val = 0;
	target(rand1);
	KUNIT_EXPECT_NE(test, 0, entry_val);
	KUNIT_EXPECT_EQ(test, entry_val + div_factor, exit_val);

	entry_val = 0;
	exit_val = 0;
	target2(rand1);
	KUNIT_EXPECT_NE(test, 0, entry_val);
	KUNIT_EXPECT_EQ(test, entry_val + div_factor, exit_val);

	KUNIT_EXPECT_EQ(test, 0, unregister_fprobe(&fp));
}

static void test_fprobe_syms(struct kunit *test)
{
	static const char *syms[] = {"fprobe_selftest_target", "fprobe_selftest_target2"};
	struct fprobe fp = {
		.entry_handler = fp_entry_handler,
		.exit_handler = fp_exit_handler,
	};

	current_test = test;
	KUNIT_EXPECT_EQ(test, 0, register_fprobe_syms(&fp, syms, 2));

	entry_val = 0;
	exit_val = 0;
	target(rand1);
	KUNIT_EXPECT_NE(test, 0, entry_val);
	KUNIT_EXPECT_EQ(test, entry_val + div_factor, exit_val);

	entry_val = 0;
	exit_val = 0;
	target2(rand1);
	KUNIT_EXPECT_NE(test, 0, entry_val);
	KUNIT_EXPECT_EQ(test, entry_val + div_factor, exit_val);

	KUNIT_EXPECT_EQ(test, 0, unregister_fprobe(&fp));
}

/* Test private entry_data */
static void test_fprobe_data(struct kunit *test)
{
	struct fprobe fp = {
		.entry_handler = fp_entry_handler,
		.exit_handler = fp_exit_handler,
		.entry_data_size = sizeof(u32),
	};

	current_test = test;
	KUNIT_EXPECT_EQ(test, 0, register_fprobe(&fp, "fprobe_selftest_target", NULL));

	target(rand1);

	KUNIT_EXPECT_EQ(test, 0, unregister_fprobe(&fp));
}

static void test_fprobe_skip(struct kunit *test)
{
	struct fprobe fp = {
		.entry_handler = fp_entry_handler,
		.exit_handler = fp_exit_handler,
	};

	current_test = test;
	KUNIT_EXPECT_EQ(test, 0, register_fprobe(&fp, "fprobe_selftest_target", NULL));

	entry_return_value = 1;
	entry_val = 0;
	exit_val = 0;
	target(rand1);
	KUNIT_EXPECT_NE(test, 0, entry_val);
	KUNIT_EXPECT_EQ(test, 0, exit_val);
	KUNIT_EXPECT_EQ(test, 0, fp.nmissed);
	entry_return_value = 0;

	KUNIT_EXPECT_EQ(test, 0, unregister_fprobe(&fp));
}

static int recur_count;
static int recur_high;

static notrace int fp_recur_entry_handler(struct fprobe *fp, unsigned long ip,
					  unsigned long ret_ip,
					  struct ftrace_regs *fregs, void *data)
{
	/* fgraph/fprobe allows one-level nesting. */
	if (recur_count > 1)
		return 0;

	recur_count++;
	if (recur_count > recur_high)
		recur_high = recur_count;
	target3(rand1);
	recur_count--;

	return 0;
}

static notrace void fp_recur_exit_handler(struct fprobe *fp, unsigned long ip,
					  unsigned long ret_ip,
					  struct ftrace_regs *fregs, void *data)
{
	/* fgraph/fprobe allows one-level nesting. */
	if (recur_count > 1)
		return;

	recur_count++;
	if (recur_count > recur_high)
		recur_high = recur_count;
	target3(rand1);
	recur_count--;
}

static void test_fprobe_recursion_entry(struct kunit *test)
{
	struct fprobe fp = {
		.entry_handler = fp_recur_entry_handler,
	};
	struct fprobe fp3 = {
		.entry_handler = fp_recur_entry_handler,
	};

	current_test = test;
	recur_count = 0;
	recur_high = 0;
	KUNIT_EXPECT_EQ(test, 0, register_fprobe(&fp, "fprobe_selftest_target", NULL));
	KUNIT_EXPECT_EQ(test, 0, register_fprobe(&fp3, "fprobe_selftest_target3", NULL));

	target(rand1);

	KUNIT_EXPECT_LE(test, recur_high, 2);
	KUNIT_EXPECT_EQ(test, 0, unregister_fprobe(&fp3));
	KUNIT_EXPECT_EQ(test, 0, unregister_fprobe(&fp));
}

static void test_fprobe_recursion_exit(struct kunit *test)
{
	struct fprobe fp = {
		.exit_handler = fp_recur_exit_handler,
	};
	struct fprobe fp3 = {
		.exit_handler = fp_recur_exit_handler,
	};

	current_test = test;
	recur_count = 0;
	KUNIT_EXPECT_EQ(test, 0, register_fprobe(&fp, "fprobe_selftest_target", NULL));
	KUNIT_EXPECT_EQ(test, 0, register_fprobe(&fp3, "fprobe_selftest_target3", NULL));

	target(rand1);

	KUNIT_EXPECT_LE(test, recur_high, 2);
	KUNIT_EXPECT_EQ(test, 0, unregister_fprobe(&fp3));
	KUNIT_EXPECT_EQ(test, 0, unregister_fprobe(&fp));
}

static unsigned long get_ftrace_location(void *func)
{
	unsigned long size, addr = (unsigned long)func;

	if (!kallsyms_lookup_size_offset(addr, &size, NULL) || !size)
		return 0;

	return ftrace_location_range(addr, addr + size - 1);
}

static int fprobe_test_init(struct kunit *test)
{
	rand1 = get_random_u32_above(div_factor);
	target = fprobe_selftest_target;
	target2 = fprobe_selftest_target2;
	target3 = fprobe_selftest_target3;
	target_ip = get_ftrace_location(target);
	target2_ip = get_ftrace_location(target2);
	target3_ip = get_ftrace_location(target3);

	return 0;
}

static struct kunit_case fprobe_testcases[] = {
	KUNIT_CASE(test_fprobe_entry),
	KUNIT_CASE(test_fprobe),
	KUNIT_CASE(test_fprobe_syms),
	KUNIT_CASE(test_fprobe_data),
	KUNIT_CASE(test_fprobe_skip),
	KUNIT_CASE(test_fprobe_recursion_entry),
	KUNIT_CASE(test_fprobe_recursion_exit),
	{}
};

static struct kunit_suite fprobe_test_suite = {
	.name = "fprobe_test",
	.init = fprobe_test_init,
	.test_cases = fprobe_testcases,
};

kunit_test_suites(&fprobe_test_suite);
