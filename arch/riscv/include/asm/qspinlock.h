/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c), 2025 Alibaba Damo Academy
 * Authors:
 *	Guo Ren <guoren@kernel.org>
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
 * The KVM guests fall back to a Test-and-Set spinlock, because fair locks
 * have horrible lock 'holder' preemption issues. The test_and_set_spinlock_key
 * would shortcut for the queued_spin_lock_slowpath() function that allow
 * virt_spin_lock to hijack it.
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
