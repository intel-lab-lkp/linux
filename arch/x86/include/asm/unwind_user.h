/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UNWIND_USER_H
#define _ASM_X86_UNWIND_USER_H

#ifdef CONFIG_UNWIND_USER

#include <linux/sched/task_stack.h>
#include <asm/ptrace.h>
#include <asm/uprobes.h>

static inline int unwind_user_word_size(struct pt_regs *regs)
{
	/* We can't unwind VM86 stacks */
	if (regs->flags & X86_VM_MASK)
		return 0;
	return user_64bit_mode(regs) ? 8 : 4;
}

#ifdef CONFIG_X86_64

static inline int unwind_user_get_reg(unsigned long *val, unsigned int regnum)
{
	struct pt_regs *regs = task_pt_regs(current);

	/* TODO: eh_frame is currently limited to x86-64 */
	if (!user_64bit_mode(regs))
		return -EINVAL;

	switch (regnum) {
	/* DWARF register numbers 0..15 */
	case  0: *val = regs->ax; break;
	case  1: *val = regs->dx; break;
	case  2: *val = regs->cx; break;
	case  3: *val = regs->bx; break;
	case  4: *val = regs->si; break;
	case  5: *val = regs->di; break;
	case  6: *val = regs->bp; break;
	case  7: *val = regs->sp; break;
	case  8: *val = regs->r8; break;
	case  9: *val = regs->r9; break;
	case 10: *val = regs->r10; break;
	case 11: *val = regs->r11; break;
	case 12: *val = regs->r12; break;
	case 13: *val = regs->r13; break;
	case 14: *val = regs->r14; break;
	case 15: *val = regs->r15; break;
	default:
		pr_debug("%s (%d): %s(%u): unsupported register number\n",
			 current->comm, current->pid, __func__, regnum);
		return -EINVAL;
	}
	return 0;
}
#define unwind_user_get_reg unwind_user_get_reg

#endif /* CONFIG_X86_64 */

#endif /* CONFIG_UNWIND_USER */

#ifdef CONFIG_HAVE_UNWIND_USER_FP

#define ARCH_INIT_USER_FP_FRAME(ws)			\
	.cfa		= {				\
		.rule		= UNWIND_USER_CFA_RULE_FP_OFFSET,\
		.offset		=  2*(ws),		\
			},				\
	.ra		= {				\
		.rule		= UNWIND_USER_RULE_CFA_OFFSET_DEREF,\
		.offset		= -1*(ws),		\
			},				\
	.fp		= {				\
		.rule		= UNWIND_USER_RULE_CFA_OFFSET_DEREF,\
		.offset		= -2*(ws),		\
			},				\
	.sp_off		= 0,				\
	.outermost	= false,

#define ARCH_INIT_USER_FP_ENTRY_FRAME(ws)		\
	.cfa		= {				\
		.rule		= UNWIND_USER_CFA_RULE_SP_OFFSET,\
		.offset		=  1*(ws),		\
			},				\
	.ra		= {				\
		.rule		= UNWIND_USER_RULE_CFA_OFFSET_DEREF,\
		.offset		= -1*(ws),		\
			},				\
	.fp		= {				\
		.rule		= UNWIND_USER_RULE_RETAIN,\
			},				\
	.sp_off		= 0,				\
	.outermost	= false,

static inline bool unwind_user_at_function_start(struct pt_regs *regs)
{
	return is_uprobe_at_func_entry(regs);
}
#define unwind_user_at_function_start unwind_user_at_function_start

#endif /* CONFIG_HAVE_UNWIND_USER_FP */

#include <asm-generic/unwind_user.h>

#endif /* _ASM_X86_UNWIND_USER_H */
