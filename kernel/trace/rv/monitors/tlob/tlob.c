// SPDX-License-Identifier: GPL-2.0
/*
 * tlob: task latency over budget monitor
 *
 * Track the elapsed wall-clock time of a marked code path and detect when
 * a monitored task exceeds its per-task latency budget.  CLOCK_MONOTONIC
 * is used so both on-CPU and off-CPU time count toward the budget.
 *
 * On a budget violation, two tracepoints are emitted from the hrtimer
 * callback: error_env_tlob signals the violation, and detail_env_tlob
 * provides a per-state time breakdown (running_ns, waiting_ns, sleeping_ns)
 * that pinpoints whether the overrun occurred in running, waiting, or sleeping state.
 *
 * The monitor uses RV_MON_PER_OBJ: per-task state (struct tlob_task_state)
 * is stored as monitor_target in the framework's hash table.
 *
 * One HA clock invariant is enforced:
 *   clk_elapsed < BUDGET_NS()   (active in all states)
 *
 * task_start uses da_handle_start_event() to set the initial state, then
 * calls ha_reset_clk_ns() + ha_start_timer_ns() directly to initialise the
 * clock and arm the budget timer.  No synthetic event is needed.
 * The HA timer is cancelled synchronously by ha_cancel_timer_sync() in
 * tlob_stop_task().
 *
 * Copyright (C) 2026 Wen Yang <wen.yang@linux.dev>
 */
#include <linux/completion.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/refcount.h>
#include <linux/rv.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/tracefs.h>
#include <linux/uaccess.h>
#include <kunit/visibility.h>
#include <rv/instrumentation.h>
#include <rv/rv_uprobe.h>
#include <uapi/linux/rv.h>
#include "../../rv.h"

#define MODULE_NAME "tlob"

#include <trace/events/sched.h>
#include <rv_trace.h>

/*
 * Per-fd private data; one instance per open /dev/rv fd.
 * monitoring: set while TRACE_START is active; cleared at TRACE_STOP.
 * budget_exceeded: set by hrtimer callback; read at TRACE_STOP to report
 * -EOVERFLOW even when cleanup was claimed by a concurrent stop_all or
 * a task-exit handler.
 */
struct tlob_fpriv {
	struct task_struct	*task;
	bool			monitoring;
	bool			budget_exceeded;
};

/*
 * Per-task latency monitoring state.  One instance per monitoring window.
 * Stored as monitor_target in da_monitor_storage; freed via call_rcu.
 */
struct tlob_task_state {
	struct task_struct	*task;		/* via get_task_struct */
	u64			threshold_us;	/* budget in microseconds */

	/* 1 = cleanup claimed; ha_setup_invariants won't restart the timer. */
	atomic_t		stopping;

	/* Serialises the ns accumulators; held briefly (hardirq-safe). */
	raw_spinlock_t		entry_lock;
	u64			running_ns;	/* time in running state  */
	u64			waiting_ns;	/* time in waiting state  */
	u64			sleeping_ns;	/* time in sleeping state */
	ktime_t			last_ts;

	/* store-release in TRACE_START ioctl, load-acquire in reset_notify. */
	struct tlob_fpriv	*fpriv;

	struct rcu_head		rcu;		/* for call_rcu() teardown */
};

#define RV_MON_TYPE RV_MON_PER_OBJ
#define HA_TIMER_TYPE HA_TIMER_HRTIMER
/* Pool mode: da_handle_start_event uses da_fill_empty_storage, not kmalloc. */
#define DA_SKIP_AUTO_ALLOC

/* Type for da_monitor_storage.target; must be defined before the includes. */
typedef struct tlob_task_state *monitor_target;

/* Forward-declared so da_monitor_reset_hook works before ha_monitor.h. */
static inline void tlob_reset_notify(struct da_monitor *da_mon);
#define da_monitor_reset_hook tlob_reset_notify

/*
 * When the hrtimer fires (budget elapsed), the HA framework emits
 * error_env_tlob with this label instead of the generic "none".
 */
#define MONITOR_TIMER_EVENT_NAME "budget_exceeded"

#include "tlob.h"
#include <rv/ha_monitor.h>

/*
 * Called from da_monitor_reset() on both normal stop and hrtimer expiry.
 * On violation (stopping==0), emits detail_env_tlob.
 */
static inline void tlob_reset_notify(struct da_monitor *da_mon)
{
	struct ha_monitor *ha_mon = to_ha_monitor(da_mon);
	struct tlob_task_state *ws;

	ha_monitor_reset_env(da_mon);

	ws = ha_get_target(ha_mon);
	if (!ws)
		return;

	/*
	 * Emit per-state breakdown on budget violation only.
	 * stopping==0: timer callback owns this path (genuine overrun).
	 * stopping==1: normal stop claimed ownership first; skip.
	 */
	if (!atomic_read(&ws->stopping)) {
		unsigned int curr_state = READ_ONCE(da_mon->curr_state);
		u64 running_ns, waiting_ns, sleeping_ns, partial_ns;
		struct tlob_fpriv *fp;
		unsigned long flags;

		/*
		 * Snapshot accumulators; partial_ns covers curr_state time
		 * not yet folded in (transition-out pending).
		 */
		raw_spin_lock_irqsave(&ws->entry_lock, flags);
		partial_ns   = ktime_get_ns() - ktime_to_ns(ws->last_ts);
		running_ns   = ws->running_ns  +
			       (curr_state == running_tlob  ? partial_ns : 0);
		waiting_ns   = ws->waiting_ns  +
			       (curr_state == waiting_tlob  ? partial_ns : 0);
		sleeping_ns  = ws->sleeping_ns +
			       (curr_state == sleeping_tlob ? partial_ns : 0);
		raw_spin_unlock_irqrestore(&ws->entry_lock, flags);

		trace_detail_env_tlob(da_get_id(da_mon), ws->threshold_us,
				      running_ns, waiting_ns, sleeping_ns);

		/*
		 * Latch violation in the fd so TRACE_STOP can return -EOVERFLOW
		 * even if a concurrent stop_all or task-exit handler claims
		 * cleanup first.  Pairs with smp_store_release in TRACE_START.
		 */
		fp = smp_load_acquire(&ws->fpriv);
		if (fp)
			WRITE_ONCE(fp->budget_exceeded, true);
	}
}

