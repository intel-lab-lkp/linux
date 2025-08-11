/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_FUTEX_H
#define __ASM_FUTEX_H

#include <linux/futex.h>
#include <linux/uaccess.h>
#include <linux/stringify.h>

#include <asm/errno.h>

#define LLSC_MAX_LOOPS	128 /* What's the largest number you can think of? */

#define LLSC_FUTEX_ATOMIC_OP(op, asm_op)				\
static __always_inline int						\
__llsc_futex_atomic_##op(int oparg, u32 __user *uaddr, int *oval)	\
{									\
	unsigned int loops = LLSC_MAX_LOOPS;				\
	int ret, val, tmp;						\
									\
	uaccess_enable_privileged();					\
	asm volatile("// __llsc_futex_atomic_" #op "\n"		\
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

LLSC_FUTEX_ATOMIC_OP(add, add)
LLSC_FUTEX_ATOMIC_OP(or, orr)
LLSC_FUTEX_ATOMIC_OP(and, and)
LLSC_FUTEX_ATOMIC_OP(eor, eor)

static __always_inline int
__llsc_futex_atomic_set(int oparg, u32 __user *uaddr, int *oval)
{
	unsigned int loops = LLSC_MAX_LOOPS;
	int ret, val;

	uaccess_enable_privileged();
	asm volatile("//__llsc_futex_xchg\n"
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
__llsc_futex_cmpxchg(u32 __user *uaddr, u32 oldval, u32 newval, u32 *oval)
{
	int ret = 0;
	unsigned int loops = LLSC_MAX_LOOPS;
	u32 val, tmp;

	uaccess_enable_privileged();
	asm volatile("//__llsc_futex_cmpxchg\n"
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

#define FUTEX_ATOMIC_OP(op)						\
static __always_inline int						\
__futex_atomic_##op(int oparg, u32 __user *uaddr, int *oval)		\
{									\
	return __llsc_futex_atomic_##op(oparg, uaddr, oval);		\
}

FUTEX_ATOMIC_OP(add)
FUTEX_ATOMIC_OP(or)
FUTEX_ATOMIC_OP(and)
FUTEX_ATOMIC_OP(eor)
FUTEX_ATOMIC_OP(set)

static __always_inline int
__futex_cmpxchg(u32 __user *uaddr, u32 oldval, u32 newval, u32 *oval)
{
	return __llsc_futex_cmpxchg(uaddr, oldval, newval, oval);
}

static inline int
arch_futex_atomic_op_inuser(int op, int oparg, int *oval, u32 __user *_uaddr)
{
	int ret;
	u32 __user *uaddr;

	if (!access_ok(_uaddr, sizeof(u32)))
		return -EFAULT;

	uaddr = __uaccess_mask_ptr(_uaddr);

	switch (op) {
	case FUTEX_OP_SET:
		ret = __futex_atomic_set(oparg, uaddr, oval);
		break;
	case FUTEX_OP_ADD:
		ret = __futex_atomic_add(oparg, uaddr, oval);
		break;
	case FUTEX_OP_OR:
		ret = __futex_atomic_or(oparg, uaddr, oval);
		break;
	case FUTEX_OP_ANDN:
		ret = __futex_atomic_and(~oparg, uaddr, oval);
		break;
	case FUTEX_OP_XOR:
		ret = __futex_atomic_eor(oparg, uaddr, oval);
		break;
	default:
		ret = -ENOSYS;
	}

	return ret;
}

static inline int
futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *_uaddr,
			      u32 oldval, u32 newval)
{
	u32 __user *uaddr;

	if (!access_ok(_uaddr, sizeof(u32)))
		return -EFAULT;

	uaddr = __uaccess_mask_ptr(_uaddr);

	return __futex_cmpxchg(uaddr, oldval, newval, uval);
}

#endif /* __ASM_FUTEX_H */
