/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2009 Chen Liqin <liqin.chen@sunplusct.com>
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 */

#ifndef _ASM_RISCV_THREAD_INFO_H
#define _ASM_RISCV_THREAD_INFO_H

#include <asm/page.h>
#include <linux/const.h>
#include <linux/sizes.h>

/* thread information allocation */
#ifdef CONFIG_KASAN
#define KASAN_STACK_ORDER	1
#else
#define KASAN_STACK_ORDER	0
#endif
#define THREAD_SIZE_ORDER	(CONFIG_THREAD_SIZE_ORDER + KASAN_STACK_ORDER)
#define THREAD_SIZE		(PAGE_SIZE << THREAD_SIZE_ORDER)

/*
 * By aligning VMAP'd stacks to 2 * THREAD_SIZE, we can detect overflow by
 * checking sp & (1 << THREAD_SHIFT), which we can do cheaply in the entry
 * assembly.
 */
#ifdef CONFIG_VMAP_STACK
#define THREAD_ALIGN            (2 * THREAD_SIZE)
#else
#define THREAD_ALIGN            THREAD_SIZE
#endif

#define THREAD_SHIFT            (PAGE_SHIFT + THREAD_SIZE_ORDER)
#define OVERFLOW_STACK_SIZE     SZ_4K

#define IRQ_STACK_SIZE		THREAD_SIZE

#ifndef __ASSEMBLER__

#include <asm/processor.h>
#include <asm/csr.h>

/*
 * low level task data that entry.S needs immediate access to
 * - this struct should fit entirely inside of one cache line
 * - if the members of this struct changes, the assembly constants
 *   in asm-offsets.c must be updated accordingly
 * - thread_info is included in task_struct at an offset of 0.  This means that
 *   tp points to both thread_info and task_struct.
 */
struct thread_info {
	unsigned long		flags;		/* low level flags */
	int                     preempt_count;  /* 0=>preemptible, <0=>BUG */
	/*
	 * These stack pointers are overwritten on every system call or
	 * exception.  SP is also saved to the stack it can be recovered when
	 * overwritten.
	 */
	long			kernel_sp;	/* Kernel stack pointer */
	long			user_sp;	/* User stack pointer */
	int			cpu;
	unsigned long		syscall_work;	/* SYSCALL_WORK_ flags */
#ifdef CONFIG_SHADOW_CALL_STACK
	void			*scs_base;
	void			*scs_sp;
#endif
#ifdef CONFIG_64BIT
	/*
	 * Used in handle_exception() to save a0, a1 and a2 before knowing if we
	 * can access the kernel stack.
	 */
	unsigned long		a0, a1, a2;
#endif
#ifdef CONFIG_RISCV_USER_CFI
	struct cfi_state	user_cfi_state;
#endif
};

#ifdef CONFIG_SHADOW_CALL_STACK
#define INIT_SCS							\
	.scs_base	= init_shadow_call_stack,			\
	.scs_sp		= init_shadow_call_stack,
#else
#define INIT_SCS
#endif

/*
 * macros/functions for gaining access to the thread information structure
 *
 * preempt_count needs to be 1 initially, until the scheduler is functional.
 */
#define INIT_THREAD_INFO(tsk)			\
{						\
	.flags		= 0,			\
	.preempt_count	= INIT_PREEMPT_COUNT,	\
	INIT_SCS				\
}

void arch_release_task_struct(struct task_struct *tsk);
int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src);

/*
 * RISC-V stack frame layout (with frame pointer enabled).
 *
 * Reference: RISC-V ELF psABI, Frame Pointer Convention
 *   https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/
 *   riscv-cc.adoc#frame-pointer-convention
 *
 * high addr
 *   +------------------+  <--- fp (s0) points here
 *   |   saved ra       |  fp - 1*sizeof(void*) (return address)
 *   |   saved fp       |  fp - 2*sizeof(void*) (previous frame pointer)
 *   +------------------+
 *   |   local vars     |
 *   |   arguments      |
 *   +------------------+  <--- sp
 * low addr
 *
 * The struct stackframe { fp, ra } lives at (fp - sizeof(stackframe)),
 * i.e. fp[-2]=saved_fp and fp[-1]=saved_ra.
 *
 * For usercopy safety, we allow copies within [prev_fp, fp - 2*sizeof(void*))
 * for each frame in the chain, where prev_fp is the fp of the previous
 * (lower) frame.  This covers local variables and arguments but excludes
 * the saved ra/fp slots at the top of the frame.
 *
 * We walk the frame chain starting from __builtin_frame_address(0) (the
 * current frame), with prev_fp initialized to current_stack_pointer.
 * Using current_stack_pointer -- rather than the 'stack' argument (which is
 * the thread's entire stack base) -- ensures that objects in already-returned
 * frames (address below current sp) are correctly detected as BAD_STACK,
 * because no live frame in the chain will claim that region.
 */
__no_kmsan_checks
static inline int arch_within_stack_frames(const void * const stack,
					   const void * const stackend,
					   const void *obj, unsigned long len)
{
#if defined(CONFIG_FRAME_POINTER)
	const void *fp = (const void *)__builtin_frame_address(0);
	const void *prev_fp = (const void *)current_stack_pointer;

	/*
	 * Walk the frame chain. Each iteration checks whether [obj, obj+len)
	 * falls within the local-variable area of the current frame:
	 *
	 *   [prev_fp, fp - 2*sizeof(void*))
	 *
	 * i.e. from the base of this frame (sp of this frame, which equals
	 * the fp of the frame below) up to (but not including) the saved
	 * fp/ra area at the top of this frame.
	 */
	while (stack + 2 * sizeof(void *) <= fp && fp < stackend) {
		const void *frame_vars_end = (const char *)fp - 2 * sizeof(void *);

		if (obj + len <= frame_vars_end) {
			if (obj >= prev_fp)
				return GOOD_FRAME;
			return BAD_STACK;
		}
		prev_fp = fp;
		fp = *(const void * const *)frame_vars_end;
	}
	return BAD_STACK;
#else
	return NOT_STACK;
#endif
}

#endif /* !__ASSEMBLER__ */

/*
 * thread information flags
 * - these are process state flags that various assembly files may need to
 *   access
 * - pending work-to-be-done flags are in lowest half-word
 * - other flags in upper half-word(s)
 */

/*
 * Tell the generic TIF infrastructure which bits riscv supports
 */
#define HAVE_TIF_NEED_RESCHED_LAZY
#define HAVE_TIF_RESTORE_SIGMASK

#include <asm-generic/thread_info_tif.h>

#define TIF_32BIT			16	/* compat-mode 32bit process */
#define TIF_RISCV_V_DEFER_RESTORE	17	/* restore Vector before returing to user */

#define _TIF_RISCV_V_DEFER_RESTORE	BIT(TIF_RISCV_V_DEFER_RESTORE)

#endif /* _ASM_RISCV_THREAD_INFO_H */
