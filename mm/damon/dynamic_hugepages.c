// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 HUAWEI, Inc.
 *             https://www.huawei.com
 *
 * Author: Asier Gutierrez <gutierrez.asier@huawei-partners.com>
 */

#define pr_fmt(fmt) "damon-dynamic-hotpages: " fmt

#include <linux/damon.h>
#include <linux/kstrtox.h>
#include <linux/list_sort.h>
#include <linux/module.h>
#include <linux/sched/loadavg.h>

#include "modules-common.h"

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_dynamic_hotpages."

#define MAX_MONITORED_PIDS 3
#define HIGHEST_MIN_ACCESS 90
#define HIGH_ACC_THRESHOLD 50
#define MID_ACC_THRESHOLD 15
#define LOW_ACC_THRESHOLD 2

static struct task_struct *monitor_thread;

struct mutex enable_disable_lock;

/*
 * Enable or disable DAMON_HOT_HUGEPAGE.
 *
 * You can enable DAMON_HOT_HUGEPAGE by setting the value of this parameter
 * as ``Y``. Setting it as ``N`` disables DAMON_HOT_HUGEPAGE.  Note that
 * DAMON_HOT_HUGEPAGE could do no real monitoring and reclamation due to the
 * watermarks-based activation condition.  Refer to below descriptions for the
 * watermarks parameter for this.
 */
static bool enabled __read_mostly;

/*
 * DAMON_HOT_HUGEPAGE monitoring period.
 */
static unsigned long monitor_period __read_mostly = 5000000;
module_param(monitor_period, ulong, 0600);

static long monitored_pids[MAX_MONITORED_PIDS];
module_param_array(monitored_pids, long, NULL, 0400);

static int damon_dynamic_hotpages_turn(bool on);

static struct damos_quota damon_dynamic_hotpages_quota = {
	/* use up to 10 ms time, reclaim up to 128 MiB per 1 sec by default */
	.ms = 10,
	.sz = 0,
	.reset_interval = 1000,
	/* Within the quota, page out older regions first. */
	.weight_sz = 0,
	.weight_nr_accesses = 0,
	.weight_age = 1
};
DEFINE_DAMON_MODULES_DAMOS_TIME_QUOTA(damon_dynamic_hotpages_quota);

static struct damos_watermarks damon_dynamic_hotpages_wmarks = {
	.metric = DAMOS_WMARK_FREE_MEM_RATE,
	.interval = 5000000, /* 5 seconds */
	.high = 900, /* 90 percent */
	.mid = 800, /* 80 percent */
	.low = 50, /* 5 percent */
};
DEFINE_DAMON_MODULES_WMARKS_PARAMS(damon_dynamic_hotpages_wmarks);

static struct damon_attrs damon_dynamic_hotpages_mon_attrs = {
	.sample_interval = 5000, /* 5 ms */
	.aggr_interval = 100000, /* 100 ms */
	.ops_update_interval = 0,
	.min_nr_regions = 10,
	.max_nr_regions = 1000,
};
DEFINE_DAMON_MODULES_MON_ATTRS_PARAMS(damon_dynamic_hotpages_mon_attrs);

struct task_monitor_node {
	pid_t pid;

	struct damon_ctx *ctx;
	struct damon_target *target;
	struct damon_call_control call_control;
	u64 previous_utime;
	unsigned long load;
	struct damos_stat stat;
	int min_access;

	struct list_head list;
	struct list_head sorted_list;
	struct list_head active_monitoring;
};

static void find_top_n(struct list_head *task_monitor,
								struct list_head *sorted_tasks)
{
	struct task_monitor_node *entry, *to_test, *tmp;
	struct list_head *pos;
	int i;

	list_for_each_entry(entry, task_monitor, list) {
		i = 0;
		list_for_each(pos, sorted_tasks) {
			i++;
			to_test = list_entry(pos, struct task_monitor_node, sorted_list);

			if (entry->load > to_test->load) {
				list_add_tail(&entry->sorted_list, pos);

				i = MAX_MONITORED_PIDS;
			}

			if (i == MAX_MONITORED_PIDS)
				break;
		}

		if (i < MAX_MONITORED_PIDS)
			list_add_tail(&entry->sorted_list, sorted_tasks);
	}

	i = 0;
	list_for_each_entry_safe(entry, tmp, sorted_tasks, sorted_list) {
		if (i < MAX_MONITORED_PIDS)
			continue;
		list_del_init(&entry->sorted_list);

	}
}

static struct damos *damon_dynamic_hotpages_new_scheme(int min_access,
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
		&damon_dynamic_hotpages_quota,
		/* (De)activate this according to the watermarks. */
		&damon_dynamic_hotpages_wmarks, NUMA_NO_NODE);
}

static int damon_dynamic_hotpages_apply_parameters(
					struct task_monitor_node *monitored_task,
				    int min_access,
					enum damos_action action)
{
	struct damos *scheme;
	struct damon_ctx *param_ctx;
	struct damon_target *param_target;
	struct damos_filter *filter;
	struct pid *spid;
	int err;