#define BUDGET_US(ha_mon) (ha_get_target(ha_mon)->threshold_us)
#define BUDGET_NS(ha_mon) (BUDGET_US(ha_mon) * 1000ULL)

/* HA constraint functions (called by ha_monitor_handle_constraint) */

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_tlob env, u64 time_ns)
{
	if (env == clk_elapsed_tlob)
		return ha_get_clk_ns(ha_mon, env, time_ns);
	return ENV_INVALID_VALUE;
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_tlob env, u64 time_ns)
{
	if (env == clk_elapsed_tlob)
		ha_reset_clk_ns(ha_mon, env, time_ns);
}

/*
 * ha_verify_invariants - clk_elapsed < BUDGET_NS must hold in all states.
 */
static inline bool ha_verify_invariants(struct ha_monitor *ha_mon,
					enum states curr_state, enum events event,
					enum states next_state, u64 time_ns)
{
	if (curr_state == running_tlob)
		return ha_check_invariant_ns(ha_mon, clk_elapsed_tlob, time_ns);
	else if (curr_state == sleeping_tlob)
		return ha_check_invariant_ns(ha_mon, clk_elapsed_tlob, time_ns);
	else if (curr_state == waiting_tlob)
		return ha_check_invariant_ns(ha_mon, clk_elapsed_tlob, time_ns);
	return true;
}

/*
 * Convert invariant (deadline) to guard (reset anchor) on state transitions.
 * Skip if uninitialised (ENV_INVALID_VALUE): the race between
 * da_handle_start_event() and ha_reset_clk_ns() would give U64_MAX - BUDGET_NS.
 */
static inline void ha_convert_inv_guard(struct ha_monitor *ha_mon,
					enum states curr_state, enum events event,
					enum states next_state, u64 time_ns)
{
	if (curr_state == next_state)
		return;
	if (curr_state == running_tlob &&
	    !ha_monitor_env_invalid(ha_mon, clk_elapsed_tlob))
		ha_inv_to_guard(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon), time_ns);
	else if (curr_state == sleeping_tlob &&
		 !ha_monitor_env_invalid(ha_mon, clk_elapsed_tlob))
		ha_inv_to_guard(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon), time_ns);
	else if (curr_state == waiting_tlob &&
		 !ha_monitor_env_invalid(ha_mon, clk_elapsed_tlob))
		ha_inv_to_guard(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon), time_ns);
}

/* No per-event guard conditions for tlob; invariants suffice. */
static inline bool ha_verify_guards(struct ha_monitor *ha_mon,
				    enum states curr_state, enum events event,
				    enum states next_state, u64 time_ns)
{
	return true;
}

/*
 * Arm or cancel the HA budget timer on state transitions.
 * Guard on stopping: sched_switch events can arrive after ha_cancel_timer_sync,
 * restarting the timer and triggering an ODEBUG "activate active" splat.
 */
static inline void ha_setup_invariants(struct ha_monitor *ha_mon,
				       enum states curr_state, enum events event,
				       enum states next_state, u64 time_ns)
{
	if (next_state == curr_state)
		return;
	if (next_state == running_tlob) {
		if (!atomic_read_acquire(&ha_get_target(ha_mon)->stopping))
			ha_start_timer_ns(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon), time_ns);
	} else if (next_state == sleeping_tlob) {
		if (!atomic_read_acquire(&ha_get_target(ha_mon)->stopping))
			ha_start_timer_ns(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon), time_ns);
	} else if (next_state == waiting_tlob) {
		if (!atomic_read_acquire(&ha_get_target(ha_mon)->stopping))
			ha_start_timer_ns(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon), time_ns);
	} else if (curr_state == running_tlob)
		ha_cancel_timer(ha_mon);
	else if (curr_state == waiting_tlob)
		ha_cancel_timer(ha_mon);
	else if (curr_state == sleeping_tlob)
		ha_cancel_timer(ha_mon);
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	if (!ha_verify_invariants(ha_mon, curr_state, event, next_state, time_ns))
		return false;

	ha_convert_inv_guard(ha_mon, curr_state, event, next_state, time_ns);

	if (!ha_verify_guards(ha_mon, curr_state, event, next_state, time_ns))
		return false;

	ha_setup_invariants(ha_mon, curr_state, event, next_state, time_ns);

	return true;
}

static struct kmem_cache *tlob_state_cache;

static atomic_t tlob_num_monitored = ATOMIC_INIT(0);

/* Uprobe binding list; protected by tlob_uprobe_mutex. */
static LIST_HEAD(tlob_uprobe_list);
static DEFINE_MUTEX(tlob_uprobe_mutex);

/*
 * Serialises duplicate-check + da_create_or_get() to prevent two concurrent
 * callers for the same pid from both inserting into the hash table.
 */
static DEFINE_MUTEX(tlob_start_mutex);

