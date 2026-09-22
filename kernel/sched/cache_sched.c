// SPDX-License-Identifier: GPL-2.0-only
#include "sched.h"

#define rcu_deref_sched_cache_grp(tsk) \
	rcu_dereference_check((tsk)->sched_cache_grp, (tsk) == current)

static struct sched_cache_group *sched_cache_replace_grp(struct task_struct *p,
							 struct sched_cache_group *new)
{
	struct sched_cache_group *old;

	old = rcu_deref_sched_cache_grp(p);
	rcu_assign_pointer(p->sched_cache_grp, new);

	return old;
}

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

void sched_cache_fork(struct task_struct *p)
{
	/*
	 * The child takes its own reference on the mm's cache group, separate
	 * from the reference held by the mm. @p is not yet visible to readers,
	 * so a plain initializing store is enough.
	 */
	RCU_INIT_POINTER(p->sched_cache_grp,
			 sched_cache_group_get(p->mm->sched_cache_grp));
}

void sched_cache_fork_cleanup(struct task_struct *p)
{
	/*
	 * A fork that fails after sched_cache_fork() never reaches exit_mm(),
	 * so drop the reference here. @p never became visible, so there are no
	 * concurrent readers and the reference we hold keeps the group alive.
	 */
	sched_cache_group_put(rcu_access_pointer(p->sched_cache_grp));
	RCU_INIT_POINTER(p->sched_cache_grp, NULL);
}

void sched_cache_exec_mmap(struct task_struct *p, struct mm_struct *mm)
{
	struct sched_cache_group *old;

	/*
	 * Acquire the new reference before publishing the pointer, then drop
	 * the old one. @p is current and the only writer of its own pointer.
	 */
	old = sched_cache_replace_grp(p, sched_cache_group_get(mm->sched_cache_grp));
	sched_cache_group_put(old);
}

void sched_cache_exit_mm(struct task_struct *p)
{
	struct sched_cache_group *grp = sched_cache_replace_grp(p, NULL);

#ifdef CONFIG_NUMA_BALANCING
	/*
	 * Subtract this task's footprint from the group before dropping the
	 * reference, so the group footprint converges as its threads exit.
	 * Unlocked for performance; clamp to avoid underflow.
	 */
	if (grp && p->total_numa_faults) {
		unsigned long fp = READ_ONCE(grp->footprint);
		unsigned long sub = min(fp, p->total_numa_faults);

		WRITE_ONCE(grp->footprint, fp - sub);
	}
#endif
	sched_cache_group_put(grp);
}

static void sched_cache_group_free_rcu(struct rcu_head *rcu)
{
	struct sched_cache_group *grp =
		container_of(rcu, struct sched_cache_group, rcu);

	free_percpu(grp->pcpu_sched);
	kfree(grp);
}

void sched_cache_group_put(struct sched_cache_group *grp)
{
	if (!grp || !refcount_dec_and_test(&grp->refcnt))
		return;

	call_rcu(&grp->rcu, sched_cache_group_free_rcu);
}