	err = damon_modules_new_ctx_target(&param_ctx, &param_target,
					   DAMON_OPS_VADDR);
	if (err)
		return err;

	err = -EINVAL;
	spid = find_get_pid(monitored_task->pid);
	if (!spid) {
		put_pid(spid);
		goto out;
	}

	param_target->pid = spid;

	err = damon_set_attrs(param_ctx, &damon_dynamic_hotpages_mon_attrs);
	if (err)
		goto out;

	err = -ENOMEM;
	scheme = damon_dynamic_hotpages_new_scheme(min_access, action);
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

static int damon_dynamic_hotpages_damon_call_fn(void *arg)
{
	struct task_monitor_node *monitored_task = arg;
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
		err = damon_dynamic_hotpages_apply_parameters(
			monitored_task, min_access, DAMOS_COLLAPSE);
	} else if (min_access > MID_ACC_THRESHOLD) {
		min_access = min_access - 5;
		err = damon_dynamic_hotpages_apply_parameters(
			monitored_task, min_access, DAMOS_COLLAPSE);
	} else if (min_access > LOW_ACC_THRESHOLD) {
		min_access = min_access - 1;
		err = damon_dynamic_hotpages_apply_parameters(
			monitored_task, min_access, DAMOS_COLLAPSE);
	}
	return err;
}

static int damon_dynamic_hotpages_init_task(
								struct task_monitor_node *task_monitor)
{
	int err = 0;
	struct pid *spid;
	struct damon_ctx *ctx = task_monitor->ctx;
	struct damon_target *target = task_monitor->target;

	if (!ctx || !target)
		damon_modules_new_ctx_target(&ctx, &target, DAMON_OPS_VADDR);

	if (ctx->kdamond)
		return 0;

	spid = find_get_pid(task_monitor->pid);
	if (!spid) {
		put_pid(spid);
		return -ESRCH;
	}

	target->pid = spid;

	if (err)
		return err;

	task_monitor->call_control.fn = damon_dynamic_hotpages_damon_call_fn;
	task_monitor->call_control.repeat = true;
	task_monitor->call_control.data = task_monitor;

	struct damos *scheme =
		damon_dynamic_hotpages_new_scheme(HIGHEST_MIN_ACCESS, DAMOS_COLLAPSE);
	if (!scheme)
		return -ENOMEM;

	damon_set_schemes(ctx, &scheme, 1);

	task_monitor->ctx = ctx;
	err = damon_start(&task_monitor->ctx, 1, false);
	if (err)
		return err;

	return damon_call(task_monitor->ctx, &task_monitor->call_control);
}

static int add_monitored_task(struct task_struct *task,
								struct list_head *task_monitor)
{
	struct task_struct *thread;
	struct task_monitor_node *task_node;
	u64 total_time = 0;

	task_node = kzalloc(sizeof(struct task_monitor_node), GFP_KERNEL);
	if (!task_node)
		return -ENOMEM;

	INIT_LIST_HEAD(&task_node->list);
	INIT_LIST_HEAD(&task_node->sorted_list);
	INIT_LIST_HEAD(&task_node->active_monitoring);

	task_node->min_access = HIGHEST_MIN_ACCESS;
	task_node->pid = task_pid_nr(task);

	list_add_tail(&task_node->list, task_monitor);

	for_each_thread(task, thread)
		total_time += thread->utime;

	task_node->previous_utime = total_time;
	return 0;
}

static int damon_dynamic_hotpages_attach_tasks(
										struct list_head *task_monitor_sorted,
										struct list_head *task_monitor_active)
{
	struct task_monitor_node *sorted_task_node, *tmp;
	int err;
	int i = 0;

	sorted_task_node = list_first_entry(
		task_monitor_sorted, struct task_monitor_node, sorted_list);
	while (i < MAX_MONITORED_PIDS && !list_entry_is_head(sorted_task_node,
								task_monitor_sorted, sorted_list)) {
		if (sorted_task_node->ctx && sorted_task_node->ctx->kdamond)
			list_move(&sorted_task_node->active_monitoring,
				  task_monitor_active);
		else {
			rcu_read_lock();
			if (!find_vpid(sorted_task_node->pid)) {
				sorted_task_node->ctx = NULL;
				sorted_task_node = list_next_entry(
					sorted_task_node, sorted_list);

				rcu_read_unlock();
				continue;
			}
			rcu_read_unlock();

			err = damon_dynamic_hotpages_init_task(sorted_task_node);
			if (err) {
				sorted_task_node->ctx = NULL;
				sorted_task_node = list_next_entry(
					sorted_task_node, sorted_list);
				continue;
			}

			list_add(&sorted_task_node->active_monitoring,
				 task_monitor_active);
		}

		monitored_pids[i] = sorted_task_node->pid;
		sorted_task_node = list_next_entry(sorted_task_node, sorted_list);

		i++;
	}

