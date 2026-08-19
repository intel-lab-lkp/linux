// SPDX-License-Identifier: GPL-2.0
/*
 * tlob: task latency over budget monitor
 *
 * Tracks the elapsed wall-clock time (CLOCK_MONOTONIC) of a marked code
 * path and flags per-task latency-budget overruns.  The hrtimer callback
 * emits error_env_tlob on violation plus detail_env_tlob, a per-state
 * (running/waiting/sleeping) time breakdown.
 *
 * RV_MON_PER_OBJ: per-task state (struct tlob_task_state) lives as
 * monitor_target in the framework's hash table.  One HA clock invariant:
 * clk_elapsed < BUDGET_NS() in running/waiting/sleeping (stopped parks).
 *
 * Copyright (C) 2026 Wen Yang <wen.yang@linux.dev>
 */
#include <linux/kernel.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/rv.h>
#include <linux/slab.h>
#include <kunit/visibility.h>
#include <rv/instrumentation.h>
#include <rv/rv_uprobe.h>
#include <rv.h>

#define MODULE_NAME "tlob"

#include <trace/events/sched.h>
#include <rv_trace.h>

/*
 * Per-task latency monitoring state.  One instance per monitoring window.
 * Stored as monitor_target in da_monitor_storage; freed via call_rcu.
 */
enum tlob_acc_idx {
	TLOB_ACC_RUNNING,
	TLOB_ACC_WAITING,
	TLOB_ACC_SLEEPING,
	TLOB_ACC_MAX,
};

struct tlob_task_state {
	struct task_struct	*task;		/* via get_task_struct */
	u64			threshold_ns;	/* budget in nanoseconds */

	/*
	 * Per-window: 1 = this window ended (stop or timer expiry).  Blocks
	 * timer re-arm in ha_setup_invariants(); cleared on restart.
	 */
	atomic_t		stopping;

	/*
	 * Per-task, one-shot: final teardown has claimed this slot; never
	 * reset (a window can end and restart, the task cannot).  atomic_t
	 * so cmpxchg is well-defined on every arch.
	 */
	atomic_t		destroying;

	bool			budget_exceeded;

	/*
	 * Opaque owner: the binding that started this task.  Set once on
	 * fresh allocation (NULL for callers with no binding), cleared by
	 * tlob_unbind_reap() for an active task whose binding is removed.
	 * Immutable elsewhere.  Protected by tlob_ws_lock.
	 */
	void			*binding;
	/*
	 * Linked into binding->started_list for the whole lifetime (not just
	 * while parked) so unbind reaping finds parked and active tasks.
	 * Protected by tlob_ws_lock.
	 */
	struct list_head	started_node;

	/* Serialises accs_ns[]; held briefly (hardirq-safe). */
	raw_spinlock_t		entry_lock;
	u64			accs_ns[TLOB_ACC_MAX]; /* per-state elapsed ns */
	ktime_t			last_ts;

	struct rcu_head		rcu;
};

#define RV_MON_TYPE RV_MON_PER_OBJ
#define HA_TIMER_TYPE HA_TIMER_HRTIMER

typedef struct tlob_task_state *monitor_target;

static inline void tlob_reset_notify(struct da_monitor *da_mon);
#define da_monitor_reset_hook tlob_reset_notify

static inline void tlob_extra_cleanup(struct da_monitor *da_mon);
#define da_extra_cleanup tlob_extra_cleanup

#define EVENT_NONE_LBL "budget_exceeded"

#include "tlob.h"

#define DA_MON_POOL_SIZE TLOB_MAX_MONITORED

#include <rv/ha_monitor.h>

