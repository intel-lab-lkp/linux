/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_UNWIND_USER_H
#define _ASM_X86_UNWIND_USER_H

#ifdef CONFIG_UNWIND_USER

#include <asm/ptrace.h>
#include <asm/uprobes.h>

static inline int unwind_user_word_size(struct pt_regs *regs)
{
	/* We can't unwind VM86 stacks */
	if (regs->flags & X86_VM_MASK)
		return 0;
	return user_64bit_mode(regs) ? 8 : 4;
}

#endif /* CONFIG_UNWIND_USER */

#ifdef CONFIG_HAVE_UNWIND_USER_FP

#define ARCH_INIT_USER_FP_FRAME(ws)			\
	.cfa_off	=  2*(ws),			\
	.ra		= {				\
		.rule		= UNWIND_USER_RULE_CFA_OFFSET_DEREF,\
		.offset		= -1*(ws),		\
			},				\
	.fp		= {				\
		.rule		= UNWIND_USER_RULE_CFA_OFFSET_DEREF,\
		.offset		= -2*(ws),		\
			},				\
	.use_fp		= true,				\
	.outermost	= false,

#define ARCH_INIT_USER_FP_ENTRY_FRAME(ws)		\
	.cfa_off	=  1*(ws),			\
	.ra		= {				\
		.rule		= UNWIND_USER_RULE_CFA_OFFSET_DEREF,\
		.offset		= -1*(ws),		\
			},				\
	.fp		= {				\
		.rule		= UNWIND_USER_RULE_RETAIN,\
			},				\
	.use_fp		= false,			\
	.outermost	= false,

static inline bool unwind_user_at_function_start(struct pt_regs *regs)
{
	return is_uprobe_at_func_entry(regs);
}
#define unwind_user_at_function_start unwind_user_at_function_start

#endif /* CONFIG_HAVE_UNWIND_USER_FP */

#include <asm-generic/unwind_user.h>

#endif /* _ASM_X86_UNWIND_USER_H */
