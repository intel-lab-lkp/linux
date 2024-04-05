// SPDX-License-Identifier: GPL-2.0-only
/*
 * Process number limiting controller for cgroups.
 *
 * Used to allow a cgroup hierarchy to stop any new processes from fork()ing
 * after a certain limit is reached.
 *
 * Since it is trivial to hit the task limit without hitting any kmemcg limits
 * in place, PIDs are a fundamental resource. As such, PID exhaustion must be
 * preventable in the scope of a cgroup hierarchy by allowing resource limiting
 * of the number of tasks in a cgroup.
 *
 * In order to use the `pids` controller, set the maximum number of tasks in
 * pids.max (this is not available in the root cgroup for obvious reasons). The
 * number of processes currently in the cgroup is given by pids.current.
 * Organisational operations are not blocked by cgroup policies, so it is
 * possible to have pids.current > pids.max. However, it is not possible to
 * violate a cgroup policy through fork(). fork() will return -EAGAIN if forking
 * would cause a cgroup policy to be violated.
 *
 * To set a cgroup to have no limit, set pids.max to "max". This is the default
 * for all new cgroups (N.B. that PID limits are hierarchical, so the most
 * stringent limit in the hierarchy is followed).
 *
 * pids.current tracks all child cgroup hierarchies, so parent/pids.current is
 * a superset of parent/child/pids.current.
 *
 * Copyright (C) 2015 Aleksa Sarai <cyphar@cyphar.com>
 */

#include <linux/kernel.h>
#include <linux/threads.h>
#include <linux/atomic.h>
#include <linux/cgroup.h>
#include <linux/slab.h>
#include <linux/sched/task.h>

#define PIDS_MAX (PID_MAX_LIMIT + 1ULL)
#define PIDS_MAX_STR "max"

enum pidcg_event {
	/* Fork failed in subtree because this pids_cgroup limit was hit. */
	PIDCG_MAX,
	/* Fork failed in this pids_cgroup because ancestor limit was hit. */
	PIDCG_MAX_IMPOSED,
	NR_PIDCG_EVENTS,
};

struct pids_cgroup {
	struct cgroup_subsys_state	css;

	/*
	 * Use 64-bit types so that we can safely represent "max" as
	 * %PIDS_MAX = (%PID_MAX_LIMIT + 1).
	 */
	atomic64_t			counter;
	atomic64_t			limit;
	int64_t				watermark;

	/* Handles for pids.events[.local] */
	struct cgroup_file		events_file;
	struct cgroup_file		events_local_file;

	atomic64_t			events[NR_PIDCG_EVENTS];
	atomic64_t			events_local[NR_PIDCG_EVENTS];
};

static struct pids_cgroup *css_pids(struct cgroup_subsys_state *css)
{
	return container_of(css, struct pids_cgroup, css);
}

static struct pids_cgroup *parent_pids(struct pids_cgroup *pids)
{
	return css_pids(pids->css.parent);
}

static struct cgroup_subsys_state *
pids_css_alloc(struct cgroup_subsys_state *parent)
{
	struct pids_cgroup *pids;

	pids = kzalloc(sizeof(struct pids_cgroup), GFP_KERNEL);
	if (!pids)
		return ERR_PTR(-ENOMEM);

	atomic64_set(&pids->limit, PIDS_MAX);
	return &pids->css;
}

static void pids_css_free(struct cgroup_subsys_state *css)
{
	kfree(css_pids(css));
}

static void pids_update_watermark(struct pids_cgroup *p, int64_t nr_pids)
{
	/*
	 * This is racy, but we don't need perfectly accurate tallying of
	 * the watermark, and this lets us avoid extra atomic overhead.
	 */
	if (nr_pids > READ_ONCE(p->watermark))
		WRITE_ONCE(p->watermark, nr_pids);
}

/**
 * pids_cancel - uncharge the local pid count
 * @pids: the pid cgroup state
 * @num: the number of pids to cancel
 *
 * This function will WARN if the pid count goes under 0, because such a case is
 * a bug in the pids controller proper.
 */
static void pids_cancel(struct pids_cgroup *pids, int num)
{
	/*
	 * A negative count (or overflow for that matter) is invalid,
	 * and indicates a bug in the `pids` controller proper.
	 */
	WARN_ON_ONCE(atomic64_add_negative(-num, &pids->counter));
}

/**
 * pids_uncharge - hierarchically uncharge the pid count
 * @pids: the pid cgroup state
 * @num: the number of pids to uncharge
 */
static void pids_uncharge(struct pids_cgroup *pids, int num)
{
	struct pids_cgroup *p;

	for (p = pids; parent_pids(p); p = parent_pids(p))
		pids_cancel(p, num);
}

/**
 * pids_try_charge - hierarchically try to charge the pid count
 * @pids: the pid cgroup state
 * @num: the number of pids to charge
 * @root: charge only under this root (NULL is global root)
 * @fail: storage of pid cgroup causing the fail
 *
 * This function follows the set limit. It will fail if the charge would cause
 * the new value to exceed the hierarchical limit and fail is set. Returns 0 if
 * no limit was hit, otherwise -EAGAIN.
 */
