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

#define FUTEX_ATOMIC_OP(op)										\
static __always_inline int										\
__futex_atomic_##op(int oparg, u32 __user *uaddr, int *oval)	\
{									\
	return __lsui_ll_sc_u_body(futex_atomic_##op, oparg, uaddr, oval);					\
}

FUTEX_ATOMIC_OP(add)
FUTEX_ATOMIC_OP(or)
FUTEX_ATOMIC_OP(and)
FUTEX_ATOMIC_OP(eor)
FUTEX_ATOMIC_OP(set)

#undef FUTEX_ATOMIC_OP

static __always_inline int
__futex_cmpxchg(u32 __user *uaddr, u32 oldval, u32 newval, u32 *oval)
{
	return __lsui_ll_sc_u_body(futex_cmpxchg, uaddr, oldval, newval, oval);
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
