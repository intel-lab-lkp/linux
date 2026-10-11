// SPDX-License-Identifier: GPL-2.0
/*
 * tlob: task latency over budget monitor
 *
 * Flags budget overruns of a marked user-space code path: the hrtimer
 * callback emits error_env_tlob on violation.  The per-state
 * (running/waiting/sleeping) breakdown is derived from event_tlob in user
 * space (see Documentation/trace/rv/monitor_tlob.rst).
 *
 * RV_MON_PER_OBJ: the per-task record lives as monitor_target in the
 * framework's hash table.  One HA clock invariant: clk_elapsed <
 * BUDGET_NS() in running/waiting/sleeping (stopped exempt).
 *
 * Copyright (C) 2026 Wen Yang <wen.yang@linux.dev>
 */
#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/mempool.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/rv.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <rv/instrumentation.h>
#include <rv/rv_uprobe.h>
#include <rv.h>

#define MODULE_NAME "tlob"

#include <trace/events/sched.h>
#include <rv_trace.h>

struct tlob_uprobe_binding;

/*
 * The record's lifecycle -- not the task's scheduler state and not the
 * automaton's curr_state:
 *   TLOB_OPEN    - owns the live window
 *   TLOB_CLOSED  - the window ended, the record waits for a restart
 *   TLOB_RETIRED - a teardown path claimed the record; it is never reused
 */
enum tlob_record_state {
	TLOB_OPEN,
	TLOB_CLOSED,
	TLOB_RETIRED,
};

/*
 * One per-task monitoring record, reused across that task's windows: from
 * tlob_rec_pool, hashed as monitor_target, freed via call_rcu.
 */
struct tlob_record {
	atomic_t		state;
	u64			threshold_ns;

	pid_t			id;
	/*
	 * Owner binding, NULL once claimed for teardown.  started_node is then
	 * owned by the claimer's list instead.  Both under tlob_binding_lock.
	 */
	struct tlob_uprobe_binding	*binding;
	struct list_head	started_node;

	struct rcu_head		rcu;
};

#define RV_MON_TYPE RV_MON_PER_OBJ
#define HA_TIMER_TYPE HA_TIMER_HRTIMER

typedef struct tlob_record *monitor_target;

static inline void tlob_reset_notify(struct da_monitor *da_mon);
#define da_monitor_reset_hook tlob_reset_notify

static inline void tlob_extra_cleanup(struct da_monitor *da_mon);
#define da_extra_cleanup tlob_extra_cleanup

#define EVENT_NONE_LBL "budget_exceeded"

#include "tlob.h"

#define DA_MON_POOL_SIZE TLOB_MAX_MONITORED

#include <rv/ha_monitor.h>

/*
 * Reset hook: claim the window end so the record can be restarted.  Runs on
 * timer expiry (violation), teardown and disable; stop uses "stop".
 */
static inline void tlob_reset_notify(struct da_monitor *da_mon)
{
	struct ha_monitor *ha_mon = to_ha_monitor(da_mon);
	struct tlob_record *rec;

	/* Teardown disarms via da_monitor_free_hook() and frees rec. */
	if (unlikely(READ_ONCE(ha_mon_destroying)))
		return;

	ha_monitor_reset_env(da_mon);

	rec = ha_get_target(ha_mon);
	if (!rec)
		return;

	/* A stop or teardown may have taken the window first. */
	atomic_cmpxchg(&rec->state, TLOB_OPEN, TLOB_CLOSED);
}

#define BUDGET_NS(ha_mon) (ha_get_target(ha_mon)->threshold_ns)

static u64 ha_get_env(struct ha_monitor *ha_mon, enum envs_tlob env,
		      u64 time_ns)
{
	if (env == clk_elapsed_tlob)
		return ha_get_clk_ns(ha_mon, env, time_ns);
	return ENV_INVALID_VALUE;
}

static void ha_reset_env(struct ha_monitor *ha_mon, enum envs_tlob env,
			 u64 time_ns)
{
	if (env == clk_elapsed_tlob)
		ha_reset_clk_ns(ha_mon, env, time_ns);
}

/*
 * clk_elapsed <= BUDGET_NS in running/waiting/sleeping; stopped exempt.
 *
 * tlob needs no guards: env_store stays in guard (anchor) form for the whole
 * window, so ha_inv_to_guard() must never be applied to it; that would
 * subtract BUDGET_NS and skew every check by one budget.
 */
