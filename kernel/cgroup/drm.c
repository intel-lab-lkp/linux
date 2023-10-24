/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#include <linux/cgroup.h>
#include <linux/cgroup_drm.h>
#include <linux/list.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/signal.h>
#include <linux/slab.h>

#include <drm/drm_drv.h>

struct drm_cgroup_state {
	struct cgroup_subsys_state css;

	struct list_head clients;

	unsigned int weight;

	unsigned int sum_children_weights;

	bool over;
	bool over_budget;

	u64 total_us;

	u64 per_s_budget_us;
	u64 prev_active_us;
	u64 active_us;
};

struct drm_root_cgroup_state {
	struct drm_cgroup_state drmcs;

	unsigned int period_us;

	unsigned int last_scan_duration_us;
	ktime_t prev_timestamp;

	struct delayed_work scan_work;
};

static struct drm_root_cgroup_state root_drmcs = {
	.drmcs.clients = LIST_HEAD_INIT(root_drmcs.drmcs.clients),
};

static DEFINE_MUTEX(drmcg_mutex);

static int drmcg_period_ms = 2000;
module_param(drmcg_period_ms, int, 0644);

static inline struct drm_cgroup_state *
css_to_drmcs(struct cgroup_subsys_state *css)
{
	return container_of(css, struct drm_cgroup_state, css);
}

static u64 drmcs_get_active_time_us(struct drm_cgroup_state *drmcs)
{
	struct drm_file *fpriv;
	u64 total = 0;

	lockdep_assert_held(&drmcg_mutex);

	list_for_each_entry(fpriv, &drmcs->clients, clink) {
		const struct drm_cgroup_ops *cg_ops =
			fpriv->minor->dev->driver->cg_ops;

		if (cg_ops && cg_ops->active_time_us)
			total += cg_ops->active_time_us(fpriv);
	}

	return total;
}

static void
drmcs_signal_budget(struct drm_cgroup_state *drmcs, u64 usage, u64 budget)
{
	struct drm_file *fpriv;

	lockdep_assert_held(&drmcg_mutex);

	list_for_each_entry(fpriv, &drmcs->clients, clink) {
		const struct drm_cgroup_ops *cg_ops =
			fpriv->minor->dev->driver->cg_ops;

		if (cg_ops && cg_ops->signal_budget)
			cg_ops->signal_budget(fpriv, usage, budget);
	}
}

static u64
drmcs_read_weight(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct drm_cgroup_state *drmcs = css_to_drmcs(css);

	return drmcs->weight;
}

static int
drmcs_write_weight(struct cgroup_subsys_state *css, struct cftype *cftype,
		   u64 weight)
{
	struct drm_cgroup_state *drmcs = css_to_drmcs(css);
	int ret;

	if (weight < CGROUP_WEIGHT_MIN || weight > CGROUP_WEIGHT_MAX)
		return -ERANGE;

	ret = mutex_lock_interruptible(&drmcg_mutex);
	if (ret)
		return ret;
	drmcs->weight = weight;
	mutex_unlock(&drmcg_mutex);

	return 0;
}

static int drmcs_show_stat(struct seq_file *sf, void *v)
{
	struct drm_cgroup_state *drmcs = css_to_drmcs(seq_css(sf));
	u64 val;

#ifndef CONFIG_64BIT
	mutex_lock(&drmcg_mutex);
#endif
	val = drmcs->total_us;
#ifndef CONFIG_64BIT
	mutex_unlock(&drmcg_mutex);
#endif

	seq_printf(sf, "usage_usec %llu\n", val);

	return 0;
}

