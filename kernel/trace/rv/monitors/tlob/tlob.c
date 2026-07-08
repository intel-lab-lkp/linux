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
 * that pinpoints whether the overrun occurred in the running, waiting,
 * or sleeping state.
 *
 * The monitor uses RV_MON_PER_OBJ: per-task state (struct tlob_task_state)
 * is stored as monitor_target in the framework's hash table.
 *
 * One HA clock invariant is enforced:
 *   clk_elapsed < BUDGET_NS()   (active in all states)
 *
 * Copyright (C) 2026 Wen Yang <wen.yang@linux.dev>
 */
#include <linux/kernel.h>
#include <linux/llist.h>
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

	/* 1 = cleanup claimed; ha_setup_invariants won't restart the timer. */
	atomic_t		stopping;

	/* Serialises accs_ns[]; held briefly (hardirq-safe). */
	raw_spinlock_t		entry_lock;
	u64			accs_ns[TLOB_ACC_MAX]; /* per-state elapsed ns */
	ktime_t			last_ts;

	struct rcu_head		rcu;
	/* Free-list node; active only between call_rcu() return and next alloc. */
	struct llist_node	free_node;
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
 * Called from da_monitor_reset() on both normal stop and hrtimer expiry.
 * On violation (stopping==0), emits detail_env_tlob.
 */
