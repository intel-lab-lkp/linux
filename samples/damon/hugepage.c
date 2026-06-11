// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 HUAWEI, Inc.
 *             https://www.huawei.com
 *
 * Author: Asier Gutierrez <gutierrez.asier@huawei-partners.com>
 */

#define pr_fmt(fmt) "damon_sample_hugepage: " fmt

#include <linux/damon.h>
#include <linux/kstrtox.h>
#include <linux/module.h>

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_sample_hugepage."

static bool enabled __read_mostly;

static unsigned long target_pid;
module_param(target_pid, ulong, 0600);

/* By default, total huge pages to system memory usage ratio set to 10% */
static unsigned long quota_percentage_hugepage __read_mostly = 1000;
module_param(quota_percentage_hugepage, ulong, 0600);

static unsigned long quota_autotune_feedback __read_mostly;
module_param(quota_autotune_feedback, ulong, 0600);

static struct damon_ctx *ctx;
static struct pid *target_pidp;

static int damon_sample_hugepage_damon_call_fn(void *data)
{
	struct damon_ctx *c = data;
	struct damon_target *t;

	damon_for_each_target(t, c) {
		struct damon_region *r;
		unsigned long hugepages = 0;

		damon_for_each_region(r, t) {
			if (r->nr_accesses > 0)
				hugepages += r->ar.end - r->ar.start;
		}
		hugepages = hugepages / HPAGE_PMD_SIZE;
		pr_info("hugepage: %lu\n", hugepages);
	}
	return 0;
}

static struct damon_call_control call_control = {
	.fn = damon_sample_hugepage_damon_call_fn,
	.repeat = true,
};

static int damon_sample_hugepage_start(void)
{
	int err;
	struct damon_target *target;
	struct damos *scheme;
	struct damos_quota_goal *goal;

	pr_info("start\n");


	ctx = damon_new_ctx();
	if (!ctx)
		return -ENOMEM;
	if (damon_select_ops(ctx, DAMON_OPS_VADDR)) {
		damon_destroy_ctx(ctx);
		return -EINVAL;
	}

	target = damon_new_target();
	if (!target) {
		damon_destroy_ctx(ctx);
		return -ENOMEM;
	}
	damon_add_target(ctx, target);
	target_pidp = find_get_pid(target_pid);
	if (!target_pidp) {
		damon_destroy_ctx(ctx);
		return -EINVAL;
	}
	target->pid = target_pidp;

	scheme = damon_new_scheme(&(struct damos_access_pattern) {
				.min_sz_region = HPAGE_PMD_SIZE,
				.max_sz_region = ULONG_MAX,
				.min_nr_accesses = 0,
				.max_nr_accesses = UINT_MAX,
				.min_age_region = 50,
				.max_age_region = UINT_MAX},
				DAMOS_COLLAPSE, 0,
				&(struct damos_quota) {
				.ms = 10,
				.sz = 128 * 1024 * 1024,
				.reset_interval = 1000,
				.weight_sz = 0,
				.weight_nr_accesses = 1,
				.weight_age = 1,
				.goal_tuner = DAMOS_QUOTA_GOAL_TUNER_TEMPORAL},
				&(struct damos_watermarks){}, NUMA_NO_NODE);
	if (!scheme) {
		damon_destroy_ctx(ctx);
		return -ENOMEM;
	}
	damon_set_schemes(ctx, &scheme, 1);
	goal = damos_new_quota_goal(DAMOS_QUOTA_HUGEPAGE_MEM_BP,
				quota_percentage_hugepage);
	if (!goal) {
		damon_destroy_ctx(ctx);
		return -ENOMEM;
	}
	damos_add_quota_goal(&scheme->quota, goal);

	if (quota_autotune_feedback) {
		goal = damos_new_quota_goal(DAMOS_QUOTA_USER_INPUT, 10000);
		if (!goal) {
			damon_destroy_ctx(ctx);
			return -ENOMEM;
		}
		goal->current_value = quota_autotune_feedback;
		damos_add_quota_goal(&scheme->quota, goal);
	}

	err = damon_start(&ctx, 1, true);
	if (err) {
		damon_destroy_ctx(ctx);
		return err;
	}

	call_control.data = ctx;
	err = damon_call(ctx, &call_control);
	if (err) {
		damon_stop(&ctx, 1);
		damon_destroy_ctx(ctx);
	}
	return err;
}

static void damon_sample_hugepage_stop(void)
{
	pr_info("stop\n");
	if (ctx) {
		damon_stop(&ctx, 1);
		damon_destroy_ctx(ctx);
	}
}
static int damon_sample_hugepage_enabled_store(const char *val,
				const struct kernel_param *kp)
{
	bool is_enabled = enabled;
	int err;

	err = kstrtobool(val, &enabled);
	if (err)
		return err;

	if (enabled == is_enabled)
		return 0;

	if (!damon_initialized())
		return 0;

	if (enabled) {
		err = damon_sample_hugepage_start();
		if (err)
			enabled = false;
		return err;
	}
	damon_sample_hugepage_stop();
	return 0;
}

static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_sample_hugepage_enabled_store,
	.get = param_get_bool,
};

module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled,
	"Enable or disable DAMON_HUGEPAGE (default: disabled)");

static int __init damon_sample_hugepage_init(void)
{
	int err = 0;

	if (!damon_initialized()) {
		if (enabled)
			enabled = false;
		pr_warn("Module not initialized\n");
		return -ENOMEM;
	}

	if (enabled) {
		err = damon_sample_hugepage_start();
		if (err)
			enabled = false;
	}
	return err;
}

module_init(damon_sample_hugepage_init);
