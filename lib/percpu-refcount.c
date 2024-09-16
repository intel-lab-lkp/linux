// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/llist.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/percpu-refcount.h>

#include "percpu-refcount.h"

/*
 * Initially, a percpu refcount is just a set of percpu counters. Initially, we
 * don't try to detect the ref hitting 0 - which means that get/put can just
 * increment or decrement the local counter. Note that the counter on a
 * particular cpu can (and will) wrap - this is fine, when we go to shutdown the
 * percpu counters will all sum to the correct value
 *
 * (More precisely: because modular arithmetic is commutative the sum of all the
 * percpu_count vars will be equal to what it would have been if all the gets
 * and puts were done to a single integer, even if some of the percpu integers
 * overflow or underflow).
 *
 * The real trick to implementing percpu refcounts is shutdown. We can't detect
 * the ref hitting 0 on every put - this would require global synchronization
 * and defeat the whole purpose of using percpu refs.
 *
 * What we do is require the user to keep track of the initial refcount; we know
 * the ref can't hit 0 before the user drops the initial ref, so as long as we
 * convert to non percpu mode before the initial ref is dropped everything
 * works.
 *
 * Converting to non percpu mode is done with some RCUish stuff in
 * percpu_ref_kill. Additionally, we need a bias value so that the
 * atomic_long_t can't hit 0 before we've added up all the percpu refs.
 */

#define PERCPU_COUNT_BIAS	(1LU << (BITS_PER_LONG - 1))

static DEFINE_SPINLOCK(percpu_ref_switch_lock);
static DECLARE_WAIT_QUEUE_HEAD(percpu_ref_switch_waitq);
static LLIST_HEAD(percpu_ref_manage_head);

static unsigned long __percpu *percpu_count_ptr(struct percpu_ref *ref)
{
	return (unsigned long __percpu *)
		(ref->percpu_count_ptr & ~__PERCPU_REF_ATOMIC_DEAD);
}

int percpu_ref_switch_to_managed(struct percpu_ref *ref);

/**
 * percpu_ref_init - initialize a percpu refcount
 * @ref: percpu_ref to initialize
 * @release: function which will be called when refcount hits 0
 * @flags: PERCPU_REF_INIT_* flags
 * @gfp: allocation mask to use
 *
 * Initializes @ref.  @ref starts out in percpu mode with a refcount of 1 unless
 * @flags contains PERCPU_REF_INIT_ATOMIC or PERCPU_REF_INIT_DEAD.  These flags
 * change the start state to atomic with the latter setting the initial refcount
 * to 0.  See the definitions of PERCPU_REF_INIT_* flags for flag behaviors.
 *
 * Note that @release must not sleep - it may potentially be called from RCU
 * callback context by percpu_ref_kill().
 */
int percpu_ref_init(struct percpu_ref *ref, percpu_ref_func_t *release,
		    unsigned int flags, gfp_t gfp)
{
	size_t align = max_t(size_t, 1 << __PERCPU_REF_FLAG_BITS,
			     __alignof__(unsigned long));
	unsigned long start_count = 0;
	struct percpu_ref_data *data;

	ref->percpu_count_ptr = (unsigned long)
		__alloc_percpu_gfp(sizeof(unsigned long), align, gfp);
	if (!ref->percpu_count_ptr)
		return -ENOMEM;

	data = kzalloc(sizeof(*ref->data), gfp);
	if (!data) {
		free_percpu((void __percpu *)ref->percpu_count_ptr);
		ref->percpu_count_ptr = 0;
		return -ENOMEM;
	}

	if (flags & PERCPU_REF_REL_MANAGED)
		flags |= PERCPU_REF_ALLOW_REINIT;

	data->force_atomic = flags & PERCPU_REF_INIT_ATOMIC;
	data->allow_reinit = flags & PERCPU_REF_ALLOW_REINIT;

	if (flags & (PERCPU_REF_INIT_ATOMIC | PERCPU_REF_INIT_DEAD)) {
		ref->percpu_count_ptr |= __PERCPU_REF_ATOMIC;
		data->allow_reinit = true;
	} else {
		start_count += PERCPU_COUNT_BIAS;
	}

	if (flags & PERCPU_REF_INIT_DEAD)
		ref->percpu_count_ptr |= __PERCPU_REF_DEAD;
	else
		start_count++;

	atomic_long_set(&data->count, start_count);

	data->release = release;
	data->confirm_switch = NULL;
	data->ref = ref;
	ref->data = data;
	init_llist_node(&data->node);

	if (flags & PERCPU_REF_REL_MANAGED)
		percpu_ref_switch_to_managed(ref);

	return 0;
}
EXPORT_SYMBOL_GPL(percpu_ref_init);