/*
 * Counts open /dev/rv fds plus one synthetic ref held while enabled.
 * __tlob_destroy_monitor() drops the synthetic ref and waits for zero
 * before teardown, preventing kmem_cache_zalloc() on a destroyed cache.
 */
static refcount_t tlob_fd_refcount = REFCOUNT_INIT(0);
static DECLARE_COMPLETION(tlob_fd_released);

/* Per-uprobe-binding state: a start + stop probe pair for one binary region. */
struct tlob_uprobe_binding {
	struct list_head	list;
	u64			threshold_us;
	char			binpath[TLOB_MAX_PATH];
	loff_t			offset_start;
	loff_t			offset_stop;
	struct rv_uprobe	*start_probe;
	struct rv_uprobe	*stop_probe;
};

/* RCU callback: free the slab once no readers remain. */
static void tlob_free_rcu(struct rcu_head *head)
{
	struct tlob_task_state *ws =
		container_of(head, struct tlob_task_state, rcu);
	kmem_cache_free(tlob_state_cache, ws);
}

/*
 * handle_sched_switch - advance the DA on every context switch.
 *
 * Generates three DA events:
 *   prev, prev_state != 0  -> sleep_tlob    (running -> sleeping)
 *   prev, prev_state == 0  -> preempt_tlob  (running -> waiting)
 *   next                   -> switch_in_tlob (waiting -> running)
 */
static void handle_sched_switch(void *data, bool preempt_unused,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	struct tlob_task_state *ws;
	unsigned long flags;
	bool do_prev = false, do_next = false;
	bool prev_preempted;
	ktime_t now;

	rcu_read_lock();

	ws = da_get_target_by_id(prev->pid);
	if (ws) {
		raw_spin_lock_irqsave(&ws->entry_lock, flags);
		now = ktime_get();
		ws->running_ns += ktime_to_ns(ktime_sub(now, ws->last_ts));
		ws->last_ts = now;
		/* prev_state == 0: TASK_RUNNING (preempted); != 0: sleeping. */
		prev_preempted = (prev_state == 0);
		do_prev = true;
		raw_spin_unlock_irqrestore(&ws->entry_lock, flags);
	}

	ws = da_get_target_by_id(next->pid);
	if (ws) {
		raw_spin_lock_irqsave(&ws->entry_lock, flags);
		now = ktime_get();
		ws->waiting_ns += ktime_to_ns(ktime_sub(now, ws->last_ts));
		ws->last_ts = now;
		do_next = true;
		raw_spin_unlock_irqrestore(&ws->entry_lock, flags);
	}

	rcu_read_unlock();

	if (do_prev)
		da_handle_event(prev->pid, NULL,
				prev_preempted ? preempt_tlob : sleep_tlob);
	if (do_next)
		da_handle_event(next->pid, NULL, switch_in_tlob);
}

/*
 * handle_sched_wakeup - sleeping -> waiting transition.
 *
 * try_to_wake_up() skips TASK_RUNNING tasks, so this never fires for a
 * task already in running or waiting state.
 */
static void handle_sched_wakeup(void *data, struct task_struct *p)
{
	struct tlob_task_state *ws;
	unsigned long flags;
	bool found = false;

	rcu_read_lock();
	ws = da_get_target_by_id(p->pid);
	if (ws) {
		ktime_t now = ktime_get();

		raw_spin_lock_irqsave(&ws->entry_lock, flags);
		ws->sleeping_ns += ktime_to_ns(ktime_sub(now, ws->last_ts));
		ws->last_ts = now;
		raw_spin_unlock_irqrestore(&ws->entry_lock, flags);
		found = true;
	}
	rcu_read_unlock();

	if (found)
		da_handle_event(p->pid, NULL, wakeup_tlob);
}

/*
 * handle_sched_process_exit - clean up if a task exits without TRACE_STOP.
 *
 * Called in do_exit() context; the task still has a valid pid here.
 */
static void handle_sched_process_exit(void *data, struct task_struct *p,
				       bool group_dead)
{
	struct tlob_task_state *ws;
	bool found = false;

	rcu_read_lock();
	ws = da_get_target_by_id(p->pid);
	found = !!ws;
	rcu_read_unlock();

	if (found)
		tlob_stop_task(p);
}



/**
 * tlob_start_task - begin monitoring @task with budget @threshold_us us.
 * @task:         Task to monitor; may be current or another task.
 * @threshold_us: Latency budget in microseconds (wall-clock; running + waiting + sleeping). > 0.
 *
 * Returns 0, -ENODEV, -EALREADY, -ENOSPC, or -ENOMEM.
 */
