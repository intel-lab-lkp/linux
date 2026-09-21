/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mutexes: blocking mutual exclusion locks
 *
 * started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 */
#include <linux/mutex.h>

/*
 * Where a mutex keeps its wait_lock differs between the two mutex
 * implementations: native mutexes embed it directly, while PREEMPT_RT mutexes
 * wrap an rt_mutex and keep it in rt_mutex_base. Provide a common accessor for
 * the scheduler's proxy-execution code, which takes this lock to pin down a
 * mutex owner.
 */
static inline raw_spinlock_t *mutex_wait_lock(struct mutex *lock)
{
#ifdef CONFIG_PREEMPT_RT
	return &lock->rtmutex.wait_lock;
#else
	return &lock->wait_lock;
#endif
}

#ifndef CONFIG_PREEMPT_RT
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
 */
#define MUTEX_FLAG_WAITERS	0x01
#define MUTEX_FLAG_HANDOFF	0x02
#define MUTEX_FLAG_PICKUP	0x04

#define MUTEX_FLAGS		0x07

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

#else /* CONFIG_PREEMPT_RT */

/*
 * On PREEMPT_RT a mutex is an rt_mutex, which keeps track of its owner in
 * rt_mutex_base::owner. Expose it through the same helper the native mutex
 * path uses so that the proxy-execution scheduler code builds for both.
 */
static inline struct task_struct *__mutex_owner(struct mutex *lock)
{
	if (!lock)
		return NULL;
	return rt_mutex_owner(&lock->rtmutex);
}

#endif /* !CONFIG_PREEMPT_RT */