static bool __start_scanning(unsigned int period_us)
{
	struct drm_cgroup_state *root = &root_drmcs.drmcs;
	struct cgroup_subsys_state *node;
	ktime_t start, now;
	bool ok = false;

	lockdep_assert_held(&drmcg_mutex);

	start = ktime_get();
	if (period_us > root_drmcs.last_scan_duration_us)
		period_us -= root_drmcs.last_scan_duration_us;

	rcu_read_lock();

	css_for_each_descendant_post(node, &root->css) {
		struct drm_cgroup_state *drmcs = css_to_drmcs(node);

		if (!css_tryget_online(node))
			goto out;

		drmcs->active_us = 0;
		drmcs->sum_children_weights = 0;

		if (period_us && node == &root->css)
			drmcs->per_s_budget_us =
				DIV_ROUND_UP_ULL((u64)period_us * USEC_PER_SEC,
						 USEC_PER_SEC);
		else
			drmcs->per_s_budget_us = 0;

		css_put(node);
	}

	css_for_each_descendant_post(node, &root->css) {
		struct drm_cgroup_state *drmcs = css_to_drmcs(node);
		struct drm_cgroup_state *parent;
		u64 active;

		if (!css_tryget_online(node))
			goto out;
		if (!node->parent) {
			css_put(node);
			continue;
		}
		if (!css_tryget_online(node->parent)) {
			css_put(node);
			goto out;
		}
		parent = css_to_drmcs(node->parent);

		active = drmcs_get_active_time_us(drmcs);
		if (period_us && active > drmcs->prev_active_us) {
			drmcs->active_us += active - drmcs->prev_active_us;
			drmcs->total_us += drmcs->active_us;
		}
		drmcs->prev_active_us = active;

		parent->active_us += drmcs->active_us;
		parent->total_us += drmcs->active_us;
		parent->sum_children_weights += drmcs->weight;

		css_put(node);
		css_put(&parent->css);
	}

	ok = true;
	now = ktime_get();
	root_drmcs.last_scan_duration_us = ktime_to_us(ktime_sub(now, start));
	root_drmcs.prev_timestamp = now;

out:
	rcu_read_unlock();

	return ok;
}

static void scan_worker(struct work_struct *work)
{
	struct drm_cgroup_state *root = &root_drmcs.drmcs;
	struct cgroup_subsys_state *node;
	unsigned int period_us;

	mutex_lock(&drmcg_mutex);

	rcu_read_lock();

	if (WARN_ON_ONCE(!css_tryget_online(&root->css))) {
		rcu_read_unlock();
		mutex_unlock(&drmcg_mutex);
		return;
	}

	period_us = ktime_to_us(ktime_sub(ktime_get(),
					  root_drmcs.prev_timestamp));

	/*
	 * 1st pass - reset working values and update hierarchical weights and
	 * GPU utilisation.
	 */
	if (!__start_scanning(period_us))
		goto out_retry; /*
				 * Always come back later if scanner races with
				 * core cgroup management. (Repeated pattern.)
				 */

	css_for_each_descendant_pre(node, &root->css) {
		struct drm_cgroup_state *drmcs = css_to_drmcs(node);
		struct cgroup_subsys_state *css;
		u64 reused_us = 0, unused_us = 0;
		unsigned int over_weights = 0;

		if (!css_tryget_online(node))
			goto out_retry;

		/*
		 * 2nd pass - calculate initial budgets, mark over budget
		 * siblings and add up unused budget for the group.
		 */
		css_for_each_child(css, &drmcs->css) {
			struct drm_cgroup_state *sibling = css_to_drmcs(css);

			if (!css_tryget_online(css)) {
				css_put(node);
				goto out_retry;
			}

			sibling->per_s_budget_us  =
				DIV_ROUND_UP_ULL(drmcs->per_s_budget_us *
						 sibling->weight,
						 drmcs->sum_children_weights);

			sibling->over = sibling->active_us >
					sibling->per_s_budget_us;
			if (sibling->over)
				over_weights += sibling->weight;
			else
				unused_us += sibling->per_s_budget_us -
					     sibling->active_us;

			css_put(css);
		}

		/*
		 * 3rd pass - spread unused budget according to relative weights
		 * of over budget siblings.
		 */
		while (over_weights && reused_us < unused_us) {
			unsigned int under = 0;

			unused_us -= reused_us;
			reused_us = 0;

			css_for_each_child(css, &drmcs->css) {
				struct drm_cgroup_state *sibling;
				u64 extra_us, max_us, need_us;

				if (!css_tryget_online(css)) {
					css_put(node);
					goto out_retry;
				}

				sibling = css_to_drmcs(css);
				if (!sibling->over) {
					css_put(css);
					continue;
				}

				extra_us = DIV_ROUND_UP_ULL(unused_us *
							    sibling->weight,
							    over_weights);
				max_us = sibling->per_s_budget_us + extra_us;
				if (max_us > sibling->active_us)
					need_us = sibling->active_us -
						  sibling->per_s_budget_us;
				else
					need_us = extra_us;
				reused_us += need_us;
				sibling->per_s_budget_us += need_us;
				sibling->over = sibling->active_us  >
						sibling->per_s_budget_us;
				if (!sibling->over)
					under += sibling->weight;

				css_put(css);
			}

			over_weights -= under;
		}

		css_put(node);
	}

	/*
	 * 4th pass - send out over/under budget notifications.
	 */
	css_for_each_descendant_post(node, &root->css) {
		struct drm_cgroup_state *drmcs = css_to_drmcs(node);

		if (!css_tryget_online(node))
			goto out_retry;

		if (drmcs->over || drmcs->over_budget)
			drmcs_signal_budget(drmcs,
					    drmcs->active_us,
					    drmcs->per_s_budget_us);
		drmcs->over_budget = drmcs->over;

		css_put(node);
	}

out_retry:
	rcu_read_unlock();
	mutex_unlock(&drmcg_mutex);

	period_us = READ_ONCE(root_drmcs.period_us);
	if (period_us)
		schedule_delayed_work(&root_drmcs.scan_work,
				      usecs_to_jiffies(period_us));

	css_put(&root->css);
}

