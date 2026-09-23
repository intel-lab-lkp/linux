/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SPARC_CAS_32_H
#define _SPARC_CAS_32_H

#include <linux/spinlock.h>
#include <linux/types.h>

struct pt_regs;

asmlinkage void handle_sparc32_cas(struct pt_regs *regs, unsigned long pc,
				   unsigned long npc, unsigned long psr);

/* Returns 0 and stores the value found in *prev, or -EFAULT. */
int __sparc32_casa(u32 *addr, u32 oldval, u32 newval, u32 *prev);

/* The same on a user address, using the user data ASI. */
int __sparc32_casa_user(u32 __user *addr, u32 oldval, u32 newval, u32 *prev);

#define SPARC32_ATOMIC_LOCKS	256

extern raw_spinlock_t sparc32_atomic_locks[SPARC32_ATOMIC_LOCKS];

/* Hash on page-offset bits only, so one physical word maps to one lock
 * through any virtual address.  asm/futex_32.h must hash identically.
 */
static inline raw_spinlock_t *sparc32_atomic_lock(const void __user *uaddr)
{
	unsigned long ua = (unsigned long)uaddr;

	return &sparc32_atomic_locks[(ua >> 2) & (SPARC32_ATOMIC_LOCKS - 1)];
}

#endif /* _SPARC_CAS_32_H */
