// SPDX-License-Identifier: GPL-2.0
/*
 * tlob: task latency over budget monitor
 *
 * Track the elapsed wall-clock time of a marked code path and detect when
 * a monitored task exceeds its per-task latency budget.  CLOCK_MONOTONIC
 * is used so both on-CPU and off-CPU time count toward the budget.
 *
 * Per-task state is maintained in a spinlock-protected hash table.  A
 * one-shot hrtimer fires at the deadline; if the task has not called
 * trace_stop by then, a violation is recorded.
 *
 * Up to TLOB_MAX_MONITORED tasks may be tracked simultaneously.
 *
 * Copyright (C) 2026 Wen Yang <wen.yang@linux.dev>
 */
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/ftrace.h>
#include <linux/hash.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/poll.h>
#include <linux/rv.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/tracefs.h>
#include <linux/uaccess.h>
#include <linux/uprobes.h>
#include <kunit/visibility.h>
#include <rv/instrumentation.h>

/* rv_interface_lock is defined in kernel/trace/rv/rv.c */
extern struct mutex rv_interface_lock;

#define MODULE_NAME "tlob"

#include <rv_trace.h>
#include <trace/events/sched.h>

#define RV_MON_TYPE RV_MON_PER_TASK
#include "tlob.h"
#include <rv/da_monitor.h>

/* Hash table size; must be a power of two. */
#define TLOB_HTABLE_BITS		6
#define TLOB_HTABLE_SIZE		(1 << TLOB_HTABLE_BITS)

/* Maximum binary path length for uprobe binding. */
#define TLOB_MAX_PATH			256

/* Per-task latency monitoring state. */
struct tlob_task_state {
	struct hlist_node	hlist;
	struct task_struct	*task;
	u64			threshold_us;
	u64			tag;
	struct hrtimer		deadline_timer;
	int			canceled;	/* protected by entry_lock */
	struct file		*notify_file;	/* NULL or held reference */

	/*
	 * entry_lock serialises the mutable accounting fields below.
	 * Lock order: tlob_table_lock -> entry_lock (never reverse).
	 */
	raw_spinlock_t		entry_lock;
	u64			on_cpu_us;
	u64			off_cpu_us;
	ktime_t			last_ts;
	u32			switches;
	u8			da_state;

	struct rcu_head		rcu;	/* for call_rcu() teardown */
};

/* Per-uprobe-binding state: a start + stop probe pair for one binary region. */
struct tlob_uprobe_binding {
	struct list_head	list;
	u64			threshold_us;
	struct path		path;
	char			binpath[TLOB_MAX_PATH];	/* canonical path for read/remove */
	loff_t			offset_start;
	loff_t			offset_stop;
	struct uprobe_consumer	entry_uc;
	struct uprobe_consumer	stop_uc;
	struct uprobe		*entry_uprobe;
	struct uprobe		*stop_uprobe;
};

/* Object pool for tlob_task_state. */
static struct kmem_cache *tlob_state_cache;

/* Hash table and lock protecting table structure (insert/delete/canceled). */
static struct hlist_head tlob_htable[TLOB_HTABLE_SIZE];
static DEFINE_RAW_SPINLOCK(tlob_table_lock);
static atomic_t tlob_num_monitored = ATOMIC_INIT(0);

/* Uprobe binding list; protected by tlob_uprobe_mutex. */
static LIST_HEAD(tlob_uprobe_list);
static DEFINE_MUTEX(tlob_uprobe_mutex);

/* Forward declaration */
static enum hrtimer_restart tlob_deadline_timer_fn(struct hrtimer *timer);

/* Hash table helpers */

static unsigned int tlob_hash_task(const struct task_struct *task)
{
	return hash_ptr((void *)task, TLOB_HTABLE_BITS);
}

/*
 * tlob_find_rcu - look up per-task state.
 * Must be called under rcu_read_lock() or with tlob_table_lock held.
 */