static void drmcs_free(struct cgroup_subsys_state *css)
{
	if (css != &root_drmcs.drmcs.css)
		kfree(css_to_drmcs(css));
}

static void record_baseline_utilisation(void)
{
	/*
	 * Re-capture baseline group GPU times to avoid downward jumps.
	 *
	 * __start_scanning can fail if hierarchy members transition their
	 * online status while it is traversing the tree, so retry with a little
	 * bit of back-off to be nice, although it is not really needed but
	 * callers are also not latency sensitive, especially since retrying is
	 * very unlikely during stable system operation.
	 */
	while (!__start_scanning(0))
		synchronize_rcu();
}

static struct cgroup_subsys_state *
drmcs_alloc(struct cgroup_subsys_state *parent_css)
{
	struct drm_cgroup_state *drmcs;

	if (!parent_css) {
		drmcs = &root_drmcs.drmcs;
		INIT_DELAYED_WORK(&root_drmcs.scan_work, scan_worker);
	} else {
		drmcs = kzalloc(sizeof(*drmcs), GFP_KERNEL);
		if (!drmcs)
			return ERR_PTR(-ENOMEM);

		INIT_LIST_HEAD(&drmcs->clients);
	}

	drmcs->weight = CGROUP_WEIGHT_DFL;

	return &drmcs->css;
}

static int drmcs_online(struct cgroup_subsys_state *css)
{
	if (css == &root_drmcs.drmcs.css && drmcg_period_ms) {
		const int min_period_ms = 500;
		int period_ms;

		mutex_lock(&drmcg_mutex);
		record_baseline_utilisation();
		if (drmcg_period_ms < min_period_ms) {
			period_ms = min_period_ms;
			pr_notice("Capping DRM control group scanning to %ums\n",
				  period_ms);
		} else {
			period_ms = drmcg_period_ms;
		}
		root_drmcs.period_us = period_ms * 1000;
		mod_delayed_work(system_wq,
				 &root_drmcs.scan_work,
				 usecs_to_jiffies(root_drmcs.period_us));
		mutex_unlock(&drmcg_mutex);
	}

	return 0;
}

static void drmcs_offline(struct cgroup_subsys_state *css)
{
	bool flush = false;

	if (css != &root_drmcs.drmcs.css)
		return;

	mutex_lock(&drmcg_mutex);
	if (root_drmcs.period_us) {
		root_drmcs.period_us = 0;
		cancel_delayed_work(&root_drmcs.scan_work);
		flush = true;
	}
	mutex_unlock(&drmcg_mutex);

	if (flush)
		flush_delayed_work(&root_drmcs.scan_work);
}

static struct drm_cgroup_state *old_drmcs;

static int drmcs_can_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *task;

	task = cgroup_taskset_first(tset, &css);
	old_drmcs = css_to_drmcs(task_css(task, drm_cgrp_id));

	return 0;
}

