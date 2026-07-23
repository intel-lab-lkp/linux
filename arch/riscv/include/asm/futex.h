/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2006  Ralf Baechle (ralf@linux-mips.org)
 * Copyright (c) 2018  Jim Wilson (jimw@sifive.com)
 */

#ifndef _ASM_RISCV_FUTEX_H
#define _ASM_RISCV_FUTEX_H

#include <linux/futex.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <asm/asm.h>
#include <asm/asm-extable.h>
#include <asm/errata_list.h>

/* We don't even really need the extable code, but for now keep it simple */
#ifndef CONFIG_MMU
#define __enable_user_access()		do { } while (0)
#define __disable_user_access()		do { } while (0)
#endif

#define __futex_atomic_op(insn, ret, oldval, uaddr, oparg, temp)	\
{								\
	__enable_user_access();					\
	ALT_FUTEX_ATOMIC_OP(insn, ret, oldval, uaddr, oparg, temp);	\
	__disable_user_access();				\
}

#define __futex_atomic_swap(ret, oldval, uaddr, oparg, temp)	\
{								\
	__enable_user_access();					\
	ALT_FUTEX_ATOMIC_SWAP(ret, oldval, uaddr, oparg, temp);	\
	__disable_user_access();				\
}

static inline int
arch_futex_atomic_op_inuser(int op, int oparg, int *oval, u32 __user *uaddr)
{
	int __maybe_unused oldval = 0, ret = 0, temp = 0;

	if (!access_ok(uaddr, sizeof(u32)))
		return -EFAULT;

	switch (op) {
	case FUTEX_OP_SET:
		__futex_atomic_swap(ret, oldval, uaddr, oparg, temp);
		break;
	case FUTEX_OP_ADD:
		__futex_atomic_op(add,
				  ret, oldval, uaddr, oparg, temp);
		break;
	case FUTEX_OP_OR:
		__futex_atomic_op(or,
				  ret, oldval, uaddr, oparg, temp);
		break;
	case FUTEX_OP_ANDN:
		__futex_atomic_op(and,
				  ret, oldval, uaddr, ~oparg, temp);
		break;
	case FUTEX_OP_XOR:
		__futex_atomic_op(xor,
				  ret, oldval, uaddr, oparg, temp);
		break;
	default:
		ret = -ENOSYS;
	}

	if (!ret)
		*oval = oldval;

	return ret;
}

static inline int
futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
			      u32 oldval, u32 newval)
{
	int ret = 0;
	u32 val;
	uintptr_t tmp;

	if (!access_ok(uaddr, sizeof(u32)))
		return -EFAULT;

	__enable_user_access();
	__asm__ __volatile__ (
	"1:	lr.w %[v],%[u]			        \n"
	"	bne %[v],%z[ov],3f			\n"
	"2:	sc.w.aqrl %[t],%z[nv],%[u]		\n"
	"	bnez %[t],1b				\n"
	"3:						\n"
		_ASM_EXTABLE_UACCESS_ERR(1b, 3b, %[r])	\
		_ASM_EXTABLE_UACCESS_ERR(2b, 3b, %[r])	\
	: [r] "+r" (ret), [v] "=&r" (val), [u] "+m" (*uaddr), [t] "=&r" (tmp)
	: [ov] "Jr" ((long)(int)oldval), [nv] "Jr" (newval)
	: "memory");
	__disable_user_access();

	*uval = val;
	return ret;
}

#endif /* _ASM_RISCV_FUTEX_H */