static inline bool ha_verify_invariants(struct ha_monitor *ha_mon,
					enum states curr_state,
					enum events event,
					enum states next_state,
					u64 time_ns)
{
	if (curr_state == stopped_tlob)
		return true;
	return ha_check_invariant_ns(ha_mon, clk_elapsed_tlob, time_ns,
				     BUDGET_NS(ha_mon));
}

/*
 * Only start restarts a closed window; a sched event racing the window
 * end must not re-arm the timer.  Restart re-anchors the clock to the
 * event time; a stale anchor would underflow the timer delay.
 */
static inline void ha_setup_invariants(struct ha_monitor *ha_mon,
				       enum states curr_state,
				       enum events event,
				       enum states next_state,
				       u64 time_ns)
{
	struct tlob_record *rec = ha_get_target(ha_mon);

	if (next_state == stopped_tlob)
		return;

	if (next_state >= state_max_tlob) {
		ha_cancel_timer(ha_mon);
		return;
	}

	if (event != start_tlob) {
		/* A sched event racing the window end must not re-arm. */
		if (atomic_read_acquire(&rec->state) != TLOB_OPEN)
			return;
		ha_start_timer_ns(ha_mon, clk_elapsed_tlob,
				  BUDGET_NS(ha_mon), time_ns);
		return;
	}

	ha_reset_clk_ns(ha_mon, clk_elapsed_tlob, time_ns);
	ha_start_timer_ns(ha_mon, clk_elapsed_tlob, BUDGET_NS(ha_mon),
			  time_ns);
	/* Reopen only a closed record; a teardown claim keeps it closed. */
	atomic_cmpxchg_release(&rec->state, TLOB_CLOSED, TLOB_OPEN);
}

static bool ha_verify_constraint(struct ha_monitor *ha_mon,
				 enum states curr_state, enum events event,
				 enum states next_state, u64 time_ns)
{
	if (!ha_verify_invariants(ha_mon, curr_state, event,
				  next_state, time_ns))
		return false;

	ha_setup_invariants(ha_mon, curr_state, event, next_state, time_ns);

	return true;
}

/*
 * TLOB_MAX_MONITORED pre-allocated slots: bounded start latency,
 * -ENOSPC past the cap; mempool_free() is RCU-callback safe.
 *
 * A published slot owes readers a grace period, so every return goes through
 * the RCU callback.
 */
static mempool_t tlob_rec_pool;

static void tlob_rec_return_cb(struct rcu_head *head)
{
	struct tlob_record *rec =
		container_of(head, struct tlob_record, rcu);

	mempool_free(rec, &tlob_rec_pool);
}

static struct tlob_record *tlob_rec_alloc(void)
{
	struct tlob_record *rec =
		mempool_alloc_preallocated(&tlob_rec_pool);

	if (!rec)
		return NULL;

	memset(rec, 0, sizeof(*rec));
	INIT_LIST_HEAD(&rec->started_node);
	return rec;
}

/* Protected by tlob_uprobe_mutex. */
static LIST_HEAD(tlob_uprobe_list);
static DEFINE_MUTEX(tlob_uprobe_mutex);

/*
 * Serialises tlob_record ownership: restart, detach, unbind reap.  Lock order:
 * tlob_uprobe_mutex first.  Critical sections touch only lists and atomics, so
 * a PREEMPT_RT promotion is safe.
 */
static DEFINE_SPINLOCK(tlob_binding_lock);

/* Per-binding state: a start + stop probe pair for one binary region. */
struct tlob_uprobe_binding {
	struct list_head	list;
	char			name[TLOB_MAX_NAME];
	u64			threshold_ns;
	char			binpath[TLOB_MAX_PATH];
	loff_t			offset_start;
	loff_t			offset_stop;
	/*
	 * Keeps the dentry hashed: later lookups of binpath (exec, re-add)
	 * must hit the inode the probes are keyed on.
	 */
	struct path		path;
	/* Records started here.  tlob_binding_lock. */
	struct list_head	started_list;
	/* Claimed by quiesce, destroyed by reap.  tlob_binding_lock. */
	struct list_head	quiesced_list;
	struct rv_uprobe		start_probe;
	struct rv_uprobe		stop_probe;
};

static inline void tlob_detach_from_binding(struct tlob_record *rec)
{
	guard(spinlock)(&tlob_binding_lock);

	if (!rec->binding)
		return;
	rec->binding = NULL;
	list_del_init(&rec->started_node);
}

