// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <kunit/device.h>
#include <linux/coresight.h>

#include "coresight-priv.h"
#include "coresight-trbe.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

static void test_compute_offset(struct kunit *test)
{
	struct perf_output_handle handle = { 0 };
	struct trbe_buf buf = { 0 };
	struct trbe_cpudata cpudata = { .trbe_align = PAGE_SIZE };
	unsigned long limit;

	if (!static_branch_unlikely(&trbe_trigger_mode_bypass))
		return;

	cpudata.trbe_hw_align = 1;

	buf.nr_pages = SZ_1M / SZ_4K;
	buf.cpudata = &cpudata;

	handle.rb = (void *)&buf;

	/*
	 * ### : Free space, $$$ : Filled space
	 *
	 * |################|################|
	 * `head            `wakeup
	 * `tail            `limit
	 */
	handle.head = 0;
	handle.size = SZ_1M;
	handle.wakeup = SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M / 2);

	/*
	 * |################|################|
	 * `head            `wakeup         `tail
	 *                  `limit
	 */
	handle.head = 0;
	handle.size = SZ_1M - 1;
	handle.wakeup = SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M / 2);

	/*
	 * |#################################|
	 * `head                            `tail
	 * `wakeup                         `limit
	 */
	handle.head = 0;
	handle.size = SZ_1M - 1;
	handle.wakeup = 0;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, 0);

	/*
	 * |#################################|
	 * `head                            `tail
	 *                                   `wakeup
	 *                                 `limit
	 */
	handle.head = 0;
	handle.size = SZ_1M - 1;
	handle.wakeup = SZ_1M;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M - SZ_4K);

	/*
	 * |$$$$$$$$$$$$$$$$|########|#######|
	 *                  `head           `tail
	 *                           `wakeup
	 *                                 `limit
	 */
	handle.head = SZ_1M / 2;
	handle.size = SZ_1M / 2 - 1;
	handle.wakeup = SZ_1M * 3 / 4;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M * 3 / 4);

	/*
	 * |$$$$$$$$|$$$$$$$|################|
	 *                  `head           `tail
	 *          `wakeup
	 *                                 `limit
	 */
	handle.head = SZ_1M / 2;
	handle.size = SZ_1M / 2 - 1;
	handle.wakeup = SZ_1M * 1 / 4;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M - SZ_4K);

	/*
	 * |$$$$$$$$$$$$$$$$|################|
	 *                  `head           `tail
	 *                                  `wakeup
	 *                                 `limit
	 */
	handle.head = SZ_1M / 2;
	handle.size = SZ_1M / 2 - 1;
	handle.wakeup = SZ_1M - 1;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M - SZ_4K);

	/*
	 * |#########|$$$$$$$$$$|########|###|
	 *           `tail      `head    `wakeup
	 *                                   `limit
	 */
	handle.head = SZ_1M * 3 / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = handle.head + SZ_1M / 8;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M * 7 / 8);

	/*
	 * |####|####|$$$$$$$$$$|############|
	 *           `tail      `head
	 *      `wakeup
	 *                                   `limit
	 */
	handle.head = SZ_1M * 3 / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M + SZ_1M / 8;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M);

	/*
	 * |#######|########|$$$$$$$$$$$$$$$$|
	 * `head   `wakeup  `>tail
	 *         `limit
	 */
	handle.head = SZ_1M;
	handle.wakeup = SZ_1M + SZ_1M / 8;
	handle.size = SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M / 8);

	/*
	 * |#######|$$$$$$$$$$$$$$$$$|#######|
	 *         `tail             `head
	 *         `wakeup
	 *                                   `limit
	 */
	handle.head = SZ_1M * 3 / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M + SZ_1M / 4;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M);

	/*
	 * |#######|$$$$$$$$|$$$$$$$$|#######|
	 *         `tail    `wakeup  `head
	 *                                   `limit
	 */
	handle.head = SZ_1M * 3 / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M + SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M);

	/*
	 * |$$$$$$$|########|########|$$$$$$$|
	 *         `head    `wakeup  `tail
	 *                           `limit
	 */
	handle.head = SZ_1M / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M / 2);

	/*
	 * |$$$$$$$|#################|$$$$$$$|
	 *         `head             `tail
	 *                           `wakeup
	 *                           `limit
	 */
	handle.head = SZ_1M / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M * 3 / 4;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M * 3 / 4);

	/*
	 * |$$$$$$$|#################|$$$$$$$|
	 * `wakeup `head             `tail
	 *                           `limit
	 */
	handle.head = SZ_1M / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = 0;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, SZ_1M * 3 / 4);

	/*
	 * |$$$$$$|$$$$$$$$$$$$$$$$$$$$$$$$$$|
	 *        `head
	 *        `tail
	 */
	handle.head = SZ_1M / 4;
	handle.size = 0;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, 0);

	/*
	 * |$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$|#$|
	 *                                `head
	 *                                  `tail
	 */
	handle.head = SZ_1M - SZ_1K * 2;
	handle.size = SZ_1K;
	handle.wakeup = 0;

	limit = __trbe_normal_offset(&handle);
	KUNIT_ASSERT_EQ(test, limit, 0);
}