/*
 * da_monitor_reset_hook: runs on hrtimer expiry, final teardown, and
 * monitor disable.  A normal stop never resets: tlob_stop_task() dispatches
 * "stop" (running -> stopped, tlob.dot) instead.  Only timer expiry is a
 * genuine budget violation.
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
	 * stopping==1 means tlob_stop_task() ended this window already.
	 * acquire pairs with the _release clear in ha_setup_invariants().
	 */
	if (atomic_read_acquire(&ws->stopping))
		return;

	/*
	 * Monitor disable (ha_mon_destroying set) is not a violation: the
	 * teardown paths free ws regardless.  Couples to an HA-layer flag
	 * with no public contract; a framework-level equivalent would be
	 * cleaner.
	 */
	if (unlikely(READ_ONCE(ha_mon_destroying)))
		return;

	/* Genuine expiry: end the window so a later start takes the restart path. */
	atomic_set(&ws->stopping, 1);

	/* Stamped regardless of the tracepoint; tlob_stop_task() reads it. */
	WRITE_ONCE(ws->budget_exceeded, true);

	if (!trace_detail_env_tlob_enabled())
		return;

	unsigned int curr_state = READ_ONCE(da_mon->curr_state);
	u64 accs[TLOB_ACC_MAX], partial_ns;
	unsigned long flags;

	/* Snapshot accumulators; partial_ns covers curr_state time not yet folded in. */
	raw_spin_lock_irqsave(&ws->entry_lock, flags);
	partial_ns = ktime_get_ns() - ktime_to_ns(ws->last_ts);
	accs[TLOB_ACC_RUNNING]  = ws->accs_ns[TLOB_ACC_RUNNING]  +
				  (curr_state == running_tlob  ? partial_ns : 0);
	accs[TLOB_ACC_WAITING]  = ws->accs_ns[TLOB_ACC_WAITING]  +
				  (curr_state == waiting_tlob  ? partial_ns : 0);
	accs[TLOB_ACC_SLEEPING] = ws->accs_ns[TLOB_ACC_SLEEPING] +
				  (curr_state == sleeping_tlob ? partial_ns : 0);
	raw_spin_unlock_irqrestore(&ws->entry_lock, flags);

	trace_detail_env_tlob(da_get_id(da_mon), ws->threshold_ns,
			      accs[TLOB_ACC_RUNNING],
			      accs[TLOB_ACC_WAITING],
			      accs[TLOB_ACC_SLEEPING]);
}

#define BUDGET_NS(ha_mon) (ha_get_target(ha_mon)->threshold_ns)

/* HA constraint functions (called by ha_monitor_handle_constraint) */

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_tlob env,
		      u64 time_ns)
{
	if (env == clk_elapsed_tlob)
		return ha_get_clk_ns(ha_mon, env, time_ns);
	return ENV_INVALID_VALUE;
}

/*
 * Invariant: clk_elapsed < BUDGET_NS in running/waiting/sleeping.  "stopped"
 * is exempt: the parked period must not be measured against the old window's
 * clock anchor (restart from "stopped" would otherwise spuriously overrun).
 */
static inline bool ha_verify_invariants(struct ha_monitor *ha_mon,
					enum states curr_state, enum events event,
					enum states next_state, u64 time_ns)
{
	if (curr_state == stopped_tlob)
		return true;
	return ha_check_invariant_ns(ha_mon, clk_elapsed_tlob, time_ns, BUDGET_NS(ha_mon));
}

/*
 * The clock stays in guard (anchor) representation all window: env_store
 * holds the window-start timestamp, re-anchored on start/restart.
 * ha_invariant_passed_ns() never stores the deadline representation (the
 * framework dropped ha_set_invariant_ns(), commit ab2900ae252b), so calling
 * ha_inv_to_guard() here would subtract BUDGET_NS from the anchor and skew
 * every check by one budget.  nomiss likewise never converts.
 */

/* No per-event guard conditions for tlob; invariants suffice. */
static inline bool ha_verify_guards(struct ha_monitor *ha_mon,
				    enum states curr_state, enum events event,
				    enum states next_state, u64 time_ns)
{
	return true;
}

/*
 * Guard on stopping: a sched_switch after ha_cancel_timer_sync() would
 * re-arm the timer (ODEBUG splat).  _acquire pairs with cmpxchg_release in
 * tlob_stop_task.
 *
 * Entering stopped_tlob also resets env_store to the invalid sentinel, so a
 * restart re-anchors the clock; a stale anchor would wrap the restart's
 * timer delay to ~U64_MAX.
 */
static inline void ha_setup_invariants(struct ha_monitor *ha_mon,
				       enum states curr_state, enum events event,
				       enum states next_state, u64 time_ns)
{
	if (next_state == stopped_tlob) {
		/*
		 * Window ending: reset env_store to the invalid sentinel so
		 * the next window gets a fresh clock anchor.  Keep stopping==1
		 * so __tlob_acc() continues to block sched events while parked.
		 */
		ha_monitor_reset_all_stored(ha_mon);
		return;
	}

