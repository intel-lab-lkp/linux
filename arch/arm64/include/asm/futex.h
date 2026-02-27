/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_FUTEX_H
#define __ASM_FUTEX_H

#include <linux/futex.h>
#include <linux/uaccess.h>

#include <asm/errno.h>
#include <asm/lsui.h>

#define FUTEX_MAX_LOOPS	128 /* What's the largest number you can think of? */

#define LLSC_FUTEX_ATOMIC_OP(op, insn)					\
static __always_inline int						\
__llsc_futex_atomic_##op(int oparg, u32 __user *uaddr, int *oval)	\
{									\
	unsigned int loops = FUTEX_MAX_LOOPS;				\
	int ret, oldval, newval;					\
									\
	uaccess_enable_privileged();					\
	asm volatile("// __llsc_futex_atomic_" #op "\n"			\
"	prfm	pstl1strm, %2\n"					\
"1:	ldxr	%w[oldval], %2\n"					\
	insn "\n"							\
"2:	stlxr	%w0, %w[newval], %2\n"					\
"	cbz	%w0, 3f\n"						\
"	sub	%w4, %w4, %w0\n"					\
"	cbnz	%w4, 1b\n"						\
"	mov	%w0, %w6\n"						\
"3:\n"									\
"	dmb	ish\n"							\
	_ASM_EXTABLE_UACCESS_ERR(1b, 3b, %w0)				\
	_ASM_EXTABLE_UACCESS_ERR(2b, 3b, %w0)				\
	: "=&r" (ret), [oldval] "=&r" (oldval), "+Q" (*uaddr),		\
	  [newval] "=&r" (newval),					\
	  "+r" (loops)							\
	: [oparg] "r" (oparg), "Ir" (-EAGAIN)				\
	: "memory");							\
	uaccess_disable_privileged();					\
									\
	if (!ret)							\
		*oval = oldval;						\
									\
	return ret;							\
}

LLSC_FUTEX_ATOMIC_OP(add, "add	%w[newval], %w[oldval], %w[oparg]")
LLSC_FUTEX_ATOMIC_OP(or,  "orr	%w[newval], %w[oldval], %w[oparg]")
LLSC_FUTEX_ATOMIC_OP(and, "and	%w[newval], %w[oldval], %w[oparg]")
LLSC_FUTEX_ATOMIC_OP(eor, "eor	%w[newval], %w[oldval], %w[oparg]")
LLSC_FUTEX_ATOMIC_OP(set, "mov	%w[newval], %w[oparg]")

