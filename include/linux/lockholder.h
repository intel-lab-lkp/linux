/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Opt-in tracking of sleeping lock holders.
 *
 * Each task counts the opted-in sleeping locks it holds. This gives a
 * cheap way to check whether a task holds a lock that other tasks may be
 * waiting for, e.g. to avoid throttling it in memory.high enforcement.
 *
 * No lock is tracked by default. Until the first lock opts in, the hooks
 * are a static branch that is off.
 */
#ifndef __LINUX_LOCKHOLDER_H
#define __LINUX_LOCKHOLDER_H

#include <linux/bug.h>
#include <linux/jump_label.h>
#include <linux/sched.h>

#ifdef CONFIG_TRACK_LOCK_HOLDERS

DECLARE_STATIC_KEY_FALSE(lock_holder_tracking_key);

/*
 * Turn the hooks on. There is no way to turn them off, as locks never
 * opt out. May sleep; call after jump_label_init().
 */
void lock_holder_tracking_enable(void);

static __always_inline void __lock_holder_acquired(void)
{
	current->nr_tracked_locks++;
}

static __always_inline void __lock_holder_released(void)
{
	/*
	 * Only the task changes its own count, so zero here means a missed
	 * acquire hook, not a race. Don't wrap around: the task would look
	 * like a lock holder forever.
	 */
	if (WARN_ON_ONCE(!current->nr_tracked_locks))
		return;
	current->nr_tracked_locks--;
}

/*
 * Hooks for lock implementations. @tracked says whether this lock opted
 * in. It is only evaluated once the static branch is on, so nothing runs
 * until a lock opts in.
 */
#define lock_holder_acquired_if(tracked)				\
do {									\
	if (static_branch_unlikely(&lock_holder_tracking_key) &&	\
	    (tracked))							\
		__lock_holder_acquired();				\
} while (0)

#define lock_holder_released_if(tracked)				\
do {									\
	if (static_branch_unlikely(&lock_holder_tracking_key) &&	\
	    (tracked))							\
		__lock_holder_released();				\
} while (0)

/* For lock types that keep the opt-in in a bool "tracked" member. */
#define lock_holder_acquired(lock)	lock_holder_acquired_if((lock)->tracked)
#define lock_holder_released(lock)	lock_holder_released_if((lock)->tracked)

static inline void lock_holder_task_init(struct task_struct *p)
{
	p->nr_tracked_locks = 0;
}

static inline unsigned int task_nr_tracked_locks(struct task_struct *p)
{
	/*
	 * Only @p writes this, so a cross-task read can see a stale value.
	 * Callers asking about another task want a hint, not a guarantee.
	 */
	return data_race(p->nr_tracked_locks);
}

#else /* !CONFIG_TRACK_LOCK_HOLDERS */

#define lock_holder_acquired_if(tracked)	do { (void)(tracked); } while (0)
#define lock_holder_released_if(tracked)	do { (void)(tracked); } while (0)
#define lock_holder_acquired(lock)	do { (void)(lock); } while (0)
#define lock_holder_released(lock)	do { (void)(lock); } while (0)

static inline void lock_holder_task_init(struct task_struct *p) { }

static inline unsigned int task_nr_tracked_locks(struct task_struct *p)
{
	return 0;
}

#endif /* CONFIG_TRACK_LOCK_HOLDERS */

/**
 * task_holds_tracked_lock - check if a task holds an opted-in lock
 * @p: task to check
 *
 * For a task other than current, the answer can change at any time.
 * Always false without CONFIG_TRACK_LOCK_HOLDERS.
 */
static inline bool task_holds_tracked_lock(struct task_struct *p)
{
	return task_nr_tracked_locks(p) != 0;
}

/**
 * current_holds_tracked_lock - check if current holds an opted-in lock
 */
static inline bool current_holds_tracked_lock(void)
{
	return task_holds_tracked_lock(current);
}

#endif /* __LINUX_LOCKHOLDER_H */
