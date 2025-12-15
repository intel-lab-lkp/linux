// SPDX-License-Identifier: GPL-2.0
/*
 * The Proactive Reclamation DAMON-based Module (prdm) is a loadable kernel
 * module that provides out-of-the-box proactive memory reclamation.  It
 * monitors the access patterns of specified processes, finds regions that seem
 * infrequently accessed, and proactively pages out those regions.
 *
 * Based on samples/damon/prcl.c written by SeongJae Park <sj@kernel.org>.
 * This module extends the original by adding support for monitoring multiple
 * target processes concurrently.
 *
 * Author: Enze Li <lienze@kylinos.cn>
 * Copyright (C) 2025 KylinSoft Corporation.
 */

#define pr_fmt(fmt) "damon-prdm: " fmt

#include <linux/damon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "damon_prdm."

static struct damon_ctx *ctx;
static LIST_HEAD(target_list);
static int target_count;

struct prdm_target {
	struct pid *ppid;
	int pid;
	struct list_head list;
};

static int damon_prdm_target_pid_store(const char *val,
				       const struct kernel_param *kp);
static int damon_prdm_target_pid_show(char *buffer,
				      const struct kernel_param *kp);

static const struct kernel_param_ops target_pid_param_ops = {
	.set = damon_prdm_target_pid_store,
	.get = damon_prdm_target_pid_show,
};

static int target_pid __read_mostly;
module_param_cb(target_pid, &target_pid_param_ops, &target_pid, 0600);
MODULE_PARM_DESC(target_pid, "Specifies the pids of the target processes to be monitored");

static int damon_prdm_enable_store(const char *val,
				   const struct kernel_param *kp);

static const struct kernel_param_ops enabled_param_ops = {
	.set = damon_prdm_enable_store,
	.get = param_get_bool,
};

static bool enabled __read_mostly;
module_param_cb(enabled, &enabled_param_ops, &enabled, 0600);
MODULE_PARM_DESC(enabled, "Enable or disable DAMON_PRDM");

static int damon_prdm_start(void)
{
	struct damon_target *target;
	struct damos *scheme;
	struct prdm_target *t;
	struct list_head *pos;
	struct task_struct *task;
	int err, exist_target_num;

	pr_info("start\n");

	if (target_count <= 0)
		return -EINVAL;

	ctx = damon_new_ctx();
	if (!ctx)
		return -ENOMEM;
	if (damon_select_ops(ctx, DAMON_OPS_VADDR)) {
		damon_destroy_ctx(ctx);
		return -EINVAL;
	}

	exist_target_num = 0;
	list_for_each(pos, &target_list) {
		t = list_entry(pos, struct prdm_target, list);
		task = get_pid_task(t->ppid, PIDTYPE_PID);
		if (task) {
			target = damon_new_target();
			if (!target) {
				damon_destroy_ctx(ctx);
				return -ENOMEM;
			}
			target->pid = t->ppid;
			damon_add_target(ctx, target);
			exist_target_num++;
			put_task_struct(task);
		}
	}

	if (exist_target_num <= 0)
		return -EINVAL;

	scheme = damon_new_scheme(
			&(struct damos_access_pattern) {
			.min_sz_region = PAGE_SIZE,
			.max_sz_region = ULONG_MAX,
			.min_nr_accesses = 0,
			.max_nr_accesses = 0,
			.min_age_region = 50,
			.max_age_region = UINT_MAX},
			DAMOS_PAGEOUT,
			0,
			&(struct damos_quota){},
			&(struct damos_watermarks){},
			NUMA_NO_NODE);
	if (!scheme) {
		damon_destroy_ctx(ctx);
		return -ENOMEM;
	}
	damon_set_schemes(ctx, &scheme, 1);

	err = damon_start(&ctx, 1, true);
	if (err)
		return err;

	return 0;
}

static void damon_prdm_stop(void)
{
	pr_info("stop\n");
	if (ctx) {
		damon_stop(&ctx, 1);
		damon_destroy_ctx(ctx);
		ctx = NULL;
	}
}

static int damon_prdm_target_pid_store(const char *val,
				       const struct kernel_param *kp)
{
	int err;
	struct prdm_target *pt;

	if (!damon_initialized())
		return 0;

	err = kstrtoint(val, 0, &target_pid);
	if (err)
		return err;

	pt = kmalloc(sizeof(*pt), GFP_KERNEL);
	if (!pt)
		return -ENOMEM;

	pt->ppid = find_get_pid(target_pid);
	pt->pid = target_pid;
	INIT_LIST_HEAD(&pt->list);
	list_add_tail(&pt->list, &target_list);
	target_count++;

	return 0;
}

static int damon_prdm_target_pid_show(char *buffer,
				      const struct kernel_param *kp)
{
	char buf[1024];
	struct list_head *pos;
	struct prdm_target *t;
	struct task_struct *task;
	int len = 0;

	if (!damon_initialized())
		return 0;

	len = snprintf(buf, sizeof(buf), "%s:", "Tasks");
	list_for_each(pos, &target_list) {
		t = list_entry(pos, struct prdm_target, list);
		task = get_pid_task(t->ppid, PIDTYPE_PID);
		if (task) {
			len += snprintf(buf + len, sizeof(buf) - len, " %d",
					t->pid);
			put_task_struct(task);
		} else {
			len += snprintf(buf + len, sizeof(buf) - len,
					" %d(exited)", t->pid);
		}
	}

	return scnprintf(buffer, 1024, "%s\n", buf);
}

static int damon_prdm_enable_store(const char *val,
				   const struct kernel_param *kp)
{
	bool is_enabled = enabled;
	int err;

	if (!damon_initialized())
		return 0;

	err = kstrtobool(val, &enabled);
	if (err)
		return err;

	if (enabled == is_enabled)
		return 0;

	if (enabled) {
		err = damon_prdm_start();
		if (err)
			enabled = false;
		return err;
	}
	damon_prdm_stop();
	return 0;
}

static void __exit damon_prdm_exit(void)
{
	struct prdm_target *entry, *tmp;

	pr_debug("%s", __func__);
	damon_prdm_stop();

	list_for_each_entry_safe(entry, tmp, &target_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	INIT_LIST_HEAD(&target_list);
	target_count = 0;
}

static int __init damon_prdm_init(void)
{
	if (!damon_initialized()) {
		if (enabled)
			enabled = false;
		return -ENOMEM;
	}

	pr_debug("%s", __func__);
	return 0;
}

module_init(damon_prdm_init);
module_exit(damon_prdm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("A DAMON module for proactive reclamation of multiple processes");
MODULE_AUTHOR("SeongJae Park <sj@kernel.org>, Enze Li <lienze@kylinos.cn>");
