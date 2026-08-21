/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_RISCV_PERCPU_H
#define _ASM_RISCV_PERCPU_H

#include <linux/preempt.h>

#include <asm/cmpxchg.h>

#define _protect_cmpxchg_local(pcp, o, n)				\
({									\
	typeof(pcp) *__ptr;						\
	typeof(pcp) __pcpu_old = (o);					\
	typeof(pcp) __pcpu_new = (n);					\
	typeof(*raw_cpu_ptr(&(pcp))) __ret;				\
	preempt_disable_notrace();					\
	__ptr = raw_cpu_ptr(&(pcp));					\
	__ret = cmpxchg_local(__ptr, __pcpu_old, __pcpu_new);		\
	preempt_enable_notrace();					\
	__ret;								\
})

#define this_cpu_cmpxchg_4(pcp, o, n)	_protect_cmpxchg_local(pcp, o, n)

#ifdef CONFIG_64BIT
#define this_cpu_cmpxchg_8(pcp, o, n)	_protect_cmpxchg_local(pcp, o, n)
#define this_cpu_cmpxchg64(pcp, o, n)	this_cpu_cmpxchg_8(pcp, o, n)

#if defined(CONFIG_RISCV_ISA_ZACAS) && defined(CONFIG_TOOLCHAIN_HAS_ZACAS)
#define _protect_cmpxchg128_local(pcp, o, n)			\
({								\
	typeof(pcp) *__ptr;						\
	typeof(pcp) __pcpu_old = (o);					\
	typeof(pcp) __pcpu_new = (n);					\
	typeof(pcp) __ret;						\
	preempt_disable_notrace();					\
	__ptr = raw_cpu_ptr(&(pcp));					\
	__ret = cmpxchg128_local(__ptr, __pcpu_old, __pcpu_new);	\
	preempt_enable_notrace();					\
	__ret;							\
})

#define this_cpu_cmpxchg128(pcp, o, n)	\
	_protect_cmpxchg128_local(pcp, o, n)
#endif
#endif

#include <asm-generic/percpu.h>

#endif /* _ASM_RISCV_PERCPU_H */