	if (atomic_read_acquire(&ha_get_target(ha_mon)->stopping)) {
		/*
		 * Restart (stopped -> running): arm the timer, then clear
		 * stopping so __tlob_acc() admits sched events only once the
		 * state is already running_tlob.  _release pairs with the
		 * acquires in __tlob_acc/tlob_reset_notify.
		 */
		if (next_state < state_max_tlob)
			ha_start_timer_ns(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon), time_ns);
		atomic_set_release(&ha_get_target(ha_mon)->stopping, 0);
		return;
	}

	if (next_state < state_max_tlob)
		ha_start_timer_ns(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon), time_ns);
	else
		ha_cancel_timer(ha_mon);
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	if (!ha_verify_invariants(ha_mon, curr_state, event, next_state, time_ns))
		return false;

	if (!ha_verify_guards(ha_mon, curr_state, event, next_state, time_ns))
		return false;

	ha_setup_invariants(ha_mon, curr_state, event, next_state, time_ns);

	return true;
}

/*
 * Pre-allocated pool of TLOB_MAX_MONITORED slots.  mempool_alloc_preallocated()
 * pops a reserve slot without touching the allocator (bounded start latency;
 * -ENOSPC past the cap).  Slots return via destroy/cleanup; mempool_free() is
 * safe from RCU-callback context.
 */
static mempool_t tlob_ws_pool;

static void tlob_ws_return_cb(struct rcu_head *head)
{
	struct tlob_task_state *ws =
		container_of(head, struct tlob_task_state, rcu);

	mempool_free(ws, &tlob_ws_pool);
}

/* Direct return without RCU delay (ws was never published to the hash). */
static void tlob_ws_direct_return(struct tlob_task_state *ws)
{
	mempool_free(ws, &tlob_ws_pool);
}

static struct tlob_task_state *tlob_ws_alloc(void)
{
	struct tlob_task_state *ws =
		mempool_alloc_preallocated(&tlob_ws_pool);

	if (!ws)
		return NULL;

	memset(ws, 0, sizeof(*ws));
	INIT_LIST_HEAD(&ws->started_node);
	return ws;
}

/*
 * Uprobe binding list; protected by tlob_uprobe_mutex.  When both are
 * taken, tlob_uprobe_mutex is always acquired before tlob_ws_lock:
 * inverting the order would be a silent lock-order inversion.
 */
static LIST_HEAD(tlob_uprobe_list);
static DEFINE_MUTEX(tlob_uprobe_mutex);

/* Serialises tlob_task_state ownership: restart, detach, unbind reap. */
static DEFINE_SPINLOCK(tlob_ws_lock);

/* Per-uprobe-binding state: a start + stop probe pair for one binary region. */
struct tlob_uprobe_binding {
	struct list_head	list;
	u64			threshold_ns;
	char			binpath[TLOB_MAX_PATH];
	loff_t			offset_start;
	loff_t			offset_stop;
	/*
	 * All tlob_task_states this binding ever started, for each task's
	 * lifetime.  Protected by tlob_ws_lock.
	 */
	struct list_head	started_list;
	DECLARE_RV_UPROBE(start_probe);
	DECLARE_RV_UPROBE(stop_probe);
};

/*
 * Unlink ws from its binding's started_list before returning it to the pool.
 * ws->binding is left stale: the next tlob_ws_alloc() memsets it, and the
 * restart path checks destroying first.  Idempotent (list_del_init no-op).
 */
static inline void tlob_detach_from_binding(struct tlob_task_state *ws)
{
	if (!ws->binding)
		return;
	guard(spinlock)(&tlob_ws_lock);
	list_del_init(&ws->started_node);
}

/*
 * Per-entry teardown during monitor disable.  cmpxchg on destroying
 * (0->1) claims ownership -- not stopping, which can be long-lived (a
 * parked task).
 *
 * No timer cancel or locking needed: disable_tlob() already synced every
 * uprobe/tracepoint, da_monitor_destroy() ran da_monitor_reset_all() +
 * synchronize_rcu(), and ha_mon_destroying blocks new timer callbacks.
 */
