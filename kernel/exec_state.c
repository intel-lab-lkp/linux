// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/coredump.h>
#include <linux/sched/exec_state.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>

static struct kmem_cache *task_exec_state_cachep;

static void __free_task_exec_state(struct rcu_head *rcu)
{
	struct task_exec_state *es = container_of(rcu, struct task_exec_state, rcu);

	put_user_ns(es->user_ns);
	kmem_cache_free(task_exec_state_cachep, es);
}

void put_task_exec_state(struct task_exec_state *es)
{
	if (es && refcount_dec_and_test(&es->count))
		call_rcu(&es->rcu, __free_task_exec_state);
}

struct task_exec_state *alloc_task_exec_state(struct user_namespace *user_ns)
{
	struct task_exec_state *es;

	es = kmem_cache_alloc(task_exec_state_cachep, GFP_KERNEL);
	if (!es)
		return NULL;
	refcount_set(&es->count, 1);
	es->dumpable = TASK_DUMPABLE_OFF;
	es->user_ns = get_user_ns(user_ns);
	return es;
}

struct task_exec_state *task_exec_state_rcu(const struct task_struct *tsk)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_held() && !lockdep_is_held(&tsk->alloc_lock),
			 "task_exec_state_rcu() requires RCU or task_lock");
	WARN_ON_ONCE(!tsk->exec_state);
	return rcu_dereference(tsk->exec_state);
}

struct task_exec_state *task_exec_state_replace(struct task_struct *tsk,
						struct task_exec_state *exec_state)
{
	/*
	 * Updates must hold both locks so callers needing a consistent
	 * snapshot of mm + dumpability are covered.
	 */
	lockdep_assert_held(&tsk->alloc_lock);
	lockdep_assert_held_write(&tsk->signal->exec_update_lock);

	return rcu_replace_pointer(tsk->exec_state, exec_state, true);
}

/*
 * exec_state is anchored to the execve() that established the current
 * privilege domain.  All clone() variants refcount-share it; only a
 * subsequent execve() in the child swaps in a fresh one.
 */
void copy_exec_state(struct task_struct *tsk)
{
	struct task_exec_state *es = current->exec_state;

	refcount_inc(&es->count);
	rcu_assign_pointer(tsk->exec_state, es);
}

/*
 * Store TASK_DUMPABLE_* on current->exec_state.  All callers
 * (commit_creds, begin_new_exec, prctl(PR_SET_DUMPABLE)) act on the
 * running task, which guarantees ->exec_state is allocated and cannot
 * be replaced under us.
 */
void task_exec_state_set_dumpable(enum task_dumpable value)
{
	struct task_exec_state *es;

	if (WARN_ON(value > TASK_DUMPABLE_ROOT))
		value = TASK_DUMPABLE_OFF;

	es = rcu_dereference_protected(current->exec_state, true);
	WRITE_ONCE(es->dumpable, value);
}

enum task_dumpable task_exec_state_get_dumpable(struct task_struct *task)
{
	struct task_exec_state *es;

	guard(rcu)();
	es = rcu_dereference(task->exec_state);
	return READ_ONCE(es->dumpable);
}

void __init exec_state_init(void)
{
	task_exec_state_cachep = kmem_cache_create("task_exec_state",
			sizeof(struct task_exec_state), 0,
			SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT,
			NULL);
}
