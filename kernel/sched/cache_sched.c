// SPDX-License-Identifier: GPL-2.0-only
#include "sched.h"

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