int tlob_start_task(struct task_struct *task, u64 threshold_us)
{
	struct tlob_task_state *ws_existing;
	struct tlob_task_state *ws;
	struct da_monitor *da_mon;
	struct ha_monitor *ha_mon;
	u64 now_ns;
	int ret;

	if (!da_monitor_enabled())
		return -ENODEV;

	if (threshold_us == 0)
		return -ERANGE;

	/* Serialise duplicate-check + da_create_or_get for the same pid. */
	guard(mutex)(&tlob_start_mutex);

	rcu_read_lock();
	ws_existing = da_get_target_by_id(task->pid);
	if (ws_existing) {
		rcu_read_unlock();
		return -EALREADY;
	}
	rcu_read_unlock();

	ws = kmem_cache_zalloc(tlob_state_cache, GFP_KERNEL);
	if (!ws)
		return -ENOMEM;

	ws->task = task;
	get_task_struct(task);
	ws->threshold_us = threshold_us;
	ws->last_ts = ktime_get();
	raw_spin_lock_init(&ws->entry_lock);

	/* Claim a pool slot (no kmalloc; DA_SKIP_AUTO_ALLOC + prealloc). */
	ret = da_create_or_get(task->pid, ws);
	if (ret) {
		put_task_struct(task);
		kmem_cache_free(tlob_state_cache, ws);
		return ret;
	}

	atomic_inc(&tlob_num_monitored);

	/* Hold RCU across handle + timer setup to keep da_mon valid. */
	rcu_read_lock();
	da_handle_start_event(task->pid, ws, switch_in_tlob);
	da_mon = da_get_monitor(task->pid, NULL);
	if (unlikely(!da_mon)) {
		/* Slot registered; missing da_mon means concurrent destroy. */
		rcu_read_unlock();
		da_destroy_storage(task->pid);
		atomic_dec(&tlob_num_monitored);
		put_task_struct(task);
		kmem_cache_free(tlob_state_cache, ws);
		return -ENOMEM;
	}
	ha_mon = to_ha_monitor(da_mon);
	now_ns = ktime_get_ns();
	ha_reset_env(ha_mon, clk_elapsed_tlob, now_ns);
	ha_start_timer_ns(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon), now_ns);
	rcu_read_unlock();

	return 0;
}
EXPORT_SYMBOL_GPL(tlob_start_task);

/**
 * tlob_stop_task - stop monitoring @task.
 * @task: Task to stop.
 *
 * CAS on ws->stopping (0->1) under RCU claims cleanup ownership;
 * the winner cancels the timer synchronously and frees all resources.
 *
 * Returns 0, -EOVERFLOW (budget exceeded), -ESRCH (not monitored),
 * or -EAGAIN (concurrent caller claimed cleanup).
 */
int tlob_stop_task(struct task_struct *task)
{
	struct da_monitor *da_mon;
	struct ha_monitor *ha_mon;
	struct tlob_task_state *ws;
	bool budget_exceeded;

	rcu_read_lock();
	ws = da_get_target_by_id(task->pid);
	if (!ws) {
		rcu_read_unlock();
		return -ESRCH;
	}

	da_mon = da_get_monitor(task->pid, NULL);
	if (unlikely(!da_mon)) {
		/* ws in hash but da_mon gone; internal inconsistency. */
		rcu_read_unlock();
		WARN_ON_ONCE(1);
		return -ESRCH;
	}

	ha_mon = to_ha_monitor(da_mon);

	/*
	 * CAS (0->1) claims cleanup ownership under RCU (ws guaranteed valid).
	 * _release pairs with atomic_read_acquire in ha_setup_invariants.
	 */
	if (atomic_cmpxchg_release(&ws->stopping, 0, 1) != 0) {
		rcu_read_unlock();
		return -EAGAIN;
	}

	rcu_read_unlock();

	/* Wait for in-flight timer callback before reading da_monitoring. */
	ha_cancel_timer_sync(ha_mon);

	/* Timer fired first -> budget exceeded; otherwise reset normally. */
	rcu_read_lock();
	budget_exceeded = !da_monitoring(da_mon);
	if (!budget_exceeded)
		da_monitor_reset(da_mon);
	rcu_read_unlock();
	da_destroy_storage(task->pid);
	atomic_dec(&tlob_num_monitored);

	put_task_struct(ws->task);
	call_rcu(&ws->rcu, tlob_free_rcu);
	return budget_exceeded ? -EOVERFLOW : 0;
}
EXPORT_SYMBOL_GPL(tlob_stop_task);

static void tlob_stop_all(void)
{
	struct da_monitor_storage *ms;
	pid_t pids[TLOB_MAX_MONITORED];
	int bkt, n = 0;

	/* Snapshot pids under RCU; re-derive ws under a fresh lock below. */
	rcu_read_lock();
	hash_for_each_rcu(da_monitor_ht, bkt, ms, node) {
		if (ms->target && n < TLOB_MAX_MONITORED)
			pids[n++] = ms->id;
	}
	rcu_read_unlock();

	for (int i = 0; i < n; i++) {
		pid_t pid = pids[i];
		struct da_monitor *da_mon;
		struct ha_monitor *ha_mon;
		struct tlob_task_state *ws;

		rcu_read_lock();
		da_mon = da_get_monitor(pid, NULL);
		if (!da_mon) {
			/* Cleaned up by tlob_stop_task or exit handler. */
			rcu_read_unlock();
			continue;
		}

		ws = da_get_target(da_mon);
		ha_mon = to_ha_monitor(da_mon);

		/* CAS (0->1) claims ownership; skip if another caller won. */
		if (atomic_cmpxchg_release(&ws->stopping, 0, 1) != 0) {
			rcu_read_unlock();
			continue;
		}
		rcu_read_unlock();

		ha_cancel_timer_sync(ha_mon);

		scoped_guard(rcu) {
			da_monitor_reset(da_mon);
		}
		da_destroy_storage(pid);
		atomic_dec(&tlob_num_monitored);
		put_task_struct(ws->task);
		call_rcu(&ws->rcu, tlob_free_rcu);
	}
}

static int tlob_uprobe_entry_handler(struct rv_uprobe *p, struct pt_regs *regs,
				     __u64 *data)
{
	struct tlob_uprobe_binding *b = p->priv;

	tlob_start_task(current, b->threshold_us);
	return 0;
}

static int tlob_uprobe_stop_handler(struct rv_uprobe *p, struct pt_regs *regs,
				    __u64 *data)
{
	tlob_stop_task(current);
	return 0;
}