static __always_inline int
__llsc_futex_cmpxchg(u32 __user *uaddr, u32 oldval, u32 newval, u32 *oval)
{
	int ret = 0;
	unsigned int loops = FUTEX_MAX_LOOPS;
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

#ifdef CONFIG_ARM64_LSUI

/*
 * FEAT_LSUI is supported since Armv9.6, where FEAT_PAN is mandatory.
 * However, this assumption may not always hold:
 *
 *   - Some CPUs advertise FEAT_LSUI but lack FEAT_PAN.
 *   - Virtualisation or ID register overrides may expose invalid
 *     feature combinations.
 *
 * Rather than disabling FEAT_LSUI when FEAT_PAN is absent, wrap LSUI
 * instructions with uaccess_ttbr0_enable()/disable() when
 * ARM64_SW_TTBR0_PAN is enabled.
 */

#define LSUI_FUTEX_ATOMIC_OP(op, asm_op)				\
static __always_inline int						\
__lsui_futex_atomic_##op(int oparg, u32 __user *uaddr, int *oval)	\
{									\
	int ret = 0;							\
	int oldval;							\
									\
	uaccess_ttbr0_enable();						\
									\
	asm volatile("// __lsui_futex_atomic_" #op "\n"			\
	__LSUI_PREAMBLE							\
"1:	" #asm_op "al	%w3, %w2, %1\n"					\
"2:\n"									\
	_ASM_EXTABLE_UACCESS_ERR(1b, 2b, %w0)				\
	: "+r" (ret), "+Q" (*uaddr), "=r" (oldval)			\
	: "r" (oparg)							\
	: "memory");							\
									\
	uaccess_ttbr0_disable();					\
									\
	if (!ret)							\
		*oval = oldval;						\
	return ret;							\
}

LSUI_FUTEX_ATOMIC_OP(add, ldtadd)
LSUI_FUTEX_ATOMIC_OP(or, ldtset)
LSUI_FUTEX_ATOMIC_OP(andnot, ldtclr)
LSUI_FUTEX_ATOMIC_OP(set, swpt)

static __always_inline int
__lsui_cmpxchg64(u64 __user *uaddr, u64 *oldval, u64 newval)
{
	int ret = 0;

	uaccess_ttbr0_enable();

	asm volatile("// __lsui_cmpxchg64\n"
	__LSUI_PREAMBLE
"1:	casalt	%2, %3, %1\n"
"2:\n"
	_ASM_EXTABLE_UACCESS_ERR(1b, 2b, %w0)
	: "+r" (ret), "+Q" (*uaddr), "+r" (*oldval)
	: "r" (newval)
	: "memory");

	uaccess_ttbr0_disable();

	return ret;
}

static __always_inline int
__lsui_cmpxchg32(u32 __user *uaddr, u32 oldval, u32 newval, u32 *oval)
{
	u64 __user *uaddr64;
	bool futex_pos, other_pos;
	int ret, i;
	u32 other, orig_other;
	union {
		u32 futex[2];
		u64 raw;
	} oval64, orig64, nval64;

	uaddr64 = (u64 __user *) PTR_ALIGN_DOWN(uaddr, sizeof(u64));
	futex_pos = !IS_ALIGNED((unsigned long)uaddr, sizeof(u64));
	other_pos = !futex_pos;

	oval64.futex[futex_pos] = oldval;
	ret = get_user(oval64.futex[other_pos], (u32 __user *)uaddr64 + other_pos);
	if (ret)
		return -EFAULT;

	ret = -EAGAIN;
	for (i = 0; i < FUTEX_MAX_LOOPS; i++) {
		orig64.raw = nval64.raw = oval64.raw;

		nval64.futex[futex_pos] = newval;

		if (__lsui_cmpxchg64(uaddr64, &oval64.raw, nval64.raw))
			return -EFAULT;

		oldval = oval64.futex[futex_pos];
		other = oval64.futex[other_pos];
		orig_other = orig64.futex[other_pos];

		if (other == orig_other) {
			ret = 0;
			break;
		}
	}

	if (!ret)
		*oval = oldval;

	return ret;
}

static __always_inline int
__lsui_futex_atomic_and(int oparg, u32 __user *uaddr, int *oval)
{
	/*
	 * Undo the bitwise negation applied to the oparg passed from
	 * arch_futex_atomic_op_inuser() with FUTEX_OP_ANDN.
	 */
	return __lsui_futex_atomic_andnot(~oparg, uaddr, oval);
}

static __always_inline int
__lsui_futex_atomic_eor(int oparg, u32 __user *uaddr, int *oval)
{
	u32 oldval, newval, val;
	int ret, i;

	if (get_user(oldval, uaddr))
		return -EFAULT;

	/*
	 * there are no ldteor/stteor instructions...
	 */
	for (i = 0; i < FUTEX_MAX_LOOPS; i++) {
		newval = oldval ^ oparg;

		ret = __lsui_cmpxchg32(uaddr, oldval, newval, &val);
		if (ret)
			return ret;

		if (val == oldval) {
			*oval = val;
			return 0;
		}

		oldval = val;
	}

	return -EAGAIN;
}

static __always_inline int
__lsui_futex_cmpxchg(u32 __user *uaddr, u32 oldval, u32 newval, u32 *oval)
{
	return __lsui_cmpxchg32(uaddr, oldval, newval, oval);
}
#endif	/* CONFIG_ARM64_LSUI */


#define FUTEX_ATOMIC_OP(op)						\
static __always_inline int						\
__futex_atomic_##op(int oparg, u32 __user *uaddr, int *oval)		\
{									\
	return __lsui_llsc_body(futex_atomic_##op, oparg, uaddr, oval);	\
}

FUTEX_ATOMIC_OP(add)
FUTEX_ATOMIC_OP(or)
FUTEX_ATOMIC_OP(and)
FUTEX_ATOMIC_OP(eor)
FUTEX_ATOMIC_OP(set)

static __always_inline int
__futex_cmpxchg(u32 __user *uaddr, u32 oldval, u32 newval, u32 *oval)
{
	return __lsui_llsc_body(futex_cmpxchg, uaddr, oldval, newval, oval);
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
