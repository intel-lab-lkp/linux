// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */
#include <linux/types.h>
#include <vdso/futex.h>
#include "extable.h"

#ifdef CONFIG_X86_64
# define ASM_PTR_BIT_SET	"btsq "
# define ASM_PTR_SET		"movq "
#else
# define ASM_PTR_BIT_SET	"btsl "
# define ASM_PTR_SET		"movl "
#endif

u32 __vdso_robust_futex_unlock(u32 *uaddr, uintptr_t *op_pending_addr)
{
	u32 val = 0;

	/*
	 * Within the ip range identified by the futex exception table,
	 * the register "eax" contains the value loaded by xchg. This is
	 * expected by futex_vdso_exception() to check whether waiters
	 * need to be woken up. This register state is transferred to
	 * bit 1 (NEED_ACTION) of *op_pending_addr before the ip range
	 * ends.
	 */
	asm volatile (	_ASM_VDSO_EXTABLE_FUTEX_HANDLE(1f, 3f)
			/* Exchange uaddr (store-release). */
			"xchg %[uaddr], %[val]\n\t"
			"1:\n\t"
			/* Test if FUTEX_WAITERS (0x80000000) is set. */
			"test %[val], %[val]\n\t"
			"js 2f\n\t"
			/* Clear *op_pending_addr if there are no waiters. */
			ASM_PTR_SET "$0, %[op_pending_addr]\n\t"
			"jmp 3f\n\t"
			"2:\n\t"
			/* Set bit 1 (NEED_ACTION) in *op_pending_addr. */
			ASM_PTR_BIT_SET "$1, %[op_pending_addr]\n\t"
			"3:\n\t"
			: [val] "+a" (val),
			  [uaddr] "+m" (*uaddr)
			: [op_pending_addr] "m" (*op_pending_addr)
			: "memory");
	return val;
}

u32 robust_futex_unlock(u32 *, uintptr_t *)
	__attribute__((weak, alias("__vdso_robust_futex_unlock")));

int __vdso_robust_pi_futex_try_unlock(u32 *uaddr, u32 *expected, uintptr_t *op_pending_addr)
{
	u32 newval = 0, orig, expect = *expected;

	orig = expect;
	/*
	 * The ZF is set/cleared by cmpxchg and expected to stay
	 * invariant for the rest of the code region.
	 */
	asm volatile (	_ASM_VDSO_EXTABLE_PI_FUTEX_HANDLE(1f, 3f)
			/* Compare-and-exchange uaddr (store-release). Set/clear the ZF. */
			"lock; cmpxchg %[newval], %[uaddr]\n\t"
			"1:\n\t"
			/* Check whether cmpxchg fails. */
			"jnz 2f\n\t"
			/* Clear *op_pending_addr. */
			ASM_PTR_SET "$0, %[op_pending_addr]\n\t"
			"jmp 3f\n\t"
			"2:\n\t"
			/* Set bit 1 (NEED_ACTION) in *op_pending_addr. */
			ASM_PTR_BIT_SET "$1, %[op_pending_addr]\n\t"
			"3:\n\t"
			: [expect] "+a" (expect),
			  [uaddr] "+m" (*uaddr)
			: [op_pending_addr] "m" (*op_pending_addr),
			  [newval] "r" (newval)
			: "memory");
	*expected = expect;
	return expect == orig;
}

int robust_pi_futex_try_unlock(u32 *, u32 *, uintptr_t *)
	__attribute__((weak, alias("__vdso_robust_pi_futex_try_unlock")));
