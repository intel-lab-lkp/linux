// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 HUAWEI, Inc.
 *             https://www.huawei.com
 *
 * Author: Asier Gutierrez <gutierrez.asier@huawei-partners.com>
 */

#define pr_fmt(fmt) "damon-hugepage: " fmt

#include <linux/damon.h>
#include <linux/kstrtox.h>
#include <linux/module.h>

#include "modules-common.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_hugepage."

/*
 * Enable or disable DAMON_HUGEPAGE.
 *
 * You can enable DAMON_HUGEPAGE by setting the value of this parameter
 * as ``Y``. Setting it as ``N`` disables DAMON_HUGEPAGE.
 */
static bool enabled __read_mostly;

/*
 * Make DAMON_HUGEPAGE reads the input parameters again, except ``enabled``.
 *
 * Input parameters that updated while DAMON_HUGEPAGE is running are not applied
 * by default.  Once this parameter is set as ``Y``, DAMON_HUGEPAGE reads values
 * of parametrs except ``enabled`` again.  Once the re-reading is done, this
 * parameter is set as ``N``.  If invalid parameters are found while the
 * re-reading, DAMON_HUGEPAGE will be disabled.
 */
static bool commit_inputs __read_mostly;
module_param(commit_inputs, bool, 0600);

/*
 * Scale factor for DAMON_HUGEPAGE to ops address conversion.
 *
 * This parameter must not be set to 0.
 */
static unsigned long addr_unit __read_mostly = 1;

static unsigned long monitored_pid;
module_param(monitored_pid, ulong, 0600);

/*
 * Time threshold for hot memory regions identification in microseconds.
 *
 * If a memory region has been accessed for this or longer time,
 * DAMON_HUGEPAGE identifies the region as hot, and collapse it into a huge
 * page.  100 microseconds by default.
 */
static unsigned long min_age __read_mostly = 100000;
module_param(min_age, ulong, 0600);

static struct damos_quota damon_hugepage_quota = {
	/* use up to 10 ms time, collapse up to 128 MiB per 1 sec by default */
	.ms = 10,
	.sz = 128 * 1024 * 1024,
	.reset_interval = 1000,
	.weight_sz = 0,
	.weight_nr_accesses = 1,
	.weight_age = 1
};
DEFINE_DAMON_MODULES_DAMOS_QUOTAS(damon_hugepage_quota);


/*
 * Desired ratio of huge pages use vs total anonymous memory usage.
 *
 * While keeping the caps that set by other quotas, DAMON_HUGEPAGE automatically
 * increases and decreases the effective level of the quota to achieve the
 * desired ratio.
 *
 * 100 bp by default.
 */
static unsigned long quota_percentage_hugepage __read_mostly = 100;
module_param(quota_percentage_hugepage, ulong, 0600);

/*
 * User-specifiable feedback for auto-tuning of the effective quota.
 *
 * While keeping the caps that set by other quotas, DAMON_HUGEPAGE automatically
 * increases and decreases the effective level of the quota aiming receiving this
 * feedback of value ``10,000`` from the user.  DAMON_HUGEPAGE assumes the feedback
 * value and the quota are positively proportional.  Value zero means disabling
 * this auto-tuning feature.
 *
 * Disabled by default.
 *
 */
static unsigned long quota_autotune_feedback __read_mostly;
module_param(quota_autotune_feedback, ulong, 0600);

/*
 * PID of the DAMON thread
 *
 * If DAMON_HUGEPAGE is enabled, this becomes the PID of the worker thread.
 * Else, -1.
 */
static int kdamond_pid __read_mostly = -1;
module_param(kdamond_pid, int, 0400);

static struct damos_stat damon_hugepage_stat;
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(damon_hugepage_stat,
		hugepage_tried_regions, hugepage_regions, quota_exceeds);

static struct damon_attrs damon_hugepage_mon_attrs = {
	.sample_interval = 5 * USEC_PER_MSEC,
	.aggr_interval = 100 * USEC_PER_MSEC,
	.ops_update_interval = 60 * USEC_PER_MSEC * MSEC_PER_SEC,
	.min_nr_regions = 10,
	.max_nr_regions = 1000,
};
DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS(damon_hugepage_mon_attrs);

static struct damon_ctx *ctx;
static struct damon_target *target;

static struct damos *damon_hugepage_new_scheme(void)
{
	struct damos_access_pattern pattern = {
		/* Find regions having PMD_SIZE or larger size */
		.min_sz_region = PMD_SIZE,
		.max_sz_region = ULONG_MAX,
		.min_nr_accesses = 0,
		.max_nr_accesses = UINT_MAX,
		.min_age_region = min_age /
			damon_hugepage_mon_attrs.aggr_interval,
		.max_age_region = UINT_MAX,
	};

	return damon_new_scheme(
		&pattern,
		/* synchrounous partial collapse as soon as found */
		DAMOS_COLLAPSE, 0,
		/* under the quota. */
		&damon_hugepage_quota,
		&(struct damos_watermarks){}, NUMA_NO_NODE);
}