static inline void tlob_extra_cleanup(struct da_monitor *da_mon)
{
	struct ha_monitor *ha_mon = to_ha_monitor(da_mon);
	struct tlob_task_state *ws = ha_get_target(ha_mon);

	if (!ws)
		return;

	if (atomic_cmpxchg_release(&ws->destroying, 0, 1) != 0)
		return;

	tlob_detach_from_binding(ws);
	put_task_struct(ws->task);
	/*
	 * da_monitor_destroy() has already called synchronize_rcu(); no
	 * reader holds ws.  Return the slot directly without call_rcu.
	 */
	mempool_free(ws, &tlob_ws_pool);
}

/*
 * Accumulate elapsed ns into accs_ns[idx] since last_ts and advance it.
 * Returns true if monitored with an active window.  The stopping gate is
 * what keeps scheduler events from reaching a parked task (no "stopped"
 * self-loops, see tlob.h) and keeps accs_ns[] from growing while parked.
 */
static inline bool __tlob_acc(struct task_struct *task, ktime_t now,
			       enum tlob_acc_idx idx)
{
	struct tlob_task_state *ws;
	unsigned long flags;

	guard(rcu)();
	ws = da_get_target_by_id(task->pid);
	/* acquire pairs with the _release clear in ha_setup_invariants(). */
	if (!ws || atomic_read_acquire(&ws->stopping))
		return false;
	raw_spin_lock_irqsave(&ws->entry_lock, flags);
	ws->accs_ns[idx] += ktime_to_ns(ktime_sub(now, ws->last_ts));
	ws->last_ts = now;
	raw_spin_unlock_irqrestore(&ws->entry_lock, flags);
	return true;
}

static inline bool tlob_acc_running(struct task_struct *task, ktime_t now)
{
	return __tlob_acc(task, now, TLOB_ACC_RUNNING);
}

static inline bool tlob_acc_waiting(struct task_struct *task, ktime_t now)
{
	return __tlob_acc(task, now, TLOB_ACC_WAITING);
}

/*
 * handle_sched_switch - advance the DA on every context switch.
 *
 * Emits sleep (running -> sleeping), preempt (running -> waiting) for prev,
 * and switch_in (waiting -> running) for next.  One ktime_get() shared by
 * both acc calls keeps prev/next on the same context-switch timestamp.
 *
 * No waiting->sleeping edge: a task blocks (calls schedule()) only on CPU
 * (running); waiting means TASK_RUNNING on the runqueue.
 */
static void handle_sched_switch(void *data, bool preempt_unused,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	ktime_t now = ktime_get();
	bool prev_preempted = (prev_state == 0);

	if (tlob_acc_running(prev, now))
		da_handle_event(prev->pid, NULL,
				prev_preempted ? preempt_tlob : sleep_tlob);
	if (tlob_acc_waiting(next, now))
		da_handle_event(next->pid, NULL, switch_in_tlob);
}

static inline bool tlob_acc_sleeping(struct task_struct *task, ktime_t now)
{
	return __tlob_acc(task, now, TLOB_ACC_SLEEPING);
}

/*
 * handle_sched_wakeup - sleeping -> waiting transition.  try_to_wake_up()
 * skips TASK_RUNNING tasks, so this never fires for running/waiting.
 */
static void handle_sched_wakeup(void *data, struct task_struct *p)
{
	ktime_t now = ktime_get();

	if (tlob_acc_sleeping(p, now))
		da_handle_event(p->pid, NULL, wakeup_tlob);
}

/* Forward decl: used by handle_sched_process_exit() and tlob_unbind_reap(). */
static int tlob_stop_task(struct task_struct *task, void *binding);
static void tlob_destroy_task(struct task_struct *task);

/*
 * handle_sched_process_exit - clean up a task that exits without hitting its
 * STOP uprobe (killed, unmapped mid-region, ...).  The task is always in
 * running_tlob here: do_exit() runs in the task's own context, which
 * required passing through switch_in_tlob (running).  tlob_stop_task() ends
 * the window (or is a harmless -EAGAIN/-ESRCH); tlob_destroy_task() then
 * frees the slot, as no restart can follow exit.
 */
static void handle_sched_process_exit(void *data, struct task_struct *p,
				       bool group_dead)
{
	tlob_stop_task(p, NULL);
	tlob_destroy_task(p);
}

