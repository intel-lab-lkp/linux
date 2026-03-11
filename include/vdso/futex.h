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
 * Expected use of this vDSO:
 *
 * if ((__vdso_robust_futex_unlock((u32 *) &mutex->__data.__lock, &pd->robust_head.list_op_pending)
 *     & FUTEX_WAITERS) != 0)
 *         futex_wake((u32 *) &mutex->__data.__lock, 1, private);
 * WRITE_ONCE(pd->robust_head.list_op_pending, 0);
 *
 * Returns:	The old value present at *uaddr.
 */
extern u32 __vdso_robust_futex_unlock(u32 *uaddr, uintptr_t *op_pending_addr);

#endif /* _VDSO_FUTEX_H */
