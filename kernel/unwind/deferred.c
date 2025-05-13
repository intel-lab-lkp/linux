// SPDX-License-Identifier: GPL-2.0
/*
 * Deferred user space unwinding
 */
#include <linux/sched/task_stack.h>
#include <linux/unwind_deferred.h>
#include <linux/sched/clock.h>
#include <linux/task_work.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>

#define UNWIND_MAX_ENTRIES 512

/* Guards adding to or removing from the list of callbacks */
static DEFINE_MUTEX(callback_mutex);
static LIST_HEAD(callbacks);
static unsigned long unwind_mask;
DEFINE_STATIC_SRCU(unwind_srcu);

/*
 * Read the task context timestamp, if this is the first caller then
 * it will set the timestamp.
 */
static u64 get_timestamp(struct unwind_task_info *info)
{
	lockdep_assert_irqs_disabled();

	/*
	 * Note, the timestamp is generated on the first request.
	 * If it exists here, then the timestamp is earlier than
	 * this request and it means that this request will be
	 * valid for the stracktrace.
	 */
	if (!info->timestamp) {
		WRITE_ONCE(info->timestamp, local_clock());
		barrier();
		/*
		 * If an NMI came in and set a timestamp, it means that
		 * it happened before this timestamp was set (otherwise
		 * the NMI would have used this one). Use the NMI timestamp
		 * instead.
		 */
		if (unlikely(info->nmi_timestamp)) {
			WRITE_ONCE(info->timestamp, info->nmi_timestamp);
			barrier();
			WRITE_ONCE(info->nmi_timestamp, 0);
		}
	}

	return info->timestamp;
}

/**
 * unwind_deferred_trace - Produce a user stacktrace in faultable context
 * @trace: The descriptor that will store the user stacktrace
 *
 * This must be called in a known faultable context (usually when entering
 * or exiting user space). Depending on the available implementations
 * the @trace will be loaded with the addresses of the user space stacktrace
 * if it can be found.
 *
 * Return: 0 on success and negative on error
 *         On success @trace will contain the user space stacktrace
 */
int unwind_deferred_trace(struct unwind_stacktrace *trace)
{
	struct unwind_task_info *info = &current->unwind_info;
	struct unwind_cache *cache;

	/* Should always be called from faultable context */
	might_fault();

	if (current->flags & PF_EXITING)
		return -EINVAL;

	if (!info->cache) {
		info->cache = kzalloc(struct_size(cache, entries, UNWIND_MAX_ENTRIES),
				      GFP_KERNEL);
		if (!info->cache)
			return -ENOMEM;
	}

	cache = info->cache;
	trace->entries = cache->entries;

	if (cache->nr_entries) {
		/*
		 * The user stack has already been previously unwound in this
		 * entry context.  Skip the unwind and use the cache.
		 */
		trace->nr = cache->nr_entries;
		return 0;
	}

	trace->nr = 0;
	unwind_user(trace, UNWIND_MAX_ENTRIES);

	cache->nr_entries = trace->nr;

	return 0;
}

static void unwind_deferred_task_work(struct callback_head *head)
{
	struct unwind_task_info *info = container_of(head, struct unwind_task_info, work);
	struct unwind_stacktrace trace;
	struct unwind_work *work;
	u64 timestamp;
	struct task_struct *task = current;
	int idx;

	if (WARN_ON_ONCE(!info->pending))
		return;

	/* Allow work to come in again */
	WRITE_ONCE(info->pending, 0);

	/*
	 * From here on out, the callback must always be called, even if it's
	 * just an empty trace.
	 */
	trace.nr = 0;
	trace.entries = NULL;

	unwind_deferred_trace(&trace);

	/* Check if the timestamp was only set by NMI */
	if (info->nmi_timestamp) {
		WRITE_ONCE(info->timestamp, info->nmi_timestamp);
		barrier();
		WRITE_ONCE(info->nmi_timestamp, 0);
	}

	timestamp = info->timestamp;

	idx = srcu_read_lock(&unwind_srcu);
	list_for_each_entry_srcu(work, &callbacks, list,
				 srcu_read_lock_held(&unwind_srcu)) {
		if (task->unwind_mask & (1UL << work->bit)) {
			work->func(work, &trace, timestamp);
			clear_bit(work->bit, &current->unwind_mask);
		}
	}
	srcu_read_unlock(&unwind_srcu, idx);
}