/**
 * tlob_start_task - begin monitoring @task with budget @threshold_ns ns.
 * @task:         Task to monitor; may be current or another task.
 * @threshold_ns: Budget in ns, in [1000, TLOB_MAX_THRESHOLD_NS].
 * @binding:      Opaque owner, recorded on fresh allocation and checked for
 *                an exact match on restart; NULL for callers that never
 *                restart a parked window.
 *
 * Allocates a fresh entry if @task has none, or restarts a parked entry in
 * place when @binding matches (see tlob.dot: "start" fires from both
 * running and stopped).
 *
 * Returns 0, -ENODEV, -ERANGE, -EALREADY, -ESRCH, or -ENOSPC (fresh start
 * past pool capacity).
 */
static int tlob_start_task(struct task_struct *task, u64 threshold_ns, void *binding)
{
	struct tlob_task_state *ws;

	if (!da_monitor_enabled())
		return -ENODEV;

	if (threshold_ns < TLOB_MIN_THRESHOLD_NS ||
	    threshold_ns > TLOB_MAX_THRESHOLD_NS)
		return -ERANGE;

	/* Serialise duplicate-check + pool-slot claim; see tlob_ws_lock. */
	guard(spinlock)(&tlob_ws_lock);

	/*
	 * da_get_target_by_id() uses hash_for_each_possible_rcu(), which
	 * requires an RCU read-side critical section.
	 */
	scoped_guard(rcu) {
		ws = da_get_target_by_id(task->pid);
		if (ws) {
			if (!atomic_read(&ws->stopping))
				return -EALREADY;
			if (atomic_read(&ws->destroying))
				return -ESRCH;
			/*
			 * Exact match only.  An orphaned parked ws (binding
			 * cleared while active, then parked) is not adopted:
			 * that would need re-linking into the new binding's
			 * started_list.  Accepted gap; the slot is reclaimed
			 * at task exit.
			 */
			if (ws->binding != binding)
				return -EALREADY;

			/* Restart in place: same slot, hash entry, task ref, list node. */
			ws->threshold_ns = threshold_ns;
			WRITE_ONCE(ws->budget_exceeded, false);
			memset(ws->accs_ns, 0, sizeof(ws->accs_ns));
			ws->last_ts = ktime_get();

			/*
			 * Keep stopping set: __tlob_acc() gates out sched
			 * events until ha_setup_invariants() clears it after
			 * the state is running_tlob.  Clearing here would let
			 * events hit stopped_tlob (INVALID transitions).
			 */

			/* Only failure here: monitor disabled since the check above. */
			if (!da_handle_start_run_event(task->pid, ws, start_tlob))
				return -ENODEV;
			return 0;
		}
	}

	ws = tlob_ws_alloc();
	if (!ws)
		return -ENOSPC;

	ws->task = task;
	get_task_struct(task);
	ws->threshold_ns = threshold_ns;
	ws->last_ts = ktime_get();
	raw_spin_lock_init(&ws->entry_lock);
	ws->binding = binding;
	if (binding)
		list_add_tail(&ws->started_node,
			      &((struct tlob_uprobe_binding *)binding)->started_list);

	/* Dispatch failed (pool exhausted or monitor disabled): unwind the slot. */
	if (!da_handle_start_run_event(task->pid, ws, start_tlob)) {
		if (binding)
			list_del_init(&ws->started_node);
		put_task_struct(task);
		tlob_ws_direct_return(ws);
		return -ENOSPC;
	}

	return 0;
}

/**
 * tlob_stop_task - end the current monitoring window for @task.
 * @task: Task to stop.
 * @binding: Opaque owner; must match ws->binding to end a normal (uprobe)
 *           window.  NULL (task exit) skips the check.
 *
 * Ends the window (dispatches "stop") but does NOT free the entry: it stays
 * parked so a later tlob_start_task() can restart it.  Call
 * tlob_destroy_task() once @task will never restart.
 *
 * cmpxchg on stopping (0->1) under RCU claims ownership; the winner cancels
 * the timer synchronously.
 *
 * Returns 0, -EOVERFLOW (budget exceeded), -ESRCH (not monitored),
 * -EAGAIN (window already ended), or -EALREADY (owned by another binding).
 */