static struct tlob_task_state *tlob_find_rcu(struct task_struct *task)
{
	struct tlob_task_state *ws;
	unsigned int h = tlob_hash_task(task);

	hlist_for_each_entry_rcu(ws, &tlob_htable[h], hlist,
				 lockdep_is_held(&tlob_table_lock))
		if (ws->task == task)
			return ws;
	return NULL;
}

/* Allocate and initialise a new per-task state entry. */
static struct tlob_task_state *tlob_alloc(struct task_struct *task,
					  u64 threshold_us, u64 tag)
{
	struct tlob_task_state *ws;

	ws = kmem_cache_zalloc(tlob_state_cache, GFP_ATOMIC);
	if (!ws)
		return NULL;

	ws->task = task;
	get_task_struct(task);
	ws->threshold_us = threshold_us;
	ws->tag = tag;
	ws->last_ts = ktime_get();
	ws->da_state = on_cpu_tlob;
	raw_spin_lock_init(&ws->entry_lock);
	hrtimer_setup(&ws->deadline_timer, tlob_deadline_timer_fn,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	return ws;
}

/* RCU callback: free the slab once no readers remain. */
static void tlob_free_rcu_slab(struct rcu_head *head)
{
	struct tlob_task_state *ws =
		container_of(head, struct tlob_task_state, rcu);
	kmem_cache_free(tlob_state_cache, ws);
}

/* Arm the one-shot deadline timer for threshold_us microseconds. */
static void tlob_arm_deadline(struct tlob_task_state *ws)
{
	hrtimer_start(&ws->deadline_timer,
		      ns_to_ktime(ws->threshold_us * NSEC_PER_USEC),
		      HRTIMER_MODE_REL);
}

/*
 * Push a violation record into a monitor fd's ring buffer (softirq context).
 * Drop-new policy: discard incoming record when full.  smp_store_release on
 * data_head pairs with smp_load_acquire in the consumer.
 */
static void tlob_event_push(struct rv_file_priv *priv,
			    const struct tlob_event *info)
{
	struct tlob_ring *ring = &priv->ring;
	unsigned long flags;
	u32 head, tail;

	spin_lock_irqsave(&ring->lock, flags);

	head = ring->page->data_head;
	tail = READ_ONCE(ring->page->data_tail);

	if (head - tail > ring->mask) {
		/* Ring full: drop incoming record. */
		ring->page->dropped++;
		spin_unlock_irqrestore(&ring->lock, flags);
		return;
	}

	ring->data[head & ring->mask] = *info;
	/* pairs with smp_load_acquire() in the consumer */
	smp_store_release(&ring->page->data_head, head + 1);

	spin_unlock_irqrestore(&ring->lock, flags);

	wake_up_interruptible_poll(&priv->waitq, EPOLLIN | EPOLLRDNORM);
}

#if IS_ENABLED(CONFIG_KUNIT)
void tlob_event_push_kunit(struct rv_file_priv *priv,
			  const struct tlob_event *info)
{
	tlob_event_push(priv, info);
}
EXPORT_SYMBOL_IF_KUNIT(tlob_event_push_kunit);
#endif /* CONFIG_KUNIT */

/*
 * Budget exceeded: remove the entry, record the violation, and inject
 * budget_expired into the DA.
 *
 * Lock order: tlob_table_lock -> entry_lock.  tlob_stop_task() sets
 * ws->canceled under both locks; if we see it here the stop path owns cleanup.
 * fput/put_task_struct are done before call_rcu(); the RCU callback only
 * reclaims the slab.
 */
static enum hrtimer_restart tlob_deadline_timer_fn(struct hrtimer *timer)
{
	struct tlob_task_state *ws =
		container_of(timer, struct tlob_task_state, deadline_timer);
	struct tlob_event info = {};
	struct file *notify_file;
	struct task_struct *task;
	unsigned long flags;
	/* snapshots taken under entry_lock */
	u64 on_cpu_us, off_cpu_us, threshold_us, tag;
	u32 switches;
	bool on_cpu;
	bool push_event = false;

	raw_spin_lock_irqsave(&tlob_table_lock, flags);
	/* stop path sets canceled under both locks; if set it owns cleanup */
	if (ws->canceled) {
		raw_spin_unlock_irqrestore(&tlob_table_lock, flags);
		return HRTIMER_NORESTART;
	}

	/* Finalize accounting and snapshot all fields under entry_lock. */
	raw_spin_lock(&ws->entry_lock);

	{
		ktime_t now = ktime_get();
		u64 delta_us = ktime_to_us(ktime_sub(now, ws->last_ts));

		if (ws->da_state == on_cpu_tlob)
			ws->on_cpu_us += delta_us;
		else
			ws->off_cpu_us += delta_us;
	}

	ws->canceled  = 1;
	on_cpu_us     = ws->on_cpu_us;
	off_cpu_us    = ws->off_cpu_us;
	threshold_us  = ws->threshold_us;
	tag           = ws->tag;
	switches      = ws->switches;
	on_cpu        = (ws->da_state == on_cpu_tlob);
	notify_file   = ws->notify_file;
	if (notify_file) {
		info.tid          = task_pid_vnr(ws->task);
		info.threshold_us = threshold_us;
		info.on_cpu_us    = on_cpu_us;
		info.off_cpu_us   = off_cpu_us;
		info.switches     = switches;
		info.state        = on_cpu ? 1 : 0;
		info.tag          = tag;
		push_event        = true;
	}

	raw_spin_unlock(&ws->entry_lock);

	hlist_del_rcu(&ws->hlist);
	atomic_dec(&tlob_num_monitored);
	/*
	 * Hold a reference so task remains valid across da_handle_event()
	 * after we drop tlob_table_lock.
	 */
	task = ws->task;
	get_task_struct(task);
	raw_spin_unlock_irqrestore(&tlob_table_lock, flags);

	/*
	 * Both locks are now released; ws is exclusively owned (removed from
	 * the hash table with canceled=1).  Emit the tracepoint and push the
	 * violation record.
	 */
	trace_tlob_budget_exceeded(ws->task, threshold_us, on_cpu_us,
				   off_cpu_us, switches, on_cpu, tag);

	if (push_event) {
		struct rv_file_priv *priv = notify_file->private_data;

		if (priv)
			tlob_event_push(priv, &info);
	}

	da_handle_event(task, budget_expired_tlob);

	if (notify_file)
		fput(notify_file);		/* ref from fget() at TRACE_START */
	put_task_struct(ws->task);		/* ref from tlob_alloc() */
	put_task_struct(task);			/* extra ref from get_task_struct() above */
	call_rcu(&ws->rcu, tlob_free_rcu_slab);
	return HRTIMER_NORESTART;
}

/* Tracepoint handlers */

/*
 * handle_sched_switch - advance the DA and accumulate on/off-CPU time.
 *
 * RCU read-side for lock-free lookup; entry_lock for per-task accounting.
 * da_handle_event() is called after rcu_read_unlock() to avoid holding the
 * read-side critical section across the RV framework.
 */
static void handle_sched_switch(void *data, bool preempt,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned int prev_state)
{
	struct tlob_task_state *ws;
	unsigned long flags;
	bool do_prev = false, do_next = false;
	ktime_t now;

	rcu_read_lock();

	ws = tlob_find_rcu(prev);
	if (ws) {
		raw_spin_lock_irqsave(&ws->entry_lock, flags);
		if (!ws->canceled) {
			now = ktime_get();
			ws->on_cpu_us += ktime_to_us(ktime_sub(now, ws->last_ts));
			ws->last_ts = now;
			ws->switches++;
			ws->da_state = off_cpu_tlob;
			do_prev = true;
		}
		raw_spin_unlock_irqrestore(&ws->entry_lock, flags);
	}

	ws = tlob_find_rcu(next);
	if (ws) {
		raw_spin_lock_irqsave(&ws->entry_lock, flags);
		if (!ws->canceled) {
			now = ktime_get();
			ws->off_cpu_us += ktime_to_us(ktime_sub(now, ws->last_ts));
			ws->last_ts = now;
			ws->da_state = on_cpu_tlob;
			do_next = true;
		}
		raw_spin_unlock_irqrestore(&ws->entry_lock, flags);
	}

	rcu_read_unlock();

	if (do_prev)
		da_handle_event(prev, switch_out_tlob);
	if (do_next)
		da_handle_event(next, switch_in_tlob);
}

static void handle_sched_wakeup(void *data, struct task_struct *p)
{
	struct tlob_task_state *ws;
	unsigned long flags;
	bool found = false;

	rcu_read_lock();
	ws = tlob_find_rcu(p);
	if (ws) {
		raw_spin_lock_irqsave(&ws->entry_lock, flags);
		found = !ws->canceled;
		raw_spin_unlock_irqrestore(&ws->entry_lock, flags);
	}
	rcu_read_unlock();

	if (found)
		da_handle_event(p, sched_wakeup_tlob);
}

/* -----------------------------------------------------------------------
 * Core start/stop helpers (also called from rv_dev.c)
 * -----------------------------------------------------------------------
 */

/*
 * __tlob_insert - insert @ws into the hash table and arm its deadline timer.
 *
 * Re-checks for duplicates and capacity under tlob_table_lock; the caller
 * may have done a lock-free pre-check before allocating @ws.  On failure @ws
 * is freed directly (never in table, so no call_rcu needed).
 */
static int __tlob_insert(struct task_struct *task, struct tlob_task_state *ws)
{
	unsigned int h;
	unsigned long flags;

	raw_spin_lock_irqsave(&tlob_table_lock, flags);
	if (tlob_find_rcu(task)) {
		raw_spin_unlock_irqrestore(&tlob_table_lock, flags);
		if (ws->notify_file)
			fput(ws->notify_file);
		put_task_struct(ws->task);
		kmem_cache_free(tlob_state_cache, ws);
		return -EEXIST;
	}
	if (atomic_read(&tlob_num_monitored) >= TLOB_MAX_MONITORED) {
		raw_spin_unlock_irqrestore(&tlob_table_lock, flags);
		if (ws->notify_file)
			fput(ws->notify_file);
		put_task_struct(ws->task);
		kmem_cache_free(tlob_state_cache, ws);
		return -ENOSPC;
	}
	h = tlob_hash_task(task);
	hlist_add_head_rcu(&ws->hlist, &tlob_htable[h]);
	atomic_inc(&tlob_num_monitored);
	raw_spin_unlock_irqrestore(&tlob_table_lock, flags);

	da_handle_start_run_event(task, trace_start_tlob);
	tlob_arm_deadline(ws);
	return 0;
}

/**
 * tlob_start_task - begin monitoring @task with latency budget @threshold_us.
 *
 * @notify_file: /dev/rv fd whose ring buffer receives a tlob_event on
 *               violation; caller transfers the fget() reference to tlob.c.
 *               Pass NULL for synchronous mode (violations only via
 *               TRACE_STOP return value and the tlob_budget_exceeded event).
 *
 * Returns 0, -ENODEV, -EEXIST, -ENOSPC, or -ENOMEM.  On failure the caller
 * retains responsibility for any @notify_file reference.
 */
int tlob_start_task(struct task_struct *task, u64 threshold_us,
		    struct file *notify_file, u64 tag)
{
	struct tlob_task_state *ws;
	unsigned long flags;

	if (!tlob_state_cache)
		return -ENODEV;

	if (threshold_us > (u64)KTIME_MAX / NSEC_PER_USEC)
		return -ERANGE;

	/* Quick pre-check before allocation. */
	raw_spin_lock_irqsave(&tlob_table_lock, flags);
	if (tlob_find_rcu(task)) {
		raw_spin_unlock_irqrestore(&tlob_table_lock, flags);
		return -EEXIST;
	}
	if (atomic_read(&tlob_num_monitored) >= TLOB_MAX_MONITORED) {
		raw_spin_unlock_irqrestore(&tlob_table_lock, flags);
		return -ENOSPC;
	}
	raw_spin_unlock_irqrestore(&tlob_table_lock, flags);

	ws = tlob_alloc(task, threshold_us, tag);
	if (!ws)
		return -ENOMEM;

	ws->notify_file = notify_file;
	return __tlob_insert(task, ws);
}
EXPORT_SYMBOL_GPL(tlob_start_task);

/**
 * tlob_stop_task - stop monitoring @task before the deadline fires.
 *
 * Sets canceled under entry_lock (inside tlob_table_lock) before calling
 * hrtimer_cancel(), racing safely with the timer callback.
 *
 * Returns 0 if within budget, -ESRCH if the entry is gone (deadline already
 * fired, or TRACE_START was never called).
 */
int tlob_stop_task(struct task_struct *task)
{
	struct tlob_task_state *ws;
	struct file *notify_file;
	unsigned long flags;

	raw_spin_lock_irqsave(&tlob_table_lock, flags);
	ws = tlob_find_rcu(task);
	if (!ws) {
		raw_spin_unlock_irqrestore(&tlob_table_lock, flags);
		return -ESRCH;
	}

	/* Prevent handle_sched_switch from updating accounting after removal. */
	raw_spin_lock(&ws->entry_lock);
	ws->canceled = 1;
	raw_spin_unlock(&ws->entry_lock);

	hlist_del_rcu(&ws->hlist);
	atomic_dec(&tlob_num_monitored);
	raw_spin_unlock_irqrestore(&tlob_table_lock, flags);

	hrtimer_cancel(&ws->deadline_timer);

	da_handle_event(task, trace_stop_tlob);

	notify_file = ws->notify_file;
	if (notify_file)
		fput(notify_file);
	put_task_struct(ws->task);
	call_rcu(&ws->rcu, tlob_free_rcu_slab);

	return 0;
}
EXPORT_SYMBOL_GPL(tlob_stop_task);

/* Stop monitoring all tracked tasks; called on monitor disable. */
static void tlob_stop_all(void)
{
	struct tlob_task_state *batch[TLOB_MAX_MONITORED];
	struct tlob_task_state *ws;
	struct hlist_node *tmp;
	unsigned long flags;
	int n = 0, i;

	raw_spin_lock_irqsave(&tlob_table_lock, flags);
	for (i = 0; i < TLOB_HTABLE_SIZE; i++) {
		hlist_for_each_entry_safe(ws, tmp, &tlob_htable[i], hlist) {
			raw_spin_lock(&ws->entry_lock);
			ws->canceled = 1;
			raw_spin_unlock(&ws->entry_lock);
			hlist_del_rcu(&ws->hlist);
			atomic_dec(&tlob_num_monitored);
			if (n < TLOB_MAX_MONITORED)
				batch[n++] = ws;
		}
	}
	raw_spin_unlock_irqrestore(&tlob_table_lock, flags);

	for (i = 0; i < n; i++) {
		ws = batch[i];
		hrtimer_cancel(&ws->deadline_timer);
		da_handle_event(ws->task, trace_stop_tlob);
		if (ws->notify_file)
			fput(ws->notify_file);
		put_task_struct(ws->task);
		call_rcu(&ws->rcu, tlob_free_rcu_slab);
	}
}

/* uprobe binding helpers */

static int tlob_uprobe_entry_handler(struct uprobe_consumer *uc,
				     struct pt_regs *regs, __u64 *data)
{
	struct tlob_uprobe_binding *b =
		container_of(uc, struct tlob_uprobe_binding, entry_uc);

	tlob_start_task(current, b->threshold_us, NULL, (u64)b->offset_start);
	return 0;
}

static int tlob_uprobe_stop_handler(struct uprobe_consumer *uc,
				    struct pt_regs *regs, __u64 *data)
{
	tlob_stop_task(current);
	return 0;
}

/*
 * Register start + stop entry uprobes for a binding.
 * Both are plain entry uprobes (no uretprobe), so a wrong offset never
 * corrupts the call stack; the worst outcome is a missed stop (hrtimer
 * fires and reports a budget violation).
 * Called with tlob_uprobe_mutex held.
 */
static int tlob_add_uprobe(u64 threshold_us, const char *binpath,
			   loff_t offset_start, loff_t offset_stop)
{
	struct tlob_uprobe_binding *b, *tmp_b;
	char pathbuf[TLOB_MAX_PATH];
	struct inode *inode;
	char *canon;
	int ret;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	if (binpath[0] != '/') {
		kfree(b);
		return -EINVAL;
	}

	b->threshold_us = threshold_us;
	b->offset_start = offset_start;
	b->offset_stop  = offset_stop;

	ret = kern_path(binpath, LOOKUP_FOLLOW, &b->path);
	if (ret)
		goto err_free;

	if (!d_is_reg(b->path.dentry)) {
		ret = -EINVAL;
		goto err_path;
	}

	/* Reject duplicate start offset for the same binary. */
	list_for_each_entry(tmp_b, &tlob_uprobe_list, list) {
		if (tmp_b->offset_start == offset_start &&
		    tmp_b->path.dentry == b->path.dentry) {
			ret = -EEXIST;
			goto err_path;
		}
	}

	/* Store canonical path for read-back and removal matching. */
	canon = d_path(&b->path, pathbuf, sizeof(pathbuf));
	if (IS_ERR(canon)) {
		ret = PTR_ERR(canon);
		goto err_path;
	}
	strscpy(b->binpath, canon, sizeof(b->binpath));

	b->entry_uc.handler = tlob_uprobe_entry_handler;
	b->stop_uc.handler  = tlob_uprobe_stop_handler;

	inode = d_real_inode(b->path.dentry);

	b->entry_uprobe = uprobe_register(inode, offset_start, 0, &b->entry_uc);
	if (IS_ERR(b->entry_uprobe)) {
		ret = PTR_ERR(b->entry_uprobe);
		b->entry_uprobe = NULL;
		goto err_path;
	}

	b->stop_uprobe = uprobe_register(inode, offset_stop, 0, &b->stop_uc);
	if (IS_ERR(b->stop_uprobe)) {
		ret = PTR_ERR(b->stop_uprobe);
		b->stop_uprobe = NULL;
		goto err_entry;
	}

	list_add_tail(&b->list, &tlob_uprobe_list);
	return 0;

err_entry:
	uprobe_unregister_nosync(b->entry_uprobe, &b->entry_uc);
	uprobe_unregister_sync();
err_path:
	path_put(&b->path);
err_free:
	kfree(b);
	return ret;
}

/*
 * Remove the uprobe binding for (offset_start, binpath).
 * binpath is resolved to a dentry for comparison so symlinks are handled
 * correctly.  Called with tlob_uprobe_mutex held.
 */
static void tlob_remove_uprobe_by_key(loff_t offset_start, const char *binpath)
{
	struct tlob_uprobe_binding *b, *tmp;
	struct path remove_path;

	if (kern_path(binpath, LOOKUP_FOLLOW, &remove_path))
		return;

	list_for_each_entry_safe(b, tmp, &tlob_uprobe_list, list) {
		if (b->offset_start != offset_start)
			continue;
		if (b->path.dentry != remove_path.dentry)
			continue;
		uprobe_unregister_nosync(b->entry_uprobe, &b->entry_uc);
		uprobe_unregister_nosync(b->stop_uprobe,  &b->stop_uc);
		list_del(&b->list);
		uprobe_unregister_sync();
		path_put(&b->path);
		kfree(b);
		break;
	}

	path_put(&remove_path);
}

/* Unregister all uprobe bindings; called from disable_tlob(). */
static void tlob_remove_all_uprobes(void)
{
	struct tlob_uprobe_binding *b, *tmp;

	mutex_lock(&tlob_uprobe_mutex);
	list_for_each_entry_safe(b, tmp, &tlob_uprobe_list, list) {
		uprobe_unregister_nosync(b->entry_uprobe, &b->entry_uc);
		uprobe_unregister_nosync(b->stop_uprobe,  &b->stop_uc);
		list_del(&b->list);
		path_put(&b->path);
		kfree(b);
	}
	mutex_unlock(&tlob_uprobe_mutex);
	uprobe_unregister_sync();
}

/*
 * tracefs "monitor" file
 *
 * Read:  one "threshold_us:0xoffset_start:0xoffset_stop:binary_path\n"
 *        line per registered uprobe binding.
 * Write: "threshold_us:offset_start:offset_stop:binary_path" - add uprobe binding
 *        "-offset_start:binary_path"                         - remove uprobe binding
 */

static ssize_t tlob_monitor_read(struct file *file,
				 char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	/* pid(10) + threshold(20) + 2 offsets(2*18) + path(256) + delimiters */
	const int line_sz = TLOB_MAX_PATH + 72;
	struct tlob_uprobe_binding *b;
	char *buf, *p;
	int n = 0, buf_sz, pos = 0;
	ssize_t ret;

	mutex_lock(&tlob_uprobe_mutex);
	list_for_each_entry(b, &tlob_uprobe_list, list)
		n++;
	mutex_unlock(&tlob_uprobe_mutex);

	buf_sz = (n ? n : 1) * line_sz + 1;
	buf = kmalloc(buf_sz, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&tlob_uprobe_mutex);
	list_for_each_entry(b, &tlob_uprobe_list, list) {
		p = b->binpath;
		pos += scnprintf(buf + pos, buf_sz - pos,
				 "%llu:0x%llx:0x%llx:%s\n",
				 b->threshold_us,
				 (unsigned long long)b->offset_start,
				 (unsigned long long)b->offset_stop,
				 p);
	}
	mutex_unlock(&tlob_uprobe_mutex);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, pos);
	kfree(buf);
	return ret;
}

