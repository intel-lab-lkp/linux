/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 GUO Ren (XuanTie) <guoren@kernel.org>
 */

#ifndef _ASM_RISCV_QSPINLOCK_H
#define _ASM_RISCV_QSPINLOCK_H

#ifdef CONFIG_PARAVIRT_SPINLOCKS
#include <asm/qspinlock_paravirt.h>

/* How long a lock should spin before we consider blocking */
#define SPIN_THRESHOLD		(1 << 15)

extern bool nopvspin;

void native_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
void __pv_init_lock_hash(void);
void __pv_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);

static inline void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
	static_call(pv_queued_spin_lock_slowpath)(lock, val);
}

#define queued_spin_unlock	queued_spin_unlock
static inline void queued_spin_unlock(struct qspinlock *lock)
{
	static_call(pv_queued_spin_unlock)(lock);
}
#endif /* CONFIG_PARAVIRT_SPINLOCKS */

#include <asm-generic/qspinlock.h>
#include <asm/jump_label.h>

/*
 * KVM guests fall back to a Test-and-Set spinlock because fair locks suffer
 * from severe lock-holder-preemption issues. When virt_spin_lock_key is
 * enabled, virt_spin_lock() shortcuts queued_spin_lock_slowpath() and hijacks
 * the lock acquisition.
 */
DECLARE_STATIC_KEY_FALSE(virt_spin_lock_key);

#define virt_spin_lock rv_virt_spin_lock
static inline bool rv_virt_spin_lock(struct qspinlock *lock)
{
	if (!static_branch_likely(&virt_spin_lock_key))
		return false;

	do {
		smp_cond_load_relaxed((s32 *)&lock->val, VAL == 0);
	} while (atomic_cmpxchg(&lock->val, 0, _Q_LOCKED_VAL) != 0);

	return true;
}

#endif /* _ASM_RISCV_QSPINLOCK_H */