/*
 * Register start + stop entry uprobes for a binding.
 * Called with tlob_uprobe_mutex held.
 */
static int tlob_add_uprobe(u64 threshold_us, const char *binpath,
			   loff_t offset_start, loff_t offset_stop)
{
	struct tlob_uprobe_binding *b, *tmp_b;
	char pathbuf[TLOB_MAX_PATH];
	struct path path;
	char *canon;
	int ret;

	if (binpath[0] != '/')
		return -EINVAL;

	b = kzalloc_obj(*b, GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->threshold_us = threshold_us;
	b->offset_start = offset_start;
	b->offset_stop  = offset_stop;

	ret = kern_path(binpath, LOOKUP_FOLLOW, &path);
	if (ret)
		goto err_free;

	if (!d_is_reg(path.dentry)) {
		ret = -EINVAL;
		goto err_path;
	}

	/* Reject duplicate start offset for the same binary. */
	list_for_each_entry(tmp_b, &tlob_uprobe_list, list) {
		if (tmp_b->offset_start == offset_start &&
		    tmp_b->start_probe->path.dentry == path.dentry) {
			ret = -EEXIST;
			goto err_path;
		}
	}

	canon = d_path(&path, pathbuf, sizeof(pathbuf));
	if (IS_ERR(canon)) {
		ret = PTR_ERR(canon);
		goto err_path;
	}
	strscpy(b->binpath, canon, sizeof(b->binpath));

	/* Both probes share b (priv) and path; attach_path refs path itself. */
	b->start_probe = rv_uprobe_attach_path(&path, offset_start,
					       tlob_uprobe_entry_handler, NULL, b);
	if (IS_ERR(b->start_probe)) {
		ret = PTR_ERR(b->start_probe);
		b->start_probe = NULL;
		goto err_path;
	}

	b->stop_probe = rv_uprobe_attach_path(&path, offset_stop,
					      tlob_uprobe_stop_handler, NULL, b);
	if (IS_ERR(b->stop_probe)) {
		ret = PTR_ERR(b->stop_probe);
		b->stop_probe = NULL;
		goto err_start;
	}

	path_put(&path);
	list_add_tail(&b->list, &tlob_uprobe_list);
	return 0;

err_start:
	rv_uprobe_detach(b->start_probe);
err_path:
	path_put(&path);
err_free:
	kfree(b);
	return ret;
}

static int tlob_remove_uprobe_by_key(loff_t offset_start, const char *binpath)
{
	struct tlob_uprobe_binding *b, *tmp;
	struct path remove_path;
	int ret;

	ret = kern_path(binpath, LOOKUP_FOLLOW, &remove_path);
	if (ret)
		return ret;

	ret = -ENOENT;
	list_for_each_entry_safe(b, tmp, &tlob_uprobe_list, list) {
		if (b->offset_start != offset_start)
			continue;
		if (b->start_probe->path.dentry != remove_path.dentry)
			continue;
		list_del(&b->list);
		rv_uprobe_detach(b->start_probe);
		rv_uprobe_detach(b->stop_probe);
		kfree(b);
		ret = 0;
		break;
	}

	path_put(&remove_path);
	return ret;
}

static void tlob_remove_all_uprobes(void)
{
	struct tlob_uprobe_binding *b, *tmp;
	LIST_HEAD(pending);

	mutex_lock(&tlob_uprobe_mutex);
	list_for_each_entry_safe(b, tmp, &tlob_uprobe_list, list) {
		list_move(&b->list, &pending);
		rv_uprobe_unregister_nosync(b->start_probe);
		rv_uprobe_unregister_nosync(b->stop_probe);
	}
	mutex_unlock(&tlob_uprobe_mutex);

	if (list_empty(&pending))
		return;

	/*
	 * One global barrier for all probes dequeued above; no new handlers
	 * for any of them can fire after this returns.
	 */
	rv_uprobe_sync();

	list_for_each_entry_safe(b, tmp, &pending, list) {
		rv_uprobe_free(b->start_probe);
		rv_uprobe_free(b->stop_probe);
		kfree(b);
	}
}

static ssize_t tlob_monitor_read(struct file *file,
				 char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	const int line_sz = TLOB_MAX_PATH + 128;
	struct tlob_uprobe_binding *b;
	char *buf, *p;
	int n = 0, buf_sz, pos = 0;
	ssize_t ret;

	mutex_lock(&tlob_uprobe_mutex);
	list_for_each_entry(b, &tlob_uprobe_list, list)
		n++;

	buf_sz = (n ? n : 1) * line_sz + 1;
	buf = kmalloc(buf_sz, GFP_KERNEL);
	if (!buf) {
		mutex_unlock(&tlob_uprobe_mutex);
		return -ENOMEM;
	}

	list_for_each_entry(b, &tlob_uprobe_list, list) {
		p = b->binpath;
		pos += scnprintf(buf + pos, buf_sz - pos,
				 "p %s:0x%llx 0x%llx threshold=%llu\n",
				 p,
				 (unsigned long long)b->offset_start,
				 (unsigned long long)b->offset_stop,
				 b->threshold_us);
	}
	mutex_unlock(&tlob_uprobe_mutex);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, pos);
	kfree(buf);
	return ret;
}

/*
 * Parse "p PATH:OFFSET_START OFFSET_STOP threshold=US".
 * PATH may contain ':'; the last ':' separates path from offset.
 * Returns 0 or -EINVAL.
 */
