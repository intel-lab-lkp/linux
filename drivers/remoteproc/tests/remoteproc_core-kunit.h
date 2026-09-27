/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _REMOTEPROC_CORE_KUNIT_H
#define _REMOTEPROC_CORE_KUNIT_H

#include <kunit/test.h>

/*
 * Tests for rproc_check_carveout_da().
 *
 * The function is static, so this header is #include'd directly at the
 * bottom of remoteproc_core.c (see CONFIG_REMOTEPROC_KUNIT_TEST), giving
 * it access to the symbol despite the lack of external linkage.
 *
 * rproc_check_carveout_da() only touches &rproc->dev for dev_err() on
 * its failure paths, so a zeroed struct rproc is sufficient here -- no
 * device registration needed. dev_err()'s formatting chain
 * (dev_driver_string() -> "" on NULL driver/bus/class, dev_name() ->
 * NULL on an unregistered device, and the kernel's own %s NULL guard
 * in lib/vsprintf.c) is safe end-to-end against a bare zeroed device.
 */

static void rproc_test_check_carveout_len_exceeds(struct kunit *test)
{
	struct rproc rproc = { 0 };
	struct rproc_mem_entry mem = { 0 };
	int ret;

	/* mem->len = 0x100; request len 0x200 with no da constraint */
	mem.da = FW_RSC_ADDR_ANY;
	mem.len = 0x100;

	ret = rproc_check_carveout_da(&rproc, &mem, FW_RSC_ADDR_ANY, 0x200);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void rproc_test_check_carveout_da_any_mismatch(struct kunit *test)
{
	struct rproc rproc = { 0 };
	struct rproc_mem_entry mem = { 0 };
	int ret;

	/* caller wants a specific da, but the carveout itself has none set */
	mem.da = FW_RSC_ADDR_ANY;
	mem.len = 0x100;

	ret = rproc_check_carveout_da(&rproc, &mem, 0x1000, 0x50);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void rproc_test_check_carveout_da_negative_delta(struct kunit *test)
{
	struct rproc rproc = { 0 };
	struct rproc_mem_entry mem = { 0 };
	int ret;

	/* requested da (0x1000) lies before the carveout's da (0x2000) */
	mem.da = 0x2000;
	mem.len = 0x100;

	ret = rproc_check_carveout_da(&rproc, &mem, 0x1000, 0x10);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void rproc_test_check_carveout_da_exceeds_end(struct kunit *test)
{
	struct rproc rproc = { 0 };
	struct rproc_mem_entry mem = { 0 };
	int ret;

	/* delta (0x80) + len (0x100) = 0x180, one byte over mem->len (0x100) -> 0x180 > 0x100 */
	mem.da = 0x1000;
	mem.len = 0x100;

	ret = rproc_check_carveout_da(&rproc, &mem, 0x1080, 0x100);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void rproc_test_check_carveout_da_exact_fit(struct kunit *test)
{
	struct rproc rproc = { 0 };
	struct rproc_mem_entry mem = { 0 };
	int ret;

	/*
	 * Boundary case: delta (0xf0) + len (0x10) == mem->len (0x100)
	 * exactly. The check is "> mem->len", so this must pass -- this
	 * is exactly the kind of edge an off-by-one would break.
	 */
	mem.da = 0x1000;
	mem.len = 0x100;

	ret = rproc_check_carveout_da(&rproc, &mem, 0x10f0, 0x10);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static void rproc_test_check_carveout_da_any_skip_check(struct kunit *test)
{
	struct rproc rproc = { 0 };
	struct rproc_mem_entry mem = { 0 };
	int ret;

	/*
	 * da == FW_RSC_ADDR_ANY -> address check is skipped entirely,
	 * only len is validated against mem->len.
	 */
	mem.da = 0x1000;
	mem.len = 0x100;

	ret = rproc_check_carveout_da(&rproc, &mem, FW_RSC_ADDR_ANY, 0x50);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static struct kunit_case rproc_core_test_cases[] = {
	KUNIT_CASE(rproc_test_check_carveout_len_exceeds),
	KUNIT_CASE(rproc_test_check_carveout_da_any_mismatch),
	KUNIT_CASE(rproc_test_check_carveout_da_negative_delta),
	KUNIT_CASE(rproc_test_check_carveout_da_exceeds_end),
	KUNIT_CASE(rproc_test_check_carveout_da_exact_fit),
	KUNIT_CASE(rproc_test_check_carveout_da_any_skip_check),
	{}
};

static struct kunit_suite rproc_core_test_suite = {
	.name = "remoteproc_core",
	.test_cases = rproc_core_test_cases,
};

kunit_test_suite(rproc_core_test_suite);

#endif /* _REMOTEPROC_CORE_KUNIT_H */