static int pids_try_charge(struct pids_cgroup *pids, int num, struct pids_cgroup *root, struct pids_cgroup **fail)
{
	struct pids_cgroup *p, *q;
	int ret = 0;

	for (p = pids; parent_pids(p) && p != root; p = parent_pids(p)) {
		int64_t new = atomic64_add_return(num, &p->counter);
		int64_t limit = atomic64_read(&p->limit);

		/*
		 * Since new is capped to the maximum number of pid_t, if
		 * p->limit is %PIDS_MAX then we know that this test will never
		 * fail.
		 */
		if (new > limit) {
			ret = -EAGAIN;
			if (fail) {
				*fail = p;
				goto revert;
			}
		}
		/*
		 * Not technically accurate if we go over limit somewhere up
		 * the hierarchy, but that's tolerable for the watermark.
		 */
		pids_update_watermark(p, new);
	}

	return ret;

revert:
	for (q = pids; q != p; q = parent_pids(q))
		pids_cancel(q, num);
	pids_cancel(p, num);

	return ret;
}

/**
 * pids_tranfer_charge - charge/uncharge in subtree betwee src and dst
 * @src: pid cgroup state to uncharge
 * @dst: pid cgroup state to charge
 * @num: the number of pids to transfer
 *
 * The function updates charged pids in subtree whose root is the closest
 * common ancestor of @src and @dst. This root and its ancestors are not
 * modified (their limits are not enacted).
 *
 * Returns 0 if no limit was hit, -EAGAIN if a limit on path [@dst, @comm) was
 * hit (charges are transferred despite the limit).
 */
static int pids_tranfer_charge(struct pids_cgroup *src, struct pids_cgroup *dst, int num)
{
	struct pids_cgroup *p, *comm = src;
	int ret;

	/* for stable cgroup tree */
	lockdep_assert_held(&cgroup_mutex);

	while (!cgroup_is_descendant(dst->css.cgroup, comm->css.cgroup))
		comm = parent_pids(comm);

	ret = pids_try_charge(dst, num, comm, NULL);

	for (p = src; p != comm; p = parent_pids(p))
		pids_cancel(p, num);
	return ret;
}

static int pids_can_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *dst_css;

	cgroup_taskset_for_each(task, dst_css, tset) {
		struct pids_cgroup *pids = css_pids(dst_css);
		struct cgroup_subsys_state *old_css;
		struct pids_cgroup *old_pids;

		/*
		 * No need to pin @old_css between here and cancel_attach()
		 * because cgroup core protects it from being freed before
		 * the migration completes or fails.
		 */
		old_css = task_css(task, pids_cgrp_id);
		old_pids = css_pids(old_css);

		(void) pids_tranfer_charge(old_pids, pids, 1);
	}

	return 0;
}

static void pids_cancel_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *dst_css;

	cgroup_taskset_for_each(task, dst_css, tset) {
		struct pids_cgroup *pids = css_pids(dst_css);
		struct cgroup_subsys_state *old_css;
		struct pids_cgroup *old_pids;

		old_css = task_css(task, pids_cgrp_id);
		old_pids = css_pids(old_css);

		(void) pids_tranfer_charge(pids, old_pids, 1);
	}
}

static void pids_event(struct pids_cgroup *pids_forking,
		       struct pids_cgroup *pids_over_limit)
{
	struct pids_cgroup *p = pids_forking;
	bool limit = false;

	/* Only log the first time limit is hit. */
	if (atomic64_inc_return(&p->events_local[PIDCG_MAX_IMPOSED]) == 1) {
		pr_info("cgroup: fork rejected by pids controller in ");
		pr_cont_cgroup_path(p->css.cgroup);
		pr_cont("\n");
	}
	cgroup_file_notify(&p->events_local_file);
	/* Events are only notified in pids_forking on v1 */
	if (!cgroup_subsys_on_dfl(pids_cgrp_subsys))
		return;

	for (; parent_pids(p); p = parent_pids(p)) {
		atomic64_inc(&p->events[PIDCG_MAX_IMPOSED]);

		if (p == pids_over_limit) {
			limit = true;
			atomic64_inc(&p->events_local[PIDCG_MAX]);
			cgroup_file_notify(&p->events_local_file);
		}
		if (limit)
			atomic64_inc(&p->events[PIDCG_MAX]);

		cgroup_file_notify(&p->events_file);
	}
}

/*
 * task_css_check(true) in pids_can_fork() and pids_cancel_fork() relies
 * on cgroup_threadgroup_change_begin() held by the copy_process().
 */
