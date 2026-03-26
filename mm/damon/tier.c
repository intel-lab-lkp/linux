// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON-based NUMA Memory Tiering
 *
 * Promotes hot pages from slow NUMA node(s) to fast NUMA node(s) and demotes
 * cold pages in the opposite direction, based on DAMON-observed access
 * patterns.  Adjusts the aggressiveness of each direction aiming for a target
 * utilization of the fast (promote_target_nid) node.
 */

#define pr_fmt(fmt) "damon-tier: " fmt

#include <linux/damon.h>
#include <linux/kstrtox.h>
#include <linux/module.h>

#include "modules-common.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_tier."

/*
 * Enable or disable DAMON_TIER.
 *
 * You can enable DAMON_TIER by setting the value of this parameter as ``Y``.
 * Setting it as ``N`` disables DAMON_TIER.  Note that DAMON_TIER could do no
 * real monitoring and migration due to the watermarks-based activation
 * condition.  Refer to below descriptions for the watermarks parameter for
 * this.
 */
static bool enabled __read_mostly;

/*
 * Make DAMON_TIER reads the input parameters again, except ``enabled``.
 *
 * Input parameters that updated while DAMON_TIER is running are not applied
 * by default.  Once this parameter is set as ``Y``, DAMON_TIER reads values
 * of parameters except ``enabled`` again.  Once the re-reading is done, this
 * parameter is set as ``N``.  If invalid parameters are found while the
 * re-reading, DAMON_TIER will be disabled.
 */
static bool commit_inputs __read_mostly;
module_param(commit_inputs, bool, 0600);

/*
 * NUMA node ID of the fast (promote target) memory tier.
 *
 * Pages that are hot on the slow node will be migrated to this node.
 * Cold pages on this node will be demoted to the slow node.  0 by default.
 */
static int promote_target_nid __read_mostly;
module_param(promote_target_nid, int, 0600);

/*
 * NUMA node ID of the slow (demote target) memory tier.
 *
 * Pages that are cold on the fast node will be migrated to this node.
 * Hot pages on this node will be promoted to the fast node.  1 by default.
 */
static int demote_target_nid __read_mostly = 1;
module_param(demote_target_nid, int, 0600);

/*
 * Desired utilization of the fast node in basis points (1/10,000).
 *
 * DAMON_TIER automatically adjusts the promotion and demotion quotas to keep
 * the fast node at this utilization level.  9960 (99.6 %) by default.
 */
static unsigned long promote_target_mem_used_bp __read_mostly = 9960;
module_param(promote_target_mem_used_bp, ulong, 0600);

/*
 * Desired free ratio of the fast node in basis points for demotion.
 *
 * DAMON_TIER adjusts the demotion quota aiming to keep at least this much
 * free memory on the fast node.  40 (0.4 %) by default.
 */
static unsigned long demote_target_mem_free_bp __read_mostly = 40;
module_param(demote_target_mem_free_bp, ulong, 0600);

static struct damos_quota damon_tier_quota = {
	/* 200 MiB per 1 sec by default */
	.ms = 0,
	.sz = 200 * 1024 * 1024,
	.reset_interval = 1000,
	/* Ignore region size; prioritize by access pattern */
	.weight_sz = 0,
	.weight_nr_accesses = 100,
	.weight_age = 100,
};
DEFINE_DAMON_MODULES_DAMOS_QUOTAS(damon_tier_quota);

static struct damos_watermarks damon_tier_wmarks = {
	.metric = DAMOS_WMARK_FREE_MEM_RATE,
	.interval = 5000000,	/* 5 seconds */
	.high = 200,		/* 20 percent */
	.mid = 150,		/* 15 percent */
	.low = 50,		/* 5 percent */
};
DEFINE_DAMON_MODULES_WMARKS_PARAMS(damon_tier_wmarks);