static int tlob_stop_task(struct task_struct *task, void *binding)
{
	struct ha_monitor *ha_mon;
	struct tlob_task_state *ws;
	bool budget_exceeded;

	scoped_guard(rcu) {
		ha_mon = ha_get_monitor(task->pid, NULL);
		if (!ha_mon)
			return -ESRCH;

		ws = ha_get_target(ha_mon);
		if (WARN_ON_ONCE(!ws))
			return -ESRCH;

		/* Only the binding that opened the window may end it; NULL
		 * (task exit) skips the check.  Symmetric with the restart
		 * check in tlob_start_task(). */
		if (binding && ws->binding != binding)
			return -EALREADY;

		/* cmpxchg (0->1) claims the window under RCU; _release pairs
		 * with the acquire in ha_setup_invariants(). */
		if (atomic_cmpxchg_release(&ws->stopping, 0, 1) != 0)
			return -EAGAIN;

		/*
		 * ws may be destroyed concurrently (unbind -> call_rcu), so
		 * keep its access under RCU; dispatch re-looks-up under RCU.
		 */
		ha_cancel_timer_sync(ha_mon);
		budget_exceeded = READ_ONCE(ws->budget_exceeded);
	}

	/* running -> stopped: no reset or destroy, the entry stays parked. */
	da_handle_event(task->pid, NULL, stop_tlob);

	return budget_exceeded ? -EOVERFLOW : 0;
}

/*
 * tlob_destroy_task - final teardown for @task's entry: frees the pool slot,
 * drops the task_struct ref, removes the hash entry, whether active or parked.
 * Idempotent via the destroying cmpxchg (same pattern as tlob_extra_cleanup()).
 * Callers must end the window first (see handle_sched_process_exit()).
 */
static void tlob_destroy_task(struct task_struct *task)
{
	struct ha_monitor *ha_mon;
	struct tlob_task_state *ws;

	scoped_guard(rcu) {
		ha_mon = ha_get_monitor(task->pid, NULL);
		if (!ha_mon)
			return;
		ws = ha_get_target(ha_mon);
		if (WARN_ON_ONCE(!ws))
			return;
		if (atomic_cmpxchg_release(&ws->destroying, 0, 1) != 0)
			return;
	}

	tlob_detach_from_binding(ws);

	/* Force the window ended: @task may never have reached STOP or a timer. */
	atomic_set(&ws->stopping, 1);
	ha_cancel_timer_sync(ha_mon);

	scoped_guard(rcu) {
		da_monitor_reset(&ha_mon->da_mon);
	}
	da_destroy_storage(task->pid);

	put_task_struct(ws->task);
	call_rcu(&ws->rcu, tlob_ws_return_cb);
}

static int tlob_uprobe_entry_handler(struct uprobe_consumer *self,
				     struct pt_regs *regs, __u64 *data)
{
	struct tlob_uprobe_binding *b =
		container_of(self, struct tlob_uprobe_binding, start_probe.uc);

	tlob_start_task(current, b->threshold_ns, b);
	return 0;
}

static int tlob_uprobe_stop_handler(struct uprobe_consumer *self,
				    struct pt_regs *regs, __u64 *data)
{
	struct tlob_uprobe_binding *b =
		container_of(self, struct tlob_uprobe_binding, stop_probe.uc);

	tlob_stop_task(current, b);
	return 0;
}

/*
 * Register start + stop entry uprobes for a binding.
 * Called with tlob_uprobe_mutex held.
 */