/*
 * Parse "threshold_us:offset_start:offset_stop:binary_path".
 * binary_path comes last so it may freely contain ':'.
 * Returns 0 on success.
 */
VISIBLE_IF_KUNIT int tlob_parse_uprobe_line(char *buf, u64 *thr_out,
					    char **path_out,
					    loff_t *start_out, loff_t *stop_out)
{
	unsigned long long thr;
	long long start, stop;
	int n = 0;

	/*
	 * %llu : decimal-only (microseconds)
	 * %lli : auto-base, accepts 0x-prefixed hex for offsets
	 * %n   : records the byte offset of the first path character
	 */
	if (sscanf(buf, "%llu:%lli:%lli:%n", &thr, &start, &stop, &n) != 3)
		return -EINVAL;
	if (thr == 0 || n == 0 || buf[n] == '\0')
		return -EINVAL;
	if (start < 0 || stop < 0)
		return -EINVAL;

	*thr_out   = thr;
	*start_out = start;
	*stop_out  = stop;
	*path_out  = buf + n;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_parse_uprobe_line);

static ssize_t tlob_monitor_write(struct file *file,
				  const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	char buf[TLOB_MAX_PATH + 64];
	loff_t offset_start, offset_stop;
	u64 threshold_us;
	char *binpath;
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (count > 0 && buf[count - 1] == '\n')
		buf[count - 1] = '\0';

	/* Remove request: "-offset_start:binary_path" */
	if (buf[0] == '-') {
		long long off;
		int n = 0;

		if (sscanf(buf + 1, "%lli:%n", &off, &n) != 1 || n == 0)
			return -EINVAL;
		binpath = buf + 1 + n;
		if (binpath[0] != '/')
			return -EINVAL;

		mutex_lock(&tlob_uprobe_mutex);
		tlob_remove_uprobe_by_key((loff_t)off, binpath);
		mutex_unlock(&tlob_uprobe_mutex);

		return (ssize_t)count;
	}

	/*
	 * Uprobe binding: "threshold_us:offset_start:offset_stop:binary_path"
	 * binpath points into buf at the start of the path field.
	 */
	ret = tlob_parse_uprobe_line(buf, &threshold_us,
				     &binpath, &offset_start, &offset_stop);
	if (ret)
		return ret;

	mutex_lock(&tlob_uprobe_mutex);
	ret = tlob_add_uprobe(threshold_us, binpath, offset_start, offset_stop);
	mutex_unlock(&tlob_uprobe_mutex);
	return ret ? ret : (ssize_t)count;
}

