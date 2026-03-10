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

#define MAX_MONITORED_PIDS 100
#define HIGHEST_MIN_ACCESS 90
#define HIGH_ACC_THRESHOLD 50
#define MID_ACC_THRESHOLD 15
#define LOW_ACC_THRESHOLD 2

static struct task_struct *monitor_thread;

struct mutex enable_disable_lock;

/*
 * Enable or disable DAMON_HUGEPAGE.
 *
 * You can enable DAMON_HUGEPAGE by setting the value of this parameter
 * as ``Y``. Setting it as ``N`` disables DAMON_HOT_HUGEPAGE.  Note that
 * DAMON_HOT_HUGEPAGE could do no real monitoring and reclamation due to the
 * watermarks-based activation condition.  Refer to below descriptions for the
 * watermarks parameter for this.
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
 * DAMON_HUGEPAGE monitoring period in microseconds.
 * 5000000 = 5s
 */
static unsigned long monitor_period __read_mostly = 5000000;
module_param(monitor_period, ulong, 0600);

static long monitored_pids[MAX_MONITORED_PIDS];
static int num_monitored_pids;
module_param_array(monitored_pids, long, &num_monitored_pids, 0600);

static struct damos_quota damon_hugepage_quota = {
	/* use up to 10 ms time, reclaim up to 128 MiB per 1 sec by default */
	.ms = 10,
	.sz = 0,
	.reset_interval = 1000,
	/* Within the quota, page out older regions first. */
	.weight_sz = 0,
	.weight_nr_accesses = 0,
	.weight_age = 1
};
DEFINE_DAMON_MODULES_DAMOS_TIME_QUOTA(damon_hugepage_quota);

static struct damos_watermarks damon_hugepage_wmarks = {
	.metric = DAMOS_WMARK_FREE_MEM_RATE,
	.interval = 5000000, /* 5 seconds */
	.high = 900, /* 90 percent */
	.mid = 800, /* 80 percent */
	.low = 50, /* 5 percent */
};
DEFINE_DAMON_MODULES_WMARKS_PARAMS(damon_hugepage_wmarks);

static struct damon_attrs damon_hugepage_mon_attrs = {
	.sample_interval = 5000, /* 5 ms */
	.aggr_interval = 100000, /* 100 ms */
	.ops_update_interval = 0,
	.min_nr_regions = 10,
	.max_nr_regions = 1000,
};
DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS(damon_hugepage_mon_attrs);

struct hugepage_task {
	struct damon_ctx *ctx;
	int pid;
	struct damon_target *target;
	struct damon_call_control call_control;
	struct list_head list;
};

static struct damos *damon_hugepage_new_scheme(int min_access,
						   enum damos_action action)
{
	struct damos_access_pattern pattern = {
		/* Find regions having PAGE_SIZE or larger size */
		.min_sz_region = PMD_SIZE,
		.max_sz_region = ULONG_MAX,
		/* and not accessed at all */
		.min_nr_accesses = min_access,
		.max_nr_accesses = 100,
		/* for min_age or more micro-seconds */
		.min_age_region = 0,
		.max_age_region = UINT_MAX,
	};

	return damon_new_scheme(
		&pattern,
		/* synchrounous partial collapse as soon as found */
		action, 0,
		/* under the quota. */
		&damon_hugepage_quota,
		/* (De)activate this according to the watermarks. */
		&damon_hugepage_wmarks, NUMA_NO_NODE);
}

static int damon_hugepage_apply_parameters(
				struct hugepage_task *monitored_task,
				int min_access,
				enum damos_action action)
{
	struct damos *scheme;
	struct damon_ctx *param_ctx;
	struct damon_target *param_target;
	struct damos_filter *filter;
	int err;
	struct pid *spid;

	err = damon_modules_new_ctx_target(&param_ctx, &param_target,
					   DAMON_OPS_VADDR);
	if (err)
		return err;

	spid = find_get_pid(monitored_task->pid);
	if (!spid)
		return err;

	param_target->pid = spid;

	err = damon_set_attrs(param_ctx, &damon_hugepage_mon_attrs);
	if (err)
		goto out;

	err = -ENOMEM;
	scheme = damon_hugepage_new_scheme(min_access, action);
	if (!scheme)
		goto out;

	damon_set_schemes(param_ctx, &scheme, 1);

	filter = damos_new_filter(DAMOS_FILTER_TYPE_ANON, true, false);
	if (!filter)
		goto out;
	damos_add_filter(scheme, filter);

	err = damon_commit_ctx(monitored_task->ctx, param_ctx);
out:
	damon_destroy_ctx(param_ctx);
	return err;
}

static int damon_hugepage_damon_call_fn(void *arg)
{
	struct hugepage_task *monitored_task = arg;
	struct damon_ctx *ctx = monitored_task->ctx;
	struct damos *scheme;
	int err = 0;
	int min_access;
	struct damos_stat stat;

	damon_for_each_scheme(scheme, ctx)
		stat = scheme->stat;
	scheme = list_first_entry(&ctx->schemes, struct damos, list);

	if (ctx->passed_sample_intervals < scheme->next_apply_sis)
		return err;

	if (stat.nr_applied)
		return err;

	min_access = scheme->pattern.min_nr_accesses;

	if (min_access > HIGH_ACC_THRESHOLD) {
		min_access = min_access - 10;
		err = damon_hugepage_apply_parameters(
			monitored_task, min_access, DAMOS_COLLAPSE);
	} else if (min_access > MID_ACC_THRESHOLD) {
		min_access = min_access - 5;
		err = damon_hugepage_apply_parameters(
			monitored_task, min_access, DAMOS_COLLAPSE);
	} else if (min_access > LOW_ACC_THRESHOLD) {
		min_access = min_access - 1;
		err = damon_hugepage_apply_parameters(
			monitored_task, min_access, DAMOS_COLLAPSE);
	}
	return err;
}