	i = 0;
	list_for_each_entry_safe(sorted_task_node, tmp, task_monitor_active,
				 active_monitoring) {
		if (i < MAX_MONITORED_PIDS) {
			i++;
			continue;
		}

		if (sorted_task_node->ctx) {
			damon_stop(&sorted_task_node->ctx, 1);
			damon_destroy_ctx(sorted_task_node->ctx);
			sorted_task_node->ctx = NULL;
		}

		list_del_init(&sorted_task_node->active_monitoring);
	}
	return 0;
}

static int damon_dynamic_hugepage_sync_monitored_pids(
										struct list_head *task_monitor,
										struct list_head *task_monitor_active)
{
	u64 total_time, delta;
	struct task_struct *entry_task, *thread, *current_task;
	struct task_monitor_node *entry, *next_ptr;
	struct list_head *pos = task_monitor->next;
	struct list_head *next_pos;
	int err = 0;

	LIST_HEAD(task_to_be_removed);

	rcu_read_lock();
	current_task = next_task(&init_task);

	while ((current_task != &init_task)) {
		if (pos == task_monitor) {
			/* We reached the end of monited tasks while still having tasks
			 * in the system. This means that we have new tasks and we should
			 * add the to the monitored tasks
			 */
			err = add_monitored_task(current_task, task_monitor);
			if (err)
				goto out;
			current_task = next_task(current_task);
			continue;
		}

		entry = list_entry(pos, struct task_monitor_node, list);
		entry_task = find_get_task_by_vpid(entry->pid);

		if (!entry_task) {
			/* task doesn't exist, remove it */
			next_pos = pos->next;
			list_move(&entry->list, &task_to_be_removed);
			pos = next_pos;

			continue;
		} else {
			/* We had this task before, update load times */
			total_time = 0;
			for_each_thread(entry_task, thread)
				total_time += thread->utime;

			delta = total_time - entry->previous_utime;
			entry->previous_utime = total_time;

			/* Exponential load average to avoid peaks of short time usage,
			 * which may lead to running DAMON for a not really hot task
			 */
			entry->load = calc_load(entry->load, EXP_15, delta);
			pos = pos->next;
			current_task = next_task(current_task);
		}
		put_task_struct(entry_task);
	}
out:
	rcu_read_unlock();

	list_for_each_entry_safe(entry, next_ptr, &task_to_be_removed, list) {
		list_del_init(&entry->list);
		list_del_init(&entry->sorted_list);

		if (!list_is_head(&entry->active_monitoring,
						task_monitor_active)) {
			if (entry->ctx) {
				damon_stop(&entry->ctx, 1);
				damon_destroy_ctx(entry->ctx);
				entry->ctx = NULL;
			}
			list_del_init(&entry->active_monitoring);
		}
		kfree(entry);
	}

	return err;
}

static int damon_manager_monitor_thread(void *data)
{
	int err = 0;
	struct task_monitor_node *entry, *tmp;

	LIST_HEAD(task_monitor);
	LIST_HEAD(task_monitor_sorted);
	LIST_HEAD(task_monitor_active);

	while (!kthread_should_stop()) {
		err = damon_dynamic_hugepage_sync_monitored_pids(&task_monitor,
									&task_monitor_active);
		if (err)
			return err;

		find_top_n(&task_monitor, &task_monitor_sorted);

		err = damon_dynamic_hotpages_attach_tasks(&task_monitor_sorted,
													&task_monitor_active);
		if (err)
			return err;

		schedule_timeout_idle(usecs_to_jiffies(monitor_period));

		list_for_each_entry_safe(entry, tmp, &task_monitor_sorted,
					sorted_list)
			list_del_init(&entry->sorted_list);
	}

	list_for_each_entry_safe(entry, tmp, &task_monitor_active,
				active_monitoring) {
		if (entry->ctx) {
			err = damon_stop(&entry->ctx, 1);
			damon_destroy_ctx(entry->ctx);
			entry->ctx = NULL;
		}

		list_del_init(&entry->active_monitoring);
	}

	return err;
}

static int damon_dynamic_hotpages_start_monitor_thread(void)
{
	monitor_thread = kthread_create(damon_manager_monitor_thread, NULL,
				 "damon_dynamic");

	if (IS_ERR(monitor_thread))
		return PTR_ERR(monitor_thread);

	wake_up_process(monitor_thread);
	return 0;
}

static int damon_dynamic_hotpages_turn(bool on)
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
	err = damon_dynamic_hotpages_start_monitor_thread();
out:
	mutex_unlock(&enable_disable_lock);
	return err;
}

static int damon_dynamic_hotpages_enabled_store(const char *val,
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

	err = damon_dynamic_hotpages_turn(enable);
	if (err)
		return err;

	enabled = enable;
	return err;
}

static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_dynamic_hotpages_enabled_store,
	.get = param_get_bool,
};

module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled,
	"Enable or disable DAMON_DYNAMIC_HUGEPAGES (default: disabled)");

static int __init damon_dynamic_hotpages_init(void)
{
	int err;

	/* 'enabled' has set before this function, probably via command line */
	if (enabled)
		err = damon_dynamic_hotpages_turn(true);

	if (err && enabled)
		enabled = false;
	return err;
}

module_init(damon_dynamic_hotpages_init);