static inline void tlob_reset_notify(struct da_monitor *da_mon)
{
	struct ha_monitor *ha_mon = to_ha_monitor(da_mon);
	struct tlob_task_state *ws;

	ha_monitor_reset_env(da_mon);

	if (!trace_detail_env_tlob_enabled())
		return;

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
		u64 accs[TLOB_ACC_MAX], partial_ns;
		unsigned long flags;

		/*
		 * Snapshot accumulators; partial_ns covers curr_state time
		 * not yet folded in (transition-out pending).
		 */
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
 * ha_verify_invariants - clk_elapsed < BUDGET_NS must hold in all states.
 *
 * The invariant is uniform across running/waiting/sleeping; check it
 * unconditionally rather than enumerating each state.
 */
static inline bool ha_verify_invariants(struct ha_monitor *ha_mon,
					enum states curr_state, enum events event,
					enum states next_state, u64 time_ns)
{
	return ha_check_invariant_ns(ha_mon, clk_elapsed_tlob, time_ns);
}

/*
 * Convert invariant (deadline) to guard (reset anchor) on state transitions.
 *
 * The conversion is identical for every departing state; skip only self-loops.
 */
static inline void ha_convert_inv_guard(struct ha_monitor *ha_mon,
					enum states curr_state, enum events event,
					enum states next_state, u64 time_ns)
{
	if (curr_state != next_state)
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
 * Guard on stopping: a sched_switch arriving after ha_cancel_timer_sync()
 * would re-arm the timer and trigger an ODEBUG "activate active" splat.
 * _acquire pairs with cmpxchg_release in tlob_stop_task.
 */
static inline void ha_setup_invariants(struct ha_monitor *ha_mon,
				       enum states curr_state, enum events event,
				       enum states next_state, u64 time_ns)
{
	if (atomic_read_acquire(&ha_get_target(ha_mon)->stopping))
		return;
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

	ha_convert_inv_guard(ha_mon, curr_state, event, next_state, time_ns);

	if (!ha_verify_guards(ha_mon, curr_state, event, next_state, time_ns))
		return false;

	ha_setup_invariants(ha_mon, curr_state, event, next_state, time_ns);

	return true;
}

/*
 * Pre-allocated pool for tlob_task_state slots.  Lock-free llist so that
 * tlob_ws_return_cb() (RCU callback) can return slots without acquiring a
 * spinlock.  Same concurrency model as da_pool_storage (see da_monitor.h).
 */
static struct tlob_task_state *tlob_ws_storage;
static LLIST_HEAD(tlob_ws_free_list);

static void tlob_ws_return_cb(struct rcu_head *head)
{
	struct tlob_task_state *ws =
		container_of(head, struct tlob_task_state, rcu);

	llist_add(&ws->free_node, &tlob_ws_free_list);
}

/* Direct return to free list without RCU delay (ws was never published). */
static void tlob_ws_direct_return(struct tlob_task_state *ws)
{
	llist_add(&ws->free_node, &tlob_ws_free_list);
}

static struct tlob_task_state *tlob_ws_alloc(void)
{
	struct llist_node *node = llist_del_first(&tlob_ws_free_list);

	if (!node)
		return NULL;

	struct tlob_task_state *ws =
		llist_entry(node, struct tlob_task_state, free_node);

	memset(ws, 0, sizeof(*ws));
	return ws;
}

/* Uprobe binding list; protected by tlob_uprobe_mutex. */
static LIST_HEAD(tlob_uprobe_list);
static DEFINE_MUTEX(tlob_uprobe_mutex);

/*
 * Serialises duplicate-check + da_handle_start_run_event() per pid.
 * spinlock_t not raw_spinlock_t: uprobe handlers run under Tasks Trace
 * SRCU (rcu_read_lock_trace()), which permits sleeping on PREEMPT_RT.
 */
static DEFINE_SPINLOCK(tlob_start_lock);

/* Per-uprobe-binding state: a start + stop probe pair for one binary region. */
struct tlob_uprobe_binding {
	struct list_head	list;
	u64			threshold_ns;
	char			binpath[TLOB_MAX_PATH];
	loff_t			offset_start;
	loff_t			offset_stop;
	DECLARE_RV_UPROBE(start_probe);
	DECLARE_RV_UPROBE(stop_probe);
};

/*
 * Per-task teardown invoked by da_monitor_destroy() for each hash entry.
 * CAS on stopping (0->1) claims exclusive cleanup ownership.
 *
 * No per-entry ha_cancel_timer_sync(): da_monitor_destroy() calls
 * da_monitor_reset_all() + synchronize_rcu() before this hook, and
 * ha_mon_destroying prevents new timer callbacks from running.
 */
static inline void tlob_extra_cleanup(struct da_monitor *da_mon)
{
	struct ha_monitor *ha_mon = to_ha_monitor(da_mon);
	struct tlob_task_state *ws = ha_get_target(ha_mon);

	if (!ws)
		return;

	if (atomic_cmpxchg_release(&ws->stopping, 0, 1) != 0)
		return;

	put_task_struct(ws->task);
	/*
	 * da_monitor_destroy() has already called synchronize_rcu(); no
	 * reader holds ws.  Return the slot directly without call_rcu.
	 */
	llist_add(&ws->free_node, &tlob_ws_free_list);
}

static inline bool __tlob_acc(struct task_struct *task, ktime_t now,
			       enum tlob_acc_idx idx)
{
	struct tlob_task_state *ws;
	unsigned long flags;

	guard(rcu)();
	ws = da_get_target_by_id(task->pid);
	if (!ws)
		return false;
	raw_spin_lock_irqsave(&ws->entry_lock, flags);
	ws->accs_ns[idx] += ktime_to_ns(ktime_sub(now, ws->last_ts));
	ws->last_ts = now;
	raw_spin_unlock_irqrestore(&ws->entry_lock, flags);
	return true;
}

/* Accumulate running_ns for prev; returns true if prev is monitored. */
static inline bool tlob_acc_running(struct task_struct *task, ktime_t now)
{
	return __tlob_acc(task, now, TLOB_ACC_RUNNING);
}

/* Accumulate waiting_ns for next; returns true if next is monitored. */
static inline bool tlob_acc_waiting(struct task_struct *task, ktime_t now)
{
	return __tlob_acc(task, now, TLOB_ACC_WAITING);
}

/*
 * handle_sched_switch - advance the DA on every context switch.
 *
 * Generates three DA events:
 *   prev, prev_state != 0  -> sleep_tlob    (running -> sleeping)
 *   prev, prev_state == 0  -> preempt_tlob  (running -> waiting)
 *   next                   -> switch_in_tlob (waiting -> running)
 *
 * A single ktime_get() at handler entry is shared by both acc calls so that
 * prev's running_ns and next's waiting_ns share the same context-switch
 * timestamp; neither absorbs handler overhead into its accumulator.
 *
 * No waiting->sleeping edge exists: a task can only block voluntarily
 * (call schedule()) while it is executing on CPU, which corresponds to
 * the running DA state.  A task in the waiting state is TASK_RUNNING in
 * kernel terms (on the runqueue) and cannot block itself.
 *
 * da_handle_event() is called unconditionally: it skips tasks that have no
 * monitor entry in the hash table.
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

/* Accumulate sleeping_ns on wakeup; returns true if task is monitored. */
static inline bool tlob_acc_sleeping(struct task_struct *task, ktime_t now)
{
	return __tlob_acc(task, now, TLOB_ACC_SLEEPING);
}

/*
 * handle_sched_wakeup - sleeping -> waiting transition.
 *
 * try_to_wake_up() skips TASK_RUNNING tasks, so this never fires for a
 * task already in running or waiting state.
 */
static void handle_sched_wakeup(void *data, struct task_struct *p)
{
	ktime_t now = ktime_get();

	if (tlob_acc_sleeping(p, now))
		da_handle_event(p->pid, NULL, wakeup_tlob);
}

/*
 * handle_sched_process_exit - clean up if a task exits without TRACE_STOP.
 *
 * Called in do_exit() context; the task still has a valid pid here.
 * tlob_stop_task() returns -ESRCH if the task is not monitored, which is fine.
 */
static void handle_sched_process_exit(void *data, struct task_struct *p,
				       bool group_dead)
{
	tlob_stop_task(p);
}

/**
 * tlob_start_task - begin monitoring @task with budget @threshold_ns ns.
 * @task:         Task to monitor; may be current or another task.
 * @threshold_ns: Latency budget in nanoseconds (wall-clock; running +
 *                waiting + sleeping).
 *                Must be in [1000, TLOB_MAX_THRESHOLD_NS].
 *
 * Returns 0, -ENODEV, -ERANGE, -EALREADY, or -ENOSPC (pool at capacity).
 */
int tlob_start_task(struct task_struct *task, u64 threshold_ns)
{
	struct tlob_task_state *ws;

	if (!da_monitor_enabled())
		return -ENODEV;

	if (threshold_ns < TLOB_MIN_THRESHOLD_NS ||
	    threshold_ns > TLOB_MAX_THRESHOLD_NS)
		return -ERANGE;

	/* Serialise duplicate-check + pool-slot claim; see tlob_start_lock. */
	guard(spinlock)(&tlob_start_lock);

	/*
	 * __da_get_mon_storage() uses hash_for_each_possible_rcu(), which
	 * requires an RCU read-side critical section.  On PREEMPT_RT,
	 * spinlock_t is an rt_mutex and does not satisfy this requirement.
	 */
	scoped_guard(rcu) {
		if (da_get_target_by_id(task->pid))
			return -EALREADY;
	}

	/*
	 * Both tlob_ws_alloc() and da_handle_start_run_event() pop from
	 * pre-allocated pools of size TLOB_MAX_MONITORED; NULL return means
	 * the pool is at capacity.
	 */
	ws = tlob_ws_alloc();
	if (!ws)
		return -ENOSPC;

	ws->task = task;
	get_task_struct(task);
	ws->threshold_ns = threshold_ns;
	ws->last_ts = ktime_get();
	raw_spin_lock_init(&ws->entry_lock);

	/*
	 * da_handle_start_run_event() claims a pool slot via da_prepare_storage(),
	 * initialises the monitor, and delivers start_tlob in one step: the
	 * generated ha_setup_invariants() resets clk_elapsed and arms the timer.
	 * Returns 0 if the da_monitor_storage pool is exhausted.
	 */
	if (!da_handle_start_run_event(task->pid, ws, start_tlob)) {
		put_task_struct(task);
		tlob_ws_direct_return(ws);
		return -ENOSPC;
	}

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

	scoped_guard(rcu) {
		ws = da_get_target_by_id(task->pid);
		if (!ws)
			return -ESRCH;

		da_mon = da_get_monitor(task->pid, NULL);
		if (unlikely(WARN_ON_ONCE(!da_mon)))
			return -ESRCH;

		ha_mon = to_ha_monitor(da_mon);

		/*
		 * CAS (0->1) claims cleanup ownership under RCU (ws guaranteed valid).
		 * _release pairs with atomic_read_acquire in ha_setup_invariants.
		 */
		if (atomic_cmpxchg_release(&ws->stopping, 0, 1) != 0)
			return -EAGAIN;
	}
	/*
	 * ws and ha_mon are used below outside the RCU guard.  This is safe:
	 * the winning CAS (stopping: 0->1) is the only path that frees ws,
	 * and da_destroy_storage() below is the only call that returns the
	 * pool slot.  No concurrent path can free either object.
	 */

	/* Wait for in-flight timer callback before reading da_monitoring. */
	ha_cancel_timer_sync(ha_mon);

	/* Timer fired first -> budget exceeded; otherwise reset normally. */
	scoped_guard(rcu) {
		budget_exceeded = !da_monitoring(da_mon);
		if (!budget_exceeded)
			da_monitor_reset(da_mon);
	}
	da_destroy_storage(task->pid);

	put_task_struct(ws->task);
	call_rcu(&ws->rcu, tlob_ws_return_cb);
	return budget_exceeded ? -EOVERFLOW : 0;
}
EXPORT_SYMBOL_GPL(tlob_stop_task);

static int tlob_uprobe_entry_handler(struct uprobe_consumer *self,
				     struct pt_regs *regs, __u64 *data)
{
	struct tlob_uprobe_binding *b =
		container_of(self, struct tlob_uprobe_binding, start_probe.uc);

	tlob_start_task(current, b->threshold_ns);
	return 0;
}

static int tlob_uprobe_stop_handler(struct uprobe_consumer *self,
				    struct pt_regs *regs, __u64 *data)
{
	tlob_stop_task(current);
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
		    tmp_b->start_probe.inode == inode)
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

	/*
	 * Do NOT write "b = no_free_ptr(b)": the re-assignment restores b,
	 * causing __free(kfree) to free a live list node on exit.
	 */
	list_add_tail(&no_free_ptr(b)->list, &tlob_uprobe_list);
	return 0;
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
		if (b->start_probe.inode != inode)
			continue;
		list_del(&b->list);
		/*
		 * rv_uprobe_sync() may sleep, blocking tlob_monitor_read() on
		 * tlob_uprobe_mutex.  Safe: list_del() above made the binding
		 * invisible to new readers before we drop the mutex.
		 */
		rv_uprobe_unregister_nosync(&b->start_probe);
		rv_uprobe_unregister_nosync(&b->stop_probe);
		rv_uprobe_sync();
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

	/*
	 * One rv_uprobe_sync() covers all probes dequeued above.
	 * After this, no handler_chain() iteration can access any consumer.
	 * The embedded uprobe_consumers in each binding are safe to free.
	 */
	rv_uprobe_sync();

	list_for_each_entry_safe(b, tmp, &pending, list) {
		list_del(&b->list);
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
	unsigned int i;
	int retval;

	tlob_ws_storage = kcalloc(TLOB_MAX_MONITORED, sizeof(*tlob_ws_storage),
				  GFP_KERNEL);
	if (!tlob_ws_storage)
		return -ENOMEM;

	for (i = 0; i < TLOB_MAX_MONITORED; i++)
		llist_add(&tlob_ws_storage[i].free_node, &tlob_ws_free_list);

	retval = ha_monitor_init();
	if (retval) {
		kfree(tlob_ws_storage);
		tlob_ws_storage = NULL;
		init_llist_head(&tlob_ws_free_list);
		return retval;
	}

	rv_this.enabled = 1;
	return 0;
}

static void __tlob_destroy_monitor(void)
{
	rv_this.enabled = 0;
	tlob_remove_all_uprobes();
	ha_monitor_destroy();
	init_llist_head(&tlob_ws_free_list);
	kfree(tlob_ws_storage);
	tlob_ws_storage = NULL;
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
