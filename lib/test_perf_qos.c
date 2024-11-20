// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel module for testing performance QoS
 *
 * Copyright (2024) Linaro Ltd
 *
 * Author: Daniel Lezcano <daniel.lezcano@kernel.org>
 *
 */
#include <linux/module.h>
#include <linux/perf_qos.h>

static struct perf_qos *pq;

static int test_set_perf_limit_min(int limit)
{
	static int prev_limit = -1;

	pr_info("Performance minimal limit set to %d->%d\n",
		prev_limit, limit);

	WARN_ON_ONCE(prev_limit == limit);
	
	return 0;
}

static int test_set_perf_limit_max(int limit)
{
	static int prev_limit = -1;

	pr_info("Performance maximal limit set to %d->%d\n",
		prev_limit, limit);

	WARN_ON_ONCE(prev_limit == limit);

	return 0;
}

static int __init test_perf_qos_init(void)
{
	struct perf_qos_ops ops = {
		.set_perf_limit_max = test_set_perf_limit_max,
		.set_perf_limit_min = test_set_perf_limit_min,
	};

	struct perf_qos_value_descr descr = {
		.unit = PERF_QOS_UNIT_NORMAL,
		.limit_min = 0,
		.limit_max = 1024,
	};
	
	pq = perf_qos_device_create("dummy", &ops, &descr);
	if (IS_ERR(pq))
		return PTR_ERR(pq);

	return 0;
}

static void __exit test_perf_qos_exit(void)
{
	perf_qos_device_destroy(pq);
}

module_init(test_perf_qos_init);
module_exit(test_perf_qos_exit);

MODULE_AUTHOR("Daniel Lezcano <daniel.lezcano@kernel.org>");
MODULE_DESCRIPTION("Kernel module for testing the performance QoS");
MODULE_LICENSE("GPL");