static bool percpu_ref_is_managed(struct percpu_ref *ref)
{
	return (ref->data->aux_flags & __PERCPU_REL_MANAGED) != 0;
}

static void __percpu_ref_switch_mode(struct percpu_ref *ref,
				     percpu_ref_func_t *confirm_switch);

static int __percpu_ref_switch_to_managed(struct percpu_ref *ref)
{
	unsigned long __percpu *percpu_count;
	struct percpu_ref_data *data;
	int ret = -1;

	data = ref->data;

	if (WARN_ONCE(!percpu_ref_tryget(ref), "Percpu ref is not active"))
		return ret;

	if (WARN_ONCE(!data->allow_reinit, "Percpu ref does not allow switch"))
		goto err_switch_managed;

	if (WARN_ONCE(percpu_ref_is_managed(ref), "Percpu ref is already managed"))
		goto err_switch_managed;

	data->aux_flags |= __PERCPU_REL_MANAGED;
	data->force_atomic = false;
	if (!__ref_is_percpu(ref, &percpu_count))
		__percpu_ref_switch_mode(ref, NULL);
	/* Ensure ordering of percpu mode switch and node scan */
	smp_mb();
	llist_add(&data->node, &percpu_ref_manage_head);

	return 0;

err_switch_managed:
	percpu_ref_put(ref);
	return ret;
}

/**
 * percpu_ref_switch_to_managed - Switch an unmanaged ref to percpu mode.
 *
 * @ref: percpu_ref to switch to managed mode
 *
 */