static int tlob_add_uprobe(u64 threshold_ns, const char *binpath,
			   loff_t offset_start, loff_t offset_stop)
{
	struct tlob_uprobe_binding *tmp_b;
	char pathbuf[TLOB_MAX_PATH];
	struct inode *inode;
	struct path path __free(path_put) = {};
	char *canon;
	int ret;

	if (binpath[0] != '/')
		return -EINVAL;

	struct tlob_uprobe_binding *b __free(kfree) = kzalloc_obj(*b, GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->threshold_ns = threshold_ns;
	b->offset_start = offset_start;
	b->offset_stop  = offset_stop;
	INIT_LIST_HEAD(&b->started_list);

	ret = kern_path(binpath, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;

	if (!d_is_reg(path.dentry))
		return -EINVAL;

	inode = d_real_inode(path.dentry);

	/* Reject duplicate start offset for the same binary inode. */
	list_for_each_entry(tmp_b, &tlob_uprobe_list, list) {
		if (tmp_b->offset_start == offset_start &&
		    rv_uprobe_is_registered(&tmp_b->start_probe) &&
		    d_real_inode(tmp_b->start_probe.path.dentry) == inode)
			return -EEXIST;
	}

	canon = d_path(&path, pathbuf, sizeof(pathbuf));
	if (IS_ERR(canon))
		return PTR_ERR(canon);
	strscpy(b->binpath, canon, sizeof(b->binpath));

	b->start_probe.uc.handler = tlob_uprobe_entry_handler;
	ret = rv_uprobe_register(b->binpath, offset_start, &b->start_probe);
	if (ret)
		return ret;

	b->stop_probe.uc.handler = tlob_uprobe_stop_handler;
	ret = rv_uprobe_register(b->binpath, offset_stop, &b->stop_probe);
	if (ret) {
		rv_uprobe_unregister(&b->start_probe);
		return ret;
	}

	/* NOT "b = no_free_ptr(b)": the re-assignment would free the live node. */
	list_add_tail(&no_free_ptr(b)->list, &tlob_uprobe_list);
	return 0;
}

/*
 * tlob_unbind_reap - detach every task @b started, destroy the parked ones.
 *
 * Caller must have unregistered @b's uprobes and called rv_uprobe_sync():
 * no start/stop can then be in flight for @b, so started_list is safe to
 * walk.  Active tasks are detached (binding cleared) and left running,
 * matching unbind behaviour today; parked tasks are destroyed, or their
 * pool slot leaks until the task next exits.
 */
static void tlob_unbind_reap(struct tlob_uprobe_binding *b)
{
	struct tlob_task_state *ws, *tmp;
	LIST_HEAD(to_destroy);

	scoped_guard(spinlock, &tlob_ws_lock) {
		list_for_each_entry_safe(ws, tmp, &b->started_list, started_node) {
			list_del_init(&ws->started_node);
			ws->binding = NULL;
			if (atomic_read(&ws->stopping))
				list_add_tail(&ws->started_node, &to_destroy);
		}
	}

	list_for_each_entry_safe(ws, tmp, &to_destroy, started_node) {
		list_del_init(&ws->started_node);
		tlob_destroy_task(ws->task);
	}
}

static int tlob_remove_uprobe_by_key(loff_t offset_start, const char *binpath)
{
	struct tlob_uprobe_binding *b, *tmp;
	struct path remove_path;
	struct inode *inode;
	int ret;

	ret = kern_path(binpath, LOOKUP_FOLLOW, &remove_path);
	if (ret)
		return ret;

	inode = d_real_inode(remove_path.dentry);

	ret = -ENOENT;
	list_for_each_entry_safe(b, tmp, &tlob_uprobe_list, list) {
		if (b->offset_start != offset_start)
			continue;
		if (d_real_inode(b->start_probe.path.dentry) != inode)
			continue;
		list_del(&b->list);
		/*
		 * rv_uprobe_sync() may sleep; list_del() already made the
		 * binding invisible to new readers.
		 */
		rv_uprobe_unregister_nosync(&b->start_probe);
		rv_uprobe_unregister_nosync(&b->stop_probe);
		rv_uprobe_sync();
		tlob_unbind_reap(b);
		path_put(&b->start_probe.path);
		path_put(&b->stop_probe.path);
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
		rv_uprobe_unregister_nosync(&b->start_probe);
		rv_uprobe_unregister_nosync(&b->stop_probe);
	}
	mutex_unlock(&tlob_uprobe_mutex);

	if (list_empty(&pending))
		return;

	/* One sync covers all dequeued probes: consumers are then safe to free. */
	rv_uprobe_sync();

	list_for_each_entry_safe(b, tmp, &pending, list) {
		list_del(&b->list);
		tlob_unbind_reap(b);
		path_put(&b->start_probe.path);
		path_put(&b->stop_probe.path);
		kfree(b);
	}
}

static ssize_t tlob_monitor_read(struct file *file,
				 char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	const int line_sz = TLOB_MAX_PATH + 128;
	struct tlob_uprobe_binding *b;
	char *buf;
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
		pos += scnprintf(buf + pos, buf_sz - pos,
				 "p %s:0x%llx 0x%llx threshold=%llu\n",
				 b->binpath,
				 (unsigned long long)b->offset_start,
				 (unsigned long long)b->offset_stop,
				 b->threshold_ns);
	}
	mutex_unlock(&tlob_uprobe_mutex);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, pos);
	kfree(buf);
	return ret;
}

