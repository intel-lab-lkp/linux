/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Arm Ltd.
 */

#ifndef __ASM_FUTEX_LSUI_H
#define __ASM_FUTEX_LSUI_H

#include <linux/uaccess.h>
#include <linux/stringify.h>

#define FUTEX_ATOMIC_OP(op, asm_op, mb)					\
static __always_inline int						\
__lsui_futex_atomic_##op(int oparg, u32 __user *uaddr, int *oval)	\
{									\
	int ret = 0;							\
	int val;							\
									\
	mte_enable_tco();						\
	uaccess_ttbr0_enable();						\
									\
	asm volatile("// __lsui_futex_atomic_" #op "\n"			\
	__LSUI_PREAMBLE							\
	"1:	" #asm_op #mb "	%w3, %w2, %1\n"				\
	"2:\n"								\
	_ASM_EXTABLE_UACCESS_ERR(1b, 2b, %w0)				\
	: "+r" (ret), "+Q" (*uaddr), "=r" (val)				\
	: "r" (oparg)							\
	: "memory");							\
									\
	mte_disable_tco();						\
	uaccess_ttbr0_disable();					\
									\
	if (!ret)							\
		*oval = val;						\
									\
	return ret;							\
}

FUTEX_ATOMIC_OP(add, ldtadd, al)
FUTEX_ATOMIC_OP(or, ldtset, al)
FUTEX_ATOMIC_OP(andnot, ldtclr, al)
FUTEX_ATOMIC_OP(set, swpt, al)

#undef FUTEX_ATOMIC_OP

static __always_inline int
__lsui_futex_atomic_and(int oparg, u32 __user *uaddr, int *oval)
{
	return __lsui_futex_atomic_andnot(~oparg, uaddr, oval);
}

static __always_inline int
__lsui_futex_atomic_eor(int oparg, u32 __user *uaddr, int *oval)
{
	unsigned int loops = LL_SC_MAX_LOOPS;
	int ret, val, tmp;

	mte_enable_tco();
	uaccess_ttbr0_enable();

	asm volatile("// __lsui_futex_atomic_eor\n"
	__LSUI_PREAMBLE
	"	prfm	pstl1strm, %2\n"
	"1:	ldtxr	%w1, %2\n"
	"	eor	%w3, %w1, %w5\n"
	"2:	stltxr	%w0, %w3, %2\n"
	"	cbz	%w0, 3f\n"
	"	sub	%w4, %w4, %w0\n"
	"	cbnz	%w4, 1b\n"
	"	mov	%w0, %w6\n"
	"3:\n"
	"	dmb	ish\n"
	_ASM_EXTABLE_UACCESS_ERR(1b, 3b, %w0)
	_ASM_EXTABLE_UACCESS_ERR(2b, 3b, %w0)
	: "=&r" (ret), "=&r" (val), "+Q" (*uaddr), "=&r" (tmp),
	  "+r" (loops)
	: "r" (oparg), "Ir" (-EAGAIN)
	: "memory");

	mte_disable_tco();
	uaccess_ttbr0_disable();

	if (!ret)
		*oval = val;

	return ret;
}

static __always_inline int
__lsui_futex_cmpxchg(u32 __user *uaddr, u32 oldval, u32 newval, u32 *oval)
{
	int ret = 0;
	unsigned int loops = LL_SC_MAX_LOOPS;
	u32 val, tmp;

	mte_enable_tco();
	uaccess_ttbr0_enable();

	/*
	 * cas{al}t doesn't support word size...
	 */
	asm volatile("//__lsui_futex_cmpxchg\n"
	__LSUI_PREAMBLE
	"	prfm	pstl1strm, %2\n"
	"1:	ldtxr	%w1, %2\n"
	"	eor	%w3, %w1, %w5\n"
	"	cbnz	%w3, 4f\n"
	"2:	stltxr	%w3, %w6, %2\n"
	"	cbz	%w3, 3f\n"
	"	sub	%w4, %w4, %w3\n"
	"	cbnz	%w4, 1b\n"
	"	mov	%w0, %w7\n"
	"3:\n"
	"	dmb	ish\n"
	"4:\n"
	_ASM_EXTABLE_UACCESS_ERR(1b, 4b, %w0)
	_ASM_EXTABLE_UACCESS_ERR(2b, 4b, %w0)
	: "+r" (ret), "=&r" (val), "+Q" (*uaddr), "=&r" (tmp), "+r" (loops)
	: "r" (oldval), "r" (newval), "Ir" (-EAGAIN)
	: "memory");

	mte_disable_tco();
	uaccess_ttbr0_disable();

	if (!ret)
		*oval = oldval;

	return ret;
}

#endif /* __ASM_FUTEX_LSUI_H */