int percpu_ref_switch_to_managed(struct percpu_ref *ref)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&percpu_ref_switch_lock, flags);
	ret = __percpu_ref_switch_to_managed(ref);
	spin_unlock_irqrestore(&percpu_ref_switch_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(percpu_ref_switch_to_managed);

static void __percpu_ref_exit(struct percpu_ref *ref)
{
	unsigned long __percpu *percpu_count = percpu_count_ptr(ref);

	if (percpu_count) {
		/* non-NULL confirm_switch indicates switching in progress */
		WARN_ON_ONCE(ref->data && ref->data->confirm_switch);
		free_percpu(percpu_count);
		ref->percpu_count_ptr = __PERCPU_REF_ATOMIC_DEAD;
	}
}

/**
 * percpu_ref_exit - undo percpu_ref_init()
 * @ref: percpu_ref to exit
 *
 * This function exits @ref.  The caller is responsible for ensuring that
 * @ref is no longer in active use.  The usual places to invoke this
 * function from are the @ref->release() callback or in init failure path
 * where percpu_ref_init() succeeded but other parts of the initialization
 * of the embedding object failed.
 */
void percpu_ref_exit(struct percpu_ref *ref)
{
	struct percpu_ref_data *data = ref->data;
	unsigned long flags;

	__percpu_ref_exit(ref);

	if (!data)
		return;

	spin_lock_irqsave(&percpu_ref_switch_lock, flags);
	ref->percpu_count_ptr |= atomic_long_read(&ref->data->count) <<
		__PERCPU_REF_FLAG_BITS;
	ref->data = NULL;
	spin_unlock_irqrestore(&percpu_ref_switch_lock, flags);

	kfree(data);
}
EXPORT_SYMBOL_GPL(percpu_ref_exit);

static void percpu_ref_call_confirm_rcu(struct rcu_head *rcu)
{
	struct percpu_ref_data *data = container_of(rcu,
			struct percpu_ref_data, rcu);
	struct percpu_ref *ref = data->ref;

	data->confirm_switch(ref);
	data->confirm_switch = NULL;
	wake_up_all(&percpu_ref_switch_waitq);

	if (!data->allow_reinit)
		__percpu_ref_exit(ref);

	/* drop ref from percpu_ref_switch_to_atomic() */
	percpu_ref_put(ref);
}

static void percpu_ref_switch_to_atomic_rcu(struct rcu_head *rcu)
{
	struct percpu_ref_data *data = container_of(rcu,
			struct percpu_ref_data, rcu);
	struct percpu_ref *ref = data->ref;
	unsigned long __percpu *percpu_count = percpu_count_ptr(ref);
	static atomic_t underflows;
	unsigned long count = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		count += *per_cpu_ptr(percpu_count, cpu);

	pr_debug("global %lu percpu %lu\n",
		 atomic_long_read(&data->count), count);

	/*
	 * It's crucial that we sum the percpu counters _before_ adding the sum
	 * to &ref->count; since gets could be happening on one cpu while puts
	 * happen on another, adding a single cpu's count could cause
	 * @ref->count to hit 0 before we've got a consistent value - but the
	 * sum of all the counts will be consistent and correct.
	 *
	 * Subtracting the bias value then has to happen _after_ adding count to
	 * &ref->count; we need the bias value to prevent &ref->count from
	 * reaching 0 before we add the percpu counts. But doing it at the same
	 * time is equivalent and saves us atomic operations:
	 */
	atomic_long_add((long)count - PERCPU_COUNT_BIAS, &data->count);

	if (WARN_ONCE(atomic_long_read(&data->count) <= 0,
		      "percpu ref (%ps) <= 0 (%ld) after switching to atomic",
		      data->release, atomic_long_read(&data->count)) &&
	    atomic_inc_return(&underflows) < 4) {
		pr_err("%s(): percpu_ref underflow", __func__);
		mem_dump_obj(data);
	}

	/* @ref is viewed as dead on all CPUs, send out switch confirmation */
	percpu_ref_call_confirm_rcu(rcu);
}

static void percpu_ref_noop_confirm_switch(struct percpu_ref *ref)
{
}

static void __percpu_ref_switch_to_atomic(struct percpu_ref *ref,
					  percpu_ref_func_t *confirm_switch)
{
	if (ref->percpu_count_ptr & __PERCPU_REF_ATOMIC) {
		if (confirm_switch)
			confirm_switch(ref);
		return;
	}

	/* switching from percpu to atomic */
	ref->percpu_count_ptr |= __PERCPU_REF_ATOMIC;

	/*
	 * Non-NULL ->confirm_switch is used to indicate that switching is
	 * in progress.  Use noop one if unspecified.
	 */
	ref->data->confirm_switch = confirm_switch ?:
		percpu_ref_noop_confirm_switch;

	percpu_ref_get(ref);	/* put after confirmation */
	call_rcu_hurry(&ref->data->rcu,
		       percpu_ref_switch_to_atomic_rcu);
}

static void __percpu_ref_switch_to_percpu(struct percpu_ref *ref)
{
	unsigned long __percpu *percpu_count = percpu_count_ptr(ref);
	int cpu;

	BUG_ON(!percpu_count);

	if (!(ref->percpu_count_ptr & __PERCPU_REF_ATOMIC))
		return;

	if (WARN_ON_ONCE(!ref->data->allow_reinit))
		return;

	atomic_long_add(PERCPU_COUNT_BIAS, &ref->data->count);

	/*
	 * Restore per-cpu operation.  smp_store_release() is paired
	 * with READ_ONCE() in __ref_is_percpu() and guarantees that the
	 * zeroing is visible to all percpu accesses which can see the
	 * following __PERCPU_REF_ATOMIC clearing.
	 */
	for_each_possible_cpu(cpu)
		*per_cpu_ptr(percpu_count, cpu) = 0;

	smp_store_release(&ref->percpu_count_ptr,
			  ref->percpu_count_ptr & ~__PERCPU_REF_ATOMIC);
}

static void __percpu_ref_switch_mode(struct percpu_ref *ref,
				     percpu_ref_func_t *confirm_switch)
{
	struct percpu_ref_data *data = ref->data;

	lockdep_assert_held(&percpu_ref_switch_lock);

	/*
	 * If the previous ATOMIC switching hasn't finished yet, wait for
	 * its completion.  If the caller ensures that ATOMIC switching
	 * isn't in progress, this function can be called from any context.
	 */
	wait_event_lock_irq(percpu_ref_switch_waitq, !data->confirm_switch,
			    percpu_ref_switch_lock);

	if (data->force_atomic || percpu_ref_is_dying(ref))
		__percpu_ref_switch_to_atomic(ref, confirm_switch);
	else
		__percpu_ref_switch_to_percpu(ref);
}

static bool __percpu_ref_switch_to_atomic_checked(struct percpu_ref *ref,
						  percpu_ref_func_t *confirm_switch,
						  bool check_managed)
{
	unsigned long flags;

	spin_lock_irqsave(&percpu_ref_switch_lock, flags);
	if (check_managed && WARN_ONCE(percpu_ref_is_managed(ref),
		      "Percpu ref is managed, cannot switch to atomic mode")) {
		spin_unlock_irqrestore(&percpu_ref_switch_lock, flags);
		return false;
	}

	ref->data->force_atomic = true;
	__percpu_ref_switch_mode(ref, confirm_switch);

	spin_unlock_irqrestore(&percpu_ref_switch_lock, flags);

	return true;
}

/**
 * percpu_ref_switch_to_atomic - switch a percpu_ref to atomic mode
 * @ref: percpu_ref to switch to atomic mode
 * @confirm_switch: optional confirmation callback
 *
 * There's no reason to use this function for the usual reference counting.
 * Use percpu_ref_kill[_and_confirm]().
 *
 * Schedule switching of @ref to atomic mode.  All its percpu counts will
 * be collected to the main atomic counter.  On completion, when all CPUs
 * are guaraneed to be in atomic mode, @confirm_switch, which may not
 * block, is invoked.  This function may be invoked concurrently with all
 * the get/put operations and can safely be mixed with kill and reinit
 * operations.  Note that @ref will stay in atomic mode across kill/reinit
 * cycles until percpu_ref_switch_to_percpu() is called.
 *
 * This function may block if @ref is in the process of switching to atomic
 * mode.  If the caller ensures that @ref is not in the process of
 * switching to atomic mode, this function can be called from any context.
 */
void percpu_ref_switch_to_atomic(struct percpu_ref *ref,
				 percpu_ref_func_t *confirm_switch)
{
	(void)__percpu_ref_switch_to_atomic_checked(ref, confirm_switch, true);
}
EXPORT_SYMBOL_GPL(percpu_ref_switch_to_atomic);

static void __percpu_ref_switch_to_atomic_sync_checked(struct percpu_ref *ref, bool check_managed)
{
	if (!__percpu_ref_switch_to_atomic_checked(ref, NULL, check_managed))
		return;
	wait_event(percpu_ref_switch_waitq, !ref->data->confirm_switch);
}
/**
 * percpu_ref_switch_to_atomic_sync - switch a percpu_ref to atomic mode
 * @ref: percpu_ref to switch to atomic mode
 *
 * Schedule switching the ref to atomic mode, and wait for the
 * switch to complete.  Caller must ensure that no other thread
 * will switch back to percpu mode.
 */
void percpu_ref_switch_to_atomic_sync(struct percpu_ref *ref)
{
	__percpu_ref_switch_to_atomic_sync_checked(ref, true);
}
EXPORT_SYMBOL_GPL(percpu_ref_switch_to_atomic_sync);

static void __percpu_ref_switch_to_percpu_checked(struct percpu_ref *ref, bool check_managed)
{
	unsigned long flags;

	spin_lock_irqsave(&percpu_ref_switch_lock, flags);

	if (check_managed && WARN_ONCE(percpu_ref_is_managed(ref),
		      "Percpu ref is managed, cannot switch to percpu mode")) {
		spin_unlock_irqrestore(&percpu_ref_switch_lock, flags);
		return;
	}

	ref->data->force_atomic = false;
	__percpu_ref_switch_mode(ref, NULL);

	spin_unlock_irqrestore(&percpu_ref_switch_lock, flags);
}

/**
 * percpu_ref_switch_to_percpu - switch a percpu_ref to percpu mode
 * @ref: percpu_ref to switch to percpu mode
 *
 * There's no reason to use this function for the usual reference counting.
 * To re-use an expired ref, use percpu_ref_reinit().
 *
 * Switch @ref to percpu mode.  This function may be invoked concurrently
 * with all the get/put operations and can safely be mixed with kill and
 * reinit operations.  This function reverses the sticky atomic state set
 * by PERCPU_REF_INIT_ATOMIC or percpu_ref_switch_to_atomic().  If @ref is
 * dying or dead, the actual switching takes place on the following
 * percpu_ref_reinit().
 *
 * This function may block if @ref is in the process of switching to atomic
 * mode.  If the caller ensures that @ref is not in the process of
 * switching to atomic mode, this function can be called from any context.
 */
void percpu_ref_switch_to_percpu(struct percpu_ref *ref)
{
	__percpu_ref_switch_to_percpu_checked(ref, true);
}
EXPORT_SYMBOL_GPL(percpu_ref_switch_to_percpu);

/**
 * percpu_ref_kill_and_confirm - drop the initial ref and schedule confirmation
 * @ref: percpu_ref to kill
 * @confirm_kill: optional confirmation callback
 *
 * Equivalent to percpu_ref_kill() but also schedules kill confirmation if
 * @confirm_kill is not NULL.  @confirm_kill, which may not block, will be
 * called after @ref is seen as dead from all CPUs at which point all
 * further invocations of percpu_ref_tryget_live() will fail.  See
 * percpu_ref_tryget_live() for details.
 *
 * This function normally doesn't block and can be called from any context
 * but it may block if @confirm_kill is specified and @ref is in the
 * process of switching to atomic mode by percpu_ref_switch_to_atomic().
 *
 * There are no implied RCU grace periods between kill and release.
 */
void percpu_ref_kill_and_confirm(struct percpu_ref *ref,
				 percpu_ref_func_t *confirm_kill)
{
	unsigned long flags;

	spin_lock_irqsave(&percpu_ref_switch_lock, flags);

	WARN_ONCE(percpu_ref_is_dying(ref),
		  "%s called more than once on %ps!", __func__,
		  ref->data->release);

	ref->percpu_count_ptr |= __PERCPU_REF_DEAD;
	__percpu_ref_switch_mode(ref, confirm_kill);
	percpu_ref_put(ref);

	spin_unlock_irqrestore(&percpu_ref_switch_lock, flags);
}
EXPORT_SYMBOL_GPL(percpu_ref_kill_and_confirm);

/**
 * percpu_ref_is_zero - test whether a percpu refcount reached zero
 * @ref: percpu_ref to test
 *
 * Returns %true if @ref reached zero.
 *
 * This function is safe to call as long as @ref is between init and exit.
 */
bool percpu_ref_is_zero(struct percpu_ref *ref)
{
	unsigned long __percpu *percpu_count;
	unsigned long count, flags;

	if (__ref_is_percpu(ref, &percpu_count))
		return false;

	/* protect us from being destroyed */
	spin_lock_irqsave(&percpu_ref_switch_lock, flags);
	if (ref->data)
		count = atomic_long_read(&ref->data->count);
	else
		count = ref->percpu_count_ptr >> __PERCPU_REF_FLAG_BITS;
	spin_unlock_irqrestore(&percpu_ref_switch_lock, flags);

	return count == 0;
}
EXPORT_SYMBOL_GPL(percpu_ref_is_zero);

/**
 * percpu_ref_reinit - re-initialize a percpu refcount
 * @ref: perpcu_ref to re-initialize
 *
 * Re-initialize @ref so that it's in the same state as when it finished
 * percpu_ref_init() ignoring %PERCPU_REF_INIT_DEAD.  @ref must have been
 * initialized successfully and reached 0 but not exited.
 *
 * Note that percpu_ref_tryget[_live]() are safe to perform on @ref while
 * this function is in progress.
 */
void percpu_ref_reinit(struct percpu_ref *ref)
{
	WARN_ON_ONCE(!percpu_ref_is_zero(ref));

	percpu_ref_resurrect(ref);
}
EXPORT_SYMBOL_GPL(percpu_ref_reinit);

/**
 * percpu_ref_resurrect - modify a percpu refcount from dead to live
 * @ref: perpcu_ref to resurrect
 *
 * Modify @ref so that it's in the same state as before percpu_ref_kill() was
 * called. @ref must be dead but must not yet have exited.
 *
 * If @ref->release() frees @ref then the caller is responsible for
 * guaranteeing that @ref->release() does not get called while this
 * function is in progress.
 *
 * Note that percpu_ref_tryget[_live]() are safe to perform on @ref while
 * this function is in progress.
 */
void percpu_ref_resurrect(struct percpu_ref *ref)
{
	unsigned long __percpu *percpu_count;
	unsigned long flags;

	spin_lock_irqsave(&percpu_ref_switch_lock, flags);

	WARN_ON_ONCE(!percpu_ref_is_dying(ref));
	WARN_ON_ONCE(__ref_is_percpu(ref, &percpu_count));

	ref->percpu_count_ptr &= ~__PERCPU_REF_DEAD;
	percpu_ref_get(ref);
	if (percpu_ref_is_managed(ref)) {
		ref->data->aux_flags &= ~__PERCPU_REL_MANAGED;
		__percpu_ref_switch_to_managed(ref);
	} else {
		__percpu_ref_switch_mode(ref, NULL);
	}

	spin_unlock_irqrestore(&percpu_ref_switch_lock, flags);
}
EXPORT_SYMBOL_GPL(percpu_ref_resurrect);

#define DEFAULT_SCAN_INTERVAL_MS    5000
/* Interval duration between two ref scans. */
static ulong scan_interval = DEFAULT_SCAN_INTERVAL_MS;
module_param(scan_interval, ulong, 0444);

#define DEFAULT_MAX_SCAN_COUNT      100
/* Number of percpu refs scanned in one iteration of worker execution. */
static int max_scan_count = DEFAULT_MAX_SCAN_COUNT;
module_param(max_scan_count, int, 0444);

static void percpu_ref_release_work_fn(struct work_struct *work);

/*
 * Sentinel llist nodes for lockless list traveral and deletions by
 * the pcpu ref release worker, while nodes are added from
 * percpu_ref_init() and percpu_ref_switch_to_managed().
 *
 * Sentinel node marks the head of list traversal for the current
 * iteration of kworker execution.
 */
struct percpu_ref_sen_node {
	bool inuse;
	struct llist_node node;
};

/*
 * We need two sentinel nodes for lockless list manipulations from release
 * worker - first node will be used in current reclaim iteration. The second
 * node will be used in next iteration. Next iteration marks the first node
 * as free, for use in subsequent iteration.
 */
#define PERCPU_REF_SEN_NODES_COUNT     2

/* Track last processed percpu ref node */
static struct llist_node *last_percpu_ref_node;

static struct percpu_ref_sen_node
	percpu_ref_sen_nodes[PERCPU_REF_SEN_NODES_COUNT];

static DECLARE_DELAYED_WORK(percpu_ref_release_work, percpu_ref_release_work_fn);

static bool percpu_ref_is_sen_node(struct llist_node *node)
{
	return &percpu_ref_sen_nodes[0].node <= node &&
		node <= &percpu_ref_sen_nodes[PERCPU_REF_SEN_NODES_COUNT - 1].node;
}

static struct llist_node *percpu_ref_get_sen_node(void)
{
	int i;
	struct percpu_ref_sen_node *sn;

	for (i = 0; i < PERCPU_REF_SEN_NODES_COUNT; i++) {
		sn = &percpu_ref_sen_nodes[i];
		if (!sn->inuse) {
			sn->inuse = true;
			return &sn->node;
		}
	}

	return NULL;
}

static void percpu_ref_put_sen_node(struct llist_node *node)
{
	struct percpu_ref_sen_node *sn = container_of(node, struct percpu_ref_sen_node, node);

	sn->inuse = false;
	init_llist_node(node);
}

static void percpu_ref_put_all_sen_nodes_except(struct llist_node *node)
{
	int i;

	for (i = 0; i < PERCPU_REF_SEN_NODES_COUNT; i++) {
		if (&percpu_ref_sen_nodes[i].node == node)
			continue;
		percpu_ref_sen_nodes[i].inuse = false;
		init_llist_node(&percpu_ref_sen_nodes[i].node);
	}
}

static struct workqueue_struct *percpu_ref_release_wq;

static void percpu_ref_release_work_fn(struct work_struct *work)
{
	struct llist_node *pos, *first, *head, *prev, *next;
	struct llist_node *sen_node;
	struct percpu_ref *ref;
	int count = 0;
	bool held;
	struct llist_node *last_node = READ_ONCE(last_percpu_ref_node);

	first = READ_ONCE(percpu_ref_manage_head.first);
	if (!first)
		goto queue_release_work;

	/*
	 * Enqueue a dummy node to mark the start of scan. This dummy
	 * node is used as start point of scan and ensures that
	 * there is no additional synchronization required with new
	 * label node additions to the llist. Any new labels will
	 * be processed in next run of the kworker.
	 *
	 *                SCAN START PTR
	 *                     |
	 *                     v
	 * +----------+     +------+    +------+    +------+
	 * |          |     |      |    |      |    |      |
	 * |   head   ------> dummy|--->|label |--->| label|--->NULL
	 * |          |     | node |    |      |    |      |
	 * +----------+     +------+    +------+    +------+
	 *
	 *
	 * New label addition:
	 *
	 *                       SCAN START PTR
	 *                            |
	 *                            v
	 * +----------+  +------+  +------+    +------+    +------+
	 * |          |  |      |  |      |    |      |    |      |
	 * |   head   |--> label|--> dummy|--->|label |--->| label|--->NULL
	 * |          |  |      |  | node |    |      |    |      |
	 * +----------+  +------+  +------+    +------+    +------+
	 *
	 */
	if (last_node == NULL || last_node->next == NULL) {
retry_sentinel_get:
		sen_node = percpu_ref_get_sen_node();
		/*
		 * All sentinel nodes are in use? This should not happen, as we
		 * require only one sentinel for the start of list traversal and
		 * other sentinel node is freed during the traversal.
		 */
		if (WARN_ONCE(!sen_node, "All sentinel nodes are in use")) {
			/* Use first node as the sentinel node */
			head = first->next;
			if (!head) {
				struct llist_node *ign_node = NULL;
				/*
				 * We exhausted sentinel nodes. However, there aren't
				 * enough nodes in the llist. So, we have leaked
				 * sentinel nodes. Reclaim sentinels and retry.
				 */
				if (percpu_ref_is_sen_node(first))
					ign_node = first;
				percpu_ref_put_all_sen_nodes_except(ign_node);
				goto retry_sentinel_get;
			}
			prev = first;
		} else {
			llist_add(sen_node, &percpu_ref_manage_head);
			prev = sen_node;
			head = prev->next;
		}
	} else {
		prev = last_node;
		head = prev->next;
	}

	llist_for_each_safe(pos, next, head) {
		/* Free sentinel node which is present in the list */
		if (percpu_ref_is_sen_node(pos)) {
			prev->next = pos->next;
			percpu_ref_put_sen_node(pos);
			continue;
		}

		ref = container_of(pos, struct percpu_ref_data, node)->ref;
		__percpu_ref_switch_to_atomic_sync_checked(ref, false);
		/*
		 * Drop the ref while in RCU read critical section to
		 * prevent obj free while we manipulating node.
		 */
		rcu_read_lock();
		percpu_ref_put(ref);
		held = percpu_ref_tryget(ref);
		if (!held) {
			prev->next = pos->next;
			init_llist_node(pos);
			ref->percpu_count_ptr |= __PERCPU_REF_DEAD;
		}
		rcu_read_unlock();
		if (!held)
			continue;
		__percpu_ref_switch_to_percpu_checked(ref, false);
		count++;
		if (count == READ_ONCE(max_scan_count)) {
			WRITE_ONCE(last_percpu_ref_node, pos);
			goto queue_release_work;
		}
		prev = pos;
	}

	WRITE_ONCE(last_percpu_ref_node, NULL);
queue_release_work:
	queue_delayed_work(percpu_ref_release_wq, &percpu_ref_release_work,
			   scan_interval);
}

bool percpu_ref_test_is_percpu(struct percpu_ref *ref)
{
	unsigned long __percpu *percpu_count;

	return __ref_is_percpu(ref, &percpu_count);
}
EXPORT_SYMBOL_GPL(percpu_ref_test_is_percpu);

void percpu_ref_test_flush_release_work(void)
{
	int max_flush = READ_ONCE(max_scan_count);
	int max_count = 1000;

	/* Complete any executing release work */
	flush_delayed_work(&percpu_ref_release_work);
	/* Scan till the end of the llist */
	WRITE_ONCE(max_scan_count, INT_MAX);
	/* max scan count update visible to release work */
	smp_mb();
	flush_delayed_work(&percpu_ref_release_work);
	/* max scan count update visible to release work */
	smp_mb();
	WRITE_ONCE(max_scan_count, 1);
	/* max scan count update visible to work */
	smp_mb();
	flush_delayed_work(&percpu_ref_release_work);
	while (READ_ONCE(last_percpu_ref_node) != NULL && max_count--)
		flush_delayed_work(&percpu_ref_release_work);
	/* max scan count update visible to work */
	smp_mb();
	WRITE_ONCE(max_scan_count, max_flush);
}
EXPORT_SYMBOL_GPL(percpu_ref_test_flush_release_work);

static __init int percpu_ref_setup(void)
{
	percpu_ref_release_wq = alloc_workqueue("percpu_ref_release_wq",
				WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_FREEZABLE, 0);
	if (!percpu_ref_release_wq)
		return -ENOMEM;

	queue_delayed_work(percpu_ref_release_wq, &percpu_ref_release_work,
			   scan_interval);
	return 0;
}
early_initcall(percpu_ref_setup);
