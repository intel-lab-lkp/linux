/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Arm Ltd.
 */
#ifndef __ASM_FUTEX_LL_SC_U_H
#define __ASM_FUTEX_LL_SC_U_H

#include <linux/uaccess.h>
#include <linux/stringify.h>

#define FUTEX_ATOMIC_OP(op, asm_op)					\
static __always_inline int						\
__ll_sc_u_futex_atomic_##op(int oparg, u32 __user *uaddr, int *oval)	\
{									\
	unsigned int loops = LL_SC_MAX_LOOPS;				\
	int ret, val, tmp;						\
									\
	uaccess_enable_privileged();					\
	asm volatile("// __ll_sc_u_futex_atomic_" #op "\n"		\
	"	prfm	pstl1strm, %2\n"				\
	"1:	ldxr	%w1, %2\n"					\
	"	" #asm_op "	%w3, %w1, %w5\n"			\
	"2:	stlxr	%w0, %w3, %2\n"					\
	"	cbz	%w0, 3f\n"					\
	"	sub	%w4, %w4, %w0\n"				\
	"	cbnz	%w4, 1b\n"					\
	"	mov	%w0, %w6\n"					\
	"3:\n"								\
	"	dmb	ish\n"						\
	_ASM_EXTABLE_UACCESS_ERR(1b, 3b, %w0)				\
	_ASM_EXTABLE_UACCESS_ERR(2b, 3b, %w0)				\
	: "=&r" (ret), "=&r" (val), "+Q" (*uaddr), "=&r" (tmp),		\
	  "+r" (loops)							\
	: "r" (oparg), "Ir" (-EAGAIN)					\
	: "memory");							\
	uaccess_disable_privileged();					\
									\
	if (!ret)							\
		*oval = val;						\
									\
	return ret;							\
}

FUTEX_ATOMIC_OP(add, add)
FUTEX_ATOMIC_OP(or, orr)
FUTEX_ATOMIC_OP(and, and)
FUTEX_ATOMIC_OP(eor, eor)

#undef FUTEX_ATOMIC_OP

static __always_inline int
__ll_sc_u_futex_atomic_set(int oparg, u32 __user *uaddr, int *oval)
{
	unsigned int loops = LL_SC_MAX_LOOPS;
	int ret, val;

	uaccess_enable_privileged();
	asm volatile("//__ll_sc_u_futex_xchg\n"
	"	prfm	pstl1strm, %2\n"
	"1:	ldxr	%w1, %2\n"
	"2:	stlxr	%w0, %w4, %2\n"
	"	cbz	%w3, 3f\n"
	"	sub	%w3, %w3, %w0\n"
	"	cbnz	%w3, 1b\n"
	"	mov	%w0, %w5\n"
	"3:\n"
	"	dmb	ish\n"
	_ASM_EXTABLE_UACCESS_ERR(1b, 3b, %w0)
	_ASM_EXTABLE_UACCESS_ERR(2b, 3b, %w0)
	: "=&r" (ret), "=&r" (val), "+Q" (*uaddr), "+r" (loops)
	: "r" (oparg), "Ir" (-EAGAIN)
	: "memory");
	uaccess_disable_privileged();

	if (!ret)
		*oval = val;

	return ret;
}

static __always_inline int
__ll_sc_u_futex_cmpxchg(u32 __user *uaddr, u32 oldval, u32 newval, u32 *oval)
{
	int ret = 0;
	unsigned int loops = LL_SC_MAX_LOOPS;
	u32 val, tmp;

	uaccess_enable_privileged();
	asm volatile("//__ll_sc_u_futex_cmpxchg\n"
	"	prfm	pstl1strm, %2\n"
	"1:	ldxr	%w1, %2\n"
	"	eor	%w3, %w1, %w5\n"
	"	cbnz	%w3, 4f\n"
	"2:	stlxr	%w3, %w6, %2\n"
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
	uaccess_disable_privileged();

	if (!ret)
		*oval = val;

	return ret;
}

#endif /* __ASM_FUTEX_LL_SC_U_H */