/*
 * Per-record teardown during monitor disable.  TLOB_RETIRED claims it, so an
 * exit or unbind racing disable frees the record once.
 */
static inline void tlob_extra_cleanup(struct da_monitor *da_mon)
{
	struct ha_monitor *ha_mon = to_ha_monitor(da_mon);
	struct tlob_record *rec = ha_get_target(ha_mon);

	if (!rec)
		return;

	if (atomic_xchg(&rec->state, TLOB_RETIRED) == TLOB_RETIRED)
		return;

	tlob_detach_from_binding(rec);
	call_rcu(&rec->rcu, tlob_rec_return_cb);
}

static void handle_sched_switch(void *data, bool preempt,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	/* Preemption leaves the task on the runqueue whatever __state says. */
	bool prev_preempted = preempt || !prev_state;

	da_handle_event(prev->pid, NULL,
			prev_preempted ? preempt_tlob : sleep_tlob);
	da_handle_event(next->pid, NULL, switch_in_tlob);
}

/*
 * handle_sched_wakeup - sleeping -> waiting.  A task woken before it reached
 * schedule() is still on CPU, hence the running self-loop.
 */
static void handle_sched_wakeup(void *data, struct task_struct *p)
{
	da_handle_event(p->pid, NULL, wakeup_tlob);
}

static void tlob_stop_task(struct task_struct *task,
			   struct tlob_uprobe_binding *binding);
static void tlob_destroy_task(pid_t id);

/*
 * handle_sched_process_exit - clean up a task that exits without hitting
 * its STOP uprobe.  Always running here: do_exit() runs in the task's own
 * context; no restart can follow exit.
 */
static void handle_sched_process_exit(void *data, struct task_struct *p,
				      bool group_dead)
{
	tlob_stop_task(p, NULL);
	tlob_destroy_task(p->pid);
}

/*
 * tlob_start_task - begin monitoring @task: fresh record, or restart in
 * place when the closed record's binding matches.
 */
static int tlob_start_task(struct task_struct *task, u64 threshold_ns,
			   struct tlob_uprobe_binding *binding)
{
	struct tlob_record *rec = NULL;
	char *stopped_name = model_get_state_name(stopped_tlob);
	char *start_name = model_get_event_name(start_tlob);
	bool do_destroy = false;

	if (!da_monitor_enabled())
		return -ENODEV;

	if (threshold_ns < TLOB_MIN_THRESHOLD_NS ||
	    threshold_ns > TLOB_MAX_THRESHOLD_NS)
		return -ERANGE;

	/*
	 * Serialise duplicate-check + pool-slot claim.  da_destroy_storage()
	 * nests rcu_read_lock(), sleepable on PREEMPT_RT, so it is deferred
	 * past the spinlock; TLOB_RETIRED makes rec unreachable after unlock.
	 */
	scoped_guard(spinlock, &tlob_binding_lock) {
		scoped_guard(rcu) {
			rec = da_get_target_by_id(task->pid);
			if (rec) {
				if (atomic_read(&rec->state) == TLOB_OPEN)
					return -EALREADY;
				if (atomic_read(&rec->state) == TLOB_RETIRED)
					return -ESRCH;
				/*
				 * Between windows under a different binding:
				 * not relinked; emit the shadowing.
				 */
				if (rec->binding != binding) {
					trace_error_tlob(task->pid,
							 stopped_name,
							 start_name);
					return -EALREADY;
				}

				/*
				 * Restart in place.  A teardown may claim the
				 * record after this read; the reopen below then
				 * fails and the window stays closed.
				 */
				if (atomic_read(&rec->state) != TLOB_CLOSED)
					return -ESRCH;
				rec->threshold_ns = threshold_ns;
				/* ha_setup_invariants() resets and reopens. */
				if (!da_handle_start_run_event(task->pid,
							       rec, start_tlob))
					return -ENODEV;
				return 0;
			}
		}

		rec = tlob_rec_alloc();
		if (!rec)
			return -ENOSPC;

		rec->id = task->pid;
		rec->threshold_ns = threshold_ns;
		rec->binding = binding;

		/*
		 * Every started_list walker holds tlob_binding_lock, so linking
		 * after the hash publishes rec is not observable.
		 */
		if (da_handle_start_run_event(task->pid, rec, start_tlob)) {
			list_add_tail(&rec->started_node,
				      &binding->started_list);
		} else {
			/* Unlinked; claim it for the free. */
			atomic_set(&rec->state, TLOB_RETIRED);
			do_destroy = true;
		}
	}