static int damon_hugepage_apply_parameters(void)
{
	struct damon_ctx *param_ctx;
	struct damon_target *param_target;
	struct damos *scheme;
	struct damos_quota_goal *goal;
	struct pid *spid;
	int err;

	err = damon_modules_new_vaddr_ctx_target(&param_ctx, &param_target);
	if (err)
		return err;

	param_ctx->addr_unit = addr_unit;
    // align power of two
	param_ctx->min_region_sz = max(DAMON_MIN_REGION_SZ / ALIGN(addr_unit, 2), 1);

	spid = find_get_pid(monitored_pid);
	if (!spid) {
		err = -EINVAL;
		goto out;
	}

	param_target->pid = spid;

	if (!damon_hugepage_mon_attrs.aggr_interval) {
		err = -EINVAL;
		goto out;
	}

	err = damon_set_attrs(param_ctx, &damon_hugepage_mon_attrs);
	if (err)
		goto out;

	err = -ENOMEM;
	scheme = damon_hugepage_new_scheme();
	if (!scheme)
		goto out;
	damon_set_schemes(param_ctx, &scheme, 1);

	goal = damos_new_quota_goal(DAMOS_QUOTA_HUGEPAGE, quota_percentage_hugepage);
	if (!goal)
		goto out;
	damos_add_quota_goal(&scheme->quota, goal);

	if (quota_autotune_feedback) {
		goal = damos_new_quota_goal(DAMOS_QUOTA_USER_INPUT, 10000);
		if (!goal)
			goto out;
		goal->current_value = quota_autotune_feedback;
		damos_add_quota_goal(&scheme->quota, goal);
	}

	err = damon_commit_ctx(ctx, param_ctx);
out:
	damon_destroy_ctx(param_ctx);
	return err;
}

static int damon_hugepage_handle_commit_inputs(void)
{
	int err;

	if (!commit_inputs)
		return 0;

	err = damon_hugepage_apply_parameters();
	commit_inputs = false;
	return err;
}

static int damon_hugepage_damon_call_fn(void *arg)
{
	struct damon_ctx *c = arg;
	struct damos *s;

	/* update the stats parameter */
	damon_for_each_scheme(s, c)
		damon_hugepage_stat = s->stat;

	return damon_hugepage_handle_commit_inputs();
}

static struct damon_call_control call_control = {
	.fn = damon_hugepage_damon_call_fn,
	.repeat = true,
};

static int damon_hugepage_turn(bool on)
{
	int err;

	if (!on) {
		err = damon_stop(&ctx, 1);
		if (!err)
			kdamond_pid = -1;
		return err;
	}

	err = damon_hugepage_apply_parameters();
	if (err)
		return err;

	err = damon_start(&ctx, 1, true);
	if (err)
		return err;
	kdamond_pid = damon_kdamond_pid(ctx);
	if (kdamond_pid < 0)
		return kdamond_pid;
	return damon_call(ctx, &call_control);
}

static int damon_hugepage_addr_unit_store(const char *val,
		const struct kernel_param *kp)
{
	unsigned long input_addr_unit;
	int err = kstrtoul(val, 0, &input_addr_unit);

	if (err)
		return err;
	if (!input_addr_unit)
		return -EINVAL;

	addr_unit = input_addr_unit;
	return 0;
}

static const struct kernel_param_ops addr_unit_param_ops = {
	.set = damon_hugepage_addr_unit_store,
	.get = param_get_ulong,
};

module_param_cb(addr_unit, &addr_unit_param_ops, &addr_unit, 0600);
MODULE_PARM_DESC(addr_unit,
	"Scale factor for DAMON_HUGEPAGE to ops address conversion (default: 1)");

static int damon_hugepage_enabled_store(const char *val,
				const struct kernel_param *kp)
{
	bool is_enabled = enabled;
	bool enable;
	int err;

	err = kstrtobool(val, &enable);
	if (err)
		return err;

	if (is_enabled == enable)
		return 0;

	/* Monitored process may have exited. In that case, ctx->kdamon is set */
	/* to NULL. If enabled is set to 'off' through sysfs, we just set the  */
	/* params and exit */
	if (!damon_is_running(ctx) && !enable)
		goto set_param_out;

	/* Called before init function.  The function will handle this. */
	if (!damon_initialized())
		goto set_param_out;

	err = damon_hugepage_turn(enable);
	if (err)
		return err;

set_param_out:
	enabled = enable;
	return err;
}

static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_hugepage_enabled_store,
	.get = param_get_bool,
};

module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled,
	"Enable or disable DAMON_HUGEPAGE (default: disabled)");

static int __init damon_hugepage_init(void)
{
	int err;

	if (!damon_initialized()) {
		err = -ENOMEM;
		goto out;
	}
	err = damon_modules_new_vaddr_ctx_target(&ctx, &target);
	if (err)
		goto out;

	call_control.data = ctx;

	/* 'enabled' has set before this function, probably via command line */
	if (enabled)
		err = damon_hugepage_turn(true);

out:
	if (err && enabled)
		enabled = false;
	return err;
}

module_init(damon_hugepage_init);