static int damon_hugepage_init_task(struct hugepage_task *monitored_task)
{
	int err = 0;
	struct damon_ctx *ctx = monitored_task->ctx;
	struct damon_target *target = monitored_task->target;
	struct pid *spid;

	if (!ctx || !target)
		damon_modules_new_ctx_target(&ctx, &target, DAMON_OPS_VADDR);

	if (damon_is_running(ctx))
		return 0;

	spid = find_get_pid(monitored_task->pid);
	if (!spid)
		return err;

	target->pid = spid;

	monitored_task->call_control.fn = damon_hugepage_damon_call_fn;
	monitored_task->call_control.repeat = true;
	monitored_task->call_control.data = monitored_task;

	struct damos *scheme = damon_hugepage_new_scheme(
			HIGHEST_MIN_ACCESS, DAMOS_COLLAPSE);
	if (!scheme)
		return -ENOMEM;

	damon_set_schemes(ctx, &scheme, 1);

	monitored_task->ctx = ctx;
	err = damon_start(&monitored_task->ctx, 1, false);
	if (err)
		return err;

	return damon_call(monitored_task->ctx, &monitored_task->call_control);
}

static int add_monitored_task(int pid, struct list_head *task_monitor)
{
	struct hugepage_task *new_hugepage_task;
	int err;

	new_hugepage_task = kzalloc_obj(*new_hugepage_task);
	if (!new_hugepage_task)
		return -ENOMEM;

	new_hugepage_task->pid = pid;
	INIT_LIST_HEAD(&new_hugepage_task->list);
	err = damon_hugepage_init_task(new_hugepage_task);
	if (err)
		return err;
	list_add(&new_hugepage_task->list, task_monitor);
	return 0;
}

static int damon_hugepage_handle_commit_inputs(
		struct list_head *monitored_tasks)
{
	int i = 0;
	int err = 0;
	bool found;
	struct hugepage_task *monitored_task, *tmp;

	if (!commit_inputs)
		return 0;

	while (i < MAX_MONITORED_PIDS) {
		if (!monitored_pids[i])
			break;

		found = false;

		rcu_read_lock();
		if (!find_vpid(monitored_pids[i])) {
			rcu_read_unlock();
			continue;
		}

		rcu_read_unlock();

		list_for_each_entry_safe(monitored_task, tmp, monitored_tasks, list) {
			if (monitored_task->pid == monitored_pids[i]) {
				list_move(&monitored_task->list, monitored_tasks);
				found = true;
				break;
			}
		}
		if (!found) {
			err = add_monitored_task(monitored_pids[i], monitored_tasks);
			/* Skip failed tasks */
			if (err)
				continue;
		}
		i++;
	}

	i = 0;
	list_for_each_entry_safe(monitored_task, tmp, monitored_tasks, list) {
		i++;
		if (i <= num_monitored_pids)
			continue;

		err = damon_stop(&monitored_task->ctx, 1);
		damon_destroy_ctx(monitored_task->ctx);
		list_del(&monitored_task->list);
		kfree(monitored_task);
	}

	commit_inputs = false;
	return err;
}

static int damon_manager_monitor_thread(void *data)
{
	int err = 0;
	int i;
	struct hugepage_task *entry, *tmp;

	LIST_HEAD(monitored_tasks);

	for (i = 0; i < MAX_MONITORED_PIDS; i++) {
		if (!monitored_pids[i])
			break;

		rcu_read_lock();
		if (!find_vpid(monitored_pids[i])) {
			rcu_read_unlock();
			continue;
		}
		rcu_read_unlock();

		add_monitored_task(monitored_pids[i], &monitored_tasks);
	}


	while (!kthread_should_stop()) {
		schedule_timeout_idle(usecs_to_jiffies(monitor_period));
		err = damon_hugepage_handle_commit_inputs(&monitored_tasks);
		if (err)
			break;
	}

	list_for_each_entry_safe(entry, tmp, &monitored_tasks, list) {
		err = damon_stop(&entry->ctx, 1);
		damon_destroy_ctx(entry->ctx);
	}

	for (int i = 0; i < MAX_MONITORED_PIDS;) {
		monitored_pids[i] = 0;
		i++;
	}
	return err;
}

static int damon_hugepage_start_monitor_thread(void)
{
	num_monitored_pids = 0;
	monitor_thread = kthread_create(damon_manager_monitor_thread, NULL,
				 "damon_dynamic");

	if (IS_ERR(monitor_thread))
		return PTR_ERR(monitor_thread);

	wake_up_process(monitor_thread);
	return 0;
}

static int damon_hugepage_turn(bool on)
{
	int err = 0;

	mutex_lock(&enable_disable_lock);
	if (!on) {
		if (monitor_thread) {
			kthread_stop(monitor_thread);
			monitor_thread = NULL;
		}
		goto out;
	}
	err = damon_hugepage_start_monitor_thread();
out:
	mutex_unlock(&enable_disable_lock);
	return err;
}

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

	err = damon_hugepage_turn(enable);
	if (err)
		return err;

	enabled = enable;
	return err;
}

static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_hugepage_enabled_store,
	.get = param_get_bool,
};

module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled,
	"Enable or disable DAMON_DYNAMIC_HUGEPAGES (default: disabled)");

static int __init damon_hugepage_init(void)
{
	int err;

	/* 'enabled' has set before this function, probably via command line */
	if (enabled)
		err = damon_hugepage_turn(true);

	if (err && enabled)
		enabled = false;
	return err;
}

module_init(damon_hugepage_init);