static struct damon_attrs damon_tier_mon_attrs = {
	.sample_interval = 5000,	/* 5 ms */
	.aggr_interval = 100000,	/* 100 ms */
	.ops_update_interval = 0,
	.min_nr_regions = 10,
	.max_nr_regions = 1000,
};
DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS(damon_tier_mon_attrs);

/*
 * Start of the target memory region in physical address.
 *
 * The start physical address of memory region that DAMON_TIER will monitor.
 * By default, biggest System RAM is used as the region.
 */
static unsigned long monitor_region_start __read_mostly;
module_param(monitor_region_start, ulong, 0600);

/*
 * End of the target memory region in physical address.
 *
 * The end physical address of memory region that DAMON_TIER will monitor.
 * By default, biggest System RAM is used as the region.
 */
static unsigned long monitor_region_end __read_mostly;
module_param(monitor_region_end, ulong, 0600);

/*
 * PID of the DAMON thread
 *
 * If DAMON_TIER is enabled, this becomes the PID of the worker thread.
 * Else, -1.
 */
static int kdamond_pid __read_mostly = -1;
module_param(kdamond_pid, int, 0400);

static struct damos_stat damon_tier_promote_stat;
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(damon_tier_promote_stat,
		promote_tried_regions, promoted_regions,
		promote_quota_exceeds);

static struct damos_stat damon_tier_demote_stat;
DEFINE_DAMON_MODULES_DAMOS_STATS_PARAMS(damon_tier_demote_stat,
		demote_tried_regions, demoted_regions,
		demote_quota_exceeds);

static struct damon_ctx *ctx;
static struct damon_target *target;

static struct damos *damon_tier_new_scheme(
		struct damos_access_pattern *pattern,
		enum damos_action action, int target_nid)
{
	struct damos_quota quota = damon_tier_quota;

	/* Use half of total quota for each direction */
	quota.sz = quota.sz / 2;

	return damon_new_scheme(
			pattern,
			action,
			/* apply once per second */
			1000000,
			&quota,
			&damon_tier_wmarks,
			target_nid);
}

static struct damos *damon_tier_new_promote_scheme(void)
{
	struct damos_access_pattern pattern = {
		.min_sz_region = PAGE_SIZE,
		.max_sz_region = ULONG_MAX,
		/* hot: accessed at least once */
		.min_nr_accesses = 1,
		.max_nr_accesses = UINT_MAX,
		.min_age_region = 0,
		.max_age_region = UINT_MAX,
	};

	return damon_tier_new_scheme(&pattern, DAMOS_MIGRATE_HOT,
			promote_target_nid);
}

static struct damos *damon_tier_new_demote_scheme(void)
{
	struct damos_access_pattern pattern = {
		.min_sz_region = PAGE_SIZE,
		.max_sz_region = ULONG_MAX,
		/* cold: not accessed at all */
		.min_nr_accesses = 0,
		.max_nr_accesses = 0,
		.min_age_region = 0,
		.max_age_region = UINT_MAX,
	};

	return damon_tier_new_scheme(&pattern, DAMOS_MIGRATE_COLD,
			demote_target_nid);
}

static int damon_tier_add_quota_goals(struct damos *promote_scheme,
		struct damos *demote_scheme)
{
	struct damos_quota_goal *goal;

	goal = damos_new_quota_goal(DAMOS_QUOTA_NODE_MEM_USED_BP,
			promote_target_mem_used_bp);
	if (!goal)
		return -ENOMEM;
	goal->nid = promote_target_nid;
	damos_add_quota_goal(&promote_scheme->quota, goal);

	goal = damos_new_quota_goal(DAMOS_QUOTA_NODE_MEM_FREE_BP,
			demote_target_mem_free_bp);
	if (!goal)
		return -ENOMEM;
	goal->nid = promote_target_nid;
	damos_add_quota_goal(&demote_scheme->quota, goal);
	return 0;
}

static int damon_tier_add_filters(struct damos *promote_scheme,
		struct damos *demote_scheme)
{
	struct damos_filter *filter;

