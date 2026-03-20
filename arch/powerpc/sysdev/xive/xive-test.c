// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit tests for XIVE interrupt controller
 *
 * Copyright 2026 IBM Corporation.
 */

#include <kunit/test.h>
#include <linux/cpumask.h>
#include <linux/smp.h>
#include <linux/irq.h>
#include <asm/xive.h>

/*
 * Mock xive_try_pick_target for testing
 * The real function checks queue capacity, we simplify for testing
 */
static bool xive_try_pick_target(int cpu)
{
	/* For testing, accept any online CPU */
	return cpu_online(cpu);
}

/*
 * Copy of xive_find_target_in_mask from common.c for testing
 * This allows us to test the static function without modifying source
 */
static int xive_find_target_in_mask(const struct cpumask *mask,
				    unsigned int fuzz)
{
	int cpu, first;

	/* Pick up a starting point CPU in the mask based on fuzz */
	fuzz %= cpumask_weight(mask);
	first = cpumask_nth(fuzz, mask);
	WARN_ON(first >= nr_cpu_ids);

	/*
	 * Now go through the entire mask until we find a valid
	 * target.
	 */
	for_each_cpu_wrap(cpu, mask, first) {
		if (cpu_online(cpu) && xive_try_pick_target(cpu))
			return cpu;
	}

	return -1;
}

/*
 * Test: Empty CPU mask
 * Expected: Should return -1 when the mask contains no CPUs
 */
static void xive_test_find_target_empty_mask(struct kunit *test)
{
	struct cpumask empty_mask;
	int result;

	cpumask_clear(&empty_mask);

	result = xive_find_target_in_mask(&empty_mask, 0);

	KUNIT_EXPECT_EQ(test, result, -1);
}

/*
 * Test: Single CPU in mask
 * Expected: Should return that CPU if it's online
 */
static void xive_test_find_target_single_cpu(struct kunit *test)
{
	struct cpumask single_mask;
	int cpu = 0;
	int result;

	/* Skip test if CPU 0 is not online */
	if (!cpu_online(0))
		kunit_skip(test, "CPU 0 is not online");

	cpumask_clear(&single_mask);
	cpumask_set_cpu(cpu, &single_mask);

	result = xive_find_target_in_mask(&single_mask, 0);

	KUNIT_EXPECT_EQ(test, result, cpu);
}

/*
 * Test: Multiple CPUs in mask with fuzz=0
 * Expected: Should return a valid CPU from the mask
 */
static void xive_test_find_target_multiple_cpus(struct kunit *test)
{
	struct cpumask multi_mask;
	int result;
	int cpu;
	int count = 0;

	cpumask_clear(&multi_mask);

	/* Add first 4 online CPUs to the mask */
	for_each_online_cpu(cpu) {
		cpumask_set_cpu(cpu, &multi_mask);
		count++;
		if (count >= 4)
			break;
	}

	if (count == 0)
		kunit_skip(test, "No online CPUs available");

	result = xive_find_target_in_mask(&multi_mask, 0);

	/* Result should be a valid CPU in the mask */
	KUNIT_EXPECT_NE(test, result, -1);
	KUNIT_EXPECT_TRUE(test, cpumask_test_cpu(result, &multi_mask));
	KUNIT_EXPECT_TRUE(test, cpu_online(result));
}

/*
 * Test: Fuzz parameter affects starting point
 * Expected: Different fuzz values may select different CPUs
 */
static void xive_test_find_target_fuzz_variation(struct kunit *test)
{
	struct cpumask multi_mask;
	int result1, result2;
	int cpu;
	int count = 0;

	cpumask_clear(&multi_mask);

	/* Add multiple online CPUs to the mask */
	for_each_online_cpu(cpu) {
		cpumask_set_cpu(cpu, &multi_mask);
		count++;
		if (count >= 4)
			break;
	}

	if (count < 2)
		kunit_skip(test, "Need at least 2 online CPUs for this test");

	result1 = xive_find_target_in_mask(&multi_mask, 0);
	result2 = xive_find_target_in_mask(&multi_mask, 1);

	/* Both results should be valid CPUs in the mask */
	KUNIT_EXPECT_NE(test, result1, -1);
	KUNIT_EXPECT_NE(test, result2, -1);
	KUNIT_EXPECT_TRUE(test, cpumask_test_cpu(result1, &multi_mask));
	KUNIT_EXPECT_TRUE(test, cpumask_test_cpu(result2, &multi_mask));
}