static void test_compute_offset_and_counter(struct kunit *test)
{
	struct perf_output_handle handle = { 0 };
	struct trbe_buf buf = { 0 };
	struct trbe_cpudata cpudata = { .trbe_align = PAGE_SIZE };
	unsigned long limit;
	u64 count;

	if (static_branch_unlikely(&trbe_trigger_mode_bypass))
		return;

	cpudata.trbe_hw_align = 1;

	buf.nr_pages = SZ_1M / SZ_4K;
	buf.cpudata = &cpudata;

	handle.rb = (void *)&buf;

	/*
	 * ### : Free space, $$$ : Filled space
	 *
	 * |################|################|
	 * `head            `wakeup          `limit
	 * `tail
	 * `----- count ----'
	 */
	handle.head = 0;
	handle.size = SZ_1M;
	handle.wakeup = SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M);
	KUNIT_ASSERT_EQ(test, count, SZ_1M / 2);

	/*
	 * |################|################|
	 * `head            `wakeup         `tail
	 *                                 `limit
	 * `----- count ----'
	 */
	handle.head = 0;
	handle.size = SZ_1M - 1;
	handle.wakeup = SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M - SZ_4K);
	KUNIT_ASSERT_EQ(test, count, SZ_1M / 2);

	/*
	 * |#################################|
	 * `head                            `tail
	 * `wakeup                         `limit
	 */
	handle.head = 0;
	handle.size = SZ_1M - 1;
	handle.wakeup = 0;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M - SZ_4K);
	KUNIT_ASSERT_EQ(test, count, 0);

	/*
	 * |#################################|
	 * `head                            `tail
	 *                                   `wakeup
	 *                                 `limit
	 */
	handle.head = 0;
	handle.size = SZ_1M - 1;
	handle.wakeup = SZ_1M;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M - SZ_4K);
	KUNIT_ASSERT_EQ(test, count, 0);

	/*
	 * |$$$$$$$$$$$$$$$$|########|#######|
	 *                  `head           `tail
	 *                           `wakeup
	 *                                 `limit
	 *                  [  count ]
	 */
	handle.head = SZ_1M / 2;
	handle.size = SZ_1M / 2 - 1;
	handle.wakeup = SZ_1M * 3 / 4;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M - SZ_4K);
	KUNIT_ASSERT_EQ(test, count, SZ_1M / 4);

	/*
	 * |$$$$$$$$|$$$$$$$|################|
	 *                  `head           `tail
	 *          `wakeup
	 *                                 `limit
	 */
	handle.head = SZ_1M / 2;
	handle.size = SZ_1M / 2 - 1;
	handle.wakeup = SZ_1M * 1 / 4;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M - SZ_4K);
	KUNIT_ASSERT_EQ(test, count, 0);

	/*
	 * |$$$$$$$$$$$$$$$$|################|
	 *                  `head           `tail
	 *                                  `wakeup
	 *                                 `limit
	 */
	handle.head = SZ_1M / 2;
	handle.size = SZ_1M / 2 - 1;
	handle.wakeup = SZ_1M - 1;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M - SZ_4K);
	KUNIT_ASSERT_EQ(test, count, 0);

	/*
	 * |#########|$$$$$$$$$$|########|###|
	 *           `tail      `head    `wakeup
	 *                                   `limit
	 *                      [  count ]
	 */
	handle.head = SZ_1M * 3 / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = handle.head + SZ_1M / 8;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M);
	KUNIT_ASSERT_EQ(test, count, SZ_1M / 8);

	/*
	 * |####|####|$$$$$$$$$$|############|
	 *           `tail      `head
	 *      `wakeup
	 *                                   `limit
	 *                      [   count  >>>
	 * >>>       ]
	 */
	handle.head = SZ_1M * 3 / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M + SZ_1M / 8;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M);
	KUNIT_ASSERT_EQ(test, count, SZ_1M / 2);

	/*
	 * |#######|########|$$$$$$$$$$$$$$$$|
	 * `head   `wakeup  `>tail
	 *                  `limit
	 * [ count ]
	 */
	handle.head = SZ_1M;
	handle.wakeup = SZ_1M + SZ_1M / 8;
	handle.size = SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M / 2);
	KUNIT_ASSERT_EQ(test, count, SZ_1M / 8);

	/*
	 * |#######|$$$$$$$$$$$$$$$$$|#######|
	 *         `tail             `head
	 *         `wakeup
	 *                                   `limit
	 *                           [ count >
	 * >>>     ]
	 */
	handle.head = SZ_1M * 3 / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M + SZ_1M / 4;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M);
	KUNIT_ASSERT_EQ(test, count, SZ_1M / 2);

	/*
	 * |#######|$$$$$$$$|$$$$$$$$|#######|
	 *         `tail    `wakeup  `head
	 *                                   `limit
	 *                           [ count >
	 * >>>     ]
	 */
	handle.head = SZ_1M * 3 / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M + SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M);
	KUNIT_ASSERT_EQ(test, count, SZ_1M / 2);

	/*
	 * |$$$$$$$|########|########|$$$$$$$|
	 *         `head    `wakeup  `tail
	 *                           `limit
	 *         [ count  ]
	 */
	handle.head = SZ_1M / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M / 2;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M * 3 / 4);
	KUNIT_ASSERT_EQ(test, count, SZ_1M / 4);

	/*
	 * |$$$$$$$|#################|$$$$$$$|
	 *         `head             `tail
	 *                           `wakeup
	 *                           `limit
	 */
	handle.head = SZ_1M / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = SZ_1M * 3 / 4;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M * 3 / 4);
	KUNIT_ASSERT_EQ(test, count, 0);

	/*
	 * |$$$$$$$|#################|$$$$$$$|
	 * `wakeup `head             `tail
	 *                           `limit
	 */
	handle.head = SZ_1M / 4;
	handle.size = SZ_1M / 2;
	handle.wakeup = 0;

	limit = __trbe_normal_offset(&handle);
	buf.trbe_limit = limit;
	count = __trbe_normal_trigger_count(&handle);

	KUNIT_ASSERT_EQ(test, limit, SZ_1M * 3 / 4);
	KUNIT_ASSERT_EQ(test, count, 0);
}

static struct kunit_case coresight_trbe_testcases[] = {
	KUNIT_CASE(test_compute_offset),
	KUNIT_CASE(test_compute_offset_and_counter),
	{}
};

static struct kunit_suite coresight_trbe_test_suite = {
	.name = "coresight_trbe_test_suite",
	.test_cases = coresight_trbe_testcases,
};

kunit_test_suites(&coresight_trbe_test_suite);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Leo Yan <leo.yan@arm.com>");
MODULE_DESCRIPTION("Arm CoreSight TRBE KUnit tests");
