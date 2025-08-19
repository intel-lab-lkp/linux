/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __ASM_PERCPU_H
#define __ASM_PERCPU_H

#include <linux/preempt.h>

#define PERCPU_RW_OPS(sz)						\
static inline unsigned long __percpu_read_##sz(void *ptr)		\
{									\
	return READ_ONCE(*(u##sz *)ptr);				\
}									\
									\
static inline void __percpu_write_##sz(void *ptr, unsigned long val)	\
{									\
	WRITE_ONCE(*(u##sz *)ptr, (u##sz)val);				\
}

#define __PERCPU_AMO_OP_CASE(sfx, name, sz, amo_insn)			\
static inline void							\
__percpu_##name##_amo_case_##sz(void *ptr, unsigned long val)		\
{									\
	asm volatile (							\
		"amo" #amo_insn #sfx " zero, %[val], %[ptr]"		\
		: [ptr] "+A" (*(u##sz *)ptr)				\
		: [val] "r" ((u##sz)(val))				\
		: "memory");						\
}

#define __PERCPU_AMO_RET_OP_CASE(sfx, name, sz, amo_insn)		\
static inline u##sz							\
__percpu_##name##_return_amo_case_##sz(void *ptr, unsigned long val)	\
{									\
	register u##sz ret;						\
									\
	asm volatile (							\
		"amo" #amo_insn #sfx " %[ret], %[val], %[ptr]"		\
		: [ptr] "+A" (*(u##sz *)ptr), [ret] "=r" (ret)		\
		: [val] "r" ((u##sz)(val))				\
		: "memory");						\
									\
	return ret + val;						\
}

#define PERCPU_OP(name, amo_insn)					\
	__PERCPU_AMO_OP_CASE(.w, name, 32, amo_insn)			\
	__PERCPU_AMO_OP_CASE(.d, name, 64, amo_insn)

#define PERCPU_RET_OP(name, amo_insn)					\
	__PERCPU_AMO_RET_OP_CASE(.w, name, 32, amo_insn)		\
	__PERCPU_AMO_RET_OP_CASE(.d, name, 64, amo_insn)

PERCPU_RW_OPS(8)
PERCPU_RW_OPS(16)
PERCPU_RW_OPS(32)
PERCPU_RW_OPS(64)

PERCPU_OP(add, add)
PERCPU_OP(andnot, and)
PERCPU_OP(or, or)
PERCPU_RET_OP(add, add)

#undef PERCPU_RW_OPS
#undef __PERCPU_AMO_OP_CASE
#undef __PERCPU_AMO_RET_OP_CASE
#undef PERCPU_OP
#undef PERCPU_RET_OP

#define _pcp_protect(op, pcp, ...)					\
({									\
	preempt_disable_notrace();					\
	op(raw_cpu_ptr(&(pcp)), __VA_ARGS__);				\
	preempt_enable_notrace();					\
})

#define _pcp_protect_return(op, pcp, args...)				\
({									\
	typeof(pcp) __retval;						\
	preempt_disable_notrace();					\
	__retval = (typeof(pcp))op(raw_cpu_ptr(&(pcp)), ##args);	\
	preempt_enable_notrace();					\
	__retval;							\
})

#define this_cpu_read_1(pcp)		_pcp_protect_return(__percpu_read_8, pcp)
#define this_cpu_read_2(pcp)		_pcp_protect_return(__percpu_read_16, pcp)
#define this_cpu_read_4(pcp)		_pcp_protect_return(__percpu_read_32, pcp)
#define this_cpu_read_8(pcp)		_pcp_protect_return(__percpu_read_64, pcp)

#define this_cpu_write_1(pcp, val)	_pcp_protect(__percpu_write_8, pcp, (unsigned long)val)
#define this_cpu_write_2(pcp, val)	_pcp_protect(__percpu_write_16, pcp, (unsigned long)val)
#define this_cpu_write_4(pcp, val)	_pcp_protect(__percpu_write_32, pcp, (unsigned long)val)
#define this_cpu_write_8(pcp, val)	_pcp_protect(__percpu_write_64, pcp, (unsigned long)val)

#define this_cpu_add_4(pcp, val)	_pcp_protect(__percpu_add_amo_case_32, pcp, val)
#define this_cpu_add_8(pcp, val)	_pcp_protect(__percpu_add_amo_case_64, pcp, val)

#define this_cpu_add_return_4(pcp, val)		\
_pcp_protect_return(__percpu_add_return_amo_case_32, pcp, val)

#define this_cpu_add_return_8(pcp, val)		\
_pcp_protect_return(__percpu_add_return_amo_case_64, pcp, val)

#define this_cpu_and_4(pcp, val)	_pcp_protect(__percpu_andnot_amo_case_32, pcp, ~val)
#define this_cpu_and_8(pcp, val)	_pcp_protect(__percpu_andnot_amo_case_64, pcp, ~val)

#define this_cpu_or_4(pcp, val)	_pcp_protect(__percpu_or_amo_case_32, pcp, val)
#define this_cpu_or_8(pcp, val)	_pcp_protect(__percpu_or_amo_case_64, pcp, val)

#define this_cpu_xchg_1(pcp, val)	_pcp_protect_return(xchg_relaxed, pcp, val)
#define this_cpu_xchg_2(pcp, val)	_pcp_protect_return(xchg_relaxed, pcp, val)
#define this_cpu_xchg_4(pcp, val)	_pcp_protect_return(xchg_relaxed, pcp, val)
#define this_cpu_xchg_8(pcp, val)	_pcp_protect_return(xchg_relaxed, pcp, val)

#define this_cpu_cmpxchg_1(pcp, o, n)	_pcp_protect_return(cmpxchg_relaxed, pcp, o, n)
#define this_cpu_cmpxchg_2(pcp, o, n)	_pcp_protect_return(cmpxchg_relaxed, pcp, o, n)
#define this_cpu_cmpxchg_4(pcp, o, n)	_pcp_protect_return(cmpxchg_relaxed, pcp, o, n)
#define this_cpu_cmpxchg_8(pcp, o, n)	_pcp_protect_return(cmpxchg_relaxed, pcp, o, n)

#define this_cpu_cmpxchg64(pcp, o, n)	this_cpu_cmpxchg_8(pcp, o, n)

#define this_cpu_cmpxchg128(pcp, o, n)					\
({									\
	typedef typeof(pcp) pcp_op_T__;					\
	u128 old__, new__, ret__;					\
	pcp_op_T__ *ptr__;						\
	old__ = o;							\
	new__ = n;							\
	preempt_disable_notrace();					\
	ptr__ = raw_cpu_ptr(&(pcp));					\
	ret__ = cmpxchg128_local(ptr__, old__, new__);			\
	preempt_enable_notrace();					\
	ret__;								\
})

#include <asm-generic/percpu.h>

#endif /* __ASM_PERCPU_H */