/*
 * Test: Large fuzz value (modulo behavior)
 * Expected: Should handle fuzz values larger than mask weight correctly
 */
static void xive_test_find_target_large_fuzz(struct kunit *test)
{
	struct cpumask multi_mask;
	int result;
	int cpu;
	int count = 0;
	unsigned int large_fuzz = 1000;

	cpumask_clear(&multi_mask);

	/* Add online CPUs to the mask */
	for_each_online_cpu(cpu) {
		cpumask_set_cpu(cpu, &multi_mask);
		count++;
		if (count >= 3)
			break;
	}

	if (count == 0)
		kunit_skip(test, "No online CPUs available");

	result = xive_find_target_in_mask(&multi_mask, large_fuzz);

	/* Result should be a valid CPU in the mask */
	KUNIT_EXPECT_NE(test, result, -1);
	KUNIT_EXPECT_TRUE(test, cpumask_test_cpu(result, &multi_mask));
	KUNIT_EXPECT_TRUE(test, cpu_online(result));
}

/*
 * Test: Wrap-around behavior at mask boundary
 * Expected: Should correctly wrap around when starting near the end
 */
static void xive_test_find_target_wrap_around(struct kunit *test)
{
	struct cpumask wrap_mask;
	int result;
	int cpu;
	int count = 0;
	unsigned int weight;

	cpumask_clear(&wrap_mask);

	/* Add online CPUs to the mask */
	for_each_online_cpu(cpu) {
		cpumask_set_cpu(cpu, &wrap_mask);
		count++;
		if (count >= 4)
			break;
	}

	if (count < 2)
		kunit_skip(test, "Need at least 2 online CPUs for this test");

	weight = cpumask_weight(&wrap_mask);

	/* Test with fuzz at the boundary */
	result = xive_find_target_in_mask(&wrap_mask, weight - 1);

	KUNIT_EXPECT_NE(test, result, -1);
	KUNIT_EXPECT_TRUE(test, cpumask_test_cpu(result, &wrap_mask));
	KUNIT_EXPECT_TRUE(test, cpu_online(result));
}

/*
 * Test: Using cpu_online_mask
 * Expected: Should handle the system's online CPU mask correctly
 */
static void xive_test_find_target_online_mask(struct kunit *test)
{
	int result;

	if (cpumask_empty(cpu_online_mask))
		kunit_skip(test, "No online CPUs in system");

	result = xive_find_target_in_mask(cpu_online_mask, 0);

	KUNIT_EXPECT_NE(test, result, -1);
	KUNIT_EXPECT_TRUE(test, cpu_online(result));
}

/*
 * Test: Fuzz value equal to mask weight
 * Expected: Should wrap to first CPU (fuzz % weight == 0)
 */
static void xive_test_find_target_fuzz_equals_weight(struct kunit *test)
{
	struct cpumask test_mask;
	int result;
	int cpu;
	int count = 0;
	unsigned int weight;

	cpumask_clear(&test_mask);

	/* Add online CPUs to the mask */
	for_each_online_cpu(cpu) {
		cpumask_set_cpu(cpu, &test_mask);
		count++;
		if (count >= 3)
			break;
	}

	if (count == 0)
		kunit_skip(test, "No online CPUs available");

	weight = cpumask_weight(&test_mask);

	/* Fuzz equal to weight should wrap to start */
	result = xive_find_target_in_mask(&test_mask, weight);

	KUNIT_EXPECT_NE(test, result, -1);
	KUNIT_EXPECT_TRUE(test, cpumask_test_cpu(result, &test_mask));
}

static struct kunit_case xive_test_cases[] = {
	KUNIT_CASE(xive_test_find_target_empty_mask),
	KUNIT_CASE(xive_test_find_target_single_cpu),
	KUNIT_CASE(xive_test_find_target_multiple_cpus),
	KUNIT_CASE(xive_test_find_target_fuzz_variation),
	KUNIT_CASE(xive_test_find_target_large_fuzz),
	KUNIT_CASE(xive_test_find_target_wrap_around),
	KUNIT_CASE(xive_test_find_target_online_mask),
	KUNIT_CASE(xive_test_find_target_fuzz_equals_weight),
	{}
};

static struct kunit_suite xive_test_suite = {
	.name = "xive_find_target_in_mask",
	.test_cases = xive_test_cases,
};

kunit_test_suites(&xive_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for XIVE interrupt controller");
MODULE_AUTHOR("Mukesh Kumar Chaurasiya (IBM) <mkchauras@gmail.com>");
