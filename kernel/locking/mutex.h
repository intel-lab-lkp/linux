/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mutexes: blocking mutual exclusion locks
 *
 * started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 */
#ifndef CONFIG_PREEMPT_RT
#include <linux/lockholder.h>
#include <linux/mutex.h>
/*
 * This is the control structure for tasks blocked on mutex, which resides
 * on the blocked task's kernel stack:
 */
struct mutex_waiter {
	struct list_head	list;
	struct task_struct	*task;
	struct ww_acquire_ctx	*ww_ctx;
#ifdef CONFIG_DEBUG_MUTEXES
	void			*magic;
#endif
};

/*
 * @owner: contains: 'struct task_struct *' to the current lock owner,
 * NULL means not owned. Since task_struct pointers are aligned at
 * at least L1_CACHE_BYTES, we have low bits to store extra state.
 *
 * Bit0 indicates a non-empty waiter list; unlock must issue a wakeup.
 * Bit1 indicates unlock needs to hand the lock to the top-waiter
 * Bit2 indicates handoff has been done and we're waiting for pickup.
 * Bit3 indicates the lock opted in to holder tracking. Unlike the others
 * it is sticky: it survives unlock, so an unlocked tracked mutex reads
 * MUTEX_FLAG_TRACKED rather than 0.
 */
#define MUTEX_FLAG_WAITERS	0x01
#define MUTEX_FLAG_HANDOFF	0x02
#define MUTEX_FLAG_PICKUP	0x04
#ifdef CONFIG_TRACK_LOCK_HOLDERS
#define MUTEX_FLAG_TRACKED	0x08
#else
#define MUTEX_FLAG_TRACKED	0x00
#endif

/* The state flags, which unlock clears. */
#define MUTEX_STATE_FLAGS	(MUTEX_FLAG_WAITERS | MUTEX_FLAG_HANDOFF | \
				 MUTEX_FLAG_PICKUP)
#define MUTEX_FLAGS		(MUTEX_STATE_FLAGS | MUTEX_FLAG_TRACKED)

/*
 * The flags live below the task_struct pointer in ->owner, so every
 * task_struct has to be aligned past them. Bits 0-2 needed 8 bytes;
 * MUTEX_FLAG_TRACKED needs 16. fork_init() aligns task_struct to at
 * least L1_CACHE_BYTES and init_task is __aligned(L1_CACHE_BYTES), so
 * that is the value to check; its smallest value in the tree is 16.
 */
static_assert(L1_CACHE_BYTES > MUTEX_FLAGS);

/*
 * Internal helper function; C doesn't allow us to hide it :/
 *
 * DO NOT USE (outside of mutex & scheduler code).
 */
static inline struct task_struct *__mutex_owner(struct mutex *lock)
{
	if (!lock)
		return NULL;
	return (struct task_struct *)(atomic_long_read(&lock->owner) & ~MUTEX_FLAGS);
}

static inline bool mutex_is_tracked(struct mutex *lock)
{
	if (!IS_ENABLED(CONFIG_TRACK_LOCK_HOLDERS))
		return false;

	return atomic_long_read(&lock->owner) & MUTEX_FLAG_TRACKED;
}

/*
 * The opt-in flag as it sits in owner, for the fast paths that fold it
 * into a cmpxchg. The flag is only set once the static branch is on, so
 * an off branch means no mutex carries it and the read can be skipped.
 */
static inline unsigned long mutex_tracked_flag(struct mutex *lock)
{
#ifdef CONFIG_TRACK_LOCK_HOLDERS
	if (!static_branch_unlikely(&lock_holder_tracking_key))
		return 0;

	return atomic_long_read(&lock->owner) & MUTEX_FLAG_TRACKED;
#else
	return 0;
#endif
}

static inline struct mutex *get_task_blocked_on(struct task_struct *p)
{
	guard(raw_spinlock_irqsave)(&p->blocked_lock);
	return __get_task_blocked_on(p);
}

#ifdef CONFIG_DEBUG_MUTEXES
extern void debug_mutex_lock_common(struct mutex *lock,
				    struct mutex_waiter *waiter);
extern void debug_mutex_wake_waiter(struct mutex *lock,
				    struct mutex_waiter *waiter);
extern void debug_mutex_free_waiter(struct mutex_waiter *waiter);
extern void debug_mutex_add_waiter(struct mutex *lock,
				   struct mutex_waiter *waiter,
				   struct task_struct *task);
extern void debug_mutex_remove_waiter(struct mutex *lock, struct mutex_waiter *waiter,
				      struct task_struct *task);
extern void debug_mutex_unlock(struct mutex *lock);
extern void debug_mutex_init(struct mutex *lock);
#else /* CONFIG_DEBUG_MUTEXES */
# define debug_mutex_lock_common(lock, waiter)		do { } while (0)
# define debug_mutex_wake_waiter(lock, waiter)		do { } while (0)
# define debug_mutex_free_waiter(waiter)		do { } while (0)
# define debug_mutex_add_waiter(lock, waiter, ti)	do { } while (0)
# define debug_mutex_remove_waiter(lock, waiter, ti)	do { } while (0)
# define debug_mutex_unlock(lock)			do { } while (0)
# define debug_mutex_init(lock)				do { } while (0)
#endif /* !CONFIG_DEBUG_MUTEXES */
#endif /* CONFIG_PREEMPT_RT */