	if (do_destroy) {
		/* Lock dropped; rcu_read_lock() nesting is now safe. */
		da_destroy_storage(task->pid);
		/*
		 * da_prepare_storage() publishes before the enabled check that
		 * failed above, so a reader may hold rec: defer the return.
		 */
		call_rcu(&rec->rcu, tlob_rec_return_cb);
		return -ENOSPC;
	}

	return 0;
}

/*
 * tlob_stop_task - end @task's window; the record stays closed for a later
 * restart, tlob_destroy_task() frees it.
 */
static void tlob_stop_task(struct task_struct *task,
			   struct tlob_uprobe_binding *binding)
{
	struct ha_monitor *ha_mon;
	struct tlob_record *rec;

	scoped_guard(rcu) {
		ha_mon = ha_get_monitor(task->pid, NULL);
		if (!ha_mon)
			return;

		rec = ha_get_target(ha_mon);
		if (WARN_ON_ONCE(!rec))
			return;

		/* Only the opener may end it; NULL (task exit) skips. */
		if (binding && READ_ONCE(rec->binding) != binding)
			return;

		/* Claim the window end; the timer may have taken it first. */
		if (atomic_cmpxchg_release(&rec->state, TLOB_OPEN,
					   TLOB_CLOSED) != TLOB_OPEN)
			return;

		ha_cancel_timer_sync(ha_mon);
	}

	/* The DA returns to stopped; the record stays closed. */
	da_handle_event(task->pid, NULL, stop_tlob);
}

/*
 * tlob_destroy_claimed - teardown with TLOB_RETIRED already claimed; the claim
 * is what makes the hash removal safe.
 */
static void tlob_destroy_claimed(struct tlob_record *rec)
{
	pid_t id = rec->id;

	tlob_detach_from_binding(rec);

	scoped_guard(rcu) {
		struct ha_monitor *ha_mon = ha_get_monitor(id, NULL);

		if (!WARN_ON_ONCE(!ha_mon))
			da_monitor_reset(&ha_mon->da_mon);
	}
	da_destroy_storage(id);

	/* Readers may still hold rec; the pool slot returns after a GP. */
	call_rcu(&rec->rcu, tlob_rec_return_cb);
}

