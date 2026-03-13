/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/thread_info.h
 *
 * Copyright (C) 2002 Russell King.
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_THREAD_INFO_H
#define __ASM_THREAD_INFO_H

#include <linux/compiler.h>

#ifndef __ASSEMBLER__

struct task_struct;

#include <asm/memory.h>
#include <asm/stack_pointer.h>
#include <asm/types.h>

/*
 * low level task data that entry.S needs immediate access to.
 */
struct thread_info {
	unsigned long		flags;		/* low level flags */
#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	u64			ttbr0;		/* saved TTBR0_EL1 */
#endif
	union {
		u64		preempt_count;	/* 0 => preemptible, <0 => bug */
		struct {
#ifdef CONFIG_CPU_BIG_ENDIAN
			u32	need_resched;
			u32	count;
#else
			u32	count;
			u32	need_resched;
#endif
		} preempt;
	};
#ifdef CONFIG_SHADOW_CALL_STACK
	void			*scs_base;
	void			*scs_sp;
#endif
	u32			cpu;
	unsigned long		syscall_work;   /* SYSCALL_WORK_ flags */
};

#define thread_saved_pc(tsk)	\
	((unsigned long)(tsk->thread.cpu_context.pc))
#define thread_saved_sp(tsk)	\
	((unsigned long)(tsk->thread.cpu_context.sp))
#define thread_saved_fp(tsk)	\
	((unsigned long)(tsk->thread.cpu_context.fp))

void arch_setup_new_exec(void);
#define arch_setup_new_exec     arch_setup_new_exec

#endif

/*
 * Tell the generic TIF infrastructure which bits arm64 supports
 */
#define HAVE_TIF_NEED_RESCHED_LAZY
#define HAVE_TIF_RESTORE_SIGMASK
#define HAVE_TIF_SINGLESTEP

#include <asm-generic/thread_info_tif.h>

#define TIF_FOREIGN_FPSTATE	16	/* CPU's FP state is not current's */
#define TIF_MTE_ASYNC_FAULT	17	/* MTE Asynchronous Tag Check Fault */
#define TIF_FREEZE		18
#define TIF_32BIT		19      /* 32bit process */
#define TIF_SVE			20	/* Scalable Vector Extension in use */
#define TIF_SVE_VL_INHERIT	21	/* Inherit SVE vl_onexec across exec */
#define TIF_SSBD		22	/* Wants SSB mitigation */
#define TIF_TAGGED_ADDR		23	/* Allow tagged user addresses */
#define TIF_SME			24	/* SME in use */
#define TIF_SME_VL_INHERIT	25	/* Inherit SME vl_onexec across exec */
#define TIF_KERNEL_FPSTATE	26	/* Task is in a kernel mode FPSIMD section */
#define TIF_TSC_SIGSEGV		27	/* SIGSEGV on counter-timer access */
#define TIF_LAZY_MMU_PENDING	28	/* Ops pending for lazy mmu mode exit */

#define _TIF_FOREIGN_FPSTATE	BIT(TIF_FOREIGN_FPSTATE)
#define _TIF_32BIT		BIT(TIF_32BIT)
#define _TIF_SVE		BIT(TIF_SVE)
#define _TIF_MTE_ASYNC_FAULT	BIT(TIF_MTE_ASYNC_FAULT)
#define _TIF_TSC_SIGSEGV	BIT(TIF_TSC_SIGSEGV)

#ifdef CONFIG_SHADOW_CALL_STACK
#define INIT_SCS							\
	.scs_base	= init_shadow_call_stack,			\
	.scs_sp		= init_shadow_call_stack,
#else
#define INIT_SCS
#endif

#define INIT_THREAD_INFO(tsk)						\
{									\
	.flags		= _TIF_FOREIGN_FPSTATE,				\
	.preempt_count	= INIT_PREEMPT_COUNT,				\
	INIT_SCS							\
}

#endif /* __ASM_THREAD_INFO_H */
