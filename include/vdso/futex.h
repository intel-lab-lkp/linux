/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#ifndef _VDSO_FUTEX_H
#define _VDSO_FUTEX_H

#include <linux/types.h>

/**
 * __vdso_robust_futex_unlock - Architecture-specific vDSO implementation of robust futex unlock.
 * @uaddr:		Lock address (points to a 32-bit unsigned integer type).
 * @op_pending_addr:	Robust list operation pending address (points to a pointer type).
 *
 * This vDSO unlocks the robust futex by exchanging the content of
 * *uaddr with 0 with a store-release semantic. If the futex has
 * waiters, it sets bit 1 of *op_pending_addr, else it clears
 * *op_pending_addr. Those operations are within a code region
 * known by the kernel, making them safe with respect to asynchronous
 * program termination either from thread context or from a nested
 * signal handler.
 *
 * Returns:	The old value present at *uaddr.
 *
 * Expected use of this vDSO:
 *
 * if ((__vdso_robust_futex_unlock((u32 *) &mutex->__data.__lock, &pd->robust_head.list_op_pending)
 *     & FUTEX_WAITERS) != 0)
 *         futex_wake((u32 *) &mutex->__data.__lock, 1, private);
 * WRITE_ONCE(pd->robust_head.list_op_pending, 0);
 */
extern u32 __vdso_robust_futex_unlock(u32 *uaddr, uintptr_t *op_pending_addr);

/*
 * __vdso_robust_pi_futex_try_unlock - Architecture-specific vDSO implementation of robust PI futex unlock.
 * @uaddr:		Lock address (points to a 32-bit unsigned integer type).
 * @expected:		Expected value (in), value loaded by compare-and-exchange (out).
 * @op_pending_addr:	Robust list operation pending address (points to a pointer type).
 * 
 * The __vdso_robust_pi_futex_try_unlock vDSO try to perform a
 * compare-and-exchange with release semantic to clear the expected
 * *uaddr content. If the futex has waiters, it fails, and userspace
 * needs to call futex_unlock_pi(). Before exiting the critical section,
 * if the cmpxchg fails, it sets bit 1 of *op_pending_addr. If the
 * cmpxchg succeeds, it clears *op_pending_addr.
 * Those operations are within a code region known by the kernel, making
 * them safe with respect to asynchronous program termination either
 * from thread context or from a nested signal handler.
 * 
 * Returns:	Zero if the operation fails to release the lock, non-zero on success.
 *
 * Expected use of this vDSO:
 * 
 * int l = atomic_load_relaxed(&mutex->__data.__lock);
 * do {
 *         if (((l & FUTEX_WAITERS) != 0) || (l != READ_ONCE(pd->tid))) {
 *                 futex_unlock_pi((unsigned int *) &mutex->__data.__lock, private);
 *                 break;
 *         }
 * } while (!__vdso_robust_pi_futex_try_unlock(&mutex->__data.__lock,
 *                                             &l, &pd->robust_head.list_op_pending));
 * WRITE_ONCE(pd->robust_head.list_op_pending, 0);
 */
int __vdso_robust_pi_futex_try_unlock(u32 *uaddr, u32 *expected, uintptr_t *op_pending_addr);

#endif /* _VDSO_FUTEX_H */
