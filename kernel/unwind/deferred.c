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
 * the architecture must support a safe cmpxchg in NMI context.
 * For those architectures that do not have that, then it cannot ask
 * for a deferred user space stack trace from an NMI context. If it
 * does, then it will get -EINVAL.
 */
#if defined(CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG)
# define CAN_USE_IN_NMI		1
static inline bool try_assign_cnt(struct unwind_task_info *info, u32 cnt)
{
	u32 old = 0;

	return try_cmpxchg(&info->id.cnt, &old, cnt);
}
#else
# define CAN_USE_IN_NMI		0
/* When NMIs are not allowed, this always succeeds */
static inline bool try_assign_cnt(struct unwind_task_info *info, u32 cnt)
{
	info->id.cnt = cnt;
	return true;
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

/*
 * This is a unique percpu identifier for a given task entry context.
 * Conceptually, it's incremented every time the CPU enters the kernel from
 * user space, so that each "entry context" on the CPU gets a unique ID.  In
 * reality, as an optimization, it's only incremented on demand for the first
 * deferred unwind request after a given entry-from-user.
 *
 * It's combined with the CPU id to make a systemwide-unique "context cookie".
 */
static DEFINE_PER_CPU(u32, unwind_ctx_ctr);

/*
 * The context cookie is a unique identifier that is assigned to a user
 * space stacktrace. As the user space stacktrace remains the same while
 * the task is in the kernel, the cookie is an identifier for the stacktrace.
 * Although it is possible for the stacktrace to get another cookie if another
 * request is made after the cookie was cleared and before reentering user
 * space.
 */
static u64 get_cookie(struct unwind_task_info *info)
{
	u32 cpu_cnt;
	u32 cnt;

	if (info->id.cpu)
		return info->id.id;

	cpu_cnt = __this_cpu_read(unwind_ctx_ctr);
	cpu_cnt += 2;
	cnt = cpu_cnt | 1; /* Always make non zero */

	if (try_assign_cnt(info, cnt)) {
		/* Update the per cpu counter */
		__this_cpu_write(unwind_ctx_ctr, cpu_cnt);
	}
	/* Interrupts are disabled, the CPU will always be same */
	info->id.cpu = smp_processor_id() + 1; /* Must be non zero */

	return info->id.id;
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
	u64 cookie;
	int idx;

	if (WARN_ON_ONCE(!local_read(&info->pending)))
		return;

	/* Allow work to come in again */
	local_set(&info->pending, 0);

	/*
	 * From here on out, the callback must always be called, even if it's
	 * just an empty trace.
	 */
	trace.nr = 0;
	trace.entries = NULL;

	unwind_user_faultable(&trace);

	cookie = info->id.id;

	idx = srcu_read_lock(&unwind_srcu);
	list_for_each_entry_srcu(work, &callbacks, list,
				 srcu_read_lock_held(&unwind_srcu)) {
		if (test_bit(work->bit, &info->unwind_mask)) {
			work->func(work, &trace, cookie);
			clear_bit(work->bit, &info->unwind_mask);
		}
	}
	srcu_read_unlock(&unwind_srcu, idx);
}

/**
 * unwind_deferred_request - Request a user stacktrace on task exit
 * @work: Unwind descriptor requesting the trace
 * @cookie: The cookie of the first request made for this task
 *
 * Schedule a user space unwind to be done in task work before exiting the
 * kernel.
 *
 * The returned @cookie output is the generated cookie of the very first
 * request for a user space stacktrace for this task since it entered the
 * kernel. It can be from a request by any caller of this infrastructure.
 * Its value will also be passed to the callback function.  It can be
 * used to stitch kernel and user stack traces together in post-processing.
 *
 * It's valid to call this function multiple times for the same @work within
 * the same task entry context.  Each call will return the same cookie
 * while the task hasn't left the kernel. If the callback is not pending
 * because it has already been previously called for the same entry context,
 * it will be called again with the same stack trace and cookie.
 *
 * Return: 1 if the the callback was already queued.
 *         0 if the callback successfully was queued.
 *         Negative if there's an error.
 *         @cookie holds the cookie of the first request by any user
 */
int unwind_deferred_request(struct unwind_work *work, u64 *cookie)
{
	struct unwind_task_info *info = &current->unwind_info;
	long pending;
	int bit;
	int ret;

	*cookie = 0;

	if ((current->flags & (PF_KTHREAD | PF_EXITING)) ||
	    !user_mode(task_pt_regs(current)))
		return -EINVAL;

	/* NMI requires having safe cmpxchg operations */
	if (!CAN_USE_IN_NMI && in_nmi())
		return -EINVAL;

	/* Do not allow cancelled works to request again */
	bit = READ_ONCE(work->bit);
	if (WARN_ON_ONCE(bit < 0))
		return -EINVAL;

	guard(irqsave)();

	*cookie = get_cookie(info);

	/* This is already queued */
	if (test_bit(bit, &info->unwind_mask))
		return 1;

	/* callback already pending? */
	pending = local_read(&info->pending);
	if (pending)
		goto out;

	if (CAN_USE_IN_NMI) {
		/* Claim the work unless an NMI just now swooped in to do so. */
		if (!local_try_cmpxchg(&info->pending, &pending, 1))
			goto out;
	} else {
		local_set(&info->pending, 1);
	}

	/* The work has been claimed, now schedule it. */
	ret = task_work_add(current, &info->work, TWA_RESUME);
	if (WARN_ON_ONCE(ret)) {
		local_set(&info->pending, 0);
		return ret;
	}

 out:
	return test_and_set_bit(bit, &info->unwind_mask);
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
	if (unwind_mask == ~0UL)
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
