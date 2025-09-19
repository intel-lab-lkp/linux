/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/barrier.h
 *
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (C) 2013 Regents of the University of California
 * Copyright (C) 2017 SiFive
 */

#ifndef _ASM_RISCV_BARRIER_H
#define _ASM_RISCV_BARRIER_H

#ifndef __ASSEMBLY__
#include <asm/cmpxchg.h>
#include <asm/fence.h>

/* These barriers need to enforce ordering on both devices or memory. */
#define __mb()		RISCV_FENCE(iorw, iorw)
#define __rmb()		RISCV_FENCE(ir, ir)
#define __wmb()		RISCV_FENCE(ow, ow)

/* These barriers do not need to enforce ordering on devices, just memory. */
#define __smp_mb()	RISCV_FENCE(rw, rw)
#define __smp_rmb()	RISCV_FENCE(r, r)
#define __smp_wmb()	RISCV_FENCE(w, w)

/*
 * This is a very specific barrier: it's currently only used in two places in
 * the kernel, both in the scheduler.  See include/linux/spinlock.h for the two
 * orderings it guarantees, but the "critical section is RCsc" guarantee
 * mandates a barrier on RISC-V.  The sequence looks like:
 *
 *    lr.aq lock
 *    sc    lock <= LOCKED
 *    smp_mb__after_spinlock()
 *    // critical section
 *    lr    lock
 *    sc.rl lock <= UNLOCKED
 *
 * The AQ/RL pair provides a RCpc critical section, but there's not really any
 * way we can take advantage of that here because the ordering is only enforced
 * on that one lock.  Thus, we're just doing a full fence.
 *
 * Since we allow writeX to be called from preemptive regions we need at least
 * an "o" in the predecessor set to ensure device writes are visible before the
 * task is marked as available for scheduling on a new hart.  While I don't see
 * any concrete reason we need a full IO fence, it seems safer to just upgrade
 * this in order to avoid any IO crossing a scheduling boundary.  In both
 * instances the scheduler pairs this with an mb(), so nothing is necessary on
 * the new hart.
 */
#define smp_mb__after_spinlock()	RISCV_FENCE(iorw, iorw)

extern void __bad_size_call_parameter(void);

#define __smp_store_release(p, v)						\
do {										\
	typeof(p) __p = (p);							\
	union { typeof(*p) __val; char __c[1]; } __u =				\
		{ .__val = (__force typeof(*p)) (v) };				\
	compiletime_assert_atomic_type(*p);					\
	switch (sizeof(*p)) {							\
	case 1:									\
		asm volatile(ALTERNATIVE("fence rw, w;\t\nsb %0, 0(%1)\t\n",	\
					 SB_RL(%0, %1) "\t\nnop\t\n",		\
					 0, RISCV_ISA_EXT_ZALASR, 1)		\
					 : : "r" (*(__u8 *)__u.__c), "r" (__p)	\
					 : "memory");				\
		break;								\
	case 2:									\
		asm volatile(ALTERNATIVE("fence rw, w;\t\nsh %0, 0(%1)\t\n",	\
					 SH_RL(%0, %1) "\t\nnop\t\n",		\
					 0, RISCV_ISA_EXT_ZALASR, 1)		\
					 : : "r" (*(__u16 *)__u.__c), "r" (__p)	\
					 : "memory");				\
		break;								\
	case 4:									\
		asm volatile(ALTERNATIVE("fence rw, w;\t\nsw %0, 0(%1)\t\n",	\
					 SW_RL(%0, %1) "\t\nnop\t\n",		\
					 0, RISCV_ISA_EXT_ZALASR, 1)		\
					 : : "r" (*(__u32 *)__u.__c), "r" (__p)	\
					 : "memory");				\
		break;								\
	case 8:									\
		asm volatile(ALTERNATIVE("fence rw, w;\t\nsd %0, 0(%1)\t\n",	\
					 SD_RL(%0, %1) "\t\nnop\t\n",		\
					 0, RISCV_ISA_EXT_ZALASR, 1)		\
					 : : "r" (*(__u64 *)__u.__c), "r" (__p)	\
					 : "memory");				\
		break;								\
	default:								\
		__bad_size_call_parameter();					\
		break;								\
	}									\
} while (0)

#define __smp_load_acquire(p)							\
({										\
	union { typeof(*p) __val; char __c[1]; } __u;				\
	typeof(p) __p = (p);							\
	compiletime_assert_atomic_type(*p);					\
	switch (sizeof(*p)) {							\
	case 1:									\
		asm volatile(ALTERNATIVE("lb %0, 0(%1)\t\nfence r, rw\t\n",	\
					 LB_AQ(%0, %1) "\t\nnop\t\n",		\
					 0, RISCV_ISA_EXT_ZALASR, 1)		\
					 : "=r" (*(__u8 *)__u.__c) : "r" (__p)	\
					 : "memory");				\
		break;								\
	case 2:									\
		asm volatile(ALTERNATIVE("lh %0, 0(%1)\t\nfence r, rw\t\n",	\
					 LH_AQ(%0, %1) "\t\nnop\t\n",		\
					 0, RISCV_ISA_EXT_ZALASR, 1)		\
					 : "=r" (*(__u16 *)__u.__c) : "r" (__p)	\
					 : "memory");				\
		break;								\
	case 4:									\
		asm volatile(ALTERNATIVE("lw %0, 0(%1)\t\nfence r, rw\t\n",	\
					 LW_AQ(%0, %1) "\t\nnop\t\n",		\
					 0, RISCV_ISA_EXT_ZALASR, 1)		\
					 : "=r" (*(__u32 *)__u.__c) : "r" (__p)	\
					 : "memory");				\
		break;								\
	case 8:									\
		asm volatile(ALTERNATIVE("ld %0, 0(%1)\t\nfence r, rw\t\n",	\
					 LD_AQ(%0, %1) "\t\nnop\t\n",		\
					 0, RISCV_ISA_EXT_ZALASR, 1)		\
					 : "=r" (*(__u64 *)__u.__c) : "r" (__p)	\
					 : "memory");				\
		break;								\
	default:								\
		__bad_size_call_parameter();					\
		break;								\
	}									\
	__u.__val;								\
})

#ifdef CONFIG_RISCV_ISA_ZAWRS
#define smp_cond_load_relaxed(ptr, cond_expr) ({			\
	typeof(ptr) __PTR = (ptr);					\
	__unqual_scalar_typeof(*ptr) VAL;				\
	for (;;) {							\
		VAL = READ_ONCE(*__PTR);				\
		if (cond_expr)						\
			break;						\
		__cmpwait_relaxed(ptr, VAL);				\
	}								\
	(typeof(*ptr))VAL;						\
})
#endif

#include <asm-generic/barrier.h>

#endif /* __ASSEMBLY__ */

#endif /* _ASM_RISCV_BARRIER_H */
