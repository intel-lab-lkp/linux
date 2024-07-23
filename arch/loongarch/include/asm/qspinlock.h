/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LOONGARCH_QSPINLOCK_H
#define _ASM_LOONGARCH_QSPINLOCK_H

#include <asm/paravirt.h>

#define _Q_PENDING_LOOPS       (1 << 9)

#ifdef CONFIG_PARAVIRT_SPINLOCKS
/* How long a lock should spin before we consider blocking */
#define SPIN_THRESHOLD  (1 << 15)

extern void native_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
extern void __pv_init_lock_hash(void);
extern void __pv_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
extern void __pv_queued_spin_unlock(struct qspinlock *lock);
extern bool nopvspin;

static inline void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
	pv_queued_spin_lock_slowpath(lock, val);
}

#define queued_spin_unlock queued_spin_unlock
static inline void queued_spin_unlock(struct qspinlock *lock)
{
	pv_queued_spin_unlock(lock);
}

#define vcpu_is_preempted vcpu_is_preempted
static inline bool vcpu_is_preempted(long cpu)
{
	return pv_vcpu_is_preempted(cpu);
}
#endif

#include <asm-generic/qspinlock.h>

#endif // _ASM_LOONGARCH_QSPINLOCK_H
