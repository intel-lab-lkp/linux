/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _VDSO_FUTEX_H
#define _VDSO_FUTEX_H

#include <linux/types.h>
#include <linux/futex.h>

/**
 * __vdso_robust_futex_unlock_u32 - Architecture-specific vDSO implementation of robust futex unlock.
 * @uaddr:		Lock address (points to a 32-bit unsigned integer type).
 * @val:		New value to set in *@uaddr.
 * @robust_list_head:	The thread-specific robust list that has been registered with set_robust_list.
 *
 * This vDSO unlocks the robust futex by exchanging the content of
 * *@uaddr with @val with a store-release semantic. If the futex has
 * waiters, it sets bit 1 of *@robust_list_head->list_op_pending, else
 * it clears *@robust_list_head->list_op_pending. Those operations are
 * within a code region known by the kernel, making them safe with
 * respect to asynchronous program termination either from thread
 * context or from a nested signal handler.
 *
 * Returns:	The old value present at *@uaddr.
 *
 * Expected use of this vDSO:
 *
 * robust_list_head is the thread-specific robust list that has been
 * registered with set_robust_list.
 *
 * if ((__vdso_robust_futex_unlock_u32((u32 *) &mutex->__data.__lock, 0, robust_list_head)
 *     & FUTEX_WAITERS) != 0)
 *         futex_wake((u32 *) &mutex->__data.__lock, 1, private);
 * WRITE_ONCE(robust_list_head->list_op_pending, 0);
 */
extern u32 __vdso_robust_futex_unlock_u32(u32 *uaddr, u32 val, struct robust_list_head *robust_list_head);

/*
 * __vdso_robust_pi_futex_try_unlock_u32 - Architecture-specific vDSO implementation of robust PI futex unlock.
 * @uaddr:		Lock address (points to a 32-bit unsigned integer type).
 * @expected:		Expected value (in), value loaded by compare-and-exchange (out).
 * @val:		New value to set in *@uaddr if *@uaddr match *@expected.
 * @robust_list_head:	The thread-specific robust list that has been registered with set_robust_list.
 *
 * This vDSO try to perform a compare-and-exchange with release semantic
 * to set the expected *@uaddr content to @val. If the futex has
 * waiters, it fails, and userspace needs to call futex_unlock_pi().
 * Before exiting the critical section, if the cmpxchg fails, it sets
 * bit 1 of *@robust_list_head->list_op_pending. If the cmpxchg
 * succeeds, it clears *@robust_list_head->list_op_pending. Those
 * operations are within a code region known by the kernel, making them
 * safe with respect to asynchronous program termination either from
 * thread context or from a nested signal handler.
 *
 * Returns:	Zero if the operation fails to release the lock, non-zero on success.
 *
 * Expected use of this vDSO:
 *
 *
 * int l = atomic_load_relaxed(&mutex->__data.__lock);
 * do {
 *         if (((l & FUTEX_WAITERS) != 0) || (l != READ_ONCE(pd->tid))) {
 *                 futex_unlock_pi((unsigned int *) &mutex->__data.__lock, private);
 *                 break;
 *         }
 * } while (!__vdso_robust_pi_futex_try_unlock_u32(&mutex->__data.__lock,
 *                                                 &l, 0, robust_list_head));
 * WRITE_ONCE(robust_list_head->list_op_pending, 0);
 */
int __vdso_robust_pi_futex_try_unlock_u32(u32 *uaddr, u32 *expected, u32 val, struct robust_list_head *robust_list_head);

#endif /* _VDSO_FUTEX_H */
