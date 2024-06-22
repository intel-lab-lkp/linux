/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_QPW_H
#define _LINUX_QPW_H

#include "linux/local_lock.h"
#include "linux/workqueue.h"

#ifndef CONFIG_PREEMPT_RT

struct qpw_struct {
	struct work_struct work;
};

#define qpw_lock(lock, cpu)					\
	local_lock(lock)

#define qpw_unlock(lock, cpu)					\
	local_unlock(lock)

#define qpw_lock_irqsave(lock, flags, cpu)			\
	local_lock_irqsave(lock, flags)

#define qpw_unlock_irqrestore(lock, flags, cpu)			\
	local_unlock_irqrestore(lock, flags)

#define queue_percpu_work_on(c, wq, qpw)			\
	queue_work_on(c, wq, &(qpw)->work)

#define flush_percpu_work(qpw)					\
	flush_work(&(qpw)->work)

#define qpw_get_cpu(qpw)					\
	smp_processor_id()

#define INIT_QPW(qpw, func, c)					\
	INIT_WORK(&(qpw)->work, (func))

#else /* !CONFIG_PREEMPT_RT */

struct qpw_struct {
	struct work_struct work;
	int cpu;
};

#define qpw_lock(__lock, cpu)					\
	do {							\
		migrate_disable();				\
		spin_lock(per_cpu_ptr((__lock), cpu));		\
	} while (0)

#define qpw_unlock(__lock, cpu)					\
	do {							\
		spin_unlock(per_cpu_ptr((__lock), cpu));	\
		migrate_enable();				\
	} while (0)

#define qpw_lock_irqsave(lock, flags, cpu)			\
	do {							\
		typecheck(unsigned long, flags);		\
		flags = 0;					\
		qpw_lock(lock, cpu);				\
	} while (0)

#define qpw_unlock_irqrestore(lock, flags, cpu)			\
	qpw_unlock(lock, cpu)

#define queue_percpu_work_on(c, wq, qpw)			\
	do {							\
		struct qpw_struct *__qpw = (qpw);		\
		WARN_ON((c) != __qpw->cpu);			\
		__qpw->work.func(&__qpw->work);			\
	} while (0)

#define flush_percpu_work(qpw)					\
	do {} while (0)

#define qpw_get_cpu(w)						\
	container_of((w), struct qpw_struct, work)->cpu

#define INIT_QPW(qpw, func, c)					\
	do {							\
		struct qpw_struct *__qpw = (qpw);		\
		INIT_WORK(&__qpw->work, (func));		\
		__qpw->cpu = (c);				\
	} while (0)

#endif /* CONFIG_PREEMPT_RT */
#endif /* LINUX_QPW_H */