static const struct file_operations tlob_monitor_fops = {
	.open	= simple_open,
	.read	= tlob_monitor_read,
	.write	= tlob_monitor_write,
	.llseek	= noop_llseek,
};

/*
 * __tlob_init_monitor / __tlob_destroy_monitor - called with rv_interface_lock
 * held (required by da_monitor_init/destroy via rv_get/put_task_monitor_slot).
 */
static int __tlob_init_monitor(void)
{
	int i, retval;

	tlob_state_cache = kmem_cache_create("tlob_task_state",
					     sizeof(struct tlob_task_state),
					     0, 0, NULL);
	if (!tlob_state_cache)
		return -ENOMEM;

	for (i = 0; i < TLOB_HTABLE_SIZE; i++)
		INIT_HLIST_HEAD(&tlob_htable[i]);
	atomic_set(&tlob_num_monitored, 0);

	retval = da_monitor_init();
	if (retval) {
		kmem_cache_destroy(tlob_state_cache);
		tlob_state_cache = NULL;
		return retval;
	}

	rv_this.enabled = 1;
	return 0;
}

static void __tlob_destroy_monitor(void)
{
	rv_this.enabled = 0;
	tlob_stop_all();
	tlob_remove_all_uprobes();
	/*
	 * Drain pending call_rcu() callbacks from tlob_stop_all() before
	 * destroying the kmem_cache.
	 */
	synchronize_rcu();
	da_monitor_destroy();
	kmem_cache_destroy(tlob_state_cache);
	tlob_state_cache = NULL;
}

