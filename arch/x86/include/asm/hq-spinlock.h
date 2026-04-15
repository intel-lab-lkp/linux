/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_X86_HQ_SPINLOCK_H
#define _ASM_X86_HQ_SPINLOCK_H

extern void hq_configure_spin_lock_slowpath(void);

#ifndef CONFIG_PARAVIRT_SPINLOCKS
extern void (*hq_queued_spin_lock_slowpath)(struct qspinlock *lock, u32 val);
extern void native_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);

#define	queued_spin_unlock queued_spin_unlock
/**
 * queued_spin_unlock - release a queued spinlock
 * @lock : Pointer to queued spinlock structure
 *
 * A smp_store_release() on the least-significant byte.
 */
static inline void native_queued_spin_unlock(struct qspinlock *lock)
{
	smp_store_release(&lock->locked, 0);
}

static inline void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
	hq_queued_spin_lock_slowpath(lock, val);
}

static inline void queued_spin_unlock(struct qspinlock *lock)
{
	native_queued_spin_unlock(lock);
}
#endif // !CONFIG_PARAVIRT_SPINLOCKS

#endif // _ASM_X86_HQ_SPINLOCK_H
