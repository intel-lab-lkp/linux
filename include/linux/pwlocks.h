/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PWLOCKS_H
#define _LINUX_PWLOCKS_H

#include "linux/spinlock.h"
#include "linux/local_lock.h"
#include "linux/workqueue.h"

#ifndef CONFIG_PWLOCKS

typedef local_lock_t pw_lock_t;
typedef local_trylock_t pw_trylock_t;

struct pw_struct {
	struct work_struct work;
};

#define pw_lock_init(lock)				\
	local_lock_init(lock)

#define pw_trylock_init(lock)				\
	local_trylock_init(lock)

#define pw_lock(lock, cpu)				\
	local_lock(lock)

#define pw_lock_local(lock)				\
	local_lock(lock)

#define pw_lock_irqsave(lock, flags, cpu)		\
	local_lock_irqsave(lock, flags)

#define pw_lock_local_irqsave(lock, flags)		\
	local_lock_irqsave(lock, flags)

#define pw_trylock(lock, cpu)				\
	local_trylock(lock)

#define pw_trylock_local(lock)				\
	local_trylock(lock)

#define pw_trylock_irqsave(lock, flags, cpu)		\
	local_trylock_irqsave(lock, flags)

#define pw_unlock(lock, cpu)				\
	local_unlock(lock)

#define pw_unlock_local(lock)				\
	local_unlock(lock)

#define pw_unlock_irqrestore(lock, flags, cpu)		\
	local_unlock_irqrestore(lock, flags)

#define pw_unlock_local_irqrestore(lock, flags)		\
	local_unlock_irqrestore(lock, flags)

#define pw_lockdep_assert_held(lock)			\
	lockdep_assert_held(lock)

#define pw_queue_on(c, wq, pw)				\
	queue_work_on(c, wq, &(pw)->work)

#define pw_flush(pw)					\
	flush_work(&(pw)->work)

#define pw_get_cpu(pw)	smp_processor_id()

#define pw_is_cpu_remote(cpu)		(false)

#define INIT_PW(pw, func, c)				\
	INIT_WORK(&(pw)->work, (func))

#else /* CONFIG_PWLOCKS */

DECLARE_STATIC_KEY_MAYBE(CONFIG_PWLOCKS_DEFAULT, pw_sl);

typedef union {
	spinlock_t sl;
	local_lock_t ll;
} pw_lock_t;

typedef union {
	spinlock_t sl;
	local_trylock_t ll;
} pw_trylock_t;

struct pw_struct {
	struct work_struct work;
	int cpu;
};

#ifdef CONFIG_PREEMPT_RT
#define preempt_or_migrate_disable migrate_disable
#define preempt_or_migrate_enable migrate_enable
#else
#define preempt_or_migrate_disable preempt_disable
#define preempt_or_migrate_enable preempt_enable
#endif

#define pw_lock_init(lock)							\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		spin_lock_init(lock.sl);					\
	else									\
		local_lock_init(lock.ll);					\
} while (0)

#define pw_trylock_init(lock)							\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		spin_lock_init(lock.sl);					\
	else									\
		local_trylock_init(lock.ll);					\
} while (0)

#define pw_lock(lock, cpu)							\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		spin_lock(per_cpu_ptr(lock.sl, cpu));				\
	else									\
		local_lock(lock.ll);						\
} while (0)

#define pw_lock_local(lock)							\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl)) {		\
		preempt_or_migrate_disable();					\
		spin_lock(this_cpu_ptr(lock.sl));				\
	} else {								\
		local_lock(lock.ll);						\
	}									\
} while (0)

#define pw_lock_irqsave(lock, flags, cpu)					\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		spin_lock_irqsave(per_cpu_ptr(lock.sl, cpu), flags);	\
	else									\
		local_lock_irqsave(lock.ll, flags);				\
} while (0)

#define pw_lock_local_irqsave(lock, flags)					\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl)) {		\
		preempt_or_migrate_disable();					\
		spin_lock_irqsave(this_cpu_ptr(lock.sl), flags);		\
	} else {								\
		local_lock_irqsave(lock.ll, flags);				\
	}									\
} while (0)

#define pw_trylock(lock, cpu)							\
({										\
	int t;									\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		t = spin_trylock(per_cpu_ptr(lock.sl, cpu));			\
	else									\
		t = local_trylock(lock.ll);					\
	t;									\
})

#define pw_trylock_local(lock)							\
({										\
	int t;									\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl)) {		\
		preempt_or_migrate_disable();					\
		t = spin_trylock(this_cpu_ptr(lock.sl));			\
		if (!t)								\
			preempt_or_migrate_enable();				\
	} else {								\
		t = local_trylock(lock.ll);					\
	}									\
	t;									\
})

#define pw_trylock_irqsave(lock, flags, cpu)					\
({										\
	int t;									\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		t = spin_trylock_irqsave(per_cpu_ptr(lock.sl, cpu), flags);	\
	else									\
		t = local_trylock_irqsave(lock.ll, flags);			\
	t;									\
})

#define pw_unlock(lock, cpu)							\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		spin_unlock(per_cpu_ptr(lock.sl, cpu));			\
	else									\
		local_unlock(lock.ll);					\
} while (0)

#define pw_unlock_local(lock)							\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl)) {		\
		spin_unlock(this_cpu_ptr(lock.sl));				\
		preempt_or_migrate_enable();					\
	} else {								\
		local_unlock(lock.ll);						\
	}									\
} while (0)

#define pw_unlock_irqrestore(lock, flags, cpu)					\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		spin_unlock_irqrestore(per_cpu_ptr(lock.sl, cpu), flags);	\
	else									\
		local_unlock_irqrestore(lock.ll, flags);			\
} while (0)

#define pw_unlock_local_irqrestore(lock, flags)					\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl)) {		\
		spin_unlock_irqrestore(this_cpu_ptr(lock.sl), flags);	\
		preempt_or_migrate_enable();					\
	} else {								\
		local_unlock_irqrestore(lock.ll, flags);			\
	}									\
} while (0)

#define pw_lockdep_assert_held(lock)						\
do {										\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		lockdep_assert_held(this_cpu_ptr(lock.sl));			\
	else									\
		lockdep_assert_held(this_cpu_ptr(lock.ll));			\
} while (0)

#define pw_queue_on(c, wq, pw)							\
do {										\
	int __c = c;								\
	struct pw_struct *__pw = (pw);						\
	if (static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl)) {		\
		WARN_ON((__c) != __pw->cpu);					\
		__pw->work.func(&__pw->work);					\
	} else {								\
		queue_work_on(__c, wq, &(__pw)->work);				\
	}									\
} while (0)

/*
 * Does nothing if PWLOCKS is set to use spinlock, as the task is already done at the
 * time pw_queue_on() returns.
 */
#define pw_flush(pw)								\
do {										\
	struct pw_struct *__pw = (pw);						\
	if (!static_branch_maybe(CONFIG_PWLOCKS_DEFAULT, &pw_sl))		\
		flush_work(&__pw->work);					\
} while (0)

#define pw_get_cpu(w)			container_of((w), struct pw_struct, work)->cpu

#define pw_is_cpu_remote(cpu)		((cpu) != smp_processor_id())

#define INIT_PW(pw, func, c)							\
do {										\
	struct pw_struct *__pw = (pw);						\
	INIT_WORK(&__pw->work, (func));						\
	__pw->cpu = (c);							\
} while (0)

#endif /* CONFIG_PWLOCKS */
#endif /* LINUX_PWLOCKS_H */