/*
 * tlob_init_monitor / tlob_destroy_monitor - KUnit wrappers that acquire
 * rv_interface_lock, satisfying the lockdep_assert_held() inside
 * rv_get/put_task_monitor_slot().
 */
VISIBLE_IF_KUNIT int tlob_init_monitor(void)
{
	int ret;

	mutex_lock(&rv_interface_lock);
	ret = __tlob_init_monitor();
	mutex_unlock(&rv_interface_lock);
	return ret;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_init_monitor);

VISIBLE_IF_KUNIT void tlob_destroy_monitor(void)
{
	mutex_lock(&rv_interface_lock);
	__tlob_destroy_monitor();
	mutex_unlock(&rv_interface_lock);
}
EXPORT_SYMBOL_IF_KUNIT(tlob_destroy_monitor);

VISIBLE_IF_KUNIT int tlob_enable_hooks(void)
{
	rv_attach_trace_probe("tlob", sched_switch, handle_sched_switch);
	rv_attach_trace_probe("tlob", sched_wakeup, handle_sched_wakeup);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(tlob_enable_hooks);

VISIBLE_IF_KUNIT void tlob_disable_hooks(void)
{
	rv_detach_trace_probe("tlob", sched_switch, handle_sched_switch);
	rv_detach_trace_probe("tlob", sched_wakeup, handle_sched_wakeup);
}
EXPORT_SYMBOL_IF_KUNIT(tlob_disable_hooks);

/*
 * enable_tlob / disable_tlob - called by rv_enable/disable_monitor() which
 * already holds rv_interface_lock; call the __ variants directly.
 */
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
		tracefs_create_file("monitor", 0644, rv_this.root_d, NULL,
				    &tlob_monitor_fops);
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