static int pids_can_fork(struct task_struct *task, struct css_set *cset)
{
	struct cgroup_subsys_state *css;
	struct pids_cgroup *pids, *pids_over_limit;
	int err;

	if (cset)
		css = cset->subsys[pids_cgrp_id];
	else
		css = task_css_check(current, pids_cgrp_id, true);
	pids = css_pids(css);
	err = pids_try_charge(pids, 1, NULL, &pids_over_limit);
	if (err)
		pids_event(pids, pids_over_limit);

	return err;
}

static void pids_cancel_fork(struct task_struct *task, struct css_set *cset)
{
	struct cgroup_subsys_state *css;
	struct pids_cgroup *pids;

	if (cset)
		css = cset->subsys[pids_cgrp_id];
	else
		css = task_css_check(current, pids_cgrp_id, true);
	pids = css_pids(css);
	pids_uncharge(pids, 1);
}

static void pids_release(struct task_struct *task)
{
	struct pids_cgroup *pids = css_pids(task_css(task, pids_cgrp_id));

	pids_uncharge(pids, 1);
}

static ssize_t pids_max_write(struct kernfs_open_file *of, char *buf,
			      size_t nbytes, loff_t off)
{
	struct cgroup_subsys_state *css = of_css(of);
	struct pids_cgroup *pids = css_pids(css);
	int64_t limit;
	int err;

	buf = strstrip(buf);
	if (!strcmp(buf, PIDS_MAX_STR)) {
		limit = PIDS_MAX;
		goto set_limit;
	}

	err = kstrtoll(buf, 0, &limit);
	if (err)
		return err;

	if (limit < 0 || limit >= PIDS_MAX)
		return -EINVAL;

set_limit:
	/*
	 * Limit updates don't need to be mutex'd, since it isn't
	 * critical that any racing fork()s follow the new limit.
	 */
	atomic64_set(&pids->limit, limit);
	return nbytes;
}

static int pids_max_show(struct seq_file *sf, void *v)
{
	struct cgroup_subsys_state *css = seq_css(sf);
	struct pids_cgroup *pids = css_pids(css);
	int64_t limit = atomic64_read(&pids->limit);

	if (limit >= PIDS_MAX)
		seq_printf(sf, "%s\n", PIDS_MAX_STR);
	else
		seq_printf(sf, "%lld\n", limit);

	return 0;
}

static s64 pids_current_read(struct cgroup_subsys_state *css,
			     struct cftype *cft)
{
	struct pids_cgroup *pids = css_pids(css);

	return atomic64_read(&pids->counter);
}

static s64 pids_peak_read(struct cgroup_subsys_state *css,
			  struct cftype *cft)
{
	struct pids_cgroup *pids = css_pids(css);

	return READ_ONCE(pids->watermark);
}

static int __pids_events_show(struct seq_file *sf, bool local)
{
	struct pids_cgroup *pids = css_pids(seq_css(sf));
	atomic64_t *events = local ? pids->events_local : pids->events;

	seq_printf(sf, "max %lld\n", (s64)atomic64_read(&events[PIDCG_MAX]));
	seq_printf(sf, "max.imposed %lld\n", (s64)atomic64_read(&events[PIDCG_MAX_IMPOSED]));
	return 0;
}

static int pids_events_show(struct seq_file *sf, void *v)
{
	__pids_events_show(sf, false);
	return 0;
}

static int pids_events_local_show(struct seq_file *sf, void *v)
{
	__pids_events_show(sf, true);
	return 0;
}

static int pids_events_show_legacy(struct seq_file *sf, void *v)
{
	struct pids_cgroup *pids = css_pids(seq_css(sf));

	seq_printf(sf, "max%lld\n", (s64)atomic64_read(&pids->events_local[PIDCG_MAX_IMPOSED]));
	return 0;
}

static struct cftype pids_files[] = {
	{
		.name = "max",
		.write = pids_max_write,
		.seq_show = pids_max_show,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "current",
		.read_s64 = pids_current_read,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "peak",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = pids_peak_read,
	},
	{
		.name = "events",
		.seq_show = pids_events_show,
		.file_offset = offsetof(struct pids_cgroup, events_file),
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "events.local",
		.seq_show = pids_events_local_show,
		.file_offset = offsetof(struct pids_cgroup, events_file),
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{ }	/* terminate */
};

static struct cftype pids_files_legacy[] = {
	{
		.name = "max",
		.write = pids_max_write,
		.seq_show = pids_max_show,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "current",
		.read_s64 = pids_current_read,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "events",
		.seq_show = pids_events_show_legacy,
		.file_offset = offsetof(struct pids_cgroup, events_local_file),
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{ }	/* terminate */
};

struct cgroup_subsys pids_cgrp_subsys = {
	.css_alloc	= pids_css_alloc,
	.css_free	= pids_css_free,
	.can_attach 	= pids_can_attach,
	.cancel_attach 	= pids_cancel_attach,
	.can_fork	= pids_can_fork,
	.cancel_fork	= pids_cancel_fork,
	.release	= pids_release,
	.dfl_cftypes	= pids_files,
	.legacy_cftypes	= pids_files_legacy,
	.threaded	= true,
};