	/* skip promoting pages that are already young (recently accessed) */
	filter = damos_new_filter(DAMOS_FILTER_TYPE_YOUNG, true, true);
	if (!filter)
		return -ENOMEM;
	damos_add_filter(promote_scheme, filter);

	/* skip demoting pages that are young */
	filter = damos_new_filter(DAMOS_FILTER_TYPE_YOUNG, true, false);
	if (!filter)
		return -ENOMEM;
	damos_add_filter(demote_scheme, filter);
	return 0;
}

static int damon_tier_apply_parameters(void)
{
	struct damon_ctx *param_ctx;
	struct damon_target *param_target;
	struct damos *promote_scheme, *demote_scheme;
	int err;

	err = damon_modules_new_paddr_ctx_target(&param_ctx, &param_target);
	if (err)
		return err;

	err = damon_set_attrs(param_ctx, &damon_tier_mon_attrs);
	if (err)
		goto out;

	err = -ENOMEM;
	promote_scheme = damon_tier_new_promote_scheme();
	if (!promote_scheme)
		goto out;

	demote_scheme = damon_tier_new_demote_scheme();
	if (!demote_scheme) {
		damon_destroy_scheme(promote_scheme);
		goto out;
	}

	damon_set_schemes(param_ctx, &promote_scheme, 1);
	damon_add_scheme(param_ctx, demote_scheme);

	err = damon_tier_add_quota_goals(promote_scheme, demote_scheme);
	if (err)
		goto out;
	err = damon_tier_add_filters(promote_scheme, demote_scheme);
	if (err)
		goto out;

	err = damon_set_region_biggest_system_ram_default(param_target,
					&monitor_region_start,
					&monitor_region_end,
					param_ctx->min_region_sz);
	if (err)
		goto out;
	err = damon_commit_ctx(ctx, param_ctx);
out:
	damon_destroy_ctx(param_ctx);
	return err;
}

static int damon_tier_handle_commit_inputs(void)
{
	int err;

	if (!commit_inputs)
		return 0;

	err = damon_tier_apply_parameters();
	commit_inputs = false;
	return err;
}

static int damon_tier_damon_call_fn(void *arg)
{
	struct damon_ctx *c = arg;
	struct damos *s;

	/* update the stats parameters */
	damon_for_each_scheme(s, c) {
		if (s->action == DAMOS_MIGRATE_HOT)
			damon_tier_promote_stat = s->stat;
		else if (s->action == DAMOS_MIGRATE_COLD)
			damon_tier_demote_stat = s->stat;
	}

	return damon_tier_handle_commit_inputs();
}

static struct damon_call_control call_control = {
	.fn = damon_tier_damon_call_fn,
	.repeat = true,
};

static int damon_tier_turn(bool on)
{
	int err;

	if (!on) {
		err = damon_stop(&ctx, 1);
		if (!err)
			kdamond_pid = -1;
		return err;
	}

	err = damon_tier_apply_parameters();
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

static int damon_tier_enabled_store(const char *val,
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

	/* Called before init function.  The function will handle this. */
	if (!damon_initialized())
		goto set_param_out;

	err = damon_tier_turn(enable);
	if (err)
		return err;

set_param_out:
	enabled = enable;
	return err;
}

static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_tier_enabled_store,
	.get = param_get_bool,
};

module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled,
	"Enable or disable DAMON_TIER (default: disabled)");

static int __init damon_tier_init(void)
{
	int err;

	if (!damon_initialized()) {
		err = -ENOMEM;
		goto out;
	}
	err = damon_modules_new_paddr_ctx_target(&ctx, &target);
	if (err)
		goto out;

	call_control.data = ctx;

	/* 'enabled' has set before this function, probably via command line */
	if (enabled)
		err = damon_tier_turn(true);

out:
	if (err && enabled)
		enabled = false;
	return err;
}

module_init(damon_tier_init);