static int tlob_parse_uprobe_line(char *buf, u64 *thr_out,
				  char **path_out,
				  loff_t *start_out, loff_t *stop_out)
{
	unsigned long long thr = 0, stop_val = 0;
	long long start_val;
	char *p, *path_token, *token, *colon;
	bool got_stop = false, got_thr = false;
	int n;

	/* Must start with "p " */
	if (buf[0] != 'p' || buf[1] != ' ')
		return -EINVAL;

	p = buf + 2;
	while (*p == ' ')
		p++;

	/* First space-delimited token is PATH:OFFSET_START */
	path_token = strsep(&p, " \t");
	if (!path_token || !*path_token)
		return -EINVAL;

	/* Split at last ':' to handle paths that contain ':'. */
	colon = strrchr(path_token, ':');
	if (!colon || colon - path_token < 2)
		return -EINVAL;
	*colon = '\0';

	if (path_token[0] != '/')
		return -EINVAL;

	n = 0;
	if (sscanf(colon + 1, "%lli%n", &start_val, &n) != 1 || n == 0)
		return -EINVAL;
	if (start_val < 0)
		return -EINVAL;

	/* Remaining tokens: OFFSET_STOP threshold=US */
	while (p && (token = strsep(&p, " \t")) != NULL) {
		if (!*token)
			continue;
		if (strncmp(token, "threshold=", 10) == 0) {
			if (kstrtoull(token + 10, 0, &thr))
				return -EINVAL;
			got_thr = true;
		} else if (!got_stop) {
			long long sv;

			n = 0;
			if (sscanf(token, "%lli%n", &sv, &n) != 1 || n == 0)
				return -EINVAL;
			if (sv < 0)
				return -EINVAL;
			stop_val = (unsigned long long)sv;
			got_stop = true;
		} else {
			return -EINVAL;
		}
	}

	if (!got_stop || !got_thr || thr == 0)
		return -EINVAL;
	if (start_val == (long long)stop_val)
		return -EINVAL;

	*thr_out   = thr;
	*path_out  = path_token;
	*start_out = (loff_t)start_val;
	*stop_out  = (loff_t)stop_val;
	return 0;
}

/* Parse "-PATH:OFFSET_START" (ftrace uprobe_events removal convention). */
static int tlob_parse_remove_line(char *buf, char **path_out, loff_t *start_out)
{
	char *binpath, *colon;
	long long off;
	int n = 0;

	if (buf[0] != '-')
		return -EINVAL;
	binpath = buf + 1;
	if (binpath[0] != '/')
		return -EINVAL;
	colon = strrchr(binpath, ':');
	if (!colon || colon - binpath < 2)
		return -EINVAL;
	*colon = '\0';
	if (sscanf(colon + 1, "%lli%n", &off, &n) != 1 || n == 0)
		return -EINVAL;
	*path_out  = binpath;
	*start_out = (loff_t)off;
	return 0;
}

VISIBLE_IF_KUNIT int tlob_create_or_delete_uprobe(char *buf)
{
	loff_t offset_start, offset_stop;
	u64 threshold_us;
	char *binpath;
	int ret;

	if (buf[0] == '-') {
		ret = tlob_parse_remove_line(buf, &binpath, &offset_start);
		if (ret)
			return ret;
		mutex_lock(&tlob_uprobe_mutex);
		ret = tlob_remove_uprobe_by_key(offset_start, binpath);
		mutex_unlock(&tlob_uprobe_mutex);
		return ret;
	}
	ret = tlob_parse_uprobe_line(buf, &threshold_us, &binpath,
				     &offset_start, &offset_stop);
	if (ret)
		return ret;
	mutex_lock(&tlob_uprobe_mutex);
	ret = tlob_add_uprobe(threshold_us, binpath, offset_start, offset_stop);
	mutex_unlock(&tlob_uprobe_mutex);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_create_or_delete_uprobe);

static ssize_t tlob_monitor_write(struct file *file,
				  const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	char buf[TLOB_MAX_PATH + 128];

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (count > 0 && buf[count - 1] == '\n')
		buf[count - 1] = '\0';
	return tlob_create_or_delete_uprobe(buf) ?: (ssize_t)count;
}

static const struct file_operations tlob_monitor_fops = {
	.open	= simple_open,
	.read	= tlob_monitor_read,
	.write	= tlob_monitor_write,
	.llseek	= noop_llseek,
};

static int __tlob_init_monitor(void)
{
	int retval;

	tlob_state_cache = kmem_cache_create("tlob_task_state",
					     sizeof(struct tlob_task_state),
					     0, 0, NULL);
	if (!tlob_state_cache)
		return -ENOMEM;

	atomic_set(&tlob_num_monitored, 0);

	retval = da_monitor_init_prealloc(TLOB_MAX_MONITORED);
	if (retval) {
		kmem_cache_destroy(tlob_state_cache);
		tlob_state_cache = NULL;
		return retval;
	}

	/* Synthetic reference: held while the monitor is enabled. */
	reinit_completion(&tlob_fd_released);
	refcount_set(&tlob_fd_refcount, 1);

	rv_this.enabled = 1;
	return 0;
}

static void __tlob_destroy_monitor(void)
{
	rv_this.enabled = 0;
	/*
	 * Remove uprobes first so stop_task can't race with tlob_stop_all().
	 * rv_uprobe_sync() inside ensures all in-flight handlers have finished.
	 */
	tlob_remove_all_uprobes();
	tlob_stop_all();
	/* Wait for tlob_free_rcu and da_pool_return_cb before pool teardown. */
	synchronize_rcu();

	/*
	 * Drop the synthetic ref and wait for all open fds to close before
	 * teardown; prevents kmem_cache_zalloc() on the destroyed cache.
	 */
	if (!refcount_dec_and_test(&tlob_fd_refcount))
		wait_for_completion(&tlob_fd_released);

	da_monitor_destroy();
	kmem_cache_destroy(tlob_state_cache);
	tlob_state_cache = NULL;
}