static void tlob_destroy_task(pid_t id)
{
	struct ha_monitor *ha_mon;
	struct tlob_record *rec;

	scoped_guard(rcu) {
		ha_mon = ha_get_monitor(id, NULL);
		if (!ha_mon)
			return;
		rec = ha_get_target(ha_mon);
		if (WARN_ON_ONCE(!rec))
			return;
		if (atomic_xchg(&rec->state, TLOB_RETIRED) == TLOB_RETIRED)
			return;
	}

	tlob_destroy_claimed(rec);
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

static void tlob_unbind_quiesce(struct tlob_uprobe_binding *b);
static void tlob_unbind_reap(struct tlob_uprobe_binding *b);

/*
 * tlob_unbind_teardown - unregister @b's probes, reap the windows they opened
 * and drop @b's inode/path references.  @b must already be off
 * tlob_uprobe_list; the caller frees it.
 */
static void tlob_unbind_teardown(struct tlob_uprobe_binding *b)
{
	rv_uprobe_unregister_nosync(&b->start_probe);
	rv_uprobe_unregister_nosync(&b->stop_probe);

	tlob_unbind_quiesce(b);
	rv_uprobe_unregister_sync();
	tlob_unbind_reap(b);

	iput(b->start_probe.inode);
	iput(b->stop_probe.inode);
	path_put(&b->path);
}

static int tlob_add_uprobe(const char *name, const char *binpath,
			   loff_t offset_start, loff_t offset_stop,
			   u64 threshold_ns)
{
	struct tlob_uprobe_binding *tmp_b;
	char pathbuf[TLOB_MAX_PATH];
	struct inode *inode;
	struct path path __free(path_put) = {};
	char *canon;
	int ret;

	guard(mutex)(&tlob_uprobe_mutex);

	if (binpath[0] != '/')
		return -EINVAL;

	struct tlob_uprobe_binding *b __free(kfree) =
		kzalloc_obj(*b, GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	strscpy(b->name, name, sizeof(b->name));
	b->threshold_ns = threshold_ns;
	b->offset_start = offset_start;
	b->offset_stop  = offset_stop;
	INIT_LIST_HEAD(&b->started_list);
	INIT_LIST_HEAD(&b->quiesced_list);

	ret = kern_path(binpath, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;

	if (!d_is_reg(path.dentry))
		return -EINVAL;

	inode = d_real_inode(path.dentry);

	/*
	 * Names are unique, and so are ranges on the same binary: a task
	 * runs one window at a time, the second would silently shadow the
	 * first.  Direction normalised so a reversed start/stop still
	 * matches.
	 */
	list_for_each_entry(tmp_b, &tlob_uprobe_list, list) {
		loff_t s1, e1, s2, e2;

		if (strcmp(tmp_b->name, name) == 0)
			return -EEXIST;
		if (!rv_uprobe_is_registered(&tmp_b->start_probe))
			continue;
		if (tmp_b->start_probe.inode != inode)
			continue;

		s1 = min(offset_start, offset_stop);
		e1 = max(offset_start, offset_stop);
		s2 = min(tmp_b->offset_start, tmp_b->offset_stop);
		e2 = max(tmp_b->offset_start, tmp_b->offset_stop);
		if (s1 <= e2 && s2 <= e1)
			return -EEXIST;
	}

	canon = d_path(&path, pathbuf, sizeof(pathbuf));
	if (IS_ERR(canon))
		return PTR_ERR(canon);
	strscpy(b->binpath, canon, sizeof(b->binpath));

	b->start_probe.uc.handler = tlob_uprobe_entry_handler;
	ret = rv_uprobe_register(inode, offset_start, &b->start_probe);
	if (ret)
		return ret;

	b->stop_probe.uc.handler = tlob_uprobe_stop_handler;
	/* Both probes pin the one inode resolved above: same binary. */
	ret = rv_uprobe_register(inode, offset_stop, &b->stop_probe);
	if (ret) {
		/*
		 * The start probe was live: tear down the windows it opened
		 * before @b goes away, in the same order as the remove path.
		 */
		tlob_unbind_teardown(b);
		return ret;
	}

	b->path = path;
	path_get(&b->path);

	list_add_tail(&no_free_ptr(b)->list, &tlob_uprobe_list);
	return 0;
}

/*
 * tlob_unbind_quiesce - claim @b's windows with TLOB_RETIRED and move them to
 * @b->quiesced_list.  The claim keeps the record from restarting over the
 * uprobe barrier: a straggler START fails with -ESRCH, no timer is re-armed
 * (the record is no longer OPEN), and timer expiry finds the window already
 * closed.
 */
static void tlob_unbind_quiesce(struct tlob_uprobe_binding *b)
{
	struct tlob_record *rec, *tmp;

	scoped_guard(spinlock, &tlob_binding_lock) {
		list_for_each_entry_safe(rec, tmp, &b->started_list,
					 started_node) {
			list_del_init(&rec->started_node);
			rec->binding = NULL;
			/* Already claimed: the exit path owns the free. */
			if (atomic_xchg(&rec->state, TLOB_RETIRED) == TLOB_RETIRED)
				continue;
			list_add_tail(&rec->started_node, &b->quiesced_list);
		}
	}
}

/*
 * tlob_unbind_reap - destroy @b's windows after the uprobe barrier.  Quiescing
 * again claims whatever an in-flight handler started past the first pass; the
 * TLOB_RETIRED claim is what keeps every record here alive until it is destroyed.
 */
static void tlob_unbind_reap(struct tlob_uprobe_binding *b)
{
	struct tlob_record *rec, *tmp;
	LIST_HEAD(claimed);

	tlob_unbind_quiesce(b);

	scoped_guard(spinlock, &tlob_binding_lock)
		list_splice_tail_init(&b->quiesced_list, &claimed);

	list_for_each_entry_safe(rec, tmp, &claimed, started_node) {
		list_del_init(&rec->started_node);
		tlob_destroy_claimed(rec);
	}
}

static int tlob_remove_uprobe_by_name(const char *name)
{
	struct tlob_uprobe_binding *b, *tmp;

	guard(mutex)(&tlob_uprobe_mutex);

	list_for_each_entry_safe(b, tmp, &tlob_uprobe_list, list) {
		if (strcmp(b->name, name))
			continue;
		list_del(&b->list);
		/* Sync may sleep; list_del() already hid the binding. */
		tlob_unbind_teardown(b);
		kfree(b);
		return 0;
	}

	return -ENOENT;
}

static void tlob_remove_all_uprobes(void)
{
	struct tlob_uprobe_binding *b, *tmp;
	LIST_HEAD(pending);

	/*
	 * Held across the sync and the reap: dropping it here would let a
	 * concurrent disable tear the pools down under the reap.
	 */
	guard(mutex)(&tlob_uprobe_mutex);

	list_for_each_entry_safe(b, tmp, &tlob_uprobe_list, list) {
		list_move(&b->list, &pending);
		rv_uprobe_unregister_nosync(&b->start_probe);
		rv_uprobe_unregister_nosync(&b->stop_probe);
		tlob_unbind_quiesce(b);
	}

	if (list_empty(&pending))
		return;

	/* One sync covers all dequeued probes. */
	rv_uprobe_unregister_sync();

	list_for_each_entry_safe(b, tmp, &pending, list) {
		list_del(&b->list);
		tlob_unbind_reap(b);
		iput(b->start_probe.inode);
		iput(b->stop_probe.inode);
		path_put(&b->path);
		kfree(b);
	}
}

static void *tlob_seq_start(struct seq_file *m, loff_t *pos)
{
	mutex_lock(&tlob_uprobe_mutex);
	return seq_list_start(&tlob_uprobe_list, *pos);
}

static void *tlob_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	return seq_list_next(v, &tlob_uprobe_list, pos);
}

static void tlob_seq_stop(struct seq_file *m, void *v)
{
	mutex_unlock(&tlob_uprobe_mutex);
}

static int tlob_seq_show(struct seq_file *m, void *v)
{
	struct tlob_uprobe_binding *b =
		list_entry(v, struct tlob_uprobe_binding, list);

	seq_printf(m, "p:%s %s:0x%llx 0x%llx threshold=%llu\n",
		   b->name, b->binpath,
		   (unsigned long long)b->offset_start,
		   (unsigned long long)b->offset_stop,
		   b->threshold_ns);
	return 0;
}

static const struct seq_operations tlob_monitor_seq_ops = {
	.start	= tlob_seq_start,
	.next	= tlob_seq_next,
	.stop	= tlob_seq_stop,
	.show	= tlob_seq_show,
};

/*
 * Parse "p:NAME PATH:OFFSET_START OFFSET_STOP threshold=NS"; @buf is modified
 * and the outputs point into it.
 *
 * PATH may contain ':', so the last one separates it from OFFSET_START.  The
 * offsets are roles, not a range: STOP may sit at a lower offset than START.
 */
static int tlob_parse_uprobe_line(char *buf, char **name_out, char **path_out,
				  loff_t *start_out, loff_t *stop_out,
				  u64 *thr_out)
{
	unsigned long long thr = 0, stop_val = 0;
	long long start_val;
	char *p, *name, *path_token, *token, *colon;
	bool got_stop = false, got_thr = false;

	if (buf[0] != 'p' || buf[1] != ':')
		return -EINVAL;

	p = buf + 2;
	while (*p == ' ' || *p == '\t')
		p++;

	name = strsep(&p, " \t");
	if (strlen(name) >= TLOB_MAX_NAME || !is_good_name(name))
		return -EINVAL;

	if (!p)
		return -EINVAL;
	while (*p == ' ' || *p == '\t')
		p++;
	path_token = strsep(&p, " \t");
	if (!path_token || !*path_token)
		return -EINVAL;

	colon = strrchr(path_token, ':');
	if (!colon || colon - path_token < 2)
		return -EINVAL;
	*colon = '\0';

	if (path_token[0] != '/')
		return -EINVAL;

	if (kstrtoll(colon + 1, 0, &start_val))
		return -EINVAL;
	if (start_val < 0)
		return -EINVAL;

	while (p && (token = strsep(&p, " \t")) != NULL) {
		if (!*token)
			continue;
		if (strncmp(token, "threshold=", 10) == 0) {
			if (got_thr)
				return -EINVAL;
			if (kstrtoull(token + 10, 0, &thr))
				return -EINVAL;
			if (thr < TLOB_MIN_THRESHOLD_NS ||
			    thr > TLOB_MAX_THRESHOLD_NS)
				return -ERANGE;
			got_thr = true;
		} else if (!got_stop) {
			long long sv;

			if (kstrtoll(token, 0, &sv))
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

	*name_out  = name;
	*thr_out   = thr;
	*path_out  = path_token;
	*start_out = (loff_t)start_val;
	*stop_out  = (loff_t)stop_val;
	return 0;
}

/*
 * Parse "-:NAME" (ftrace uprobe_events removal convention).
 */
static int tlob_parse_remove_line(char *buf, char **name_out)
{
	char *name = buf + 2;

	if (buf[0] != '-' || buf[1] != ':')
		return -EINVAL;
	if (strlen(name) >= TLOB_MAX_NAME || !is_good_name(name))
		return -EINVAL;

	*name_out = name;
	return 0;
}

#if IS_ENABLED(CONFIG_RV_MONITORS_KUNIT_TEST)
#include <kunit/visibility.h>
#include "tlob_kunit.h"

const struct rv_tlob_kunit_ops rv_tlob_kunit_ops = {
	.parse_uprobe_line = tlob_parse_uprobe_line,
	.parse_remove_line = tlob_parse_remove_line,
};
EXPORT_SYMBOL_IF_KUNIT(rv_tlob_kunit_ops);
#endif

static int tlob_create_or_delete_uprobe(char *buf)
{
	loff_t offset_start, offset_stop;
	u64 threshold_ns;
	char *name, *binpath;
	int ret;

	if (buf[0] == '-' && buf[1] == ':') {
		ret = tlob_parse_remove_line(buf, &name);
		if (ret)
			return ret;
		return tlob_remove_uprobe_by_name(name);
	}
	ret = tlob_parse_uprobe_line(buf, &name, &binpath, &offset_start,
				     &offset_stop, &threshold_ns);
	if (ret)
		return ret;
	return tlob_add_uprobe(name, binpath, offset_start, offset_stop,
			       threshold_ns);
}

static ssize_t tlob_monitor_write(struct file *file,
				  const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	char buf[TLOB_MAX_PATH + TLOB_MAX_NAME + 128];

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (count > 0 && buf[count - 1] == '\n')
		buf[count - 1] = '\0';
	/* Empty line: keep 'echo > monitor' working after the O_TRUNC clear. */
	if (!buf[0])
		return count;
	return tlob_create_or_delete_uprobe(buf) ?: (ssize_t)count;
}

/*
 * ftrace dynamic-event convention: opening for write with O_TRUNC ('>'
 * redirection) clears all bindings; individual commands, including
 * removals, are appended with '>>'.
 */
static int tlob_monitor_open(struct inode *inode, struct file *file)
{
	int ret;

	ret = security_locked_down(LOCKDOWN_TRACEFS);
	if (ret)
		return ret;

	if ((file->f_mode & FMODE_WRITE) && (file->f_flags & O_TRUNC))
		tlob_remove_all_uprobes();

	return seq_open(file, &tlob_monitor_seq_ops);
}

static const struct file_operations tlob_monitor_fops = {
	.open		= tlob_monitor_open,
	.read		= seq_read,
	.write		= tlob_monitor_write,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int __tlob_init_monitor(void)
{
	int retval;

	retval = mempool_init_kmalloc_pool(&tlob_rec_pool, TLOB_MAX_MONITORED,
					   sizeof(struct tlob_record));
	if (retval)
		return retval;

	retval = ha_monitor_init();
	if (retval) {
		mempool_exit(&tlob_rec_pool);
		return retval;
	}

	return 0;
}

static void __tlob_destroy_monitor(void)
{
	rv_this.enabled = 0;
	tlob_remove_all_uprobes();
	/*
	 * A grace period only makes the call_rcu()'d tlob_rec_return_cb()
	 * callbacks eligible to run; rcu_barrier() waits until they have all
	 * returned their slots before the pool is destroyed.
	 */
	ha_monitor_destroy();
	rcu_barrier();
	mempool_exit(&tlob_rec_pool);
}

static int tlob_enable_hooks(void)
{
	rv_attach_trace_probe("tlob", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("tlob", sched_wakeup, handle_sched_wakeup);
	rv_attach_trace_probe("tlob", sched_process_exit,
			      handle_sched_process_exit);
	return 0;
}

static void tlob_disable_hooks(void)
{
	rv_detach_trace_probe("tlob", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("tlob", sched_wakeup, handle_sched_wakeup);
	rv_detach_trace_probe("tlob", sched_process_exit,
			      handle_sched_process_exit);
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
		if (!rv_create_file("monitor", RV_MODE_WRITE,
				    rv_this.root_d, NULL,
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
