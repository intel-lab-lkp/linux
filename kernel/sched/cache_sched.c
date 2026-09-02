// SPDX-License-Identifier: GPL-2.0-only
#include "sched.h"

struct sched_cache_group *sched_cache_group_get(struct sched_cache_group *grp)
{
	/*
	 * refcount_inc_not_zero() is the acquire primitive for lockless
	 * (RCU) lookups; plain refcount_inc() would scribble the count if
	 * it already reached zero. Return NULL in that case.
	 */
	if (grp && !refcount_inc_not_zero(&grp->refcnt))
		grp = NULL;

	return grp;
}

struct sched_cache_group *task_cache_group_get(struct task_struct *p)
{
	guard(rcu)();
	return sched_cache_group_get(rcu_dereference(p->sched_cache_grp));
}

static void sched_cache_group_free_rcu(struct rcu_head *rcu)
{
	struct sched_cache_group *grp =
		container_of(rcu, struct sched_cache_group, rcu);

	/* free_percpu() may be called from atomic context. */
	free_percpu(grp->pcpu_sched);
	kfree(grp);
}

void sched_cache_group_put(struct sched_cache_group *grp)
{
	if (!grp || !refcount_dec_and_test(&grp->refcnt))
		return;

	call_rcu(&grp->rcu, sched_cache_group_free_rcu);
}