/* KUnit wrappers that acquire rv_interface_lock around monitor init/destroy. */
#if IS_ENABLED(CONFIG_KUNIT)
int tlob_init_monitor(void)
{
	int ret;

	mutex_lock(&rv_interface_lock);
	ret = __tlob_init_monitor();
	mutex_unlock(&rv_interface_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(tlob_init_monitor);

void tlob_destroy_monitor(void)
{
	mutex_lock(&rv_interface_lock);
	__tlob_destroy_monitor();
	mutex_unlock(&rv_interface_lock);
}
EXPORT_SYMBOL_GPL(tlob_destroy_monitor);

int tlob_num_monitored_read(void)
{
	return atomic_read(&tlob_num_monitored);
}
EXPORT_SYMBOL_IF_KUNIT(tlob_num_monitored_read);

/* Tracepoint probes for KUnit; rv_trace.h is only included here. */
static struct tlob_captured_event     tlob_kunit_last_event;
static struct tlob_captured_error_env tlob_kunit_last_error_env;
static struct tlob_captured_detail    tlob_kunit_last_detail;
static atomic_t tlob_kunit_event_cnt    = ATOMIC_INIT(0);
static atomic_t tlob_kunit_error_env_cnt = ATOMIC_INIT(0);

static void tlob_kunit_event_probe(void *data, int id, char *state, char *event,
				   char *next_state, bool final_state)
{
	tlob_kunit_last_event.id = id;
	strscpy(tlob_kunit_last_event.state, state,
		sizeof(tlob_kunit_last_event.state));
	strscpy(tlob_kunit_last_event.event, event,
		sizeof(tlob_kunit_last_event.event));
	strscpy(tlob_kunit_last_event.next_state, next_state,
		sizeof(tlob_kunit_last_event.next_state));
	tlob_kunit_last_event.final_state = final_state;
	atomic_inc(&tlob_kunit_event_cnt);
}

static void tlob_kunit_error_env_probe(void *data, int id, char *state,
				       char *event, char *env)
{
	tlob_kunit_last_error_env.id = id;
	strscpy(tlob_kunit_last_error_env.state, state,
		sizeof(tlob_kunit_last_error_env.state));
	strscpy(tlob_kunit_last_error_env.event, event,
		sizeof(tlob_kunit_last_error_env.event));
	strscpy(tlob_kunit_last_error_env.env, env,
		sizeof(tlob_kunit_last_error_env.env));
	atomic_inc(&tlob_kunit_error_env_cnt);
}

static void tlob_kunit_detail_probe(void *data, int pid, u64 threshold_us,
				    u64 running_ns, u64 waiting_ns,
				    u64 sleeping_ns)
{
	tlob_kunit_last_detail.pid		= pid;
	tlob_kunit_last_detail.threshold_us	= threshold_us;
	tlob_kunit_last_detail.running_ns	= running_ns;
	tlob_kunit_last_detail.waiting_ns	= waiting_ns;
	tlob_kunit_last_detail.sleeping_ns	= sleeping_ns;
}

int tlob_register_kunit_probes(void)
{
	int ret;

	atomic_set(&tlob_kunit_event_cnt, 0);
	atomic_set(&tlob_kunit_error_env_cnt, 0);

	ret = register_trace_event_tlob(tlob_kunit_event_probe, NULL);
	if (ret)
		return ret;
	ret = register_trace_error_env_tlob(tlob_kunit_error_env_probe, NULL);
	if (ret) {
		unregister_trace_event_tlob(tlob_kunit_event_probe, NULL);
		return ret;
	}
	ret = register_trace_detail_env_tlob(tlob_kunit_detail_probe, NULL);
	if (ret) {
		unregister_trace_error_env_tlob(tlob_kunit_error_env_probe, NULL);
		unregister_trace_event_tlob(tlob_kunit_event_probe, NULL);
		return ret;
	}
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_register_kunit_probes);

void tlob_unregister_kunit_probes(void)
{
	unregister_trace_event_tlob(tlob_kunit_event_probe, NULL);
	unregister_trace_error_env_tlob(tlob_kunit_error_env_probe, NULL);
	unregister_trace_detail_env_tlob(tlob_kunit_detail_probe, NULL);
	tracepoint_synchronize_unregister();
}
EXPORT_SYMBOL_IF_KUNIT(tlob_unregister_kunit_probes);

int tlob_event_count_read(void)
{
	return atomic_read(&tlob_kunit_event_cnt);
}
EXPORT_SYMBOL_IF_KUNIT(tlob_event_count_read);

void tlob_event_count_reset(void)
{
	atomic_set(&tlob_kunit_event_cnt, 0);
}
EXPORT_SYMBOL_IF_KUNIT(tlob_event_count_reset);

int tlob_error_env_count_read(void)
{
	return atomic_read(&tlob_kunit_error_env_cnt);
}
EXPORT_SYMBOL_IF_KUNIT(tlob_error_env_count_read);

void tlob_error_env_count_reset(void)
{
	atomic_set(&tlob_kunit_error_env_cnt, 0);
}
EXPORT_SYMBOL_IF_KUNIT(tlob_error_env_count_reset);


const struct tlob_captured_event *tlob_last_event_read(void)
{
	return &tlob_kunit_last_event;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_last_event_read);

const struct tlob_captured_error_env *tlob_last_error_env_read(void)
{
	return &tlob_kunit_last_error_env;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_last_error_env_read);

const struct tlob_captured_detail *tlob_last_detail_read(void)
{
	return &tlob_kunit_last_detail;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_last_detail_read);

#endif /* CONFIG_KUNIT */

VISIBLE_IF_KUNIT int tlob_enable_hooks(void)
{
	rv_attach_trace_probe("tlob", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("tlob", sched_wakeup, handle_sched_wakeup);
	rv_attach_trace_probe("tlob", sched_process_exit, handle_sched_process_exit);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_enable_hooks);

VISIBLE_IF_KUNIT void tlob_disable_hooks(void)
{
	rv_detach_trace_probe("tlob", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("tlob", sched_wakeup, handle_sched_wakeup);
	rv_detach_trace_probe("tlob", sched_process_exit, handle_sched_process_exit);
}
EXPORT_SYMBOL_IF_KUNIT(tlob_disable_hooks);

static int enable_tlob(void)
{
	int retval;

	retval = __tlob_init_monitor();
	if (retval)
		return retval;

	return tlob_enable_hooks();
}

static void disable_tlob(void)
{
	tlob_disable_hooks();
	__tlob_destroy_monitor();
}

static struct rv_monitor rv_this = {
	.name		= "tlob",
	.description	= "Per-task latency-over-budget monitor.",
	.enable		= enable_tlob,
	.disable	= disable_tlob,
	.reset		= da_monitor_reset_all,
	.enabled	= 0,
};

static void *tlob_chardev_bind(void)
{
	struct tlob_fpriv *fp;

	fp = kzalloc_obj(*fp, GFP_KERNEL);
	if (!fp)
		return ERR_PTR(-ENOMEM);

	/* Pin cache/pool for fd lifetime; balanced in tlob_chardev_release.
	 * If the synthetic ref has already been dropped (__tlob_destroy_monitor
	 * ran to completion), reject the bind so the caller gets ENODEV instead
	 * of corrupting a zero refcount.
	 */
	if (!refcount_inc_not_zero(&tlob_fd_refcount)) {
		kfree(fp);
		return ERR_PTR(-ENODEV);
	}
	return fp;
}

static void tlob_chardev_release(void *priv)
{
	struct tlob_fpriv *fp = priv;

	if (fp->monitoring) {
		/* All return values are safe on close. */
		(void)tlob_stop_task(fp->task);
		put_task_struct(fp->task);
	}

	kfree(fp);

	/* Release fd's pin; if last, wake __tlob_destroy_monitor. */
	if (refcount_dec_and_test(&tlob_fd_refcount))
		complete(&tlob_fd_released);
}

static long tlob_chardev_ioctl(void *priv, unsigned int cmd, unsigned long arg)
{
	struct tlob_fpriv *fp = priv;
	struct tlob_start_args args;
	struct task_struct *task;
	int ret;

	switch (cmd) {
	case TLOB_IOCTL_TRACE_START:
		if (fp->monitoring)
			return -EALREADY;

		if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
			return -EFAULT;

		ret = tlob_start_task(current, args.threshold_us);
		if (ret)
			return ret;

		fp->task = current;
		get_task_struct(current);
		fp->budget_exceeded = false;

		/* Link fd so hrtimer callback can latch budget_exceeded. */
		scoped_guard(rcu) {
			struct tlob_task_state *ws = da_get_target_by_id(current->pid);

			if (ws)
				smp_store_release(&ws->fpriv, fp);
		}

		fp->monitoring = true;
		return 0;

	case TLOB_IOCTL_TRACE_STOP:
		if (!fp->monitoring)
			return -EINVAL;

		task = fp->task;
		fp->monitoring = false;
		fp->task = NULL;

		ret = tlob_stop_task(task);
		put_task_struct(task);

		/*
		 * -EOVERFLOW: budget exceeded; propagate to caller.
		 * -EAGAIN: concurrent stop_all claimed cleanup; fall through to
		 *   budget_exceeded latch set by the hrtimer callback.
		 * -ESRCH: task exited before TRACE_STOP (process-exit handler
		 *   claimed cleanup); same latch applies.  Not an internal error.
		 */
		if (ret == -EAGAIN || ret == -ESRCH)
			return READ_ONCE(fp->budget_exceeded) ? -EOVERFLOW : 0;
		return ret;

	default:
		return -ENOTTY;
	}
}

static const struct rv_chardev_ops tlob_chardev_ops = {
	.owner   = THIS_MODULE,
	.bind    = tlob_chardev_bind,
	.ioctl   = tlob_chardev_ioctl,
	.release = tlob_chardev_release,
};

static int __init register_tlob(void)
{
	int ret;

	ret = rv_chardev_register_monitor("tlob", &tlob_chardev_ops);
	if (ret)
		return ret;

	ret = rv_register_monitor(&rv_this, NULL);
	if (ret) {
		rv_chardev_unregister_monitor("tlob");
		return ret;
	}

	if (rv_this.root_d) {
		if (!tracefs_create_file("monitor", 0644, rv_this.root_d, NULL,
					 &tlob_monitor_fops)) {
			rv_unregister_monitor(&rv_this);
			rv_chardev_unregister_monitor("tlob");
			return -ENOMEM;
		}
	}

	return 0;
}

static void __exit unregister_tlob(void)
{
	rv_chardev_unregister_monitor("tlob");
	rv_unregister_monitor(&rv_this);
}

module_init(register_tlob);
module_exit(unregister_tlob);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Wen Yang <wen.yang@linux.dev>");
MODULE_DESCRIPTION("tlob: task latency over budget per-task monitor.");
