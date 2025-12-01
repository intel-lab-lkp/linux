/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c), 2025 Alibaba Damo Academy
 * Authors:
 *	Guo Ren <guoren@kernel.org>
 */

#ifndef _ASM_RISCV_QSPINLOCK_PARAVIRT_H
#define _ASM_RISCV_QSPINLOCK_PARAVIRT_H

void pv_wait(u8 *ptr, u8 val);
void pv_kick(int cpu);

void dummy_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
void dummy_queued_spin_unlock(struct qspinlock *lock);

DECLARE_STATIC_CALL(pv_queued_spin_lock_slowpath, dummy_queued_spin_lock_slowpath);
DECLARE_STATIC_CALL(pv_queued_spin_unlock, dummy_queued_spin_unlock);

bool __init pv_qspinlock_init(void);

void __pv_queued_spin_unlock_slowpath(struct qspinlock *lock, u8 locked);

bool pv_is_native_spin_unlock(void);

void __pv_queued_spin_unlock(struct qspinlock *lock);

#endif /* _ASM_RISCV_QSPINLOCK_PARAVIRT_H */