static void drmcs_attach(struct cgroup_taskset *tset)
{
	struct drm_cgroup_state *old = old_drmcs;
	struct cgroup_subsys_state *css;
	struct drm_file *fpriv, *next;
	struct drm_cgroup_state *new;
	struct task_struct *task;
	bool migrated = false;

	if (!old)
		return;

	task = cgroup_taskset_first(tset, &css);
	new = css_to_drmcs(task_css(task, drm_cgrp_id));
	if (new == old)
		return;

	mutex_lock(&drmcg_mutex);

	list_for_each_entry_safe(fpriv, next, &old->clients, clink) {
		cgroup_taskset_for_each(task, css, tset) {
			struct cgroup_subsys_state *old_css;

			if (task->flags & PF_KTHREAD)
				continue;
			if (!thread_group_leader(task))
				continue;

			new = css_to_drmcs(task_css(task, drm_cgrp_id));
			if (WARN_ON_ONCE(new == old))
				continue;

			if (rcu_access_pointer(fpriv->pid) != task_tgid(task))
				continue;

			if (WARN_ON_ONCE(fpriv->__css != &old->css))
				continue;

			old_css = fpriv->__css;
			fpriv->__css = &new->css;
			css_get(fpriv->__css);
			list_move_tail(&fpriv->clink, &new->clients);
			css_put(old_css);
			migrated = true;
		}
	}

	if (migrated)
		record_baseline_utilisation();

	mutex_unlock(&drmcg_mutex);

	old_drmcs = NULL;
}

static void drmcs_cancel_attach(struct cgroup_taskset *tset)
{
	old_drmcs = NULL;
}

void drmcgroup_client_open(struct drm_file *file_priv)
{
	struct drm_cgroup_state *drmcs;

	if (!file_priv->minor->dev->driver->cg_ops)
		return;

	drmcs = css_to_drmcs(task_get_css(current, drm_cgrp_id));

	mutex_lock(&drmcg_mutex);
	file_priv->__css = &drmcs->css; /* Keeps the reference. */
	list_add_tail(&file_priv->clink, &drmcs->clients);
	mutex_unlock(&drmcg_mutex);
}
EXPORT_SYMBOL_GPL(drmcgroup_client_open);

void drmcgroup_client_close(struct drm_file *file_priv)
{
	struct drm_cgroup_state *drmcs;

	drmcs = css_to_drmcs(file_priv->__css);

	if (!file_priv->minor->dev->driver->cg_ops)
		return;

	mutex_lock(&drmcg_mutex);
	list_del(&file_priv->clink);
	file_priv->__css = NULL;
	record_baseline_utilisation();
	mutex_unlock(&drmcg_mutex);

	css_put(&drmcs->css);
}
EXPORT_SYMBOL_GPL(drmcgroup_client_close);

void drmcgroup_client_migrate(struct drm_file *file_priv)
{
	struct drm_cgroup_state *src, *dst;
	struct cgroup_subsys_state *old;

	if (!file_priv->minor->dev->driver->cg_ops)
		return;

	mutex_lock(&drmcg_mutex);

	old = file_priv->__css;
	src = css_to_drmcs(old);
	dst = css_to_drmcs(task_get_css(current, drm_cgrp_id));

	if (src != dst) {
		file_priv->__css = &dst->css; /* Keeps the reference. */
		list_move_tail(&file_priv->clink, &dst->clients);
		record_baseline_utilisation();
	}

	mutex_unlock(&drmcg_mutex);

	css_put(old);
}
EXPORT_SYMBOL_GPL(drmcgroup_client_migrate);

struct cftype files[] = {
	{
		.name = "weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_u64 = drmcs_read_weight,
		.write_u64 = drmcs_write_weight,
	},
	{
		.name = "stat",
		.seq_show = drmcs_show_stat,
	},
	{ } /* Zero entry terminates. */
};

struct cgroup_subsys drm_cgrp_subsys = {
	.css_alloc	= drmcs_alloc,
	.css_free	= drmcs_free,
	.css_online	= drmcs_online,
	.css_offline	= drmcs_offline,
	.can_attach     = drmcs_can_attach,
	.attach		= drmcs_attach,
	.cancel_attach  = drmcs_cancel_attach,
	.early_init	= false,
	.legacy_cftypes	= files,
	.dfl_cftypes	= files,
};
