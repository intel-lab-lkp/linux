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
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/mm.h>

/*
 * For requesting a deferred user space stack trace from NMI context
 * the architecture must support a 64bit safe cmpxchg in NMI context.
 * For those architectures that do not have that, then it cannot ask
 * for a deferred user space stack trace from an NMI context. If it
 * does, then it will get -EINVAL.
 */
#if defined(CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG) && \
	!defined(CONFIG_GENERIC_ATOMIC64)
# define CAN_USE_IN_NMI		1
static inline u64 assign_timestamp(struct unwind_task_info *info,
				   u64 timestamp)
{
	u64 old = 0;
	if (!local64_try_cmpxchg(&info->timestamp, &old, timestamp))
		timestamp = old;
	return timestamp;
}
#else
# define CAN_USE_IN_NMI		0
static inline u64 assign_timestamp(struct unwind_task_info *info,
				   u64 timestamp)
{
	/* For archs that do not allow NMI here */
	local64_set(&info->timestamp, timestamp);
	return timestamp;
}
#endif

/* Make the cache fit in a 4K page */
#define UNWIND_MAX_ENTRIES					\
	((SZ_4K - sizeof(struct unwind_cache)) / sizeof(long))

/* Guards adding to or removing from the list of callbacks */
static DEFINE_MUTEX(callback_mutex);
static LIST_HEAD(callbacks);
static unsigned long unwind_mask;
DEFINE_STATIC_SRCU(unwind_srcu);

static inline bool unwind_pending(struct unwind_task_info *info)
{
	return test_bit(UNWIND_PENDING_BIT, &info->unwind_mask);
}

/*
 * Read the task context timestamp, if this is the first caller then
 * it will set the timestamp.
 *
 * For this to work properly, the timestamp (local_clock()) must
 * have a resolution that will guarantee a different timestamp
 * everytime a task makes a system call. That is, two short
 * system calls back to back must have a different timestamp.
 */
static u64 get_timestamp(struct unwind_task_info *info)
{
	u64 timestamp;

	lockdep_assert_irqs_disabled();

	/*
	 * Note, the timestamp is generated on the first request.
	 * If it exists here, then the timestamp is earlier than
	 * this request and it means that this request will be
	 * valid for the stracktrace.
	 */
	timestamp = local64_read(&info->timestamp);
	if (timestamp)
		return timestamp;

	return assign_timestamp(info, local_clock());
}

/**
 * unwind_user_faultable - Produce a user stacktrace in faultable context
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
int unwind_user_faultable(struct unwind_stacktrace *trace)
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
	unsigned long bits;
	u64 timestamp;
	int idx;

	if (WARN_ON_ONCE(!unwind_pending(info)))
		return;

	/* Clear pending bit but make sure to have the current bits */
	bits = READ_ONCE(info->unwind_mask);
	while (!try_cmpxchg(&info->unwind_mask, &bits, bits & ~UNWIND_PENDING))
		;

	/*
	 * From here on out, the callback must always be called, even if it's
	 * just an empty trace.
	 */
	trace.nr = 0;
	trace.entries = NULL;

	unwind_user_faultable(&trace);

	timestamp = local64_read(&info->timestamp);

	idx = srcu_read_lock(&unwind_srcu);
	list_for_each_entry_srcu(work, &callbacks, list,
				 srcu_read_lock_held(&unwind_srcu)) {
		if (test_bit(work->bit, &bits))
			work->func(work, &trace, timestamp);
	}
	srcu_read_unlock(&unwind_srcu, idx);
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
 * Note, the architecture must have a local_clock() implementation that
 * guarantees a different timestamp per task systemcall.
 *
 * It's valid to call this function multiple times for the same @work within
 * the same task entry context.  Each call will return the same timestamp
 * while the task hasn't left the kernel. If the callback is not pending because
 * it has already been previously called for the same entry context, it will be
 * called again with the same stack trace and timestamp.
 *
 * Return: 0 if the callback successfully was queued.
 *         UNWIND_ALREADY_PENDING if the the callback was already queued.
 *         UNWIND_ALREADY_EXECUTED if the callback was already called
 *                (and will not be called again)
 *         Negative if there's an error.
 *         @timestamp holds the timestamp of the first request by any user
 */
int unwind_deferred_request(struct unwind_work *work, u64 *timestamp)
{
	struct unwind_task_info *info = &current->unwind_info;
	unsigned long old, bits;
	int bit;
	int ret;

	*timestamp = 0;

	if ((current->flags & (PF_KTHREAD | PF_EXITING)) ||
	    !user_mode(task_pt_regs(current)))
		return -EINVAL;

	/* NMI requires having safe 64 bit cmpxchg operations */
	if (!CAN_USE_IN_NMI && in_nmi())
		return -EINVAL;

	/* Do not allow cancelled works to request again */
	bit = READ_ONCE(work->bit);
	if (WARN_ON_ONCE(bit < 0))
		return -EINVAL;

	guard(irqsave)();

	*timestamp = get_timestamp(info);

	old = READ_ONCE(info->unwind_mask);

	/* Is this already queued */
	if (test_bit(bit, &old)) {
		/*
		 * If pending is not set, it means this work's callback
		 * was already called.
		 */
		return old & UNWIND_PENDING ? UNWIND_ALREADY_PENDING :
			UNWIND_ALREADY_EXECUTED;
	}

	if (unwind_pending(info))
		goto out;

	/*
	 * This is the first to enable another task_work for this task since
	 * the task entered the kernel, or had already called the callbacks.
	 * Set only the bit for this work and clear all others as they have
	 * already had their callbacks called, and do not need to call them
	 * again because of this work.
	 */
	bits = UNWIND_PENDING | BIT(bit);

	/*
	 * If the cmpxchg() fails, it means that an NMI came in and set
	 * the pending bit as well as cleared the other bits. Just
	 * jump to setting the bit for this work.
	 */
	if (CAN_USE_IN_NMI) {
		if (!try_cmpxchg(&info->unwind_mask, &old, bits))
			goto out;
	} else {
		info->unwind_mask = bits;
	}

	/* The work has been claimed, now schedule it. */
	ret = task_work_add(current, &info->work, TWA_RESUME);

	if (WARN_ON_ONCE(ret))
		WRITE_ONCE(info->unwind_mask, 0);

	return ret;
 out:
	return test_and_set_bit(bit, &info->unwind_mask) ?
		UNWIND_ALREADY_PENDING : 0;
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

	__clear_bit(bit, &unwind_mask);

	synchronize_srcu(&unwind_srcu);

	guard(rcu)();
	/* Clear this bit from all threads */
	for_each_process_thread(g, t) {
		clear_bit(bit, &t->unwind_info.unwind_mask);
	}
}

int unwind_deferred_init(struct unwind_work *work, unwind_callback_t func)
{
	memset(work, 0, sizeof(*work));

	guard(mutex)(&callback_mutex);

	/* See if there's a bit in the mask available */
	if (unwind_mask == ~(UNWIND_PENDING))
		return -EBUSY;

	work->bit = ffz(unwind_mask);
	__set_bit(work->bit, &unwind_mask);

	list_add_rcu(&work->list, &callbacks);
	work->func = func;
	return 0;
}

void unwind_task_init(struct task_struct *task)
{
	struct unwind_task_info *info = &task->unwind_info;

	memset(info, 0, sizeof(*info));
	init_task_work(&info->work, unwind_deferred_task_work);
	info->unwind_mask = 0;
}

void unwind_task_free(struct task_struct *task)
{
	struct unwind_task_info *info = &task->unwind_info;

	kfree(info->cache);
	task_work_cancel(task, &info->work);
}
