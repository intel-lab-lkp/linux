/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_QSPINLOCK_PARAVIRT_H
#define __ASM_QSPINLOCK_PARAVIRT_H

void __lockfunc __pv_queued_spin_unlock_slowpath(struct qspinlock *lock, u8 locked);
#endif