/*
 * Parse "p PATH:OFFSET_START OFFSET_STOP threshold=NS".
 * PATH may contain ':'; the last ':' separates path from offset.
 * Returns 0, -EINVAL, or -ERANGE.
 */
VISIBLE_IF_KUNIT int tlob_parse_uprobe_line(char *buf, u64 *thr_out,
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

	/* Remaining tokens: OFFSET_STOP threshold=NS */
	while (p && (token = strsep(&p, " \t")) != NULL) {
		if (!*token)
			continue;
		if (strncmp(token, "threshold=", 10) == 0) {
			if (kstrtoull(token + 10, 0, &thr))
				return -EINVAL;
			if (thr < TLOB_MIN_THRESHOLD_NS || thr > TLOB_MAX_THRESHOLD_NS)
				return -ERANGE;
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

	if (!got_stop || !got_thr)
		return -EINVAL;
	if (start_val == (long long)stop_val)
		return -EINVAL;

	*thr_out   = thr;
	*path_out  = path_token;
	*start_out = (loff_t)start_val;
	*stop_out  = (loff_t)stop_val;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_parse_uprobe_line);

/*
 * Parse "-PATH:OFFSET_START" (ftrace uprobe_events removal convention).
 */
VISIBLE_IF_KUNIT int tlob_parse_remove_line(char *buf, char **path_out,
					    loff_t *start_out)
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
	if (off < 0)
		return -EINVAL;
	*path_out  = binpath;
	*start_out = (loff_t)off;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_parse_remove_line);

static int tlob_create_or_delete_uprobe(char *buf)
{
	loff_t offset_start, offset_stop;
	u64 threshold_ns;
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
	ret = tlob_parse_uprobe_line(buf, &threshold_ns, &binpath,
				     &offset_start, &offset_stop);
	if (ret)
		return ret;
	mutex_lock(&tlob_uprobe_mutex);
	ret = tlob_add_uprobe(threshold_ns, binpath, offset_start, offset_stop);
	mutex_unlock(&tlob_uprobe_mutex);
	return ret;
}

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

	retval = mempool_init_kmalloc_pool(&tlob_ws_pool, TLOB_MAX_MONITORED,
					   sizeof(struct tlob_task_state));
	if (retval)
		return retval;

	retval = ha_monitor_init();
	if (retval) {
		mempool_exit(&tlob_ws_pool);
		return retval;
	}

	rv_this.enabled = 1;
	return 0;
}

static void __tlob_destroy_monitor(void)
{
	rv_this.enabled = 0;
	tlob_remove_all_uprobes();
	/*
	 * A grace period only makes the call_rcu()'d tlob_ws_return_cb()
	 * callbacks eligible to run; rcu_barrier() waits until they have all
	 * returned their slots before the pool is destroyed.
	 */
	ha_monitor_destroy();
	rcu_barrier();
	mempool_exit(&tlob_ws_pool);
}

static int tlob_enable_hooks(void)
{
	rv_attach_trace_probe("tlob", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("tlob", sched_wakeup, handle_sched_wakeup);
	rv_attach_trace_probe("tlob", sched_process_exit, handle_sched_process_exit);
	return 0;
}

static void tlob_disable_hooks(void)
{
	rv_detach_trace_probe("tlob", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("tlob", sched_wakeup, handle_sched_wakeup);
	rv_detach_trace_probe("tlob", sched_process_exit, handle_sched_process_exit);
}

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

static int __init register_tlob(void)
{
	int ret;

	ret = rv_register_monitor(&rv_this, NULL);
	if (ret)
		return ret;

	if (rv_this.root_d) {
		if (!rv_create_file("monitor", RV_MODE_WRITE, rv_this.root_d, NULL,
				    &tlob_monitor_fops)) {
			rv_unregister_monitor(&rv_this);
			return -ENOMEM;
		}
	}

	return 0;
}

static void __exit unregister_tlob(void)
{
	rv_unregister_monitor(&rv_this);
}

module_init(register_tlob);
module_exit(unregister_tlob);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Wen Yang <wen.yang@linux.dev>");
MODULE_DESCRIPTION("tlob: task latency over budget per-task monitor.");