static int unwind_deferred_request_nmi(struct unwind_work *work, u64 *timestamp)
{
	struct unwind_task_info *info = &current->unwind_info;
	bool inited_timestamp = false;
	int ret;

	/* Always use the nmi_timestamp first */
	*timestamp = info->nmi_timestamp ? : info->timestamp;

	if (!*timestamp) {
		/*
		 * This is the first unwind request since the most recent entry
		 * from user space. Initialize the task timestamp.
		 *
		 * Don't write to info->timestamp directly, otherwise it may race
		 * with an interruption of get_timestamp().
		 */
		info->nmi_timestamp = local_clock();
		*timestamp = info->nmi_timestamp;
		inited_timestamp = true;
	}

	if (current->unwind_mask & (1UL << work->bit))
		return 1;

	if (info->pending)
		goto out;

	ret = task_work_add(current, &info->work, TWA_NMI_CURRENT);
	if (ret) {
		/*
		 * If this set nmi_timestamp and is not using it,
		 * there's no guarantee that it will be used.
		 * Set it back to zero.
		 */
		if (inited_timestamp)
			info->nmi_timestamp = 0;
		return ret;
	}

	info->pending = 1;
out:
	return test_and_set_bit(work->bit, &current->unwind_mask);
}

/**
 * unwind_deferred_request - Request a user stacktrace on task exit
 * @work: Unwind descriptor requesting the trace
 * @timestamp: The time stamp of the first request made for this task
 *
 * Schedule a user space unwind to be done in task work before exiting the
 * kernel.
 *
 * The returned @timestamp output is the timestamp of the very first request
 * for a user space stacktrace for this task since it entered the kernel.
 * It can be from a request by any caller of this infrastructure.
 * Its value will also be passed to the callback function.  It can be
 * used to stitch kernel and user stack traces together in post-processing.
 *
 * It's valid to call this function multiple times for the same @work within
 * the same task entry context.  Each call will return the same timestamp
 * while the task hasn't left the kernel. If the callback is not pending because
 * it has already been previously called for the same entry context, it will be
 * called again with the same stack trace and timestamp.
 *
 * Return: 1 if the the callback was already queued.
 *         0 if the callback successfully was queued.
 *         Negative if there's an error.
 *         @timestamp holds the timestamp of the first request by any user
 */
int unwind_deferred_request(struct unwind_work *work, u64 *timestamp)
{
	struct unwind_task_info *info = &current->unwind_info;
	int pending;
	int bit;
	int ret;

	*timestamp = 0;

	if ((current->flags & (PF_KTHREAD | PF_EXITING)) ||
	    !user_mode(task_pt_regs(current)))
		return -EINVAL;

	if (in_nmi())
		return unwind_deferred_request_nmi(work, timestamp);

	/* Do not allow cancelled works to request again */
	bit = READ_ONCE(work->bit);
	if (WARN_ON_ONCE(bit < 0))
		return -EINVAL;

	guard(irqsave)();

	*timestamp = get_timestamp(info);

	/* This is already queued */
	if (current->unwind_mask & (1UL << bit))
		return 1;

	/* callback already pending? */
	pending = READ_ONCE(info->pending);
	if (pending)
		goto out;

	/* Claim the work unless an NMI just now swooped in to do so. */
	if (!try_cmpxchg(&info->pending, &pending, 1))
		goto out;

	/* The work has been claimed, now schedule it. */
	ret = task_work_add(current, &info->work, TWA_RESUME);
	if (WARN_ON_ONCE(ret)) {
		WRITE_ONCE(info->pending, 0);
		return ret;
	}

 out:
	return test_and_set_bit(work->bit, &current->unwind_mask);
}

void unwind_deferred_cancel(struct unwind_work *work)
{
	struct task_struct *g, *t;
	int bit;

	if (!work)
		return;

	guard(mutex)(&callback_mutex);
	list_del_rcu(&work->list);
	bit = work->bit;

	/* Do not allow any more requests and prevent callbacks */
	work->bit = -1;

	clear_bit(bit, &unwind_mask);

	synchronize_srcu(&unwind_srcu);

	guard(rcu)();
	/* Clear this bit from all threads */
	for_each_process_thread(g, t) {
		clear_bit(bit, &t->unwind_mask);
	}
}

int unwind_deferred_init(struct unwind_work *work, unwind_callback_t func)
{
	memset(work, 0, sizeof(*work));

	guard(mutex)(&callback_mutex);

	/* See if there's a bit in the mask available */
	if (unwind_mask == ~0UL)
		return -EBUSY;

	work->bit = ffz(unwind_mask);
	unwind_mask |= 1UL << work->bit;

	list_add_rcu(&work->list, &callbacks);
	work->func = func;
	return 0;
}

void unwind_task_init(struct task_struct *task)
{
	struct unwind_task_info *info = &task->unwind_info;

	memset(info, 0, sizeof(*info));
	init_task_work(&info->work, unwind_deferred_task_work);
	task->unwind_mask = 0;
}

void unwind_task_free(struct task_struct *task)
{
	struct unwind_task_info *info = &task->unwind_info;

	kfree(info->cache);
	task_work_cancel(task, &info->work);
}
